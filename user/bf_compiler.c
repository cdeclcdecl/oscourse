
// responsible: ded

/* 
 * bf_compiler.c - Brainfuck JIT Compiler for JOS
 * 
 * CLI USAGE:
 *    bf_compiler [options]
 *    --REPL                       : (Not shown in help message) REPL mode (waits for IPC)
 *    -h (--help)                  : Show this help message\n"
 *    -o (--output) <file>         : Specify output file for bytecode\n"
 *    -Otime (--optimize-time)     : Enable time optimizations\n"
 *    -d (--debug)                 : Enable debug mode\n"
 *    <input_file>                 : Input file (optional)\n";
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

#define LOG(msg, ...) if (compiler_ctx.debug_mode) { cprintf("[COMPILER]: " msg, ##__VA_ARGS__); }

bf_compiler_context_t compiler_ctx = {
    .source = NULL,
    .src_len = 0,
    .code_buf = NULL,
    .code_offset = 0,
    .loop_depth = 0,
    .repl_id = 0,
    .debug_mode = false,
    .REPL_mode = false,
    .optimize_time = false,
    .input_file = NULL,
    .output_file = NULL,
};

char usage_msg[] =
    "Usage: bf_compiler [options]\n"
    "  -h (--help)                  : Show this help message\n"
    "  -o (--output) <file>         : Specify output file for bytecode\n"
    "  -Otime (--optimize-time)     : Enable time optimizations\n"
    "  -d (--debug)                 : Enable debug mode\n"
    "  <input_file>                 : Input file (optional)\n";

/*
 * CLI ARGUMENT PARSER
 * Parses flags and positional arguments into compiler_ctx and mode flags.
 */
int parse_arguments(int argc, char **argv) {
    
    // No arguments or just the program name
    if (argc < 2) {
        cprintf("%s", usage_msg);
        return -1;
    }
    
    int i = 1;
    while (i < argc) {
        // Handle help flag first
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            cprintf("%s", usage_msg);
            exit();
        }
        
        // Handle REPL mode
        else if (strcmp(argv[i], "--REPL") == 0) {
            compiler_ctx.REPL_mode = true;
            i++;
        }
        
        // Handle debug mode
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            compiler_ctx.debug_mode = true;
            i++;
        }
        
        // Handle time optimizations
        else if (strcmp(argv[i], "-Otime") == 0 || strcmp(argv[i], "--optimize-time") == 0) {
            compiler_ctx.optimize_time = true;
            i++;
        }
        
        // Handle output file
        else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                cprintf("Error: -o/--output requires a filename argument\n");
                return -1;
            }
            compiler_ctx.output_file = argv[i + 1];
            i += 2;
        }
        
        // Handle positional argument (input file)
        else if (argv[i][0] != '-') {
            if (compiler_ctx.input_file != NULL) {
                cprintf("Error: Only one input file can be specified\n");
                return -1;
            }
            compiler_ctx.input_file = argv[i];
            i++;
        }
        
        // Handle unknown argument
        else {
            cprintf("Error: Unknown argument: %s\n", argv[i]);
            cprintf("%s", usage_msg);
            return -1;
        }
    }
    
    // If no input file is specified and not in REPL mode, show error
    if (!compiler_ctx.REPL_mode && compiler_ctx.input_file == NULL) {
        cprintf("Error: Input file required\n");
        return -1;
    }

    if (!compiler_ctx.REPL_mode && compiler_ctx.output_file == NULL) {
        compiler_ctx.output_file = "out.bc";
    }
    
    // Debug output
    if (compiler_ctx.debug_mode) {
        cprintf("[COMPILER] Parsed arguments:\n");
        cprintf("  REPL_mode = %d\n", compiler_ctx.REPL_mode);
        cprintf("  debug_mode = %d\n", compiler_ctx.debug_mode);
        cprintf("  optimize_time = %d\n", compiler_ctx.optimize_time);
        cprintf("  input_file = %s\n", compiler_ctx.input_file ? compiler_ctx.input_file : "NULL");
        cprintf("  output_file = %s\n", compiler_ctx.output_file ? compiler_ctx.output_file : "NULL");
        cprintf("  repl_id = %d\n", compiler_ctx.repl_id);
    }
    
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
            case '>': case '<': case '+': case '-': case '.': case ',': case '[': case ']':
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
    static char optimized[MAX_BF_MSG_LEN];
    size_t w = 0;
    for (size_t i = 0; i < compiler_ctx.src_len && w < MAX_BF_MSG_LEN - 1; ) {
        char c = compiler_ctx.source[i];
        if (!compiler_ctx.optimize_time || (c != '>' && c != '<' && c != '+' && c != '-')) {
            optimized[w++] = c;
            i++;
            continue;
        }

        size_t run = 1;
        while (i + run < compiler_ctx.src_len && compiler_ctx.source[i + run] == c && run < 255) run++;
        for (size_t k = 0; k < run && w < MAX_BF_MSG_LEN - 1; k++) optimized[w++] = c;
        i += run;
    }
    optimized[w] = '\0';
    memcpy((void *) compiler_ctx.source, (const void *) optimized, w);
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

    /* Produce raw bytecode payload in the code buffer (no magic headers)
     * Receiver will use the IPC-provided size to determine valid bytes.
     */
    if (compiler_ctx.src_len > PAGE_SIZE) return -BF_ERR_OVERFLOW;
    memcpy(compiler_ctx.code_buf, compiler_ctx.source, compiler_ctx.src_len);
    compiler_ctx.code_offset = compiler_ctx.src_len;
    return 0;
}

/*
 * REPL COMMUNICATION
 * Sends compiled page back to the REPL/executor owner via IPC.
 */
static void send_to_repl(void) {
    ipc_send(compiler_ctx.repl_id, BF_MAGIC_EXEC, compiler_ctx.code_buf, compiler_ctx.code_offset, PROT_RW);
}

/*
 * MAIN ENTRY POINT
 * 
 * main logic of the compiler process
 *  
 */
void umain(int argc, char **argv) {


    if (parse_arguments(argc, argv) < 0) {
        cprintf("%s", usage_msg);
        return;
    }

    LOG("bf_compiler parsed arguments\n");


    // Allocate a scratch page for generated output.
    if (sys_alloc_region(0, (void *)COMPILER_TEMP_ADDR, PAGE_SIZE, PROT_RW) < 0) {
        cprintf("bf_compiler: failed to allocate code buffer page\n");
        return;
    }
    compiler_ctx.code_buf = (uint8_t *)COMPILER_TEMP_ADDR;
    compiler_ctx.code_offset = 0;

    LOG("Allocated code buffer at %p\n", compiler_ctx.code_buf);

    if (compiler_ctx.REPL_mode) {

        LOG("Entering REPL mode...\n");

        // REPL mode: wait for BF source pages and respond with compiled output.
        if (sys_alloc_region(0, (void *)(COMPILER_TEMP_ADDR + PAGE_SIZE), PAGE_SIZE, PROT_RW) < 0) {
            cprintf("bf_compiler: failed to allocate receive buffer\n");
            return;
        }

        compiler_ctx.source = (const char *)(COMPILER_TEMP_ADDR + PAGE_SIZE);

        LOG("Allocated receive buffer at %p\n", (void *)(COMPILER_TEMP_ADDR + PAGE_SIZE));

        while (1) {
            int perm = 0;
            LOG("waiting for source code from REPL\n");
            int32_t val = ipc_recv(&compiler_ctx.repl_id, (void *) compiler_ctx.source, &compiler_ctx.src_len, &perm);
            if (val < 0) {
                cprintf("bf_compiler: ipc_recv error %d\n", val);
                continue;
            }

            LOG("Received IPC message from REPL %08x, size %zu bytes\n",
                compiler_ctx.repl_id, compiler_ctx.src_len);
            
            LOG("src_len: %zu\n", compiler_ctx.src_len);
            /* Treat IPC payload as raw bytes: validate reported size and use the receive buffer */
            if (compiler_ctx.src_len == 0 || compiler_ctx.src_len > MAX_BF_MSG_LEN) {
                cprintf("bf_compiler: invalid source size %zu\n", compiler_ctx.src_len);
                continue;
            }

            compiler_ctx.source = (const char *)(COMPILER_TEMP_ADDR + PAGE_SIZE);

            int res = compile_bf_to_x86();
            if (res < 0) {
                cprintf("bf_compiler: compile failed (%d)\n", res);
                continue;
            }

            LOG("sending bytecode to REPL\n");
            send_to_repl();

            LOG("Compiled and sent bytecode of size %zu to REPL %08x\n", compiler_ctx.code_offset, compiler_ctx.repl_id);

        }

        // unreachable
    }

    // File-based mode: read input file, compile, write output file.
    LOG("Entering file-based compilation mode...\n");

    int fd_in = open(compiler_ctx.input_file, O_RDONLY);
    if (fd_in < 0) {
        cprintf("bf_compiler: failed to open input file %s\n", compiler_ctx.input_file);
        return;
    }

    ssize_t n = read(fd_in, compiler_ctx.code_buf, PAGE_SIZE);
    if (n < 0) {
        cprintf("bf_compiler: failed to read input file %s\n", compiler_ctx.input_file);
        close(fd_in);
    }

    // TODO

    return;
}