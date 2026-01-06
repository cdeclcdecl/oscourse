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

#define LOG(msg, ...) if (executor_ctx.debug_mode) { cprintf("[EXECUTOR]: " msg, ##__VA_ARGS__); }

bf_executor_ctx_t executor_ctx = {
    .tape = (uint8_t *) NULL,
    .output_format = BF_OUTPUT_ASCII,
    .debug_mode = false,
    .REPL_mode = false,
    .input_file = NULL,
    .repl_id = 0,
    .input_buf = NULL,
    .input_size = 0,
    .input_pos = 0,
};

/* Make [addr, addr+size) executable and revoke write permission.
 * Returns 0 on success, <0 on error. Caller must ensure the code bytes
 * were copied into the pages BEFORE calling this. */
int WxorX(void *addr, size_t size, enum bf_exec_mode mode)
{
    if (size == 0 || addr == NULL) return -1;

    uintptr_t start = ROUNDDOWN((uintptr_t)addr, PAGE_SIZE);
    size_t pages = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        void *pg = (void *)(start + i * PAGE_SIZE);

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

        if (sys_map_region(0, pg, 0, pg, PAGE_SIZE, want) < 0) {
            cprintf("make_region_exec: failed to change perms for %p -> 0x%x\n", pg, want);
            return -1;
        }
    }

    return 0;
}

/*
 * smart_getchar - stdin reader with line editing and puts it into executor_ctx.input_buf
 */
void smart_getchar(void) {
    
    memset(executor_ctx.input_buf, 0, executor_ctx.input_size);
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

        if (executor_ctx.input_size == PAGE_SIZE - 1) {
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
void format_output(char sym) {

    switch (executor_ctx.output_format) {
        case BF_OUTPUT_ASCII:
            cputchar(sym);
            break;
        case BF_OUTPUT_HEX:
            {
                char buf[4];
                int len = snprintf(buf, sizeof(buf), "%02X ", (unsigned char)sym);
                for (int i = 0; i < len; i++) {
                    cputchar(buf[i]);
                }
            }
            break;
        case BF_OUTPUT_DEC:
            {
                char buf[5];
                int len = snprintf(buf, sizeof(buf), "%u ", (unsigned char)sym);
                for (int i = 0; i < len; i++) {
                    cputchar(buf[i]);
                }
            }
            break;
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

// NOT USED! USE interpret_bf_bytecode INSTEAD!
// for testing purposes
// no syntax checks, optimizations, or IPC yet
// just pure interpretation
void pure_exec(char *bf, size_t code_size) {
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
                *ptr = (uint8_t) nest;
                cputchar(nest);
                break;
            case '[':
                if (*ptr == 0) {
                    nest = 1;
                    while (++i < code_size && nest > 0) {
                        if (bf[i] == '[') nest++;
                        else if (bf[i] == ']') nest--;
                    }
                } 
                break;
            case ']':
                if (*ptr != 0) {
                    nest = 1;
                    while (nest > 0 && --i >= 0) {
                        if (bf[i] == ']') nest++;
                        else if (bf[i] == '[') nest--;
                    }
                }
                break;
            default:
                break;
        }
        //cprintf("value: %u, char: %c, pos: %u\n", *ptr, *ptr, ptr - tape);
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


int interpret_bf_bytecode() {
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
                *ptr += inst.arg;
                LOG("interpret_bf_bytecode: INC_CELL after: %d\n", *ptr);
                ip++;
                LOG("interpret_bf_bytecode: ip incremented to %zu\n", ip);
                break;

            case OP_DEC_CELL:
                LOG("interpret_bf_bytecode: DEC_CELL by %d (before: %d)\n", inst.arg, *ptr);
                *ptr -= inst.arg;
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

            case OP_SEEK_RIGHT:
                LOG("interpret_bf_bytecode: SEEK_RIGHT\n");
                while (*ptr != 0) {
                    if (ptr + 1 >= tape + BF_TAPE_SIZE) {
                        cprintf("Error: Pointer out of bounds (right)\n");
                        return -BF_ERR_EXECUTION;
                    }
                    ptr++;
                }
                ip++;
                break;

            case OP_SEEK_LEFT:
                LOG("interpret_bf_bytecode: SEEK_LEFT\n");
                while (*ptr != 0) {
                    if (ptr <= tape) {
                        cprintf("Error: Pointer out of bounds (left)\n");
                        return -BF_ERR_EXECUTION;
                    }
                    ptr--;
                }
                ip++;
                break;

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
                    *dst = (uint8_t) accum;
                    *ptr = 0;
                }
                ip++;
                break;
            }

            // --- optimized opcodes will be here ---

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
int parse_repl_arguments(int argc, char **argv) {
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

void umain(int argc, char **argv) {
    
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
    executor_ctx.tape = (uint8_t *) BF_TAPE_ADDR;


    //executor_ctx.tape = (uint8_t *) BF_TAPE_ADDR;

    LOG("Allocated BF tape at %p\n", executor_ctx.tape);

    // using local bufer for tape!
    //static uint8_t local_tape[BF_TAPE_SIZE];
    //executor_ctx.tape = local_tape;

    if (sys_alloc_region(0, (void *)(EXECUTOR_TEMP_ADDR), PAGE_SIZE, PROT_RW) < 0) {
            cprintf("bf_jit_interpreter: failed to allocate receive buffer\n");
            return;
    }

    if (sys_alloc_region(0, (void *)(EXECUTOR_TEMP_ADDR + PAGE_SIZE), PAGE_SIZE, PROT_RW) < 0) {
            cprintf("bf_jit_interpreter: failed to allocate input buffer\n");
            return;
    }

    executor_ctx.code_buf = (uint8_t *)(EXECUTOR_TEMP_ADDR);
    executor_ctx.input_buf = (uint8_t *)(EXECUTOR_TEMP_ADDR + PAGE_SIZE);

    if (executor_ctx.REPL_mode) {
        LOG("Entering REPL mode...\n");

        // REPL mode: wait for compiled code pages and execute them.

        LOG("Allocated receive buffer at %p\n", (void *)(EXECUTOR_TEMP_ADDR));

        while (1) {
            int perm = 0;
            LOG("waiting for compiled code from REPL\n");

            WxorX((void *)EXECUTOR_CODE_ADDR, PAGE_SIZE, WRITABLE);

            int32_t val = ipc_recv(&executor_ctx.repl_id, (void *) executor_ctx.code_buf, &executor_ctx.code_size, &perm);
            if (val < 0) {
                cprintf("bf_jit_interpreter: ipc_recv error %d\n", val);
                continue;
            }

            WxorX((void *)EXECUTOR_CODE_ADDR, PAGE_SIZE, EXECUTABLE);

            LOG("Received IPC message from REPL %08x, size %zu bytes\n",
                val, executor_ctx.code_size);

            //LOG("Executing JIT-compiled code...\n");
            //pure_exec((char *)executor_ctx.code_buf, executor_ctx.code_size);
            LOG("Executing JIT-compiled code (interpret_bf_bytecode)...\n");
            int res = interpret_bf_bytecode();
            LOG("JIT-compiled code execution complete\n");

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
        ssize_t bytes_read = read(fd, executor_ctx.code_buf, PAGE_SIZE);
        if (bytes_read < 0) {
            cprintf("Error: Cannot read from file %s\n", executor_ctx.input_file);
            close(fd);
            return;
        }

        close(fd);

        executor_ctx.code_size = bytes_read;

        LOG("Loaded bytecode from file, size: %zu bytes\n", executor_ctx.code_size);

        WxorX((void *)EXECUTOR_CODE_ADDR, PAGE_SIZE, EXECUTABLE);

        interpret_bf_bytecode();

        LOG("Execution COMPLETE.\n");
    }
}