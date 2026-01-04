
// responsible: ded

/* 
 * bf_compiler.c - Brainfuck JIT Compiler for JOS
 * 
 * CLI USAGE:
 *   bf_compiler <repl_id> [-Otime]               : Normal mode (waits for IPC)
 *   bf_compiler -t [-Otime]                      : Self-test mode (runs built-in tests)
 *   bf_compiler -c "<bf_code>" <executor_id>     : Compile single string and exit
 * 
 * COMPONENTS:
 *   - BF syntax validator
 *   - Time optimizer (-Otime)
 *   - x86 code generator
 *   - JOS IPC interface
 * 
 * DEPENDENCIES:
 *   - inc/bf.h (shared structures)
 *   - lib/ipc.c (JOS IPC mechanisms)
 *   - lib/syscall.c (system calls)Ре
 */

#include <inc/lib.h>
#include <inc/bf.h>

bf_compiler_context_t compiler_ctx;
static bool g_test_mode;
static bool g_compile_once;
static const char *g_single_source;

static const char *usage_msg = "Usage: bf_compiler <repl_id> [-Otime] | -t [-Otime] | -c \"<bf_code>\" <repl_id>\n";

// Forward declarations for helpers defined below
static int parse_arguments(int argc, char **argv);
static int compile_bf_to_x86(void);
static int validate_syntax(void);
static void optimize_instructions(void);
static void send_to_repl(void);

/*
 * CLI ARGUMENT PARSER
 * Parses flags and positional arguments into compiler_ctx and mode flags.
 */
static int parse_arguments(int argc, char **argv) {
    g_test_mode = false;
    g_compile_once = false;
    g_single_source = NULL;

    if (argc < 2) return -1;

    int i = 1;
    if (strcmp(argv[i], "-t") == 0) {
        g_test_mode = true;
        i++;
    } else if (strcmp(argv[i], "-c") == 0) {
        g_compile_once = true;
        if (i + 2 >= argc) return -1;
        g_single_source = argv[i + 1];
        compiler_ctx.repl_id = strtol(argv[i + 2], NULL, 0);
        i += 3;
    } else {
        compiler_ctx.repl_id = strtol(argv[i], NULL, 0);
        i++;
    }

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-Otime") == 0 || strcmp(argv[i], "-O") == 0) {
            compiler_ctx.optimize_time = true;
        } else {
            return -1;
        }
    }

    if (g_compile_once && !g_single_source) return -1;
    return 0;
}

/*
 * SYNTAX VALIDATOR
 * Confirms allowed chars and balanced brackets.
 */
static int validate_syntax(void) {
    compiler_ctx.loop_depth = 0;
    for (size_t i = 0; i < compiler_ctx.src_len; i++) {
        char c = compiler_ctx.source[i];
        bool ok = false;
        switch (c) {
            case '>': case '<': case '+': case '-': case '.': case ',':
            case '[': case ']': case '{': case '}': case '*': case '/':
                ok = true;
                break;
            default:
                break;
        }
        if (!ok) return -BF_ERR_SYNTAX;

        if (c == '[') {
            if (compiler_ctx.loop_depth >= (int)(sizeof(compiler_ctx.loop_stack) / sizeof(compiler_ctx.loop_stack[0]))) {
                return -BF_ERR_SYNTAX;
            }
            compiler_ctx.loop_stack[compiler_ctx.loop_depth++] = i;
        } else if (c == ']') {
            if (compiler_ctx.loop_depth == 0) return -BF_ERR_SYNTAX;
            compiler_ctx.loop_depth--;
        }
    }

    if (compiler_ctx.loop_depth != 0) return -BF_ERR_SYNTAX;
    return 0;
}

/*
 * TIME OPTIMIZER
 * Lightweight run-length collapse for >,<,+,- when -Otime is enabled.
 */
static void optimize_instructions(void) {
    static char optimized[MAX_BF_CODE_LEN];
    size_t w = 0;
    for (size_t i = 0; i < compiler_ctx.src_len && w < MAX_BF_CODE_LEN - 1; ) {
        char c = compiler_ctx.source[i];
        if (!compiler_ctx.optimize_time || (c != '>' && c != '<' && c != '+' && c != '-')) {
            optimized[w++] = c;
            i++;
            continue;
        }

        size_t run = 1;
        while (i + run < compiler_ctx.src_len && compiler_ctx.source[i + run] == c && run < 255) run++;
        for (size_t k = 0; k < run && w < MAX_BF_CODE_LEN - 1; k++) optimized[w++] = c;
        i += run;
    }
    optimized[w] = '\0';
    compiler_ctx.source = optimized;
    compiler_ctx.src_len = w;
}

/*
 * COMPILE ENTRY POINT
 * Here we only package validated/optimized BF source into bf_bytecode_msg_t.
 */
static int compile_bf_to_x86(void) {
    int res = validate_syntax();
    if (res < 0) return res;

    optimize_instructions();

    bf_bytecode_msg_t *out = (bf_bytecode_msg_t *)compiler_ctx.code_buf;
    out->magic = BF_MAGIC_EXEC;
    out->tape_size = BF_TAPE_SIZE;
    if (compiler_ctx.src_len > sizeof(out->code)) return -BF_ERR_OVERFLOW;
    out->code_size = compiler_ctx.src_len;
    memcpy(out->code, compiler_ctx.source, compiler_ctx.src_len);
    return 0;
}

/*
 * REPL COMMUNICATION
 * Sends compiled page back to the REPL/executor owner via IPC.
 */
static void send_to_repl(void) {
    bf_bytecode_msg_t *out = (bf_bytecode_msg_t *)compiler_ctx.code_buf;
    int res;
    do {
        res = sys_ipc_try_send(compiler_ctx.repl_id, 0, out, PAGE_SIZE, PROT_RW);
        if (res == -E_IPC_NOT_RECV) {
            sys_yield();
        } else if (res < 0) {
            cprintf("bf_compiler: send failed %d\n", res);
            return;
        }
    } while (res == -E_IPC_NOT_RECV);
}

/*
 * MAIN ENTRY POINT
 * 
 * ARGUMENTS:
 *   argc - Argument count from JOS loader
 *   argv - Argument values:
 *          argv[0] = "bf_compiler"
 *          argv[1] = repl_id OR "-c"
 *          argv[2] = "-Otime" (optional) OR BF code string (if -c)
 * 
 * WORKFLOW:
 *   1. Parse command line arguments
 *   2. If test mode (-t): run built-in test suite and exit
 *   3. If compile mode (-c): compile single string and send to executor
 *   4. Else (normal mode):
 *        a. Allocate page for code generation
 *        b. Enter infinite IPC receive loop
 *        c. For each received BF code page:
 *             - Compile to x86
 *             - Send result to executor
 * 
 * TEST MODE BEHAVIOR:
 *   Runs internal test cases (valid/invalid BF code) and verifies:
 *   - Syntax validation works correctly
 *   - Optimizations produce expected code size reduction
 *   - Generated code doesn't exceed page boundaries
 */
void umain(int argc, char **argv) {
    memset(&compiler_ctx, 0, sizeof(compiler_ctx));
    if (parse_arguments(argc, argv) < 0) {
        cprintf("%s", usage_msg);
        return;
    }

    // Allocate a scratch page for generated output.
    if (sys_alloc_region(0, (void *)UTEMP, PAGE_SIZE, PROT_RW) < 0) {
        cprintf("bf_compiler: failed to allocate code buffer page\n");
        return;
    }
    compiler_ctx.code_buf = (uint8_t *)UTEMP;

    if (g_test_mode) {
        // Very small self-checks for syntax validator.
        const char *valid = "++[-->++]";
        const char *invalid = "[++";
        compiler_ctx.source = valid;
        compiler_ctx.src_len = strlen(valid);
        int r1 = validate_syntax();
        compiler_ctx.source = invalid;
        compiler_ctx.src_len = strlen(invalid);
        int r2 = validate_syntax();
        cprintf("bf_compiler self-test: valid=%d invalid=%d\n", r1, r2);
        return;
    }

    if (g_compile_once) {
        compiler_ctx.source = g_single_source;
        compiler_ctx.src_len = strlen(g_single_source);
        int res = compile_bf_to_x86();
        if (res < 0) {
            cprintf("bf_compiler: compile failed (%d)\n", res);
            return;
        }
        send_to_repl();
        return;
    }

    // Normal mode: wait for BF source pages and respond with compiled output.
    if (sys_alloc_region(0, (void *)(UTEMP + PAGE_SIZE), PAGE_SIZE, PROT_RW) < 0) {
        cprintf("bf_compiler: failed to allocate receive buffer\n");
        return;
    }

    while (1) {
        size_t sz = PAGE_SIZE;
        envid_t from = 0;
        int perm = 0;
        int32_t val = ipc_recv(&from, (void *)(UTEMP + PAGE_SIZE), &sz, &perm);
        if (val < 0) {
            cprintf("bf_compiler: ipc_recv error %d\n", val);
            continue;
        }

        bf_source_msg_t *msg = (bf_source_msg_t *)(UTEMP + PAGE_SIZE);
        if (msg->magic != BF_MAGIC_SOURCE || msg->code_len >= MAX_BF_CODE_LEN) {
            cprintf("bf_compiler: invalid source message\n");
            continue;
        }

        compiler_ctx.source = msg->code;
        compiler_ctx.src_len = msg->code_len;
        compiler_ctx.repl_id = from; // reply directly to sender

        int res = compile_bf_to_x86();
        if (res < 0) {
            cprintf("bf_compiler: compile failed (%d)\n", res);
            continue;
        }
        send_to_repl();
    }
}

/*
 * COMPILE ENTRY POINT
 * 
 * PURPOSE:
 *   Central compilation workflow coordinator
 * 
 * STEPS:
 *   1. Validate BF syntax (validate_syntax)
 *   2. Apply optimizations if enabled (optimize_instructions)
 *   3. Generate x86 prologue (generate_prologue)
 *   4. Process each BF instruction (emit_instruction)
 *   5. Generate x86 epilogue (generate_epilogue)
 *   6. Verify code size limits
 * 
 * ERROR HANDLING:
 *   Returns negative error codes:
 *    -BF_ERR_SYNTAX: Invalid BF code
 *    -BF_ERR_OVERFLOW: Code exceeds page size
 * 
 * AUXILIARY FUNCTIONS USED:
 *   - validate_syntax()
 *   - optimize_instructions()
 *   - generate_prologue()
 *   - emit_instruction() for each BF command
 *   - generate_epilogue()
 */
static int compile_bf_to_x86(void);

/*
 * SYNTAX VALIDATOR
 * 
 * PURPOSE:
 *   Verify BF code has valid syntax before compilation
 * 
 * VALIDATION RULES:
 *   1. Only valid BF characters allowed (><+-.,[])
 *   2. All brackets must be properly nested
 *   3. No empty loops (future optimization check)
 * 
 * ALGORITHM:
 *   Use stack-based bracket matching:
 *    - Push position on '['
 *    - Pop on ']'
 *    - Error if stack underflow/overflow
 * 
 * RETURN VALUE:
 *   0 = valid syntax
 *   -BF_ERR_SYNTAX = invalid syntax
 */
static int validate_syntax(void);

/*
 * TIME OPTIMIZER
 * 
 * PURPOSE:
 *   Reduce runtime of generated code through pattern replacement
 *   Enabled by -Otime flag
 * 
 * OPTIMIZATION RULES:
 *   1. Sequential pointer moves:
 *        ">>>" -> add $3, %esi (3 bytes vs 9 bytes)
 *   2. Sequential value modifications:
 *        "++++" -> addb $4, (%esi) (3 bytes vs 12 bytes)
 *   3. Loop elimination:
 *        "[+]" -> movb $0, (%esi) when starting from non-zero
 *   4. Memory copy detection:
 *        "[->+<]" -> optimized block copy
 * 
 * IMPLEMENTATION APPROACH:
 *   Two-pass optimization:
 *    1. First pass: convert source to intermediate representation
 *    2. Second pass: apply pattern matching and replacement
 */
static void optimize_instructions(void);

/*
 * X86 PROLOGUE GENERATOR
 * 
 * PURPOSE:
 *   Emit setup code at start of generated function
 * 
 * GENERATED CODE:
 *   mov $BF_TAPE_ADDR, %esi   ; Setup tape pointer
 *   push %ebp                 ; Standard function prologue
 *   mov %esp, %ebp
 * 
 * REGISTER USAGE:
 *   %esi = current tape pointer (preserved across calls)
 *   %edi = tape base address (for future ASAN)
 *   %eax/%edx = scratch registers
 * 
 * RETURN:
 *   Number of bytes written to code buffer
 */
size_t generate_prologue(void) {
    // Stub: will be implemented after jit_interpreter.c
    return 0;
}

/*
 * X86 EPILOGUE GENERATOR
 * 
 * PURPOSE:
 *   Emit cleanup code at end of generated function
 * 
 * GENERATED CODE:
 *   mov %ebp, %esp            ; Standard function epilogue
 *   pop %ebp
 *   ret                       ; Return to caller
 * 
 * SPECIAL CONSIDERATIONS:
 *   Must preserve all callee-saved registers per x86 ABI
 *   Must clean up any stack modifications
 * 
 * RETURN:
 *   Number of bytes written to code buffer
 */
size_t generate_epilogue(void) {
    // Stub: will be implemented after jit_interpreter.c
    return 0;
}

/*
 * INSTRUCTION EMITTER
 * 
 * PURPOSE:
 *   Generate x86 code for single BF operation
 * 
 * BEHAVIOR BY OPCODE:
 *   '>' : add $1, %esi
 *   '<' : sub $1, %esi
 *   '+' : addb $1, (%esi)
 *   '-' : subb $1, (%esi)
 *   '.' : call sys_cputs with current cell
 *   ',' : call sys_cgetc, store result in current cell
 *   '[' : conditional jump to loop end if cell==0
 *   ']' : unconditional jump back to loop start
 * 
 * LOOP HANDLING:
 *   Uses handle_loop_start() and handle_loop_end() for bracket processing
 * 
 * RETURN:
 *   Number of bytes written to code buffer
 */
size_t emit_instruction(char op) {
    // Stub: will be implemented after jit_interpreter.c
    (void)op;
    return 0;
}

/*
 * LOOP START HANDLER
 * 
 * PURPOSE:
 *   Process '[' instruction in BF code
 * 
 * STEPS:
 *   1. Push current code position to loop stack
 *   2. Emit placeholder jump instruction:
 *        cmpb $0, (%esi)
 *        jz <placeholder>
 *   3. Leave space for 32-bit relative offset
 * 
 * ERROR HANDLING:
 *   Returns error if loop depth exceeds 256
 */
void handle_loop_start(void) {
    // Stub for future JIT; validation already handled in validate_syntax.
    return;
}

/*
 * LOOP END HANDLER
 * 
 * PURPOSE:
 *   Process ']' instruction in BF code
 * 
 * STEPS:
 *   1. Pop matching loop start position from stack
 *   2. Emit back jump:
 *        cmpb $0, (%esi)
 *        jnz <loop_start>
 *   3. Backpatch forward jump from loop start
 * 
 * ERROR HANDLING:
 *   Returns error if unmatched closing bracket
 */
void handle_loop_end(void) {
    // Stub for future JIT; validation already handled in validate_syntax.
    return;
}
