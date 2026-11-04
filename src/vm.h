#ifndef NEPLANG_VM_H
#define NEPLANG_VM_H

#include "ir.h"

/* Runs a finalized IR program on the stack-based virtual machine. */
void vm_run(const IrProgram *program);

#endif
