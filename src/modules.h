#ifndef NEPLANG_MODULES_H
#define NEPLANG_MODULES_H

#include "ast.h"

/* Replaces every top-level 'import' in `program` with the statements of the imported file
 * (recursively, each file at most once). Imports are textual and flat: an imported file's
 * functions, classes and variables join the importer's namespace. Returns 0 on success, or
 * non-zero after reporting an error (missing file, parse error, ...).
 * `main_path` is the importing program's path; imports are looked up next to it first, then
 * in each directory of the NEPLANG_PATH environment variable. Source buffers of loaded
 * modules stay alive until modules_free(), because tokens point into them. */
int modules_resolve(AstNode *program, const char *main_path);
void modules_free(void);

#endif
