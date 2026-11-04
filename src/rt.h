#ifndef NEPLANG_RT_H
#define NEPLANG_RT_H

/* Runtime support shared by the bytecode VM and by native programs compiled through C.
 * Every operation reports a runtime error with the same wording as the tree-walking
 * interpreter. Ownership: functions that take Values consume (release) them unless the
 * comment says "borrows". */

#include "token.h"
#include "value.h"

_Noreturn void rt_error(Token at, const char *message);

extern int rt_call_depth;   /* nesting of compiled function calls (native programs) */
_Noreturn void rt_too_deep(const char *name, int length, Token at);

Value rt_unset(void);
int rt_is_unset(Value v);
_Noreturn void rt_undefined_variable(const char *name, int length, Token at);

Value rt_binop(TokenType op, Value left, Value right, Token at);
Value rt_neg(Value v, Token at);   /* unary '-' */
Value rt_pos(Value v, Token at);   /* unary '+' */
Value rt_not(Value v, Token at);   /* hoina */
Value rt_truth(Value v, Token at); /* a number -> 0 or 1 (used by ra / wa) */
int rt_condition(Value v, Token at);       /* consumes; "A condition must be a number" */
long rt_number(Value v, const char *what, Token at);  /* consumes; "<what> must be a number" */

Value rt_new_array(Value *items, int count);  /* takes ownership of the items */
Value rt_index(Value target, Value index, Token at);
void rt_set_index(Value target, Value index, Value value, Token at);
Value rt_length_of(Value v);                     /* borrows; live length of an array or string */
void rt_check_iterable(Value v, Token at);       /* borrows */
void rt_print(Value v);                          /* borrows; prints value and a newline */

enum { RT_LAMBAI, RT_THAPA, RT_HATA, RT_SANKHYA, RT_PADHA, RT_BUILTIN_COUNT };
int rt_builtin_id(const char *name, int length);  /* -1 if not a builtin */
int rt_builtin_arity(int id);
Value rt_builtin(int id, Value *args, Token at);  /* consumes the args */

#endif
