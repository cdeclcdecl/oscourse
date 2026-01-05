
// responsible: cdecl

/*
 * brainfuck.c - Brainfuck JIT REPL for JOS
 *
 * CLI USAGE:
 *   brainfuck                                  : Interactive REPL mode
 *   brainfuck <file.bf>                        : Compile and execute BF source file
 *   brainfuck -bc (--bytecode) <file.bf>       : Compile only (output bytecode to stdout)
 *   brainfuck -e (--exec) <file.bc>            : Execute precompiled bytecode file
 *   brainfuck -Otime                           : Enable time optimizations
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
    .fd = -1,
    .send_ipc_buf = NULL,
    .receive_ipc_buf = NULL,
};

#define LOG(msg, ...) if (repl_ctx.debug_mode) { cprintf("[REPL]: " msg, ##__VA_ARGS__); }

char usage_msg[] =
    "Usage: brainfuck [options]\n"
    "  -bc (--bytecode)         : Compile only (no execution)\n"
    "  -o (--output) <file>     : Specify output file for bytecode (only for -bc option) (default: out.bc)\n"
    "  -e (--exec)              : Execute precompiled bytecode\n"
    "  -Otime (--optimize)      : Enable time optimizations\n"
    "  -p (--print) <format>    : Set output format (ascii/hex/dec)\n"
    "  -h (--help)              : Show this help message\n"
    "  -d (--debug)             : Enable debug mode\n"
    "  <file>                   : Input file (optional)\n";


char repl_help_msg[] =
    "Brainfuck REPL Help:\n"
    "  Enter Brainfuck code directly to compile and execute it.\n"
    "  Special commands:\n"
    "    !help                  Show this help message\n"
    "    !fmt ascii/hex/dec     Set output format\n"
    "    !reset                 Reset REPL state\n"
    "    !quit                  Exit the REPL\n";


 /*
 * RESOURCE CLEANUP
 *
 * RESPONSIBILITIES:
 *   1. Terminate child processes:
 *        sys_env_destroy(compiler_id)
 *        sys_env_destroy(executor_id)
 *   2. Close open file descriptors
 *   3. Free allocated memory buffers
 *
 */
void cleanup_resources(void) {
    LOG("Cleaning up resources...\n");

    if (repl_ctx.compiler_id != 0) {
        LOG("Destroying compiler process %08x\n", repl_ctx.compiler_id);
        sys_env_destroy(repl_ctx.compiler_id);
    }

    if (repl_ctx.executor_id != 0) {
        LOG("Destroying executor process %08x\n", repl_ctx.executor_id);
        sys_env_destroy(repl_ctx.executor_id);
    }

    if (repl_ctx.fd != -1) {
        LOG("Closing input file descriptor\n");
        close(repl_ctx.fd);
    }

    if (repl_ctx.send_ipc_buf != NULL) {
        LOG("Freeing send IPC buffer\n");
        sys_unmap_region(0, repl_ctx.send_ipc_buf, PAGE_SIZE);
    }

    if (repl_ctx.receive_ipc_buf != NULL) {
        LOG("Freeing receive IPC buffer\n");
        sys_unmap_region(0, repl_ctx.receive_ipc_buf, PAGE_SIZE);
    }

    LOG("Resource cleanup complete\n");
    return;
}

void err_exit(void) {
    LOG("Exiting with error\n");
    cleanup_resources();
    exit();
}

/*
 * CLI ARGUMENT PARSER
 *
 * GRAMMAR:
 *   brainfuck [-bc <output_file> (optional - default: out.bc)] [-e] [-Otime] [-p <fmt>] [-h] [<file> (optional)]
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
        } else if (strcmp(argv[i], "-Otime") == 0 || strcmp(argv[i], "--optimize") == 0) {
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
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            // processing output file
            if (i + 1 >= argc) {
                cprintf("Error: -o requires a file argument\n\n");
                return -1;
            }
            i++;
            repl_ctx.output_file = argv[i];
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

    if (repl_ctx.compile_only && repl_ctx.output_file == NULL) {
        repl_ctx.output_file = "out.bc";
    }
    
    if (!repl_ctx.compile_only && repl_ctx.output_file != NULL) {
        cprintf("Error: -o option can only be used with -bc\n\n");
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
 * PROCESS SPAWNER
 *
 * spawns compiler/executor processes
 * For now, not passing any arguments because not implemented
 * 
 * RETURN:
 *  envid_t of spawned process on success
 *  0 on failure
 * 
 */
envid_t spawn_compiler(void) {

    size_t cur = 2;
    char *argv[5] = {BF_COMPILER_FILE, "--REPL", NULL, NULL, NULL};
    envid_t compiler_id;

    if (repl_ctx.optimize_time) {
        argv[cur++] = "--Otime";
    }

    if (repl_ctx.debug_mode) {
        argv[cur++] = "-d";
    }

    LOG("Spawning compiler process...\n");

    if ((compiler_id = spawn(argv[0], (const char **) argv)) < 0) {
        cprintf("Error: Failed to spawn compiler process\n");
        return 0;
    }

    LOG("Successfully spawned compiler with envid %08x\n", compiler_id);

    return compiler_id;
}

envid_t spawn_executor(void) {
    // Still TODO : cli arguments are not processed yet
    
    size_t cur = 2;
    char *argv[6] = {BF_EXECUTOR_FILE, "--REPL", NULL, NULL, NULL, NULL};
    envid_t executor_id;

    if (repl_ctx.debug_mode) {
        argv[cur++] = "-d";
    }

    switch (repl_ctx.output_format) {
        case BF_OUTPUT_ASCII:
            argv[cur++] = "--print";
            argv[cur++] = "ascii";
            break;
        case BF_OUTPUT_HEX:
            argv[cur++] = "--print";
            argv[cur++] = "hex";
            break;
        case BF_OUTPUT_DEC:
            argv[cur++] = "--print";
            argv[cur++] = "dec";
            break;
        default:
            break;
    }

    LOG("Spawning executor process...\n");

    if ((executor_id = spawn(argv[0], (const char **) argv)) < 0) {
        cprintf("Error: Failed to spawn executor process\n");
        return 0;
    }

    LOG("Successfully spawned executor with envid %08x\n", executor_id);

    return executor_id;
}

/*
 * COMPILER IPC COMMUNICATION
 * Sends raw data page to compiler/executor process via IPC.
 */
void send_to_compiler(void) {
    LOG("Sendinfg to compiler %lu bytes\n", repl_ctx.send_ipc_size);
    ipc_send(repl_ctx.compiler_id, 0, repl_ctx.send_ipc_buf, repl_ctx.send_ipc_size, PROT_RW);
}

/*
 * EXECUTOR IPC COMMUNICATION
 * Sends page with bytecode to executor process via IPC.
 *
 */
void send_to_executor(void) {
    LOG("Sending to executor %lu bytes\n", repl_ctx.send_ipc_size);
    ipc_send(repl_ctx.executor_id, 0, repl_ctx.send_ipc_buf, repl_ctx.send_ipc_size, PROT_RW);
}


/*
 * REPL LOOP
 *
 * RESPONSIBILITIES:
 *   1. Read user input line-by-line
 *   2. Process special commands (!help, !fmt, !reset, !quit)
 *   3. Send Brainfuck code to compiler via IPC
 *   4. Receive compiled bytecode from compiler via IPC
 *   5. Send bytecode to executor via IPC
 *   6. Receive execution output from executor via IPC
 *   7. Format and display output according to user settings
 *   8. repeat until !quit or EOF
 */
void
repl_loop(void) {
    // Interactive session management
    LOG("Starting interactive REPL loop...\n");

    char input_buf[PAGE_SIZE];
    size_t buf_pose = 0;
    int c;

    cprintf("Brainfuck JIT REPL [JOS Edition]\n");
    cprintf("Type '!help' for list of commands, Ctrl+D or '!quit' to exit\n\n");

    while(1) {
        cprintf(">>>> ");

        while (((c = getchar()) & 0xff) != 0xf4 && (c & 0xff) != 0x0d && buf_pose < PAGE_SIZE - 1) {
            //LOG("Read char: %c (0x%02x)\n", (char)c, (unsigned char)c);

            if ((c & 0xff) == 0x08 || (c & 0xff) == 0x7f) { // Backspace or DEL
                if (buf_pose > 0) {
                    buf_pose--;
                    // Move cursor back, overwrite with space, move back again
                    cprintf("\b \b");
                }
                continue;
            }

            if ((c & 0xff) == 0x03) { // Ctrl+C
                cprintf("^C\n");
                buf_pose = 0;
                break;
            }

            if (buf_pose < PAGE_SIZE - 1) {
                input_buf[buf_pose++] = (char)c;
                cputchar(c);
            }
        }

        LOG("Exited reading loop");

        

        if ((c & 0xff) == 0xf4) {
            LOG("Received EOF, exiting REPL loop\n");
            cprintf("^D");
            cputchar('\n');
            cprintf("\nExiting REPL...\n");
            break;
        }

        cprintf("\n");

        input_buf[buf_pose] = '\0';

        if (buf_pose > 0 && input_buf[0] == '!') {
            LOG("Processing REPL command: %s\n", input_buf);
            if (strncmp(input_buf, "!quit", 5) == 0) {
                LOG("Received !quit command, exiting REPL loop\n");
                cprintf("Exiting REPL...\n");
                break;
            } else if (strncmp(input_buf, "!help", 5) == 0) {
                cprintf("%s", repl_help_msg);
            } else if (strncmp(input_buf, "!fmt ", 5) == 0) {
                char *fmt = input_buf + 5;
                if (strncmp(fmt, "ascii", 5) == 0) {
                    repl_ctx.output_format = BF_OUTPUT_ASCII;
                    cprintf("Output format set to ASCII\n");
                } else if (strncmp(fmt, "hex", 3) == 0) {
                    repl_ctx.output_format = BF_OUTPUT_HEX;
                    cprintf("Output format set to HEX\n");
                } else if (strncmp(fmt, "dec", 3) == 0) {
                    repl_ctx.output_format = BF_OUTPUT_DEC;
                    cprintf("Output format set to DEC\n");
                } else {
                    cprintf("Error: Unknown format '%s'. Use ascii, hex, or dec.\n", fmt);
                }
            } else if (strncmp(input_buf, "!reset", 6) == 0) {
                cprintf("Resetting REPL state...\n");
                sys_env_destroy(repl_ctx.executor_id);
                repl_ctx.executor_id = spawn_executor();
                if (repl_ctx.executor_id == 0) {
                    err_exit();
                }
            } else {
                cprintf("Error: Unknown command '%s'. Type '!help' for assistance.\n", input_buf);
            }
            buf_pose = 0;
            continue;
        }

        if (buf_pose == 0) {
            LOG("Empty input, continuing REPL loop\n");
            continue; // Skipping empty lines
        }

        LOG("Received Brainfuck code input (%zu bytes)\n", buf_pose);
        LOG("Input code:\n%s\n", input_buf);

        int perm = 0;
        LOG("Copying memory into sending buf\n");
        memcpy((void *) repl_ctx.send_ipc_buf, (void *) input_buf, buf_pose);
        repl_ctx.send_ipc_size = buf_pose;
        LOG("Copied memory from buf\n");
        /* Send only the meaningful bytes (no message headers) */
        send_to_compiler();

        ipc_recv(&repl_ctx.compiler_id, repl_ctx.receive_ipc_buf, &repl_ctx.receive_ipc_size, &perm);
        LOG("Received message from compiler, ");

        LOG("Compiled bytecode size: %zu bytes\n", repl_ctx.receive_ipc_size);
        // send to executor

        memcpy((void *) repl_ctx.send_ipc_buf, (void *) repl_ctx.receive_ipc_buf, repl_ctx.receive_ipc_size);
        send_to_executor();
        LOG("Sent bytecode to executor %08x\n", repl_ctx.executor_id);
        ipc_recv(&repl_ctx.executor_id, NULL, NULL, &perm);
        LOG("Received execution completion from executor\n");

        buf_pose = 0;
    }

    LOG("Exiting REPL loop...\n");
    return;
}

void file_based_usage(void) {
    LOG("Starting file-based execution...\n");

    repl_ctx.fd = open(repl_ctx.input_file, 'r');
    if (repl_ctx.fd < 0) {
        cprintf("Error: Failed to open input file %s\n", repl_ctx.input_file);
        err_exit();
    }
    LOG("Opened input file descriptor %d\n", repl_ctx.fd);

    int n = read(repl_ctx.fd, repl_ctx.send_ipc_buf, PAGE_SIZE);
    if (n < 0) {
        cprintf("Error: Failed to read input file %s\n", repl_ctx.input_file);
        err_exit();
    }

    repl_ctx.send_ipc_size = n;
    LOG("Read %d bytes from input file\n", n);

    send_to_compiler();
    int perm = 0;
    ipc_recv(&repl_ctx.compiler_id, repl_ctx.receive_ipc_buf, &repl_ctx.send_ipc_size, &perm);
    LOG("Received compiled bytecode of size %zu bytes from compiler\n", repl_ctx.send_ipc_size);

    memcpy((void *) repl_ctx.send_ipc_buf, (void *) repl_ctx.receive_ipc_buf, repl_ctx.send_ipc_size);
    send_to_executor();
    LOG("Sent bytecode to executor %08x\n", repl_ctx.executor_id);
    ipc_recv(&repl_ctx.executor_id, NULL, NULL, &perm);
    LOG("Received execution completion from executor\n");

    LOG("Exiting file-based execution...\n");
    return;
}

void complile_only_usage(void) {
    LOG("Starting compile-only execution...\n");
    LOG("Input file: %s\n", repl_ctx.input_file);
    repl_ctx.fd = open(repl_ctx.input_file, 'r');
    if (repl_ctx.fd < 0) {
        cprintf("Error: Failed to open input file %s\n", repl_ctx.input_file);
        err_exit();
    }
    LOG("Opened input file descriptor %d\n", repl_ctx.fd);

     // send filename to compiler via IPC
    
    LOG("Reading Brainfuck source from file %s...\n", repl_ctx.input_file);
    int n = read(repl_ctx.fd, repl_ctx.send_ipc_buf, PAGE_SIZE);
    if (n < 0) {
        cprintf("Error: Failed to read input file %s\n", repl_ctx.input_file);
        err_exit();
    }
    
    repl_ctx.send_ipc_size = n;
    LOG("Read %d bytes from input file\n", n);

    send_to_compiler();

    int perm = 0;
    ipc_recv(&repl_ctx.compiler_id, repl_ctx.receive_ipc_buf, &repl_ctx.send_ipc_size, &perm);
    LOG("Received compiled bytecode of size %zu bytes from compiler\n", repl_ctx.send_ipc_size);

    // write bytecode to output file
    int fd_out = open(repl_ctx.output_file, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd_out < 0) {
        cprintf("Error: Failed to open output file %s\n", repl_ctx.output_file);
        err_exit();
    }
    LOG("Opened output file descriptor %d\n", fd_out);

    ssize_t written = write(fd_out, repl_ctx.receive_ipc_buf, repl_ctx.send_ipc_size);
    if (written < 0 || (size_t)written != repl_ctx.send_ipc_size) {
        cprintf("Error: Failed to write bytecode to output file %s\n", repl_ctx.output_file);
        close(fd_out);
        err_exit();
    }
    LOG("Wrote %zu bytes of bytecode to output file %s\n", repl_ctx.send_ipc_size, repl_ctx.output_file);

    close(fd_out);
    LOG("Closed output file descriptor\n");
    return;
}

void execute_only_usage(void) {
    LOG("Starting execute-only execution...\n");

    repl_ctx.fd = open(repl_ctx.input_file, 'r');
    if (repl_ctx.fd < 0) {
        cprintf("Error: Failed to open input file %s\n", repl_ctx.input_file);
        err_exit();
    }
    LOG("Opened input file descriptor %d\n", repl_ctx.fd);

    int n = read(repl_ctx.fd, repl_ctx.send_ipc_buf, PAGE_SIZE);
    if (n < 0) {
        cprintf("Error: Failed to read input file %s\n", repl_ctx.input_file);
        err_exit();
    }

    repl_ctx.send_ipc_size = n;
    LOG("Read %d bytes from input file\n", n);
    send_to_executor();
    int perm = 0;
    ipc_recv(&repl_ctx.executor_id, NULL, NULL, &perm);
    LOG("Received execution completion from executor\n");

    return;
}

/*
 * MAIN ENTRY POINT
 *
 * main logic of a program
 *
 */
void
umain(int argc, char **argv) {
    // Still TODO
    int res = 0;
    // parse command-line arguments
    res = parse_repl_arguments(argc, argv);
    if (res < 0) {
        cprintf("%s", usage_msg);
        exit();
    }

    LOG("Successfully parsed arguments.\n");
    LOG("\tinteractive=%d\n\tcompile_only=%d\n\texecute_only=%d\n\toptimize_time=%d\n\toutput_format=%d\n\tinput_file=%s\n",
            repl_ctx.interactive,
            repl_ctx.compile_only,
            repl_ctx.execute_only,
            repl_ctx.optimize_time,
            repl_ctx.output_format,
            repl_ctx.input_file ? repl_ctx.input_file : "NULL");

    // Allocate IPC buffers
    res = sys_alloc_region(0, (void *)REPL_TEMP_ADDR, PAGE_SIZE, PROT_RW);
    if (res < 0) {
        cprintf("Error: Failed to allocate IPC buffer for compiler\n");
        err_exit();
    }

    repl_ctx.send_ipc_buf = (void *)REPL_TEMP_ADDR;

    res = sys_alloc_region(0, (void *)(REPL_TEMP_ADDR + PAGE_SIZE), PAGE_SIZE, PROT_RW);
    if (res < 0) {
        cprintf("Error: Failed to allocate IPC buffer for executor\n");
        err_exit();
    }

    repl_ctx.receive_ipc_buf = (void *) (REPL_TEMP_ADDR + PAGE_SIZE);

    // if user wants to only execute precompiled bytecode
    if (repl_ctx.execute_only) {

        LOG("Following execute-only mode...\n");

        repl_ctx.executor_id = spawn_executor();
        if (repl_ctx.executor_id == 0) {
            err_exit();
        }

        execute_only_usage();
    }

    // if user wants to only compile Brainfuck source code
    if (repl_ctx.compile_only) {

        LOG("Following compile-only mode...\n");

        repl_ctx.compiler_id = spawn_compiler();
        if (repl_ctx.compiler_id == 0) {
            err_exit();
        }

        complile_only_usage();
    }

    // if user wants interactive REPL session
    if (repl_ctx.interactive) {

        LOG("Following interactive REPL mode...\n");

        repl_ctx.compiler_id = spawn_compiler();
        if (repl_ctx.compiler_id == 0) {
            err_exit();
        }

        repl_ctx.executor_id = spawn_executor();
        if (repl_ctx.executor_id == 0) {
            err_exit();
        }

        repl_loop();
    }

    // if user wants file-based execution
    if (repl_ctx.input_file != NULL && !repl_ctx.compile_only && !repl_ctx.execute_only && !repl_ctx.interactive) {
    
        LOG("File-based execution mode is not implemented yet\n");
    
        repl_ctx.compiler_id = spawn_compiler();
        if (repl_ctx.compiler_id == 0) {
            err_exit(); 
        }

        repl_ctx.executor_id = spawn_executor();
        if (repl_ctx.executor_id == 0) {
            err_exit();
        }

        file_based_usage();
    }



    cleanup_resources();
    LOG("Exiting program normally\n");

    return;
}
