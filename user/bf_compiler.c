
// responsible: ded

/*
 * bf_compiler.c - Brainfuck JIT Compiler for JOS
 *
 * CLI USAGE:
 *    bf_compiler [options]
 *    --REPL                       : (Not shown in help message) REPL mode (waits for IPC)
 *    -h (--help)                  : Show this help message\n"
 *    -o (--output) <file>         : Specify output file for compiled code\n"
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
        "  -o (--output) <file>         : Specify output file for compiled code\n"
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
        compiler_ctx.output_file = "out.bin";
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
try_opt_seek(const char *s, size_t len, size_t *i, bool *right, int16_t *stride) {
    // Matches: [>...] or [<...] where body is a non-empty run of only one direction, whitespace allowed
    size_t p = *i;
    if (p >= len || s[p] != '[') return false;
    p++;
    p = skip_spaces(s, len, p);
    if (p >= len) return false;
    char dir = s[p];
    if (dir != '>' && dir != '<') return false;

    int count = 0;
    while (p < len) {
        char c = s[p];
        if (c == dir) {
            count++;
            p++;
        } else if (is_space(c)) {
            p++;
        } else
            break;
    }
    if (count <= 0 || count > 32767) return false;
    p = skip_spaces(s, len, p);
    if (p >= len || s[p] != ']') return false;
    p++;
    *right = (dir == '>');
    *stride = (int16_t)count;
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
 * REAL BF->x86 COMPILATION
 * - validates brackets (validate_syntax)
 * - aggregates runs of > < + -
 * - generates x86 machine code with jump patching
 */
static int
compile_bf_to_x86(void) {
    int res = validate_syntax();
    if (res < 0) return res;
    LOG("Compile: syntax ok, src_len=%zu\n", compiler_ctx.src_len);

    /* Emit machine code into compiler_ctx.code_buf.
     * Layout:
     *  [0:CODE_HEADER_SIZE)  header (qword helper ptr)
     *  [CODE_ENTRY_OFFSET:.. ) generated code
     */

    size_t cur = CODE_ENTRY_OFFSET; /* write position in bytes */
    size_t max_bytes = MAX_BF_MSG_LEN;

/* helpers for emitting bytes */
#define EMIT_B(b)                                         \
    do {                                                  \
        if (cur + 1 > max_bytes) return -BF_ERR_OVERFLOW; \
        compiler_ctx.code_buf[cur++] = (uint8_t)(b);      \
    } while (0)
#define EMIT_BYTES(src, n)                                  \
    do {                                                    \
        if (cur + (n) > max_bytes) return -BF_ERR_OVERFLOW; \
        memcpy(&compiler_ctx.code_buf[cur], (src), (n));    \
        cur += (n);                                         \
    } while (0)
#define EMIT_IMM32(v)                \
    do {                             \
        uint32_t _v = (uint32_t)(v); \
        EMIT_B(_v & 0xff);           \
        EMIT_B((_v >> 8) & 0xff);    \
        EMIT_B((_v >> 16) & 0xff);   \
        EMIT_B((_v >> 24) & 0xff);   \
    } while (0)

    /* clear header */
    for (size_t i = 0; i < CODE_HEADER_SIZE; i++) compiler_ctx.code_buf[i] = 0;

    /* loop stack holds pairs {loop_body_start, je_disp_pos} */
    size_t loop_body_stack[256];
    size_t loop_je_pos_stack[256];
    size_t loop_sp = 0;

    for (size_t i = 0; i < compiler_ctx.src_len;) {
        char c = compiler_ctx.source[i];
        if (is_space(c)) {
            i++;
            continue;
        }

        /* -Otime optimizations */
        if (compiler_ctx.optimize_time && c == '[') {
            size_t save = i;
            bool right = false;
            int16_t off = 0;
            int16_t delta = 0;

            if (try_opt_clear(compiler_ctx.source, compiler_ctx.src_len, &i)) {
                /* emit helper CLEAR */
                EMIT_B(0xBF);
                EMIT_IMM32(OP_CLEAR);
                EMIT_B(0xBE);
                EMIT_IMM32(0);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
                LOG("Emit(opt): CLEAR\n");
                continue;
            }

            i = save;
            int16_t stride = 0;
            if (try_opt_seek(compiler_ctx.source, compiler_ctx.src_len, &i, &right, &stride)) {
                /* Guard: check if cell is zero before entering loop */
                EMIT_B(0xBF);
                EMIT_IMM32(OP_LOOP_START);
                EMIT_B(0xBE);
                EMIT_IMM32(0);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
                EMIT_B(0x85);
                EMIT_B(0xC0); /* test eax,eax */
                EMIT_B(0x0F);
                EMIT_B(0x84); /* je rel32 */
                size_t je_disp_pos = cur;
                EMIT_IMM32(0);

                /* Emit SEEK with stride */
                EMIT_B(0xBF);
                EMIT_IMM32(right ? OP_SEEK_RIGHT : OP_SEEK_LEFT);
                EMIT_B(0xBE);
                EMIT_IMM32((uint32_t)stride);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));

                /* Patch forward jump to point after SEEK */
                size_t after_seek = cur;
                int32_t forward_disp = (int32_t)((int32_t)after_seek - (int32_t)(je_disp_pos + 4));
                uint32_t _fd = (uint32_t)forward_disp;
                compiler_ctx.code_buf[je_disp_pos + 0] = _fd & 0xff;
                compiler_ctx.code_buf[je_disp_pos + 1] = (_fd >> 8) & 0xff;
                compiler_ctx.code_buf[je_disp_pos + 2] = (_fd >> 16) & 0xff;
                compiler_ctx.code_buf[je_disp_pos + 3] = (_fd >> 24) & 0xff;

                LOG("Emit(opt): SEEK_%s stride=%d\n", right ? "RIGHT" : "LEFT", stride);
                continue;
            }

            i = save;
            if (try_opt_move_add(compiler_ctx.source, compiler_ctx.src_len, &i, &off, &delta)) {
                uint32_t packed = (uint32_t)BF_PACK_MOVE_ADD(off, delta);
                EMIT_B(0xBF);
                EMIT_IMM32(OP_MOVE_ADD);
                EMIT_B(0xBE);
                EMIT_IMM32(packed);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
                LOG("Emit(opt): MOVE_ADD off=%d delta=%d\n", off, delta);
                continue;
            }

            i = save;
        }

        /* aggregate runs */
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
                /* mov edi, OP_INC_PTR */ EMIT_B(0xBF);
                EMIT_IMM32(OP_INC_PTR);
                /* mov esi, delta */ EMIT_B(0xBE);
                EMIT_IMM32(delta);
                /* call qword ptr [rip+disp32] */ EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            } else if (delta < 0) {
                EMIT_B(0xBF);
                EMIT_IMM32(OP_DEC_PTR);
                EMIT_B(0xBE);
                EMIT_IMM32(-delta);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            }
            continue;
        }

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
                EMIT_B(0xBF);
                EMIT_IMM32(OP_INC_CELL);
                EMIT_B(0xBE);
                EMIT_IMM32(delta);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            } else if (delta < 0) {
                EMIT_B(0xBF);
                EMIT_IMM32(OP_DEC_CELL);
                EMIT_B(0xBE);
                EMIT_IMM32(-delta);
                EMIT_B(0xFF);
                EMIT_B(0x15);
                EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            }
            continue;
        }

        switch (c) {
        case '.':
            EMIT_B(0xBF);
            EMIT_IMM32(OP_OUTPUT);
            EMIT_B(0xBE);
            EMIT_IMM32(0);
            EMIT_B(0xFF);
            EMIT_B(0x15);
            EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            i++;
            break;
        case ',':
            EMIT_B(0xBF);
            EMIT_IMM32(OP_INPUT);
            EMIT_B(0xBE);
            EMIT_IMM32(0);
            EMIT_B(0xFF);
            EMIT_B(0x15);
            EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            i++;
            break;
        case '[': {
            /* emit test helper */
            EMIT_B(0xBF);
            EMIT_IMM32(OP_LOOP_START);
            EMIT_B(0xBE);
            EMIT_IMM32(0);
            EMIT_B(0xFF);
            EMIT_B(0x15);
            EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            /* test eax,eax */ EMIT_B(0x85);
            EMIT_B(0xC0);
            /* je rel32 (placeholder) */ EMIT_B(0x0F);
            EMIT_B(0x84);
            size_t je_disp_pos = cur;
            EMIT_IMM32(0);
            size_t loop_body_start = cur; /* code after the je instruction */
            if (loop_sp >= 256) {         /* Check against local stack size, not global */
                LOG("LOOP STACK OVERFLOW: loop_sp=%zu\n", loop_sp);
                return -BF_ERR_OVERFLOW;
            }
            loop_body_stack[loop_sp] = loop_body_start;
            loop_je_pos_stack[loop_sp] = je_disp_pos;
            loop_sp++;
            i++;
            break;
        }
        case ']': {
            if (loop_sp == 0) return -BF_ERR_SYNTAX;
            /* emit test helper */
            EMIT_B(0xBF);
            EMIT_IMM32(OP_LOOP_START);
            EMIT_B(0xBE);
            EMIT_IMM32(0);
            EMIT_B(0xFF);
            EMIT_B(0x15);
            EMIT_IMM32((int32_t)(CODE_HELPER_PTR_OFFSET - (cur + 4)));
            /* test eax,eax */ EMIT_B(0x85);
            EMIT_B(0xC0);
            /* jne rel32 back to loop_body_start */ EMIT_B(0x0F);
            EMIT_B(0x85);
            size_t back_disp_pos = cur;
            EMIT_IMM32(0);

            /* compute back displacement and patch */
            size_t loop_body_start = loop_body_stack[loop_sp - 1];
            int32_t back_disp = (int32_t)((int32_t)loop_body_start - (int32_t)(back_disp_pos + 4));
            /* write back_disp little endian at back_disp_pos */
            uint32_t _bd = (uint32_t)back_disp;
            compiler_ctx.code_buf[back_disp_pos + 0] = _bd & 0xff;
            compiler_ctx.code_buf[back_disp_pos + 1] = (_bd >> 8) & 0xff;
            compiler_ctx.code_buf[back_disp_pos + 2] = (_bd >> 16) & 0xff;
            compiler_ctx.code_buf[back_disp_pos + 3] = (_bd >> 24) & 0xff;

            /* patch forward JE (at je_disp_pos) to point after the jne we just emitted */
            size_t je_disp_pos = loop_je_pos_stack[loop_sp - 1];
            size_t after_jne = cur; /* cur now points after the JNE immediate (we already emitted it) */
            int32_t forward_disp = (int32_t)((int32_t)after_jne - (int32_t)(je_disp_pos + 4));
            uint32_t _fd = (uint32_t)forward_disp;
            compiler_ctx.code_buf[je_disp_pos + 0] = _fd & 0xff;
            compiler_ctx.code_buf[je_disp_pos + 1] = (_fd >> 8) & 0xff;
            compiler_ctx.code_buf[je_disp_pos + 2] = (_fd >> 16) & 0xff;
            compiler_ctx.code_buf[je_disp_pos + 3] = (_fd >> 24) & 0xff;

            loop_sp--;
            i++;
            break;
        }
        default:
            i++;
            break;
        }
    }

    if (loop_sp != 0) return -BF_ERR_SYNTAX; /* unmatched '[' */

    /* epilogue: ret */
    EMIT_B(0xC3);

    compiler_ctx.code_offset = cur; /* total bytes including header */
    LOG("Compile: emitted %zu bytes\n", compiler_ctx.code_offset);
    return 0;
}


/*
 * REPL COMMUNICATION
 * Sends compiled code page back to the REPL/executor owner via IPC.
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

            LOG("sending machine code to REPL\n");
            send_to_repl(res);

            LOG("Compiled and sent machine code of size %zu to REPL %08x\n", compiler_ctx.code_offset, compiler_ctx.repl_id);
        }

        // unreachable
    }

    // File-based mode: read input file, compile, write output file.
    LOG("Entering file-based compilation mode...\n");

    // standalone compilation: read BF, compile to x86 machine code, write to file
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

    ssize_t n = 0;
    for (;;) {
        ssize_t cur = read(fd_in, (void *)(compiler_ctx.source + n), MAX_BF_MSG_LEN - n);
        if (cur < 0) {
            cprintf("failed to read input file %s\n", compiler_ctx.input_file);
            close(fd_in);
            return;
        }
        if (cur == 0) break; /* EOF */
        n += cur;
        if ((size_t)n >= MAX_BF_MSG_LEN) {
            cprintf("input file %s too large (max %lld bytes)\n", compiler_ctx.input_file, MAX_BF_MSG_LEN);
            close(fd_in);
            return;
        }
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

    /* Ensure we write the entire buffer even if write() is partial */
    size_t to_write = compiler_ctx.code_offset;
    size_t written_total = 0;
    while (written_total < to_write) {
        ssize_t w = write(fd_out, compiler_ctx.code_buf + written_total, to_write - written_total);
        if (w < 0) {
            cprintf("failed to write compiled code to %s\n", compiler_ctx.output_file);
            close(fd_out);
            return;
        }
        if (w == 0) {
            cprintf("failed to write compiled code to %s (zero bytes written)\n", compiler_ctx.output_file);
            close(fd_out);
            return;
        }
        written_total += (size_t)w;
    }

    close(fd_out);
    LOG("Standalone compile succeeded: %zu bytes -> %s\n", compiler_ctx.code_offset, compiler_ctx.output_file);
    return;
}