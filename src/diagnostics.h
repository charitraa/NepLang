#ifndef NEPLANG_DIAGNOSTICS_H
#define NEPLANG_DIAGNOSTICS_H

#include "token.h"

/* Tells diagnostics which file a source buffer belongs to. report_error() looks the token up
 * in this registry, so an error inside an imported module shows that module's own file name
 * and source line instead of the main program's. The buffer must outlive any later error. */
void diagnostics_register_source(const char *source, const char *filename);
void diagnostics_clear_sources(void);

void report_error(const char *source, const char *filename, Token token, const char *message);

#endif
