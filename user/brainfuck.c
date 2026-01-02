
// responsible: cdecl

/*
 * brainfuck.c - Brainfuck JIT REPL for JOS
 *
 * CLI USAGE:
 *   brainfuck                                  : Interactive REPL mode
 *   brainfuck -f (--file) <file.bf>            : Execute BF code from file
 *   brainfuck -bc (--bytecode) -f <file.bf>    : Compile only (output bytecode to stdout)
 *   brainfuck -e (--exec) <file.bc>            : Execute precompiled bytecode file
 *   brainfuck -Otime -f <file.bf>              : Enable time optimizations
 *   brainfuck -p (--print) ascii|hex|dec       : Set output format for execution
 *   brainfuck -h (--help)                      : Show help message
 *
 */

#include <inc/lib.h>
#include <inc/bf.h>

bf_repl_ctx_t repl_ctx = {
    .compiler_id = 0,
    .executor_id = 0,
    .interactive = true,
    .compile_only = false,
    .execute_only = false,
    .optimize_time = false,
    .output_format = BF_OUTPUT_ASCII,
    .input_file = NULL,
};

char usage_msg[] =
    "Usage: brainfuck [options]\n"
    "  -bc (--bytecode)         : Compile only (no execution)\n"
    "  -e (--exec)              : Execute precompiled bytecode\n"
    "  -O (--optimize)          : Enable time optimizations\n"
    "  -p (--print) <format>    : Set output format (ascii/hex/dec)\n"
    "  -h (--help)              : Show this help message\n"
    "  -d (--debug)             : Enable debug mode\n"
    "  <file>                   : Input file (optional)\n";


void
repl_loop(void) {
    // Interactive session management
    // TODO
    return;
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
void format_output(const char *raw_output, size_t len) {
    if (repl_ctx.debug_mode) {
        cprintf("[REPL]: Formatting output in mode %d\n", repl_ctx.output_format);
    }

    switch (repl_ctx.output_format) {
        case BF_OUTPUT_ASCII:
            for (size_t i = 0; i < len; i++) {
                char c = raw_output[i];
                if (c >= 32 && c <= 126) {
                    cputchar(c);
                } else {
                    cprintf('.'); // Non-printable as '.'
                }
            }
            cputchar('\n');
            break;

        case BF_OUTPUT_HEX:
            for (size_t i = 0; i < len; i++) {
                cprintf("%02x ", (unsigned char)raw_output[i]);
            }
            cputchar('\n');
            break;

        case BF_OUTPUT_DEC:
            for (size_t i = 0; i < len; i++) {
                cprintf("%u ", (unsigned char)raw_output[i]);
            }
            cputchar('\n');
            break;

        default:
            cprintf("Error: Unknown output format %d\n", repl_ctx.output_format);
            break;
    }

    if (repl_ctx.debug_mode) {
        cprintf("[REPL]: Output formatting complete\n");
    }
}

/*
 * CLI ARGUMENT PARSER
 *
 * GRAMMAR:
 *   brainfuck [-bc] [-e] [-O] [-p <fmt>] [-h] [<file> (optional)]
 *
 * RETURN:
 *   0 = success
 *   -1 = invalid arguments
 */
int parse_repl_arguments(int argc, char **argv) {

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            // processing debug flag
            repl_ctx.debug_mode = true;
        } else if (strcmp(argv[i], "-bc") == 0 || strcmp(argv[i], "--bytecode") == 0) {
            // processing bc flag
            repl_ctx.compile_only = true;
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--exec") == 0) {
            // processing e flag
            repl_ctx.execute_only = true;
        } else if (strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "--optimize") == 0) {
            // processing Otime flag
            repl_ctx.optimize_time = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--print") == 0) {
            // processing print format
            if (i + 1 >= argc) {
                cprintf("Error: -p requires a format argument\n\n");
                return -1;
            }
            i++;

            if (strcmp(argv[i], "ascii") == 0) {
                repl_ctx.output_format = BF_OUTPUT_ASCII;
            } else if (strcmp(argv[i], "hex") == 0) {
                repl_ctx.output_format = BF_OUTPUT_HEX;
            } else if (strcmp(argv[i], "dec") == 0) {
                repl_ctx.output_format = BF_OUTPUT_DEC;
            } else {
                cprintf("Error: Invalid output format. Choose: ascii, hex, dec\n\n");
                return -1;
            }

        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            // printing help message
            cprintf("%s", usage_msg);
            exit();
        } else {
            // processing input file
            if (repl_ctx.input_file == NULL) {
                repl_ctx.input_file = argv[i];
                continue;
            }

            cprintf("Error: Unknown argument: %s\n\n", argv[i]);
            return -1;
        }
    }

    // check that -e and -bc are not combined
    if (repl_ctx.compile_only && repl_ctx.execute_only) {
        cprintf("Error: -bc and -e options cannot be combined\n\n");
        return -1;
    }

    // check that -bc and -e have input file
    if (repl_ctx.compile_only && repl_ctx.input_file == NULL) {
        cprintf("Error: -bc requires an input file\n\n");
        return -1;
    }

    if (repl_ctx.execute_only && repl_ctx.input_file == NULL) {
        cprintf("Error: -e requires an input file\n\n");
        return -1;
    }
    
    // set interactive mode depending on flags
    if (!repl_ctx.execute_only && repl_ctx.input_file == NULL && !repl_ctx.compile_only) {
        repl_ctx.interactive = true;
    } else {
        repl_ctx.interactive = false;
    }



    return 0;
}

/*
 * RESOURCE CLEANUP
 *
 * RESPONSIBILITIES:
 *   1. Terminate child processes:
 *        sys_env_destroy(compiler_id)
 *        sys_env_destroy(executor_id)
 *   2. Close open file descriptors
 *   3. Restore original terminal settings
 *   4. Free allocated memory buffers
 *
 */
void cleanup_resources(bf_repl_ctx_t *ctx) {
    // TODO
    return;
}

/*
 * MAIN ENTRY POINT
 *
 * main logic of a program
 *
 * EXIT CODES:
 *   0 = success
 *   1 = invalid arguments
 *   2 = process spawn failure
 *   3 = IPC communication failure
 */
void
umain(int argc, char **argv) {
    int res = 0;
    // parse command-line arguments
    res = parse_repl_arguments(argc, argv);
    if (res < 0) {
        cprintf("%s", usage_msg);
        exit();
    }

    if (repl_ctx.debug_mode) {
        cprintf("[REPL]: Successfully parsed arguments.\n");
    }

    // TODO

    return;
}
