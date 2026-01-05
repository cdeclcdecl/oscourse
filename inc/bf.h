/*
 * inc/bf.h - Shared definitions for Brainfuck JIT system
 * Used by REPL, compiler, and executor components
 * Must be compatible with both user-space and kernel-space
 */

#include <inc/types.h>
#include <inc/memlayout.h>
#include <inc/mmu.h>


#define BF_REPL_IMPLEMENTED 0
#define BF_COMPILER_IMPLEMENTED 0
#define BF_EXECUTOR_IMPLEMENTED 0

#define BF_COMPILER_FILE "bf_compiler"
#define BF_EXECUTOR_FILE "bf_jit_interpreter"
#define BF_REPL_FILE "brainfuck"

/*
 * System constants
 */
#define BF_TAPE_SIZE    30000           // Standard Brainfuck tape size (30KB)
#define REPL_TEMP_ADDR ((char *)0xa00000)    // Address for temporarily mappings
#define COMPILER_TEMP_ADDR ((char *) 0xb00000)
#define EXECUTOR_TEMP_ADDR ((char *) 0xc00000)
#define BF_TAPE_ADDR    ((char *) 0xd00000)        // Fixed address for BF tape in user space
#define MAX_BF_MSG_LEN (PAGE_SIZE)  // Max BF message size per IPC message

/*
 * IPC message magic numbers
 * Used to identify message types in shared pages
 */
#define BF_MAGIC_SOURCE         0xBFC0DE01 // BF source code from REPL to compiler
#define BF_MAGIC_EXEC           0xBFC0DE02 // Executable bytecode from compiler to REPL or from REPL to executor
#define BF_MAGIC_RESULT         0xBFC0DE03 // Execution result from executor to REPL

enum {
    BF_CURSOR_RIGHT = '>',
    BF_CURSOR_LEFT  = '<',
    BF_INCREMENT    = '+',
    BF_DECREMENT    = '-',
    BF_OUTPUT       = '.',
    BF_INPUT        = ',',
    BF_CYCLE_START  = '[',
    BF_CYCLE_END    = ']',
};

/*
 * Error codes for status field
 */
enum {
    BF_SUCCESS = 0,
    BF_ERR_SYNTAX = -1,     // Invalid BF syntax (unmatched brackets)
    BF_ERR_OVERFLOW = -2,   // Code size exceeds page limit
    BF_ERR_EXECUTION = -3,  // Runtime error during execution
};

/*
 * Output format flags (for -p option)
 * Passed from REPL to executor via environment variables or IPC
 */
enum {
    BF_OUTPUT_ASCII = 0, // Default: print as ASCII characters
    BF_OUTPUT_HEX = 1,   // Print values as HEX (e.g., "48 65 6C 6C 6F")
    BF_OUTPUT_DEC = 2    // Print values as decimal numbers
};

/*
 * Compilation context structure
 * Tracks all state during compilation process
 * 
 * Fields:
 *   source - Raw BF source code received via IPC or CLI
 *   src_len - Length of source code in bytes
 *   code_buf - Page-aligned buffer for generated machine code
 *   code_offset - Current write position in code buffer
 *   debug_mode - Flag for enabling debug logging (-d)
 *   optimize_time - Flag for time optimizations (-Otime)
 *   loop_stack - Stack tracking positions of '[' for backpatching
 *   loop_depth - Current depth of nested loops (for bracket validation)
 *   input_file - Optional input file name for source code
 *   repl_id - Environment ID of REPL process (for IPC)
 */
typedef struct {
    const char *source;
    size_t src_len;
    
    uint8_t *code_buf;
    size_t code_offset;

    int loop_stack[256];
    int loop_depth;
    
    bool REPL_mode;
    bool debug_mode;
    bool optimize_time;

    const char *input_file;
    const char *output_file;
    envid_t repl_id;
} bf_compiler_context_t;


/*
 * REPL context structure
 * Holds global state for the entire session
 *
 * Fields:
 *   compiler_id - Environment ID of spawned compiler process
 *   executor_id - Environment ID of spawned executor process
 *   interactive - Flag for interactive mode (REPL loop)
 *   compile_only - Flag for -bc mode (stop after compilation)
 *   execute_only - Flag for -e mode (skip compilation)
 *   optimize_time - Flag for -Otime optimizations
 *   output_format - Output format (BF_OUTPUT_ASCII/HEX/DEC)
 *   input_file - Filename for -f mode (NULL if stdin)
 *   bytecode_file - Filename for -e or -bc mode
 */
typedef struct {
    envid_t compiler_id;
    envid_t executor_id;

    bool interactive;
    bool compile_only;
    bool execute_only;
    bool optimize_time;
    bool debug_mode;

    int output_format;
    const char *input_file;
    const char *output_file;
    char *send_ipc_buf;
    size_t send_ipc_size;
    uint8_t *receive_ipc_buf;
    size_t receive_ipc_size;
    int fd;
} bf_repl_ctx_t;

/*
 * Execution context structure
 * Manages state during program execution
 *
 * Fields:
 *   tape - Pointer to BF memory tape (30KB at BF_TAPE_ADDR)
 *   code_buf - Buffer holding JIT-compiled bytecode
 *   code_size - Size of bytecode in bytes
 *   output_format - Output format (BF_OUTPUT_ASCII/HEX/DEC)
 *   debug_mode - Flag for enabling debug logging (-d)
 *   REPL_mode - Flag indicating if running in REPL mode
 *   input_file - Optional input file name for bytecode
 */
typedef struct {
    uint8_t *tape;
    uint8_t *code_buf;
    size_t code_size;

    int output_format;
    bool debug_mode;
    bool REPL_mode;

    envid_t repl_id;
    const char *input_file;
} bf_executor_ctx_t;