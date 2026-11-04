#ifndef NEPLANG_PARSER_H
#define NEPLANG_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lexer;
    const char *source;
    const char *filename;
    Token current;
    int had_error;
    int block_depth;   /* > 0 while inside any '{ }' block */
    int loop_depth;    /* > 0 while inside a loop body ("ruk"/"agadi" are allowed) */
    int in_function;   /* 1 while parsing a 'kaam' body */
    int in_method;     /* 1 while parsing a method of a 'class' ('yo' is allowed) */
    int in_constructor; /* 1 while parsing a class's 'banau' method */
} Parser;

void parser_init(Parser *parser, Lexer *lexer, const char *source, const char *filename);
AstNode *parser_parse_program(Parser *parser);

#endif
