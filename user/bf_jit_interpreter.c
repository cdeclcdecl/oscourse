// responsible: deadlamer

/*
 * brainfuck.c - Brainfuck JIT REPL for JOS
 *
 * CLI USAGE:
 *    bf_jit_interpreter [options]
 *    --REPL                       : (Not shown in help message) REPL mode (waits for IPC)
 *    -h (--help)                  : Show this help message\n"
 *    -d (--debug)                 : Enable debug mode\n"
 *    -p (--print) ascii|hex|dec   : Set output format\n"
 *    <input_file>                 : Input file (optional)\n";
 *
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
};

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

    cprintf("bf_jit_interpreter starting...\n");
    LOG("bf_jit_interpreter parsed arguments\n");

    // Allocate BF tape
    /*
    if (sys_alloc_region(0, (void *)BF_TAPE_ADDR, BF_TAPE_SIZE, PROT_RW) < 0) {
        cprintf("bf_jit_interpreter: failed to allocate BF tape\n");
        return;
    }
    */
    executor_ctx.tape = (uint8_t *) BF_TAPE_ADDR;

    LOG("Allocated BF tape at %p\n", executor_ctx.tape);

    if (executor_ctx.REPL_mode) {
        LOG("Entering REPL mode...\n");

        // REPL mode: wait for compiled code pages and execute them.
        if (sys_alloc_region(0, (void *)(EXECUTOR_TEMP_ADDR), PAGE_SIZE, PROT_RW) < 0) {
            cprintf("bf_jit_interpreter: failed to allocate receive buffer\n");
            return;
        }

        executor_ctx.code_buf = (uint8_t *)(EXECUTOR_TEMP_ADDR);
        
        LOG("Allocated receive buffer at %p\n", (void *)(EXECUTOR_TEMP_ADDR));

        while (1) {
            int perm = 0;
            LOG("waiting for compiled code from REPL\n");
            int32_t val = ipc_recv(&executor_ctx.repl_id, (void *) executor_ctx.code_buf, &executor_ctx.code_size, &perm);
            if (val < 0) {
                cprintf("bf_jit_interpreter: ipc_recv error %d\n", val);
                continue;
            }

            LOG("Received IPC message from REPL %08x, size %zu bytes\n",
                val, executor_ctx.code_size);

            LOG("Executing JIT-compiled code...\n");
            pure_exec((char *)executor_ctx.code_buf, executor_ctx.code_size);
            
            LOG("JIT-compiled code execution complete\n");

            LOG("Sending execution completion signal back to REPL\n");
            ipc_send(executor_ctx.repl_id, 0, NULL, 0, 0);
            LOG("Sent execution completion signal back to REPL\n");

        }
    }
}