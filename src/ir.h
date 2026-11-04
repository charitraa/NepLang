#ifndef NEPLANG_IR_H
#define NEPLANG_IR_H

#include <stdio.h>
#include "ast.h"

/* The intermediate representation: a linear, stack-based instruction list per function with
 * symbolic labels. It is the single middle layer of the compiler:
 *
 *     AST --ir_compile--> IR --ir_optimize--> IR --+--> bytecode VM  (vm.c, executes it directly)
 *                                                  +--> C source     (cgen.c, then the system C compiler)
 *
 * Every function has numbered local slots (parameters first, then named locals, then hidden
 * slots for loop counters). Function 0 is the top-level program: its named variables are
 * globals, but it still has hidden slots. Classes are not supported by the compiled backends. */

typedef enum {
    IR_CONST_INT,     /* push a */
    IR_CONST_STR,     /* push string a of the program's string pool */
    IR_POP,
    IR_LOAD_LOCAL,    /* push local slot a (hidden slots, parameters) */
    IR_STORE_LOCAL,   /* pop into local slot a */
    IR_LOAD_GLOBAL,
    IR_STORE_GLOBAL,
    IR_LOAD_NAME,     /* variable read: local slot a if set, else global b; error if neither */
    IR_BINOP,         /* a = TokenType */
    IR_NEG,
    IR_POS,
    IR_NOT,           /* hoina */
    IR_TRUTH,         /* number -> 0/1 */
    IR_JUMP,          /* a = label */
    IR_JUMP_IF_FALSE, /* pop a number; jump to label a when it is 0 */
    IR_LABEL,         /* a = label id */
    IR_NEW_ARRAY,     /* pop a items, push an array */
    IR_INDEX,
    IR_SET_INDEX,     /* pop value, index, target */
    IR_LEN,           /* pop an array/string, push its length */
    IR_CHECK_ITER,    /* error unless the top of stack is an array or string */
    IR_CHECK_NUM,     /* error unless the top of stack is a number; a = IR_WHAT_* */
    IR_CALL,          /* a = function index, b = argument count */
    IR_BUILTIN,       /* a = builtin id (rt.h), b = argument count */
    IR_RETURN,        /* pop the return value */
    IR_PRINT
} IrOp;

enum { IR_WHAT_CONDITION, IR_WHAT_INDEX, IR_WHAT_RANGE_START, IR_WHAT_RANGE_END };
extern const char *const IR_WHAT[];

typedef struct {
    IrOp op;
    long a;
    long b;
    Token token;   /* source location for runtime errors (and the variable name for LOAD_NAME) */
    int depth;     /* operand-stack depth just before this instruction runs */
} IrInstr;

typedef struct {
    Token name;
    int param_count;
    int local_count;
    IrInstr *code;
    int count;
    int capacity;
    int *label_pos;   /* label id -> index of its IR_LABEL instruction (filled by ir_finalize) */
    int label_count;
    int max_depth;
} IrFunction;

typedef struct {
    IrFunction *functions;   /* [0] is the top-level program */
    int function_count;
    char **strings;
    int *string_lengths;
    int string_count;
    Token *globals;          /* global variable names; the index is the global id */
    int global_count;
} IrProgram;

/* Returns NULL (after reporting an error) when the program uses something the compiled
 * backends don't support, such as classes. */
IrProgram *ir_compile(AstNode *program, const char *source, const char *filename);
void ir_optimize(IrProgram *program);
void ir_finalize(IrProgram *program);   /* label positions, max stack depth; call after optimizing */
void ir_dump(const IrProgram *program, FILE *out);
void ir_free(IrProgram *program);

#endif
