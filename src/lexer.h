#ifndef NEPLANG_LEXER_H
#define NEPLANG_LEXER_H

#include "token.h"

typedef struct {
    const char *source;
    const char *current;
    int line;
    int col;
    const char *end;  /* if non-NULL, scanning stops here (used for "${...}" pieces) */
    const char *error_message;  /* set with some TOKEN_ERROR tokens (e.g. a bad string); else NULL */
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next_token(Lexer *lexer);

#endif
