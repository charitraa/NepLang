#ifndef NEPLANG_RT_H
#define NEPLANG_RT_H

/* Runtime support shared by all three backends: the tree-walking interpreter, the bytecode VM and
 * native programs compiled through C. Every operation reports runtime errors with the same wording
 * whichever backend runs the program. Ownership: functions that take Values consume (release) them
 * unless the comment says "borrows". */

#include <setjmp.h>
#include "ast.h"
#include "token.h"
#include "value.h"

/* Reports a runtime error and exits with status 1 -- or, when rt_recover is set (the REPL), jumps
 * back to it instead so the session can continue. */
_Noreturn void rt_error(Token at, const char *message);
extern jmp_buf *rt_recover;

/* The program's own command-line arguments (os.args()). */
void rt_set_args(int argc, char **argv);

extern int rt_call_depth;   /* nesting of compiled function calls (native programs) */
_Noreturn void rt_too_deep(const char *name, int length, Token at);

Value rt_unset(void);
int rt_is_unset(Value v);
_Noreturn void rt_undefined_variable(const char *name, int length, Token at);

Value rt_binop(TokenType op, Value left, Value right, Token at);
Value rt_neg(Value v, Token at);   /* unary '-' */
Value rt_pos(Value v, Token at);   /* unary '+' */
Value rt_not(Value v, Token at);   /* hoina */
Value rt_truth(Value v, Token at); /* a condition value -> sacho / jutho (used by ra / wa) */
int rt_condition(Value v, Token at);       /* consumes; a bool or a number (non-zero is true) */
long rt_number(Value v, const char *what, Token at);  /* borrows; "<what> must be a whole number" */

Value rt_new_array(Value *items, int count);  /* takes ownership of the items */
Value rt_index(Value target, Value index, Token at);
void rt_set_index(Value target, Value index, Value value, Token at);
Value rt_length_of(Value v);                     /* borrows; live length of an array or string */
void rt_check_iterable(Value v, Token at);       /* borrows */
void rt_print(Value v);                          /* borrows; prints value and a newline */

/* ---- types ---- */

enum { RT_CTX_VARIABLE, RT_CTX_PARAM, RT_CTX_RETURN, RT_CTX_FIELD };
/* Checks `v` against a written type and returns it (an int given to a float becomes a float).
 * `name` is the variable/parameter/field for the message; `at` is where the error points. */
Value rt_coerce(Value v, const TypeRef *type, int context, Token name, Token at);
void rt_type_text(const TypeRef *type, char *buffer, int size);   /* "int", "string?", "Manchhe" */
/* Is `klass` the class (or interface) called `name`, or does it inherit / implement it? */
int rt_is_a(const AstNode *klass, const char *name, int length);

/* ---- objects ----
 * `caller` is the class whose method is running (NULL outside methods): members named '_x' are
 * private to it and the classes it is related to by inheritance. `at` is where errors point. */
Value rt_get_field(Value object, const char *name, int length, const AstNode *caller, Token at);   /* consumes object */
void rt_set_field(Value object, const char *name, int length, Value value, const AstNode *caller, Token at);  /* consumes both */
void rt_init_field(Value object, int index, Value value, Token at);   /* borrows object; field defaults */
/* Finds method `name` of the object `receiver` (borrowed) for a call with `argc` arguments, with all
 * the interpreter's checks. Returns its index in the class's method list and sets *klass. */
int rt_find_method(Value receiver, const char *name, int length, int argc, const AstNode *caller, Token at,
                   const AstNode **klass);

/* ---- built-in functions ----
 * Global ones (lambai, thapa, ...), standard-library modules (math.sqrt, string.upper, ...), and the
 * methods of arrays and strings (a.push(x) is array.push(a, x)) share one table. */
enum { RT_LAMBAI, RT_THAPA, RT_HATA, RT_SANKHYA, RT_PADHA };   /* the first ids, for existing callers */
int rt_builtin_id(const char *name, int length);  /* a global built-in; -1 if none */
int rt_module_builtin_id(const char *module, int module_length, const char *name, int length);  /* -1 if none */
int rt_is_module(const char *name, int length);   /* a standard-library module such as "math" */
int rt_builtin_min_args(int id);
int rt_builtin_max_args(int id);
const char *rt_builtin_name(int id);              /* "lambai", "math.sqrt" */
Value rt_builtin(int id, Value *args, int argc, Token at);  /* checks the count; consumes the args */
/* The builtin behind 'receiver.name(...)' for an array or string receiver; -1 if none.
 * `kind` is VAL_ARRAY or VAL_STRING. */
int rt_value_method_id(ValueType kind, const char *name, int length);
/* Calls a method on an array or string: args[0] is the receiver, then argc - 1 arguments. Consumes them. */
Value rt_value_method(Value *args, int argc, const char *name, int length, Token at);

#endif
