#ifndef NEPLANG_TOKEN_H
#define NEPLANG_TOKEN_H

typedef enum {
    TOKEN_INT,
    TOKEN_FLOAT,    /* 3.14 -- the parser converts the text with strtod */
    TOKEN_BOOL,     /* sacho / jutho; value is 1 / 0 */
    TOKEN_KHALI,    /* khali (null) */
    TOKEN_STRING,   /* the slice includes both quotes; escapes are decoded by the parser */
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_DOT,
    TOKEN_DOT_DOT,
    TOKEN_COMMA,
    TOKEN_QUESTION,      /* '?' after a type: it may also be khali */
    TOKEN_PLUS_EQUAL,    /* += */
    TOKEN_MINUS_EQUAL,   /* -= */
    TOKEN_STAR_EQUAL,    /* *= */
    TOKEN_SLASH_EQUAL,   /* /= */
    TOKEN_PERCENT_EQUAL, /* %= */
    TOKEN_NEWLINE,
    TOKEN_IDENT,
    TOKEN_VANA,
    TOKEN_YEDI,
    TOKEN_NATARA,
    TOKEN_JABASAMMA,
    TOKEN_KO_LAGI,
    TOKEN_IN,
    TOKEN_KAAM,
    TOKEN_FARKAU,
    TOKEN_CLASS,
    TOKEN_YO,
    TOKEN_RA,       /* and */
    TOKEN_WA,       /* or */
    TOKEN_HOINA,    /* not */
    TOKEN_RUK,      /* break */
    TOKEN_AGADI,    /* continue */
    TOKEN_IMPORT,
    TOKEN_INTERFACE,
    TOKEN_SUPER,
    TOKEN_CONST,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int length;
    long value;
    int line;
    int col;
} Token;

#endif
