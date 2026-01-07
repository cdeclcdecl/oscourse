/*
 * inc/bf.h - Shared definitions for Brainfuck JIT system
 * Used by REPL, compiler, and executor components
 * Must be compatible with both user-space and kernel-space
 */

#include <inc/types.h>
#include <inc/memlayout.h>
#include <inc/mmu.h>


#define BF_REPL_IMPLEMENTED     1
#define BF_COMPILER_IMPLEMENTED 1
#define BF_EXECUTOR_IMPLEMENTED 1

#define BF_COMPILER_FILE "bf_compiler"
#define BF_EXECUTOR_FILE "bf_jit_interpreter"
#define BF_REPL_FILE     "brainfuck"


enum bf_exec_mode {
    WRITABLE = PROT_W,
    EXECUTABLE = PROT_X,
};

/*
 * System constants
 */
#define BF_TAPE_SIZE       30000              // Standard Brainfuck tape size (30KB)
#define REPL_TEMP_ADDR     ((char *)0xa00000) // Address for temporarily mappings
#define COMPILER_TEMP_ADDR ((char *)0xb00000)
#define EXECUTOR_TEMP_ADDR ((char *)0xc00000)
#define EXECUTOR_CODE_ADDR ((char *)(EXECUTOR_TEMP_ADDR + 2 * MAX_BF_MSG_LEN)) // Fixed address for JIT-compiled code in executor
/* Place BF tape after the executor regions to avoid overlaps (was hardcoded to 0xd00000).
 * Layout: EXECUTOR_TEMP_ADDR (recv/input buffers) | EXECUTOR_CODE_ADDR (exec pages) | BF_TAPE_ADDR */
#define BF_TAPE_ADDR   ((char *)(EXECUTOR_TEMP_ADDR + 3 * MAX_BF_MSG_LEN)) // Fixed address for BF tape in user space
#define MAX_BF_MSG_LEN (128 * PAGE_SIZE)                                   // Max BF message size per IPC message

/* JIT code header layout placed at the start of the exec region. */
#define CODE_HEADER_SIZE       (8)                /* one qword: pointer to bf_exec_helper */
#define CODE_HELPER_PTR_OFFSET (0)                /* offset inside header to helper pointer */
#define CODE_ENTRY_OFFSET      (CODE_HEADER_SIZE) /* start of generated code (after header) */

/*
 * IPC message magic numbers
 * Used to identify message types in shared pages
 */
#define BF_MAGIC_SOURCE 0xBFC0DE01 // BF source code from REPL to compiler
#define BF_MAGIC_EXEC   0xBFC0DE02 // Executable machine code from compiler to REPL or from REPL to executor
#define BF_MAGIC_RESULT 0xBFC0DE03 // Execution result from executor to REPL

enum {
    BF_CURSOR_RIGHT = '>',
    BF_CURSOR_LEFT = '<',
    BF_INCREMENT = '+',
    BF_DECREMENT = '-',
    BF_OUTPUT = '.',
    BF_INPUT = ',',
    BF_CYCLE_START = '[',
    BF_CYCLE_END = ']',
};

/*
 * Error codes for status field
 */
enum {
    BF_SUCCESS = 0,
    BF_ERR_SYNTAX = -1,    // Invalid BF syntax (unmatched brackets)
    BF_ERR_EXECUTION = -2, // Runtime error during execution
    BF_ERR_OVERFLOW = -3,  // Code buffer overflow during compilation
    BF_LOGIC_ERROR = -4,   // Internal logic error
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
 *   optimize_time - Flag for -Otime optimizations
 *   output_format - Output format (BF_OUTPUT_ASCII/HEX/DEC)
 *   input_file - Filename for file-based mode (NULL if stdin)
 */
typedef struct {
    envid_t compiler_id;
    envid_t executor_id;

    bool interactive;
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
 *   code_buf - Buffer holding JIT-compiled machine code
 *   code_size - Size of code in bytes
 *   output_format - Output format (BF_OUTPUT_ASCII/HEX/DEC)
 *   debug_mode - Flag for enabling debug logging (-d)
 *   REPL_mode - Flag indicating if running in REPL mode
 *   input_file - Optional input file name for compiled code
 */
typedef struct {
    uint8_t *tape;
    uint8_t *code_buf;
    size_t code_size;

    /* offset of current data pointer from tape base - used by JIT helper */
    size_t ptr_offset;
    int last_error;

    int output_format;
    bool debug_mode;
    bool REPL_mode;

    envid_t repl_id;
    uint8_t *input_buf;
    size_t input_size;
    size_t input_pos;
    const char *input_file;
} bf_executor_ctx_t;


// OP_MOVE_ADD packing helpers
// arg packs two signed 16-bit values:
//   low 16 bits  = offset (relative cell index)
//   high 16 bits = delta per unit (e.g., +1 for [->+<], -1 for [->-<], +2 for [->++<])
#define BF_PACK_MOVE_ADD(offset, delta) ((int32_t)((((uint32_t)((uint16_t)(offset))) & 0xFFFFu) | (((uint32_t)((uint16_t)(delta))) << 16)))
#define BF_UNPACK_MOVE_ADD_OFFSET(arg)  ((int16_t)((uint32_t)(arg) & 0xFFFFu))
#define BF_UNPACK_MOVE_ADD_DELTA(arg)   ((int16_t)(((uint32_t)(arg) >> 16) & 0xFFFFu))
/*
 * JIT Helper Opcodes
 * These are used by the JIT-compiled code to call back into bf_exec_helper
 * They are NOT bytecode instructions - they are passed as arguments to the helper function
 */
typedef enum {
    OP_NOP = 0,    // No operation
    OP_INC_PTR,    // > : increase data pointer
    OP_DEC_PTR,    // < : decrease data pointer
    OP_INC_CELL,   // + : increase value of current cell
    OP_DEC_CELL,   // - : decrease value of current cell
    OP_OUTPUT,     // . : show value of current cell
    OP_INPUT,      // , : put value to current cell
    OP_LOOP_START, // [ : test for loop start

    // Optimized opcodes
    OP_CLEAR,      // [-] or [+] : set current cell to 0
    OP_SEEK_RIGHT, // [>] : move pointer right until current cell becomes 0
    OP_SEEK_LEFT,  // [<] : move pointer left until current cell becomes 0
    OP_MOVE_ADD,   // [->(+/-k)<] : move current cell value to offset cell, then clear current

    OP_COUNT // label for counting
} Opcode;
