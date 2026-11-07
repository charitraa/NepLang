#ifndef NEPLANG_INTERPRETER_H
#define NEPLANG_INTERPRETER_H

#include "ast.h"

typedef struct Interpreter Interpreter;

/* Runs a whole program (after parsing, imports, class resolution and checking). */
void interpret(AstNode *program);

/* For the REPL: one interpreter whose variables, functions and classes persist from one input to
 * the next. The AST of every input must stay alive as long as the interpreter. With `repl` set, a
 * function may be redefined by a later input. */
Interpreter *interpreter_new(int repl);
void interpreter_run(Interpreter *interp, AstNode *program);
void interpreter_recover(Interpreter *interp);   /* after a runtime error (rt_recover) */
void interpreter_free(Interpreter *interp);

#endif
