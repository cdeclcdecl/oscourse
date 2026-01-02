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

/*
 * System constants
 */
#define BF_TAPE_SIZE    30000           // Standard Brainfuck tape size (30KB)
#define BF_TAPE_ADDR    0x500000        // Fixed address for BF tape in user space
#define MAX_BF_CODE_LEN (PAGE_SIZE - 8) // Max BF code size per IPC message

/*
 * IPC message magic numbers
 * Used to identify message types in shared pages
 */
#define BF_MAGIC_SOURCE 0xBFC0DE01 // BF source code from REPL to compiler
#define BF_MAGIC_EXEC   0xBFC0DE02 // Executable bytecode from compiler to REPL or from REPL to executor
#define BF_MAGIC_RESULT 0xBFC0DE03 // Execution result from executor to REPL

enum {
    BC_CURSOR_RIGHT = '>',
    BC_CURSOR_ADD   = '{',
    BC_CURSOR_LEFT  = '<',
    BC_CURSOR_SUB   = '}',
    BC_INCREMENT    = '+',
    BC_ADD          = '*',
    BC_SUB          = '/',
    BC_DECREMENT    = '-',
    BC_OUTPUT       = '.',
    BC_INPUT        = ',',
    BC_CYCLE_START  = '[',
    BC_CYCLE_END    = ']',
};


/*
 * BF source code message structure
 * Sent from REPL to compiler via IPC
 * Must fit within one page (4KB)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;             // BF_MAGIC_SOURCE
    uint32_t code_len;          // Length of BF source code
    char code[MAX_BF_CODE_LEN]; // Null-terminated BF source code
} bf_source_msg_t;

/*
 * BF bytecode message structure
 * Sent from compiler to executor via IPC
 * Contains x86 machine code ready for execution
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;               // BF_MAGIC_EXEC
    uint32_t code_size;           // Size of generated machine code (bytes)
    uint32_t tape_size;           // Required tape size (for future ASAN)
    uint8_t code[PAGE_SIZE - 12]; // Raw x86 machine code
} bf_bytecode_msg_t;

/*
 * Execution result structure
 * Sent from executor to REPL via IPC
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              // BF_MAGIC_RESULT
    int32_t status;              // 0 = success, negative = error code
    uint32_t output_len;         // Length of captured output
    char output[PAGE_SIZE - 12]; // Execution output (ASCII/HEX/DEC per flags)
} bf_result_msg_t;

/*
 * Error codes for status field
 */
enum {
    BF_SUCCESS = 0,
    BF_ERR_SYNTAX = -1,     // Invalid BF syntax (unmatched brackets)
    BF_ERR_OVERFLOW = -2,   // Code size exceeds page limit
    BF_ERR_EXECUTION = -3,  // Runtime error during execution
    BF_ERR_INVALID_MSG = -4 // Invalid IPC message format
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
 *   loop_stack - Stack tracking positions of '[' for backpatching
 *   loop_depth - Current depth of nested loops (for bracket validation)
 *   executor_id - Environment ID of executor process (0 in test mode)
 *   optimize_time - Flag enabling time optimizations (-Otime)
 *   test_mode - Flag indicating self-test mode (-t)
 */
typedef struct {
    const char *source;
    size_t src_len;
    
    uint8_t *code_buf;
    size_t code_offset;
    
    int loop_stack[256];
    int loop_depth;
    
    envid_t repl_id;
    bool optimize_time;
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
} bf_repl_ctx_t;

/*
 * Execution context structure
 * Manages state during program execution
 *
 * Fields:
 *   tape - Pointer to BF memory tape (30KB at BF_TAPE_ADDR)
 *   output_buf - Buffer for capturing program output
 *   output_pos - Current write position in output buffer
 *   output_format - Output format (BF_OUTPUT_ASCII/HEX/DEC)
 *   code_entry - Entry point of compiled code after W->X transition
 *   test_mode - Flag for self-test execution (bypasses IPC)
 *   single_exec - Flag for single execution mode (-e option)
 */
typedef struct {
    uint8_t *tape;
    char output_buf[PAGE_SIZE];
    size_t output_pos;

    int output_format;
    void (*code_entry)(uint8_t *); // Function pointer type for generated code

    bool single_exec;
} bf_executor_ctx_t;