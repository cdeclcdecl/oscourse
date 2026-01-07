// responsible: deadlamer

/*
 * brainfuck.c - Brainfuck JIT REPL for JOS
 *
 * CLI USAGE:
 *    bf_jit_interpreter [options] <input_file>
 *    <input_file>                 : Input file (optional)\n";
 *    --REPL                       : (Not shown in help message) REPL mode (waits for IPC)
 *    -h (--help)                  : Show this help message\n"
 *    -d (--debug)                 : Enable debug mode\n"
 *    -p (--print) ascii|hex|dec   : Set output format\n"
 */

#include <inc/lib.h>
#include <inc/bf.h>

#define LOG(msg, ...) \
    if (executor_ctx.debug_mode) { cprintf("[EXECUTOR]: " msg, ##__VA_ARGS__); }

bf_executor_ctx_t executor_ctx = {
        .tape = (uint8_t *)NULL,
        .output_format = BF_OUTPUT_ASCII,
        .debug_mode = false,
        .REPL_mode = false,
        .input_file = NULL,
        .repl_id = 0,
        .input_buf = NULL,
        .input_size = 0,
        .input_pos = 0,
};

// RW alias for JIT code pages (we always write/patch here)
static uintptr_t exec_code_rw_addr = 0;

// Address we actually execute from. Prefer EXECUTOR_CODE_ADDR as RX alias.
static uintptr_t exec_code_exec_addr = (uintptr_t)EXECUTOR_CODE_ADDR;

/* Make [addr, addr+size) executable and revoke write permission.
 * Returns 0 on success, <0 on error. Caller must ensure the code bytes
 * were copied into the pages BEFORE calling this. */
int
WxorX(void *addr, size_t size, enum bf_exec_mode mode) {
    if (size == 0 || addr == NULL) return -1;

    uintptr_t start = ROUNDDOWN((uintptr_t)addr, PAGE_SIZE);
    size_t size_pg = ROUNDUP(size, PAGE_SIZE);

    void *pg = (void *)(start);

    int cur = get_prot(pg);
    if (cur < 0) cur = PROT_R;

    int want;

    if (mode == WRITABLE) {
        want = (cur & ~PROT_X) | PROT_W | PROT_R;
    } else if (mode == EXECUTABLE) {
        want = (cur & ~PROT_W) | PROT_X | PROT_R;
    } else {
        want = cur;
    }

    sys_map_region(0, pg, 0, pg, size_pg, want);

    return 0;
}

/*
 * smart_getchar - stdin reader with line editing and puts it into executor_ctx.input_buf
 */
void
smart_getchar(void) {

    /* Clear the whole input buffer to MAX_BF_MSG_LEN to avoid stale data */
    memset(executor_ctx.input_buf, 0, MAX_BF_MSG_LEN);
    executor_ctx.input_size = 0;
    executor_ctx.input_pos = 0;

    int c;

    while (1) {
        c = getchar();
        if ((c & 0xff) == 0xf4) {
            executor_ctx.input_buf[executor_ctx.input_size++] = 0x00;
            cprintf("^D\n");
            break;
        }

        if ((c & 0xff) == 0x0d) {
            cprintf("\n");
            executor_ctx.input_buf[executor_ctx.input_size++] = 0x00;
            break;
        }

        if ((c & 0xff) == 0x08 || (c & 0xff) == 0x7f) { // Backspace or DEL
            if (executor_ctx.input_size > 0) {
                executor_ctx.input_size--;
                // Move cursor back, overwrite with space, move back again
                executor_ctx.input_buf[executor_ctx.input_size] = 0;
                cprintf("\b \b");
            }
            continue;
        }

        if (executor_ctx.input_size == MAX_BF_MSG_LEN - 1) {
            break;
        }

        executor_ctx.input_buf[executor_ctx.input_size++] = (uint8_t)(c & 0xff);

        LOG("smart_getchar: Read char: %c (0x%02x)\n", (char)c, (unsigned char)c);

        cputchar(c); // for echo
    }
}

/*
 * OUTPUT FORMATTER
 *
 * FORMATTING RULES:
 *   ASCII: Direct character output (non-printables as '.')
 *   HEX: Space-separated hex bytes (e.g., "48 65 6c 6c 6f")
 *   DEC: Space-separated decimal values (e.g., "72 101 108 108 111")
 *
 */
void
format_output(char sym) {

    switch (executor_ctx.output_format) {
    case BF_OUTPUT_ASCII:
        cputchar(sym);
        break;
    case BF_OUTPUT_HEX: {
        char buf[4];
        int len = snprintf(buf, sizeof(buf), "%02X ", (unsigned char)sym);
        for (int i = 0; i < len; i++) {
            cputchar(buf[i]);
        }
    } break;
    case BF_OUTPUT_DEC: {
        char buf[5];
        int len = snprintf(buf, sizeof(buf), "%u ", (unsigned char)sym);
        for (int i = 0; i < len; i++) {
            cputchar(buf[i]);
        }
    } break;
    default:
        LOG("Unknown output format %d, defaulting to ASCII\n", executor_ctx.output_format);
        if (sym >= 32 && sym <= 126) {
            cputchar(sym);
        } else {
            cputchar('.');
        }
        break;
    }
}

/*
 * JIT helper called from generated machine code.
 * Calling convention: int bf_exec_helper(int opcode, int arg)
 * - opcode in edi (rdi), arg in esi (rsi)
 * - returns int in eax (used for loop tests)
 */
int
bf_exec_helper(int opcode, int arg) {
    if (executor_ctx.tape == NULL) {
        cprintf("bf_exec_helper: tape not initialized\n");
        executor_ctx.last_error = BF_ERR_EXECUTION;
        ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
        exit();
    }

    size_t ptr = executor_ctx.ptr_offset;
    uint8_t *tape = executor_ctx.tape;

    switch (opcode) {
    case OP_INC_PTR:
        if ((ptr + (size_t)arg) >= BF_TAPE_SIZE) {
            cprintf("bf_exec_helper: PTR out of bounds (right)\n");
            executor_ctx.last_error = BF_ERR_EXECUTION;
            ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
            exit();
        }
        ptr += (size_t)arg;
        break;
    case OP_DEC_PTR:
        if ((ptr < (size_t)arg) || (ptr - (size_t)arg) >= BF_TAPE_SIZE) {
            cprintf("bf_exec_helper: PTR out of bounds (left)\n");
            executor_ctx.last_error = BF_ERR_EXECUTION;
            ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
            exit();
        }
        ptr -= (size_t)arg;
        break;
    case OP_INC_CELL: {
        uint8_t v = tape[ptr];
        v = (uint8_t)((int)v + arg);
        tape[ptr] = v;
    } break;
    case OP_DEC_CELL: {
        uint8_t v = tape[ptr];
        v = (uint8_t)((int)v - arg);
        tape[ptr] = v;
    } break;
    case OP_OUTPUT:
        format_output((char)tape[ptr]);
        break;
    case OP_INPUT:
        if (executor_ctx.input_pos >= executor_ctx.input_size) {
            smart_getchar();
        }
        tape[ptr] = executor_ctx.input_buf[executor_ctx.input_pos++];
        if (tape[ptr] == 0xf4) {
            cprintf("^D\n");
            /* EOF - leave as is */
        }
        break;
    case OP_LOOP_START:
        executor_ctx.ptr_offset = ptr; /* commit */
        return (int)tape[ptr];

    case OP_CLEAR:
        tape[ptr] = 0;
        break;

    case OP_SEEK_RIGHT:
        /* arg contains stride (how many cells to move per iteration) */
        if (arg <= 0) arg = 1; /* safety fallback */
        while (tape[ptr] != 0) {
            if (ptr + (size_t)arg >= BF_TAPE_SIZE) {
                cprintf("bf_exec_helper: SEEK_RIGHT out of bounds\n");
                executor_ctx.last_error = BF_ERR_EXECUTION;
                ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
                exit();
            }
            ptr += (size_t)arg;
        }
        break;

    case OP_SEEK_LEFT:
        /* arg contains stride (how many cells to move per iteration) */
        if (arg <= 0) arg = 1; /* safety fallback */
        while (tape[ptr] != 0) {
            if (ptr < (size_t)arg) {
                cprintf("bf_exec_helper: SEEK_LEFT out of bounds\n");
                executor_ctx.last_error = BF_ERR_EXECUTION;
                ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
                exit();
            }
            ptr -= (size_t)arg;
        }
        break;

    case OP_MOVE_ADD: {
        int32_t packed = arg;
        int16_t off = BF_UNPACK_MOVE_ADD_OFFSET(packed);
        int16_t delta = BF_UNPACK_MOVE_ADD_DELTA(packed);
        /* compute dst and bounds-check */
        ssize_t dst_idx = (ssize_t)ptr + (ssize_t)off;
        if (dst_idx < 0 || (size_t)dst_idx >= BF_TAPE_SIZE) {
            cprintf("bf_exec_helper: MOVE_ADD destination out of bounds off=%d\n", off);
            executor_ctx.last_error = BF_ERR_EXECUTION;
            ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
            exit();
        }
        size_t dst = (size_t)dst_idx;
        uint8_t v = tape[ptr];
        if (v != 0) {
            int accum = (int)tape[dst] + (int)v * (int)delta;
            tape[dst] = (uint8_t)accum;
            tape[ptr] = 0;
        }
    } break;

    default:
        cprintf("bf_exec_helper: unknown opcode %d\n", opcode);
        executor_ctx.last_error = BF_ERR_EXECUTION;
        ipc_send(executor_ctx.repl_id, BF_ERR_EXECUTION, NULL, 0, 0);
        exit();
    }

    executor_ctx.ptr_offset = ptr;
    return 0;
}

// LEGACY: Bytecode interpreter retained for tests. The JIT path now executes
// the compiled machine function directly (received via IPC and called at
// EXECUTOR_CODE_ADDR + CODE_ENTRY_OFFSET).
// The following pure_exec is retained for debugging but is no longer used by
// the REPL/executor JIT execution path.
void
pure_exec(char *bf, size_t code_size) {
    LOG("Executing bytecode of size %zu\n", code_size);
    uint8_t tape[BF_TAPE_SIZE] = {0};
    uint8_t *ptr = tape;
    int nest = 1;
    for (int i = 0; i < code_size; i++) {
        switch (bf[i]) {
        case '>':
            ptr++;
            if (ptr >= tape + BF_TAPE_SIZE) {
                ptr = tape;
            }
            break;
        case '<':
            ptr--;
            if (ptr < tape) {
                ptr = tape + BF_TAPE_SIZE - 1;
            }
            break;
        case '+':
            *ptr += 1;
            break;
        case '-':
            *ptr -= 1;
            break;
        case '.':
            format_output(*ptr);
            break;
        case ',':
            nest = getchar();
            if ((nest & 0xff) == 0xf4) {
                cprintf("^D\n");
                return;
            }
            *ptr = (uint8_t)nest;
            cputchar(nest);
            break;
        case '[':
            if (*ptr == 0) {
                nest = 1;
                while (++i < code_size && nest > 0) {
                    if (bf[i] == '[')
                        nest++;
                    else if (bf[i] == ']')
                        nest--;
                }
            }
            break;
        case ']':
            if (*ptr != 0) {
                nest = 1;
                while (nest > 0 && --i >= 0) {
                    if (bf[i] == ']')
                        nest++;
                    else if (bf[i] == '[')
                        nest--;
                }
            }
            break;
        default:
            break;
        }
        // cprintf("value: %u, char: %c, pos: %u\n", *ptr, *ptr, ptr - tape);
    }
}


/*
 * local bufer for tape is used in standalone mode
 * when out of tape returns error and stops processing
 * supports ^D while executing
 * works fine with 8 base comands in brainfuck
 * sensitive to endians
 * no syntax check. raw execute from bin file (located in fs). uses executor_ctx
 * optimitzed opcodes in future
 */


int
interpret_bf_bytecode() {
    // using global context
    uint8_t *tape = executor_ctx.tape;
    if (tape == NULL) {
        cprintf("Error: BF tape is not allocated.\n");
        return -BF_ERR_EXECUTION;
    }

    uint8_t *ptr = tape + (BF_TAPE_SIZE / 2);

    // num of instr in buffer
    size_t num_instructions = executor_ctx.code_size / sizeof(Instruction);
    Instruction *program = (Instruction *)executor_ctx.code_buf;

    // instruction pointer
    size_t ip = 0;

    // main cycle of execution
    LOG("interpret_bf_bytecode: starting execution of %zu instructions\n", num_instructions);

    while (ip < num_instructions) {
        Instruction inst = program[ip];
        LOG("interpret_bf_bytecode: executing instruction %zu: opcode=%d, arg=%d\n", ip, inst.opcode, inst.arg);

        switch (inst.opcode) {
        case OP_NOP:
            LOG("interpret_bf_bytecode: NOP\n");
            ip++;
            break;

        case OP_INC_PTR:
            LOG("interpret_bf_bytecode: INC_PTR by %d\n", inst.arg);
            LOG("interpret_bf_bytecode: ptr before INC_PTR: %p\n", ptr);
            if (ptr + inst.arg >= tape + BF_TAPE_SIZE) {
                cprintf("interpret_bf_bytecode::[ERROR] Pointer out of bounds (right)\n");
                return -BF_ERR_EXECUTION;
            }
            if (ptr + inst.arg < tape) {
                cprintf("interpret_bf_bytecode::[ERROR] Pointer out of bounds (left)\n");
                return -BF_ERR_EXECUTION;
            }
            ptr += inst.arg;
            LOG("interpret_bf_bytecode: ptr after INC_PTR: %p\n", ptr);
            ip++;
            break;

        case OP_DEC_PTR:
            LOG("interpret_bf_bytecode: DEC_PTR by %d\n", inst.arg);
            if (ptr - inst.arg < tape) {
                cprintf("Error: Pointer out of bounds (left)\n");
                return -BF_ERR_EXECUTION;
            }
            if (ptr - inst.arg >= tape + BF_TAPE_SIZE) {
                cprintf("Error: Pointer out of bounds (right)\n");
                return -BF_ERR_EXECUTION;
            }
            ptr -= inst.arg;
            ip++;
            break;
        case OP_INC_CELL:
            LOG("interpret_bf_bytecode: INC_CELL by %d (before: %d)\n", inst.arg, *ptr);
            int inc_delta = inst.arg & 0xff;
            int inc_val = (int)*ptr + inc_delta;
            *ptr = (uint8_t)inc_val;
            LOG("interpret_bf_bytecode: INC_CELL after: %d\n", *ptr);
            ip++;
            LOG("interpret_bf_bytecode: ip incremented to %zu\n", ip);
            break;

        case OP_DEC_CELL:
            LOG("interpret_bf_bytecode: DEC_CELL by %d (before: %d)\n", inst.arg, *ptr);
            int dec_delta = inst.arg & 0xff;
            int dec_val = (int)*ptr - dec_delta;
            *ptr = (uint8_t)dec_val;
            LOG("interpret_bf_bytecode: DEC_CELL after: %d\n", *ptr);
            ip++;
            break;

        case OP_OUTPUT:
            LOG("interpret_bf_bytecode: OUTPUT, *ptr = %d\n", *ptr);
            format_output(*ptr);
            LOG("interpret_bf_bytecode: format_output called\n");
            ip++;
            break;

        case OP_INPUT:
            LOG("interpret_bf_bytecode: INPUT\n");
            if (executor_ctx.input_pos >= executor_ctx.input_size) {
                LOG("interpret_bf_bytecode: need more input, calling smart_getchar\n");
                smart_getchar();
            }
            if ((*ptr = executor_ctx.input_buf[executor_ctx.input_pos++]) == 0xf4) {
                LOG("interpret_bf_bytecode: received EOF during INPUT, exiting\n");
                return BF_SUCCESS;
            }
            ip++;
            break;

        case OP_LOOP_START:
            LOG("interpret_bf_bytecode: LOOP_START\n");
            if (*ptr == 0) {
                // skip cycle body
                ip = inst.arg;
                LOG("interpret_bf_bytecode: skipping loop\n");
            } else {
                ip++;
            }
            break;

        case OP_LOOP_END:
            LOG("interpret_bf_bytecode: LOOP_END\n");
            if (*ptr != 0) {
                // go to start of cycle
                ip = inst.arg;
                LOG("interpret_bf_bytecode: jumping back to loop start\n");
            } else {
                ip++;
            }
            break;

        case OP_CLEAR:
            LOG("interpret_bf_bytecode: CLEAR (before: %d)\n", *ptr);
            *ptr = 0;
            ip++;
            break;

        case OP_SEEK_RIGHT: {
            int stride = inst.arg;
            if (stride <= 0) stride = 1;
            LOG("interpret_bf_bytecode: SEEK_RIGHT stride=%d\n", stride);
            while (*ptr != 0) {
                if (ptr + stride >= tape + BF_TAPE_SIZE) {
                    cprintf("Error: Pointer out of bounds (right)\n");
                    return -BF_ERR_EXECUTION;
                }
                ptr += stride;
            }
            ip++;
            break;
        }

        case OP_SEEK_LEFT: {
            int stride = inst.arg;
            if (stride <= 0) stride = 1;
            LOG("interpret_bf_bytecode: SEEK_LEFT stride=%d\n", stride);
            while (*ptr != 0) {
                if (ptr - stride < tape) {
                    cprintf("Error: Pointer out of bounds (left)\n");
                    return -BF_ERR_EXECUTION;
                }
                ptr -= stride;
            }
            ip++;
            break;
        }

        case OP_MOVE_ADD: {
            int16_t off = BF_UNPACK_MOVE_ADD_OFFSET(inst.arg);
            int16_t delta = BF_UNPACK_MOVE_ADD_DELTA(inst.arg);
            LOG("interpret_bf_bytecode: MOVE_ADD off=%d delta=%d\n", off, delta);

            uint8_t *dst = ptr + off;
            if (dst < tape || dst >= tape + BF_TAPE_SIZE) {
                cprintf("Error: MOVE_ADD destination out of bounds\n");
                return -BF_ERR_EXECUTION;
            }

            uint8_t v = *ptr;
            if (v != 0) {
                int accum = (int)(*dst) + (int)v * (int)delta;
                *dst = (uint8_t)accum;
                *ptr = 0;
            }
            ip++;
            break;
        }

        default:
            cprintf("Unknown opcode: %d at ip: %zu\n", inst.opcode, ip);
            return -BF_ERR_EXECUTION;
        }
    }
    LOG("interpret_bf_bytecode: execution finished\n");
    return BF_SUCCESS;
}


const char usage_msg[] =
        "Usage: bf_jit_interpreter [options]\n"
        "  -h (--help)                  : Show this help message\n"
        "  -d (--debug)                 : Enable debug mode\n"
        "  -p (--print) <format>        : Set output format (ascii/hex/dec)\n"
        "  <file>                       : Input file (optional)\n";

/*
 * CLI ARGUMENT PARSER
 *
 * USAGE: bf_jit_interpreter [-h] [-d] [-p <fmt>] [<file> (optional)]
 *
 */
int
parse_repl_arguments(int argc, char **argv) {
    if (argc < 2) {
        cprintf("%s", usage_msg);
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            // processing debug flag
            executor_ctx.debug_mode = true;
        } else if (strcmp(argv[i], "--REPL") == 0) {
            // processing REPL mode
            executor_ctx.REPL_mode = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--print") == 0) {
            // processing print format
            if (i + 1 >= argc) {
                cprintf("Error: -p requires a format argument\n\n");
                return -1;
            }
            i++;

            if (strcmp(argv[i], "ascii") == 0) {
                executor_ctx.output_format = BF_OUTPUT_ASCII;
            } else if (strcmp(argv[i], "hex") == 0) {
                executor_ctx.output_format = BF_OUTPUT_HEX;
            } else if (strcmp(argv[i], "dec") == 0) {
                executor_ctx.output_format = BF_OUTPUT_DEC;
            } else {
                cprintf("Error: Invalid output format. Choose: ascii, hex, dec\n\n");
                return -1;
            }

        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            // printing help message
            cprintf("%s", usage_msg);
            exit();
        } else if (argv[i][0] != '-') {
            // processing input file
            if (executor_ctx.input_file != NULL) {
                cprintf("Error: Only one input file can be specified\n\n");
                return -1;
            }
            executor_ctx.input_file = argv[i];
        } else {
            cprintf("Error: Unknown argument: %s\n\n", argv[i]);
            return -1;
        }
    }

    return 0;
}

static int
setup_exec_alias_mapping(size_t region_sz)
{
    const uintptr_t candidates[] = {
        (uintptr_t)EXECUTOR_CODE_ADDR + 0x100000,
        (uintptr_t)EXECUTOR_CODE_ADDR + 0x200000,
        (uintptr_t)EXECUTOR_CODE_ADDR + 0x300000,
        0x00E00000,
        0x00F00000,
    };

    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        uintptr_t va = candidates[i];
        if (va == (uintptr_t)EXECUTOR_CODE_ADDR) continue;
        if (sys_alloc_region(0, (void *)va, region_sz, PROT_RW | PROT_X) == 0) {
            exec_code_rw_addr = va;
            break;
        }
    }

    // Fallback: if no free VA for RW alias, just run RWX at EXECUTOR_CODE_ADDR
    if (exec_code_rw_addr == 0) {
        int r = sys_alloc_region(0, (void *)EXECUTOR_CODE_ADDR, region_sz, PROT_RW | PROT_X);
        if (r < 0) return r;
        exec_code_rw_addr = (uintptr_t)EXECUTOR_CODE_ADDR;
        exec_code_exec_addr = (uintptr_t)EXECUTOR_CODE_ADDR;
        return 0;
    }

    // Map RW backing into EXECUTOR_CODE_ADDR as RO(+X) alias.
    int r = sys_map_region(0,
                           (void *)exec_code_rw_addr,
                           0,
                           (void *)EXECUTOR_CODE_ADDR,
                           region_sz,
                           (PROT_R | PROT_X));
    if (r < 0) {
        // If alias mapping fails, just execute from RW addr (no W^X), but keep it working.
        exec_code_exec_addr = exec_code_rw_addr;
        return 0;
    }

    exec_code_exec_addr = (uintptr_t)EXECUTOR_CODE_ADDR;
    return 0;
}

void
umain(int argc, char **argv) {

    if (parse_repl_arguments(argc, argv) < 0) {
        cprintf("%s", usage_msg);
        return;
    }

    LOG("bf_jit_interpreter starting...\n");
    LOG("bf_jit_interpreter parsed arguments\n");

    // Allocate BF tape

    size_t tape_sz = (BF_TAPE_SIZE + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (sys_alloc_region(0, (void *)BF_TAPE_ADDR, tape_sz, PROT_RW) < 0) {
        cprintf("bf_jit_interpreter: failed to allocate BF tape\n");
        return;
    }
    executor_ctx.tape = (uint8_t *)BF_TAPE_ADDR;
    executor_ctx.ptr_offset = BF_TAPE_SIZE / 2; /* initialize data pointer to middle of tape */
    executor_ctx.last_error = BF_SUCCESS;

    LOG("Allocated BF tape at %p\n", executor_ctx.tape);

    // using local bufer for tape!
    // static uint8_t local_tape[BF_TAPE_SIZE];
    // executor_ctx.tape = local_tape;

    if (sys_alloc_region(0, (void *)(EXECUTOR_TEMP_ADDR), MAX_BF_MSG_LEN, PROT_RW) < 0) {
        cprintf("bf_jit_interpreter: failed to allocate receive buffer(s)\n");
        return;
    }

    if (sys_alloc_region(0, (void *)(EXECUTOR_TEMP_ADDR + MAX_BF_MSG_LEN), MAX_BF_MSG_LEN, PROT_RW) < 0) {
        cprintf("bf_jit_interpreter: failed to allocate input buffer(s)\n");
        return;
    }

    /* Allocate exec code area as separate region so W^X can be applied safely */
    if (setup_exec_alias_mapping(MAX_BF_MSG_LEN) < 0) {
        cprintf("bf_jit_interpreter: failed to setup exec mapping\n");
        return;
    }


    executor_ctx.code_buf = (uint8_t *)(EXECUTOR_TEMP_ADDR); /* receive buffer */
    executor_ctx.input_buf = (uint8_t *)(EXECUTOR_TEMP_ADDR + MAX_BF_MSG_LEN);

    LOG("Allocated receive buffer at %p and exec buffer at %p\n", (void *)(EXECUTOR_TEMP_ADDR), (void *)EXECUTOR_CODE_ADDR);

    if (executor_ctx.REPL_mode) {
        LOG("Entering REPL mode...\n");

        // REPL mode: wait for compiled code pages and execute them.

        while (1) {
            int perm = 0;
            LOG("waiting for compiled code from REPL\n");

            int32_t val = ipc_recv(&executor_ctx.repl_id, (void *)executor_ctx.code_buf, &executor_ctx.code_size, &perm);
            if (val < 0) {
                cprintf("bf_jit_interpreter: ipc_recv error %d\n", val);
                continue;
            }

            if (executor_ctx.code_size == 0 || executor_ctx.code_size > MAX_BF_MSG_LEN) {
                cprintf("bf_jit_interpreter: invalid code size %zu\n", executor_ctx.code_size);
                ipc_send(executor_ctx.repl_id, -BF_ERR_EXECUTION, NULL, 0, 0);
                continue;
            }

            /* copy into exec area */
            memcpy((void *)exec_code_rw_addr, executor_ctx.code_buf, executor_ctx.code_size);

            /* install helper pointer into code header so JIT code can call back */
            *(void **)((uintptr_t)exec_code_rw_addr + CODE_HELPER_PTR_OFFSET) = (void *)bf_exec_helper;


            /* execute from exec area */
            uint8_t *prev_buf = executor_ctx.code_buf;
            executor_ctx.code_buf = (uint8_t *)EXECUTOR_CODE_ADDR;

            LOG("Received IPC message from REPL %08x, size %zu bytes\n",
                val, executor_ctx.code_size);

            LOG("Executing JIT-compiled code (calling entry point)...\n");
            executor_ctx.last_error = BF_SUCCESS;
            void (*entry_fn)(void) = (void (*)(void))((uintptr_t)exec_code_exec_addr + CODE_ENTRY_OFFSET);
            entry_fn();
            int res = (executor_ctx.last_error == BF_SUCCESS) ? BF_SUCCESS : executor_ctx.last_error;
            LOG("JIT-compiled code execution complete, res=%d\n", res);

            /* restore receive buffer pointer */
            executor_ctx.code_buf = prev_buf;

            LOG("Sending execution completion signal back to REPL\n");
            ipc_send(executor_ctx.repl_id, res, NULL, 0, 0);
            LOG("Sent execution completion signal back to REPL\n");
        }
    } else { // standalone version. avoiding repl, ipc, compilers

        LOG("Using local tape at %p\n", executor_ctx.tape);

        LOG("USING STANDALONE VERSION.\n");
        int fd = open(executor_ctx.input_file, O_RDONLY);
        if (fd < 0) {
            cprintf("Error: Cannot open file %s\n", executor_ctx.input_file);
            return;
        }

        // read bytecode to buffer
        ssize_t bytes_read = read(fd, executor_ctx.code_buf, MAX_BF_MSG_LEN);
        if (bytes_read < 0) {
            cprintf("Error: Cannot read from file %s\n", executor_ctx.input_file);
            close(fd);
            return;
        }

        close(fd);

        executor_ctx.code_size = bytes_read;

        LOG("Loaded bytecode from file, size: %zu bytes\n", executor_ctx.code_size);

        if (executor_ctx.code_size == 0 || executor_ctx.code_size > MAX_BF_MSG_LEN) {
            cprintf("bf_jit_interpreter: invalid code size %zu\n", executor_ctx.code_size);
            return;
        }

        /* copy into exec area */
        memcpy((void *)exec_code_rw_addr, executor_ctx.code_buf, executor_ctx.code_size);

        /* install helper pointer into code header */
        *(void **)((uintptr_t)exec_code_rw_addr + CODE_HELPER_PTR_OFFSET) = (void *)bf_exec_helper;


        /* execute from exec area */
        uint8_t *prev_buf = executor_ctx.code_buf;
        executor_ctx.code_buf = (uint8_t *)EXECUTOR_CODE_ADDR;

        executor_ctx.last_error = BF_SUCCESS;
        void (*entry_fn)(void) = (void (*)(void))((uintptr_t)exec_code_exec_addr + CODE_ENTRY_OFFSET);
        entry_fn();
        if (executor_ctx.last_error != BF_SUCCESS) {
            cprintf("bf_jit_interpreter: JIT execution failed with %d\n", executor_ctx.last_error);
        }

        executor_ctx.code_buf = prev_buf;

        LOG("Execution COMPLETE.\n");
    }
}