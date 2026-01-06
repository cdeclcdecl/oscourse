
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

#define LOG(msg, ...) \
    if (compiler_ctx.debug_mode) { cprintf("[COMPILER]: " msg, ##__VA_ARGS__); }

// Whitespace helper (ignored by syntax checker)
static inline bool
is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

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
int
parse_arguments(int argc, char **argv) {

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
static int
validate_syntax(void) {
    compiler_ctx.loop_depth = 0;
    for (size_t i = 0; i < compiler_ctx.src_len; i++) {
        char c = compiler_ctx.source[i];
        if (is_space(c)) continue; // skip whitespace/newlines

        bool ok = (c == '>' || c == '<' || c == '+' || c == '-' || c == '.' || c == ',' || c == '[' || c == ']');
        if (!ok) return -BF_ERR_SYNTAX;

        if (c == '[') {
            if (compiler_ctx.loop_depth >= (int)(sizeof(compiler_ctx.loop_stack) / sizeof(compiler_ctx.loop_stack[0]))) {
                return -BF_ERR_SYNTAX; // loop nesting too deep
            }
            compiler_ctx.loop_stack[compiler_ctx.loop_depth++] = i;
            LOG("Validate: push '[' src=%zu depth=%d\n", i, compiler_ctx.loop_depth);
        } else if (c == ']') {
            if (compiler_ctx.loop_depth == 0) return -BF_ERR_SYNTAX; // unmatched ]
            compiler_ctx.loop_depth--;
            LOG("Validate: pop ']' src=%zu depth=%d\n", i, compiler_ctx.loop_depth);
        }
    }

    if (compiler_ctx.loop_depth != 0) return -BF_ERR_SYNTAX;
    return 0;
}

/*
 * Emit one instruction with bounds checking
 */
static int
emit_instruction(Instruction *program, size_t max_instr, size_t *count, Opcode op, int32_t arg) {
    if (*count >= max_instr) return -BF_ERR_OVERFLOW;
    program[*count].opcode = op;
    program[*count].arg = arg;
    (*count)++;
    return 0;
}

// Parse helpers for pattern-based optimizations (whitespace-tolerant)
static size_t
skip_spaces(const char *s, size_t len, size_t i) {
    while (i < len && is_space(s[i])) i++;
    return i;
}

static bool
parse_run(const char *s, size_t len, size_t *i, char a, char b, int *delta_out) {
    // Parses a run consisting of characters a/b and whitespace.
    // For '><' : a='>', b='<' => delta is (#a - #b)
    // For '+-' : a='+', b='-' => delta is (#a - #b)
    size_t p = *i;
    p = skip_spaces(s, len, p);
    int delta = 0;
    bool any = false;
    while (p < len) {
        char c = s[p];
        if (c == a) {
            delta++;
            any = true;
            p++;
        } else if (c == b) {
            delta--;
            any = true;
            p++;
        } else if (is_space(c)) {
            p++;
        } else
            break;
    }
    if (!any) return false;
    *i = p;
    *delta_out = delta;
    return true;
}

static bool
try_opt_clear(const char *s, size_t len, size_t *i) {
    // Matches: [ - ] or [ + ] with optional whitespace
    size_t p = *i;
    if (p >= len || s[p] != '[') return false;
    p++;
    p = skip_spaces(s, len, p);
    if (p >= len) return false;
    char op = s[p];
    if (op != '-' && op != '+') return false;
    p++;
    p = skip_spaces(s, len, p);
    if (p >= len || s[p] != ']') return false;
    p++;
    *i = p;
    return true;
}

static bool
try_opt_seek(const char *s, size_t len, size_t *i, bool *right) {
    // Matches: [>...] or [<...] where body is a non-empty run of only one direction, whitespace allowed
    size_t p = *i;
    if (p >= len || s[p] != '[') return false;
    p++;
    p = skip_spaces(s, len, p);
    if (p >= len) return false;
    char dir = s[p];
    if (dir != '>' && dir != '<') return false;

    bool any = false;
    while (p < len) {
        char c = s[p];
        if (c == dir) {
            any = true;
            p++;
        } else if (is_space(c)) {
            p++;
        } else
            break;
    }
    if (!any) return false;
    p = skip_spaces(s, len, p);
    if (p >= len || s[p] != ']') return false;
    p++;
    *right = (dir == '>');
    *i = p;
    return true;
}

static bool
try_opt_move_add(const char *s, size_t len, size_t *i, int16_t *off, int16_t *delta) {
    // Matches common move loops:
    //   [ -  (move to dst)  (+/- repeated)  (move back) ]
    // Examples: [->+<], [->-<], [->++<], [->>+<<]
    // Strict requirements:
    //   - source delta must be exactly -1 (to preserve termination in uint8 tape)
    //   - then move to a non-zero offset
    //   - then apply non-zero delta at destination
    //   - then move back to origin
    size_t p = *i;
    if (p >= len || s[p] != '[') return false;
    p++;

    p = skip_spaces(s, len, p);
    int src_delta = 0;
    if (!parse_run(s, len, &p, '+', '-', &src_delta)) return false;
    if (src_delta != -1) return false;

    int move_delta = 0;
    if (!parse_run(s, len, &p, '>', '<', &move_delta)) return false;
    if (move_delta == 0) return false;

    int cell_delta = 0;
    if (!parse_run(s, len, &p, '+', '-', &cell_delta)) return false;
    if (cell_delta == 0) return false;

    int back_delta = 0;
    if (!parse_run(s, len, &p, '>', '<', &back_delta)) return false;
    if (move_delta + back_delta != 0) return false;

    p = skip_spaces(s, len, p);
    if (p >= len || s[p] != ']') return false;
    p++;

    if (move_delta < -32768 || move_delta > 32767) return false;
    if (cell_delta < -32768 || cell_delta > 32767) return false;

    *off = (int16_t)move_delta;
    *delta = (int16_t)cell_delta;
    *i = p;
    return true;
}

/*
 * REAL BF->BYTECODE COMPILATION
 * - validates brackets (validate_syntax)
 * - aggregates runs of > < + -
 * - builds Instruction array with jump patching
 */
static int
compile_bf_to_x86(void) {
    int res = validate_syntax();
    if (res < 0) return res;
    LOG("Compile: syntax ok, src_len=%zu\n", compiler_ctx.src_len);

    Instruction *program = (Instruction *)compiler_ctx.code_buf;
    size_t max_instr = MAX_BF_MSG_LEN / sizeof(Instruction);
    size_t instr_count = 0;
    compiler_ctx.loop_depth = 0; // reuse loop_stack for instruction indices

    if (compiler_ctx.optimize_time) {
        LOG("Compile: -Otime enabled\n");
    }

    for (size_t i = 0; i < compiler_ctx.src_len;) {
        char c = compiler_ctx.source[i];

        if (is_space(c)) {
            i++;
            continue;
        }

        // Optional loop-level optimizations (enabled by -Otime)
        if (compiler_ctx.optimize_time && c == '[') {
            size_t save = i;
            bool right = false;
            int16_t off = 0;
            int16_t delta = 0;

            if (try_opt_clear(compiler_ctx.source, compiler_ctx.src_len, &i)) {
                res = emit_instruction(program, max_instr, &instr_count, OP_CLEAR, 0);
                LOG("Emit: CLEAR instr=%zu\n", instr_count - 1);
                if (res < 0) return res;
                continue;
            }

            i = save;
            if (try_opt_seek(compiler_ctx.source, compiler_ctx.src_len, &i, &right)) {
                res = emit_instruction(program, max_instr, &instr_count, right ? OP_SEEK_RIGHT : OP_SEEK_LEFT, 0);
                LOG("Emit: SEEK_%s instr=%zu\n", right ? "RIGHT" : "LEFT", instr_count - 1);
                if (res < 0) return res;
                continue;
            }

            i = save;
            if (try_opt_move_add(compiler_ctx.source, compiler_ctx.src_len, &i, &off, &delta)) {
                res = emit_instruction(program, max_instr, &instr_count, OP_MOVE_ADD, BF_PACK_MOVE_ADD(off, delta));
                LOG("Emit: MOVE_ADD instr=%zu off=%d delta=%d\n", instr_count - 1, off, delta);
                if (res < 0) return res;
                continue;
            }

            i = save;
        }

        // aggregate pointer shifts
        if (c == '>' || c == '<') {
            int delta = 0;
            while (i < compiler_ctx.src_len) {
                char d = compiler_ctx.source[i];
                if (d == '>') {
                    delta++;
                    i++;
                } else if (d == '<') {
                    delta--;
                    i++;
                } else if (is_space(d)) {
                    i++;
                } else
                    break;
            }
            if (delta > 0) {
                res = emit_instruction(program, max_instr, &instr_count, OP_INC_PTR, delta);
            } else if (delta < 0) {
                res = emit_instruction(program, max_instr, &instr_count, OP_DEC_PTR, -delta);
            } else {
                continue; // net zero shift
            }
            LOG("Emit: PTR delta=%d instr=%zu\n", delta, instr_count - 1);
            if (res < 0) return res;
            continue;
        }

        // aggregate cell increments
        if (c == '+' || c == '-') {
            int delta = 0;
            while (i < compiler_ctx.src_len) {
                char d = compiler_ctx.source[i];
                if (d == '+') {
                    delta++;
                    i++;
                } else if (d == '-') {
                    delta--;
                    i++;
                } else if (is_space(d)) {
                    i++;
                } else
                    break;
            }
            if (delta > 0) {
                res = emit_instruction(program, max_instr, &instr_count, OP_INC_CELL, delta);
            } else if (delta < 0) {
                res = emit_instruction(program, max_instr, &instr_count, OP_DEC_CELL, -delta);
            } else {
                continue;
            }
            LOG("Emit: CELL delta=%d instr=%zu\n", delta, instr_count - 1);
            if (res < 0) return res;
            continue;
        }

        switch (c) {
        case '.':
            res = emit_instruction(program, max_instr, &instr_count, OP_OUTPUT, 0);
            LOG("Emit: OUTPUT instr=%zu\n", instr_count - 1);
            i++;
            break;
        case ',':
            res = emit_instruction(program, max_instr, &instr_count, OP_INPUT, 0);
            LOG("Emit: INPUT instr=%zu\n", instr_count - 1);
            i++;
            break;
        case '[': {
            // record '[' position in stack, patch later
            if (compiler_ctx.loop_depth >= (int)(sizeof(compiler_ctx.loop_stack) / sizeof(compiler_ctx.loop_stack[0]))) {
                return -BF_ERR_SYNTAX;
            }
            int start_idx = (int)instr_count;
            compiler_ctx.loop_stack[compiler_ctx.loop_depth++] = start_idx;
            res = emit_instruction(program, max_instr, &instr_count, OP_LOOP_START, 0);
            LOG("Emit: LOOP_START instr=%d depth=%d\n", start_idx, compiler_ctx.loop_depth);
            i++;
            break;
        }
        case ']': {
            if (compiler_ctx.loop_depth == 0) return -BF_ERR_SYNTAX; // unmatched ]
            int start_idx = compiler_ctx.loop_stack[--compiler_ctx.loop_depth];
            // set '[' arg to instruction after matching ']'
            program[start_idx].arg = (int32_t)instr_count;
            res = emit_instruction(program, max_instr, &instr_count, OP_LOOP_END, start_idx);
            LOG("Emit: LOOP_END instr=%zu match=%d target=%d\n", instr_count - 1, start_idx, program[start_idx].arg);
            i++;
            break;
        }
        default:
            return -BF_ERR_SYNTAX;
        }

        if (res < 0) return res;
    }

    if (compiler_ctx.loop_depth != 0) return -BF_ERR_SYNTAX; // unclosed brackets

    compiler_ctx.code_offset = instr_count * sizeof(Instruction);
    LOG("Compile: done instr=%zu bytes=%zu\n", instr_count, compiler_ctx.code_offset);
    return 0;
}

/*
 * REPL COMMUNICATION
 * Sends compiled page back to the REPL/executor owner via IPC.
 */
void
send_to_repl(int res) {
    ipc_send(compiler_ctx.repl_id, res, compiler_ctx.code_buf, compiler_ctx.code_offset, PROT_RW);
}

/*
 * MAIN ENTRY POINT
 *
 * main logic of the compiler process
 *
 */
void
umain(int argc, char **argv) {


    if (parse_arguments(argc, argv) < 0) {
        cprintf("%s", usage_msg);
        return;
    }

    LOG("bf_compiler parsed arguments\n");


    // Allocate a scratch buffer for generated output (expanded)
    if (sys_alloc_region(0, (void *)COMPILER_TEMP_ADDR, MAX_BF_MSG_LEN, PROT_RW) < 0) {
        cprintf("bf_compiler: failed to allocate code buffer page(s)\n");
        return;
    }
    compiler_ctx.code_buf = (uint8_t *)COMPILER_TEMP_ADDR;
    compiler_ctx.code_offset = 0;

    LOG("Allocated code buffer at %p\n", compiler_ctx.code_buf);

    if (compiler_ctx.REPL_mode) {

        LOG("Entering REPL mode...\n");

        // REPL mode: wait for BF source pages and respond with compiled output.
        if (sys_alloc_region(0, (void *)(COMPILER_TEMP_ADDR + MAX_BF_MSG_LEN), MAX_BF_MSG_LEN, PROT_RW) < 0) {
            cprintf("bf_compiler: failed to allocate receive buffer(s)\n");
            return;
        }

        compiler_ctx.source = (const char *)(COMPILER_TEMP_ADDR + MAX_BF_MSG_LEN);

        LOG("Allocated receive buffer at %p\n", (void *)(COMPILER_TEMP_ADDR + MAX_BF_MSG_LEN));

        while (1) {
            int perm = 0;
            LOG("waiting for source code from REPL\n");
            int32_t val = ipc_recv(&compiler_ctx.repl_id, (void *)compiler_ctx.source, &compiler_ctx.src_len, &perm);
            if (val < 0) {
                cprintf("[ERROR]: ipc_recv error %d\n", val);
                ipc_send(compiler_ctx.repl_id, -BF_LOGIC_ERROR, NULL, 0, 0);
                continue;
            }

            LOG("Received IPC message from REPL %08x, size %zu bytes\n",
                compiler_ctx.repl_id, compiler_ctx.src_len);

            LOG("src_len: %zu\n", compiler_ctx.src_len);
            /* Treat IPC payload as raw bytes: validate reported size and use the receive buffer */
            if (compiler_ctx.src_len == 0 || compiler_ctx.src_len > MAX_BF_MSG_LEN) {
                cprintf("[ERROR]: invalid source size %zu\n", compiler_ctx.src_len);
                ipc_send(compiler_ctx.repl_id, -BF_LOGIC_ERROR, NULL, 0, 0);
                continue;
            }

            compiler_ctx.source = (const char *)(COMPILER_TEMP_ADDR + MAX_BF_MSG_LEN);

            int res = compile_bf_to_x86();
            if (res != BF_SUCCESS) {
                cprintf("[COMPILATION ERROR]: compile failed (%d)\n", res);
                ipc_send(compiler_ctx.repl_id, res, NULL, 0, 0);
                continue;
            }

            LOG("sending bytecode to REPL\n");
            send_to_repl(res);

            LOG("Compiled and sent bytecode of size %zu to REPL %08x\n", compiler_ctx.code_offset, compiler_ctx.repl_id);
        }

        // unreachable
    }

    // File-based mode: read input file, compile, write output file.
    LOG("Entering file-based compilation mode...\n");

    // standalone compilation: read BF, compile to Instruction[], write to file
    if (sys_alloc_region(0, (void *)(COMPILER_TEMP_ADDR + MAX_BF_MSG_LEN), MAX_BF_MSG_LEN, PROT_RW) < 0) {
        cprintf("bf_compiler: failed to allocate source buffer(s)\n");
        return;
    }
    compiler_ctx.source = (const char *)(COMPILER_TEMP_ADDR + MAX_BF_MSG_LEN);

    int fd_in = open(compiler_ctx.input_file, O_RDONLY);
    if (fd_in < 0) {
        cprintf("failed to open input file %s\n", compiler_ctx.input_file);
        return;
    }

    ssize_t n = read(fd_in, (void *)compiler_ctx.source, MAX_BF_MSG_LEN);
    if (n < 0) {
        cprintf("failed to read input file %s\n", compiler_ctx.input_file);
        close(fd_in);
        return;
    }
    close(fd_in);

    compiler_ctx.src_len = (size_t)n;

    int res = compile_bf_to_x86();
    if (res < 0) {
        cprintf("[COMPILATION ERROR]:compile failed (%d)\n", res);
        return;
    }

    int fd_out = open(compiler_ctx.output_file, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd_out < 0) {
        cprintf("failed to open output file %s\n", compiler_ctx.output_file);
        return;
    }

    ssize_t written = write(fd_out, compiler_ctx.code_buf, compiler_ctx.code_offset);
    if (written < 0 || (size_t)written != compiler_ctx.code_offset) {
        cprintf("failed to write bytecode to %s\n", compiler_ctx.output_file);
        close(fd_out);
        return;
    }

    close(fd_out);
    LOG("Standalone compile succeeded: %zu bytes -> %s\n", compiler_ctx.code_offset, compiler_ctx.output_file);
    return;
}