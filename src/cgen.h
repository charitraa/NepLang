#ifndef NEPLANG_CGEN_H
#define NEPLANG_CGEN_H

#include <stdio.h>
#include "ir.h"

/* Native backend: translates a finalized IR program into a C source file that uses the shared
 * runtime (rt.c, value.c). Each IR function becomes a C function; the operand stack becomes
 * C variables s0, s1, ... (stack depths are known statically); IR labels become C labels. */
void cgen_emit(const IrProgram *program, FILE *out);

/* Writes C to a temporary file, then runs the system C compiler (the CC environment variable,
 * or "cc") to produce the executable `output_path`. Returns 0 on success. */
int cgen_build(const IrProgram *program, const char *output_path, int keep_c);

#endif
