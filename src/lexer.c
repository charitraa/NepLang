#include <ctype.h>
#include <string.h>
#include "lexer.h"

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->col = 1;
    lexer->error_message = NULL;
    lexer->end = NULL;
}

static char peek(Lexer *lexer) {
    if (lexer->end && lexer->current >= lexer->end) return '\0';
    return *lexer->current;
}

static char advance(Lexer *lexer) {
    char c = *lexer->current;
    lexer->current++;
    if (c == '\n') {
        lexer->line++;
        lexer->col = 1;
    } else if (((unsigned char)c & 0xC0) != 0x80) {
        lexer->col++;  /* columns count characters: UTF-8 continuation bytes don't advance them */
    }
    return c;
}

static void skip_whitespace_and_comments(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '#') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
        } else {
            return;
        }
    }
}

static Token make_token(TokenType type, const char *start, int length, int line, int col) {
    Token token;
    token.type = type;
    token.start = start;
    token.length = length;
    token.value = 0;
    token.line = line;
    token.col = col;
    return token;
}

static Token number_token(Lexer *lexer) {
    const char *start = lexer->current;
    int line = lexer->line;
    int col = lexer->col;

    while (isdigit((unsigned char)peek(lexer))) {
        advance(lexer);
    }

    int length = (int)(lexer->current - start);
    Token token = make_token(TOKEN_INT, start, length, line, col);

    long value = 0;
    for (int i = 0; i < length; i++) {
        value = value * 10 + (start[i] - '0');
    }
    token.value = value;
    return token;
}

static Token error_token(Lexer *lexer, const char *start, int length, int line, int col, const char *message) {
    lexer->error_message = message;
    return make_token(TOKEN_ERROR, start, length, line, col);
}

/* Number of bytes in the UTF-8 sequence starting with `lead`, or 0 if `lead` can't start one. */
static int utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

/* "..." on a single line. Only the escapes \n \t \" \\ are allowed, and any non-ASCII
 * text must be valid UTF-8, so the interpreter can count characters safely. */
static Token string_token(Lexer *lexer) {
    const char *start = lexer->current;
    int line = lexer->line;
    int col = lexer->col;
    advance(lexer);  /* opening quote */

    for (;;) {
        char c = peek(lexer);
        if (c == '\0' || c == '\n') {
            return error_token(lexer, start, 1, line, col,
                               "This string is never closed. Add a closing '\"' on the same line.");
        }
        if (c == '"') break;

        const char *at = lexer->current;
        int at_col = lexer->col;
        if (c == '\\') {
            advance(lexer);
            char escape = peek(lexer);
            if (escape != 'n' && escape != 't' && escape != '"' && escape != '\\' && escape != '$') {
                int length = (escape == '\0' || escape == '\n') ? 1 : 2;
                return error_token(lexer, at, length, line, at_col,
                                   "Unknown escape. Inside a string, '\\' can only be followed by n (new line), "
                                   "t (tab), '\"', '\\' or '$'.");
            }
            advance(lexer);
            continue;
        }

        int length = utf8_sequence_length((unsigned char)c);
        for (int i = 1; i < length; i++) {
            if (((unsigned char)at[i] & 0xC0) != 0x80) {
                length = 0;
                break;
            }
        }
        if (length == 0) {
            return error_token(lexer, at, 1, line, at_col, "This string contains bytes that are not valid UTF-8 text.");
        }
        for (int i = 0; i < length; i++) {
            advance(lexer);
        }
    }

    advance(lexer);  /* closing quote */
    return make_token(TOKEN_STRING, start, (int)(lexer->current - start), line, col);
}

static int match(Lexer *lexer, char expected) {
    if (peek(lexer) != expected) return 0;
    advance(lexer);
    return 1;
}

static const struct {
    const char *text;
    TokenType type;
} keywords[] = {
    { "vana",      TOKEN_VANA },
    { "yedi",      TOKEN_YEDI },
    { "natara",    TOKEN_NATARA },
    { "jabasamma", TOKEN_JABASAMMA },
    { "ko_lagi",   TOKEN_KO_LAGI },
    { "in",        TOKEN_IN },
    { "kaam",      TOKEN_KAAM },
    { "farkau",    TOKEN_FARKAU },
    { "class",     TOKEN_CLASS },
    { "yo",        TOKEN_YO },
    { "ra",        TOKEN_RA },
    { "wa",        TOKEN_WA },
    { "hoina",     TOKEN_HOINA },
    { "ruk",       TOKEN_RUK },
    { "agadi",     TOKEN_AGADI },
    { "import",    TOKEN_IMPORT },
};

static TokenType keyword_type(const char *start, int length) {
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strlen(keywords[i].text) == (size_t)length && strncmp(start, keywords[i].text, (size_t)length) == 0) {
            return keywords[i].type;
        }
    }
    return TOKEN_IDENT;
}

static Token ident_token(Lexer *lexer) {
    const char *start = lexer->current;
    int line = lexer->line;
    int col = lexer->col;

    while (isalnum((unsigned char)peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    int length = (int)(lexer->current - start);
    /* sacho / jutho (true / false) are literals for 1 and 0: there is no separate bool type yet. */
    if (length == 5 && strncmp(start, "sacho", 5) == 0) {
        Token token = make_token(TOKEN_INT, start, length, line, col);
        token.value = 1;
        return token;
    }
    if (length == 5 && strncmp(start, "jutho", 5) == 0) {
        return make_token(TOKEN_INT, start, length, line, col);
    }
    return make_token(keyword_type(start, length), start, length, line, col);
}

Token lexer_next_token(Lexer *lexer) {
    lexer->error_message = NULL;
    skip_whitespace_and_comments(lexer);

    const char *start = lexer->current;
    int line = lexer->line;
    int col = lexer->col;

    char c = peek(lexer);

    if (c == '\0') {
        return make_token(TOKEN_EOF, start, 0, line, col);
    }

    if (isdigit((unsigned char)c)) {
        return number_token(lexer);
    }

    if (isalpha((unsigned char)c) || c == '_') {
        return ident_token(lexer);
    }

    if (c == '"') {
        return string_token(lexer);
    }

    /* A non-ASCII character (e.g. Devanagari): swallow the whole UTF-8 sequence so the
     * error token covers the full character rather than a single byte. */
    if ((unsigned char)c >= 0x80) {
        advance(lexer);
        while (((unsigned char)peek(lexer) & 0xC0) == 0x80) {
            advance(lexer);
        }
        return make_token(TOKEN_ERROR, start, (int)(lexer->current - start), line, col);
    }

    advance(lexer);
    switch (c) {
        case '+': return make_token(TOKEN_PLUS, start, 1, line, col);
        case '-': return make_token(TOKEN_MINUS, start, 1, line, col);
        case '*': return make_token(TOKEN_STAR, start, 1, line, col);
        case '/': return make_token(TOKEN_SLASH, start, 1, line, col);
        case '%': return make_token(TOKEN_PERCENT, start, 1, line, col);
        case '(': return make_token(TOKEN_LPAREN, start, 1, line, col);
        case ')': return make_token(TOKEN_RPAREN, start, 1, line, col);
        case '.':
            if (match(lexer, '.')) return make_token(TOKEN_DOT_DOT, start, 2, line, col);
            return make_token(TOKEN_DOT, start, 1, line, col);
        case '[': return make_token(TOKEN_LBRACKET, start, 1, line, col);
        case ']': return make_token(TOKEN_RBRACKET, start, 1, line, col);
        case ',': return make_token(TOKEN_COMMA, start, 1, line, col);
        case '{': return make_token(TOKEN_LBRACE, start, 1, line, col);
        case '}': return make_token(TOKEN_RBRACE, start, 1, line, col);
        case '=':
            if (match(lexer, '=')) return make_token(TOKEN_EQUAL_EQUAL, start, 2, line, col);
            return make_token(TOKEN_EQUAL, start, 1, line, col);
        case '!':
            if (match(lexer, '=')) return make_token(TOKEN_BANG_EQUAL, start, 2, line, col);
            return make_token(TOKEN_ERROR, start, 1, line, col);
        case '<':
            if (match(lexer, '=')) return make_token(TOKEN_LESS_EQUAL, start, 2, line, col);
            return make_token(TOKEN_LESS, start, 1, line, col);
        case '>':
            if (match(lexer, '=')) return make_token(TOKEN_GREATER_EQUAL, start, 2, line, col);
            return make_token(TOKEN_GREATER, start, 1, line, col);
        case '\n': return make_token(TOKEN_NEWLINE, start, 1, line, col);
        default:  return make_token(TOKEN_ERROR, start, 1, line, col);
    }
}
