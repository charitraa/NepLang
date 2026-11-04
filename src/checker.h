#ifndef NEPLANG_CHECKER_H
#define NEPLANG_CHECKER_H

#include "ast.h"

/* Static checking pass that runs between parsing and execution. Reports every problem it
 * can prove without running the program (undefined names, wrong argument counts, operations
 * on values whose types are known to be incompatible) and returns how many it found. */
int check_program(AstNode *program, const char *source, const char *filename);

#endif
