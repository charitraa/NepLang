#ifndef NEPLANG_CLASSES_H
#define NEPLANG_CLASSES_H

#include "ast.h"

/* Resolves inheritance for classes and interfaces, after parsing and import resolution and before
 * the checker (see the class_decl comment in ast.h for the resulting layout):
 *  - 'class Kukur(Janawar, Bolne)': at most one base may be a class (the parent), the rest must be
 *    interfaces; no cycles; a class can't redeclare an inherited field.
 *  - each class gets the parent's fields first (so inherited methods find them at the same index),
 *    then its own; its own methods, then the parent's methods it doesn't override.
 *  - an override keeps the parent method's number of parameters (except 'banau').
 *  - a class must define every method of every interface it implements (directly or by inheriting).
 * `decls` holds every AST_CLASS / AST_INTERFACE that is visible; only decls[first..count) are new
 * and get resolved (the REPL resolves each new line against the earlier ones). Returns the number
 * of errors reported. */
int classes_resolve(AstNode **decls, int count, int first);

/* Collects the class and interface declarations of a program and resolves them all. */
int classes_resolve_program(AstNode *program);

#endif
