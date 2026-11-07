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
 * globals, but it still has hidden slots. Every method is an ordinary function whose slot 0 is the
 * object ('yo') and whose parameters follow. */

typedef enum {
    IR_CONST_INT,     /* push a */
    IR_CONST_FLOAT,   /* push f */
    IR_CONST_BOOL,    /* push sacho (a = 1) or jutho (a = 0) */
    IR_CONST_NULL,    /* push khali */
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
    IR_TRUTH,         /* condition value -> sacho/jutho */
    IR_JUMP,          /* a = label */
    IR_JUMP_IF_FALSE, /* pop a condition; jump to label a when it is false */
    IR_LABEL,         /* a = label id */
    IR_NEW_ARRAY,     /* pop a items, push an array */
    IR_INDEX,
    IR_SET_INDEX,     /* pop value, index, target */
    IR_LEN,           /* pop an array/string, push its length */
    IR_CHECK_ITER,    /* error unless the top of stack is an array or string */
    IR_CHECK_NUM,     /* error unless the top of stack is a number; a = IR_WHAT_* */
    IR_CALL,          /* a = function index, b = argument count */
    IR_BUILTIN,       /* a = builtin id (rt.h), b = argument count; also standard-library module functions */
    IR_RETURN,        /* pop the return value */
    IR_PRINT,
    IR_NEW_OBJECT,    /* push a new object of class a, every field 0 */
    IR_INIT_FIELD,    /* pop a value into field a of the object now on top of the stack */
    IR_DUP,           /* push another reference to the top of the stack */
    IR_GET_FIELD,     /* pop object, push its field named by the token; a = class of the running method or -1 */
    IR_SET_FIELD,     /* pop value, then object; assign the field named by the token; a = as for GET_FIELD */
    IR_CALL_METHOD,   /* stack: object, b args. Method named by the token; a = as for GET_FIELD.
                       * On an array or string it calls that value's built-in method ('a.push(1)'). */
    IR_COERCE         /* check the top of stack against type a of the program's type table (int -> float
                       * converts); b = RT_CTX_*; the token is the variable/parameter/function name */
} IrOp;

enum { IR_WHAT_CONDITION, IR_WHAT_INDEX, IR_WHAT_RANGE_START, IR_WHAT_RANGE_END };
extern const char *const IR_WHAT[];

typedef struct {
    IrOp op;
    long a;
    long b;
    double f;      /* IR_CONST_FLOAT */
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
    const AstNode *decl;   /* the AST_CLASS or AST_INTERFACE node (outlives the program) */
    int *method_fn;        /* method position in decl -> function index (-1 for an interface's methods) */
} IrClass;

typedef struct {
    IrFunction *functions;   /* [0] is the top-level program */
    int function_count;
    char **strings;
    int *string_lengths;
    int string_count;
    Token *globals;          /* global variable names; the index is the global id */
    int global_count;
    IrClass *classes;
    int class_count;
    const TypeRef **types;   /* written types used by IR_COERCE (they point into the AST) */
    int type_count;
} IrProgram;

/* Returns NULL (after reporting an error) when the program uses something the compiled
 * backends don't support. */
IrProgram *ir_compile(AstNode *program, const char *source, const char *filename);
void ir_optimize(IrProgram *program);
void ir_finalize(IrProgram *program);   /* label positions, max stack depth; call after optimizing */
void ir_dump(const IrProgram *program, FILE *out);
void ir_free(IrProgram *program);

#endif
