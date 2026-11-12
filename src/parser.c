#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "diagnostics.h"

static AstNode *parse_comparison(Parser *parser);
static AstNode *parse_statement(Parser *parser);
static void skip_newlines(Parser *parser);
static int reject_keyword_as_name(Parser *parser, Token keyword);

static void error_at(Parser *parser, Token token, const char *message);

/* Lexer errors are reported as soon as the bad character is scanned, so the message
 * names the character instead of whatever the parser expected in its place. */
static void advance(Parser *parser) {
    parser->current = lexer_next_token(parser->lexer);

    Token token = parser->current;
    if (token.type == TOKEN_ERROR) {
        char message[256];
        if (parser->lexer->error_message) {
            snprintf(message, sizeof(message), "%s", parser->lexer->error_message);
        } else if ((unsigned char)token.start[0] >= 0x80) {
            snprintf(message, sizeof(message),
                     "Unexpected character '%.*s'. Names can only use English letters, digits, and '_'.",
                     token.length, token.start);
        } else {
            snprintf(message, sizeof(message), "Unexpected character '%.*s'.", token.length, token.start);
        }
        error_at(parser, token, message);
    }
}

static int check(Parser *parser, TokenType type) {
    return parser->current.type == type;
}

static int match(Parser *parser, TokenType type) {
    if (!check(parser, type)) return 0;
    advance(parser);
    return 1;
}

static void error_at(Parser *parser, Token token, const char *message) {
    if (parser->had_error) return;
    parser->had_error = 1;
    report_error(parser->source, parser->filename, token, message);
}

static void error_at_current(Parser *parser, const char *message) {
    error_at(parser, parser->current, message);
}

static Token expect(Parser *parser, TokenType type, const char *message) {
    Token token = parser->current;
    if (!check(parser, type)) {
        error_at_current(parser, message);
        return token;
    }
    advance(parser);
    return token;
}

static int same_name(Token a, Token b) {
    return a.length == b.length && strncmp(a.start, b.start, (size_t)a.length) == 0;
}

static int is_text(Token t, const char *text) {
    return t.length == (int)strlen(text) && strncmp(t.start, text, (size_t)t.length) == 0;
}

/* The token after the current one, without consuming anything (scans a copy of the lexer). */
static Token peek_token(Parser *parser) {
    Lexer copy = *parser->lexer;
    return lexer_next_token(&copy);
}

/* Does the current token start a written type followed by a name, as in 'int umar' or 'string? x'? */
static int at_typed_name(Parser *parser) {
    if (!check(parser, TOKEN_IDENT)) return 0;
    TokenType next = peek_token(parser).type;
    return next == TOKEN_IDENT || next == TOKEN_QUESTION;
}

static TypeRef no_type(void) {
    TypeRef type;
    memset(&type, 0, sizeof(type));
    type.kind = TYPE_NONE;
    return type;
}

/* TYPE := NAME ['?'], where NAME is int, float, bool, string, array, object, any, void or a class. */
static TypeRef parse_type(Parser *parser) {
    TypeRef type = no_type();
    Token name = expect(parser, TOKEN_IDENT, "Expected a type here, e.g. 'int', 'string' or a class name.");
    if (parser->had_error) return type;
    type.name = name;
    if (is_text(name, "int")) type.kind = TYPE_INT;
    else if (is_text(name, "float")) type.kind = TYPE_FLOAT;
    else if (is_text(name, "bool")) type.kind = TYPE_BOOL;
    else if (is_text(name, "string")) type.kind = TYPE_STRING;
    else if (is_text(name, "array")) type.kind = TYPE_ARRAY;
    else if (is_text(name, "object")) type.kind = TYPE_OBJECT;
    else if (is_text(name, "any")) type.kind = TYPE_ANY;
    else if (is_text(name, "void")) type.kind = TYPE_VOID;
    else type.kind = TYPE_CLASS;
    if (check(parser, TOKEN_QUESTION)) {
        if (type.kind == TYPE_VOID || type.kind == TYPE_ANY) {
            error_at_current(parser, "'?' (may be khali) can't be used with 'void' or 'any'.");
            return type;
        }
        type.nullable = 1;
        advance(parser);
    }
    return type;
}

/* Called with the function name consumed and '(' as the current token. */
static AstNode *parse_call(Parser *parser, Token name) {
    AstNode *call = ast_new_call(name);
    advance(parser);
    skip_newlines(parser);

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            skip_newlines(parser);
            ast_call_add_arg(call, parse_comparison(parser));
            skip_newlines(parser);
        } while (!parser->had_error && match(parser, TOKEN_COMMA));
    }

    expect(parser, TOKEN_RPAREN, "Expected ',' or ')' in the function call's argument list.");
    return call;
}

/* [a, b, c] -- newlines are allowed between items so long arrays can span lines. */
static AstNode *parse_array(Parser *parser) {
    Token open = parser->current;
    AstNode *array = ast_new_array(open);
    advance(parser);
    skip_newlines(parser);

    if (!check(parser, TOKEN_RBRACKET)) {
        do {
            skip_newlines(parser);
            ast_array_add_item(array, parse_comparison(parser));
            skip_newlines(parser);
        } while (!parser->had_error && match(parser, TOKEN_COMMA));
    }

    if (!parser->had_error && !check(parser, TOKEN_RBRACKET) && parser->current.line != open.line) {
        error_at(parser, open, "This '[' is never closed. Add a matching ']' to end the array.");
    }
    expect(parser, TOKEN_RBRACKET, "Expected ',' or ']' in the array.");
    return array;
}

/* Any number of trailing '[index]', '.field' and '.method(args)' after a value:
 * a[i], m[i][j], f(x)[0], p.naam, p.bolau(), p.saathi[0].naam. */
static AstNode *parse_postfix(Parser *parser, AstNode *node) {
    while (!parser->had_error) {
        if (check(parser, TOKEN_LBRACKET)) {
            Token open = parser->current;
            advance(parser);
            AstNode *index = parse_comparison(parser);
            expect(parser, TOKEN_RBRACKET, "Expected ']' to close the index.");
            node = ast_new_index(open, node, index);
        } else if (check(parser, TOKEN_DOT)) {
            advance(parser);
            Token name = expect(parser, TOKEN_IDENT, "Expected a field or method name after '.', e.g. 'p.naam'.");
            if (parser->had_error) break;
            if (check(parser, TOKEN_LPAREN)) {
                AstNode *call = parse_call(parser, name);
                call->as.call.receiver = node;
                node = call;
            } else {
                node = ast_new_field(node, name);
            }
        } else {
            break;
        }
    }
    return node;
}

static void *checked_malloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    return p;
}

/* Parses the source text of one "${...}" piece with a sub-parser that shares the main
 * source buffer, so tokens (and error locations) point into the real file. */
static AstNode *parse_interpolation(Parser *parser, Token literal, const char *from, const char *to) {
    Lexer lexer;
    lexer_init(&lexer, parser->source);
    lexer.current = from;
    lexer.end = to;
    lexer.line = literal.line;
    lexer.col = literal.col + (int)(from - literal.start);

    Parser sub = *parser;
    sub.lexer = &lexer;
    sub.had_error = 0;
    advance(&sub);
    AstNode *expr;
    if (sub.had_error) {
        expr = ast_new_placeholder();
    } else if (check(&sub, TOKEN_EOF)) {
        error_at(&sub, literal, "Nothing inside '${ }'. Put an expression there, e.g. \"${naam}\".");
        expr = ast_new_placeholder();
    } else {
        expr = parse_comparison(&sub);
        if (!sub.had_error && !check(&sub, TOKEN_EOF)) {
            error_at_current(&sub, "Unexpected text inside '${ }'. It must hold a single expression.");
        }
    }
    if (sub.had_error) parser->had_error = 1;
    return expr;
}

/* Wraps an interpolated expression in a call to the builtin string(), so "${x}" turns any
 * value (array, bool, khali, object) into text the same way vana prints it -- '+' alone only
 * converts numbers. Builtins are resolved before methods of 'yo', so this can't be shadowed.
 * The name token's text is a static "string" (it is only compared by name, never printed). */
static AstNode *interpolation_to_string(Token literal, AstNode *expr) {
    Token name = literal;
    name.type = TOKEN_IDENT;
    name.start = "string";
    name.length = 6;
    AstNode *call = ast_new_call(name);
    ast_call_add_arg(call, expr);
    return call;
}

/* Decodes the escapes in a string literal token. The lexer has already checked them.
 * "${expr}" pieces are turned into '"text" + string(expr) + "text"' (see
 * interpolation_to_string). */
static AstNode *parse_string(Parser *parser) {
    Token literal = parser->current;
    advance(parser);

    const char *text = literal.start + 1;       /* skip the quotes */
    int text_length = literal.length - 2;
    char *chars = checked_malloc((size_t)text_length + 1);
    int length = 0;
    AstNode *result = NULL;
    Token plus = literal;
    plus.type = TOKEN_PLUS;

    for (int i = 0; i < text_length; i++) {
        char c = text[i];
        if (c == '\\') {
            i++;
            switch (text[i]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                default:  c = text[i]; break;  /* '"', '\\' or '$' */
            }
        } else if (c == '$' && i + 1 < text_length && text[i + 1] == '{') {
            const char *from = text + i + 2;
            const char *close = memchr(from, '}', (size_t)(text_length - i - 2));
            if (!close) {
                error_at(parser, literal, "This '${' is never closed. Add a matching '}'.");
                free(chars);
                return ast_new_placeholder();
            }
            AstNode *piece = ast_new_string(literal, chars, length);
            result = result ? ast_new_binop(plus, result, piece) : piece;
            AstNode *expr = interpolation_to_string(literal, parse_interpolation(parser, literal, from, close));
            result = ast_new_binop(plus, result, expr);
            chars = checked_malloc((size_t)text_length + 1);
            length = 0;
            i = (int)(close - text);
            continue;
        }
        chars[length++] = c;
    }
    AstNode *tail = ast_new_string(literal, chars, length);
    if (!result) return tail;
    if (length > 0) result = ast_new_binop(plus, result, tail);
    else ast_free(tail);
    return result;
}

static AstNode *parse_primary(Parser *parser) {
    if (check(parser, TOKEN_INT)) {
        Token literal = parser->current;
        advance(parser);
        return ast_new_number(literal);
    }

    if (check(parser, TOKEN_FLOAT)) {
        Token literal = parser->current;
        advance(parser);
        char text[64];
        int n = literal.length < 63 ? literal.length : 63;
        memcpy(text, literal.start, (size_t)n);
        text[n] = '\0';
        return ast_new_float(literal, strtod(text, NULL));
    }

    if (check(parser, TOKEN_BOOL)) {
        Token literal = parser->current;
        advance(parser);
        return ast_new_bool(literal, (int)literal.value);
    }

    if (check(parser, TOKEN_KHALI)) {
        Token literal = parser->current;
        advance(parser);
        if (reject_keyword_as_name(parser, literal)) return ast_new_placeholder();
        return ast_new_null(literal);
    }

    if (check(parser, TOKEN_SUPER)) {
        Token keyword = parser->current;
        advance(parser);
        if (reject_keyword_as_name(parser, keyword)) return ast_new_placeholder();
        if (!parser->in_method) {
            error_at(parser, keyword, "'super' (the parent class) can only be used inside the methods of a class.");
            return ast_new_placeholder();
        }
        if (check(parser, TOKEN_LPAREN)) {
            /* super(args): runs the parent class's 'banau' on this object */
            if (!parser->in_constructor) {
                error_at(parser, keyword, "'super(...)' runs the parent's constructor, so it can only be used "
                                          "inside 'banau'. Use 'super.method(...)' to call a parent method.");
                return ast_new_placeholder();
            }
            AstNode *call = parse_call(parser, keyword);
            call->as.call.receiver = ast_new_super(keyword);
            return call;
        }
        if (!match(parser, TOKEN_DOT)) {
            error_at_current(parser, "Expected '.' and a method after 'super', e.g. 'super.bolau()'.");
            return ast_new_placeholder();
        }
        Token name = expect(parser, TOKEN_IDENT, "Expected a method name after 'super.', e.g. 'super.bolau()'.");
        if (parser->had_error) return ast_new_placeholder();
        if (!check(parser, TOKEN_LPAREN)) {
            error_at_current(parser, "'super' can only call methods, e.g. 'super.bolau()'. Fields belong to the "
                                     "object itself: use 'yo.naam'.");
            return ast_new_placeholder();
        }
        AstNode *call = parse_call(parser, name);
        call->as.call.receiver = ast_new_super(keyword);
        return call;
    }

    if (check(parser, TOKEN_STRING)) {
        return parse_string(parser);
    }

    if (check(parser, TOKEN_IDENT)) {
        Token name = parser->current;
        advance(parser);
        if (check(parser, TOKEN_LPAREN)) {
            return parse_call(parser, name);
        }
        return ast_new_variable(name);
    }

    if (check(parser, TOKEN_LBRACKET)) {
        return parse_array(parser);
    }

    if (check(parser, TOKEN_YO)) {
        Token keyword = parser->current;
        advance(parser);
        if (reject_keyword_as_name(parser, keyword)) {
            return ast_new_placeholder();
        }
        if (!parser->in_method) {
            error_at(parser, keyword,
                     "'yo' means 'this object' and can only be used inside the methods of a class.");
            return ast_new_placeholder();
        }
        return ast_new_self(keyword);
    }

    if (check(parser, TOKEN_LPAREN)) {
        advance(parser);
        skip_newlines(parser);
        AstNode *inner = parse_comparison(parser);
        skip_newlines(parser);
        expect(parser, TOKEN_RPAREN, "Expected ')' to close the expression.");
        return inner;
    }

    error_at_current(parser, "Expected a value here: a number, a string, a variable, sacho/jutho, khali, '(', '[', or a sign.");
    return ast_new_placeholder();
}

static AstNode *parse_factor(Parser *parser) {
    if (parser->had_error) return ast_new_placeholder();

    if (check(parser, TOKEN_MINUS) || check(parser, TOKEN_PLUS)) {
        Token op = parser->current;
        advance(parser);
        AstNode *operand = parse_factor(parser);
        return ast_new_unary(op, operand);
    }

    return parse_postfix(parser, parse_primary(parser));
}

static AstNode *parse_term(Parser *parser) {
    AstNode *left = parse_factor(parser);

    while (!parser->had_error &&
           (check(parser, TOKEN_STAR) || check(parser, TOKEN_SLASH) || check(parser, TOKEN_PERCENT))) {
        Token op = parser->current;
        advance(parser);
        skip_newlines(parser);
        AstNode *right = parse_factor(parser);
        left = ast_new_binop(op, left, right);
    }

    return left;
}

static AstNode *parse_expr(Parser *parser) {
    AstNode *left = parse_term(parser);

    while (!parser->had_error && (check(parser, TOKEN_PLUS) || check(parser, TOKEN_MINUS))) {
        Token op = parser->current;
        advance(parser);
        skip_newlines(parser);
        AstNode *right = parse_term(parser);
        left = ast_new_binop(op, left, right);
    }

    return left;
}

static int is_comparison(TokenType type) {
    return type == TOKEN_EQUAL_EQUAL || type == TOKEN_BANG_EQUAL ||
           type == TOKEN_LESS || type == TOKEN_LESS_EQUAL ||
           type == TOKEN_GREATER || type == TOKEN_GREATER_EQUAL;
}

/* Comparisons are non-associative: 'a < b < c' is rejected rather than
 * silently meaning '(a < b) < c'. */
static AstNode *parse_compare(Parser *parser) {
    AstNode *left = parse_expr(parser);

    if (!parser->had_error && is_comparison(parser->current.type)) {
        Token op = parser->current;
        advance(parser);
        skip_newlines(parser);
        AstNode *right = parse_expr(parser);
        left = ast_new_binop(op, left, right);

        if (!parser->had_error && is_comparison(parser->current.type)) {
            error_at_current(parser,
                             "Comparisons can't be chained. Write each comparison inside its own 'yedi' instead.");
        }
    }

    return left;
}

static AstNode *parse_not(Parser *parser) {
    if (!parser->had_error && check(parser, TOKEN_HOINA)) {
        Token op = parser->current;
        advance(parser);
        return ast_new_unary(op, parse_not(parser));
    }
    return parse_compare(parser);
}

static AstNode *parse_and(Parser *parser) {
    AstNode *left = parse_not(parser);
    while (!parser->had_error && check(parser, TOKEN_RA)) {
        Token op = parser->current;
        advance(parser);
        skip_newlines(parser);
        left = ast_new_binop(op, left, parse_not(parser));
    }
    return left;
}

/* Full expression: 'wa' (or) binds loosest, then 'ra' (and), then 'hoina' (not), then comparisons. */
static AstNode *parse_comparison(Parser *parser) {
    AstNode *left = parse_and(parser);
    while (!parser->had_error && check(parser, TOKEN_WA)) {
        Token op = parser->current;
        advance(parser);
        skip_newlines(parser);
        left = ast_new_binop(op, left, parse_and(parser));
    }
    return left;
}

/* Call right after consuming a keyword: catches 'yedi = 5' style misuse as a variable name. */
static int reject_keyword_as_name(Parser *parser, Token keyword) {
    if (!check(parser, TOKEN_EQUAL)) return 0;

    char message[128];
    snprintf(message, sizeof(message), "'%.*s' is a reserved keyword and cannot be used as a variable name.",
             keyword.length, keyword.start);
    error_at(parser, keyword, message);
    return 1;
}

static AstNode *parse_print(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_print(ast_new_placeholder());
    }

    expect(parser, TOKEN_LPAREN, "Expected '(' after 'vana'.");
    skip_newlines(parser);
    AstNode *expr = parse_comparison(parser);
    skip_newlines(parser);
    expect(parser, TOKEN_RPAREN, "Expected ')' to close 'vana(...)'.");
    return ast_new_print(expr);
}

/* In the REPL a bare expression on its own line is printed (unless it is khali). */
static AstNode *repl_echo(AstNode *expr) {
    AstNode *print = ast_new_print(expr);
    print->as.print.repl = 1;
    return print;
}

static int is_compound_assign(TokenType type) {
    return type == TOKEN_PLUS_EQUAL || type == TOKEN_MINUS_EQUAL || type == TOKEN_STAR_EQUAL ||
           type == TOKEN_SLASH_EQUAL || type == TOKEN_PERCENT_EQUAL;
}

static int at_statement_end(Parser *parser) {
    return check(parser, TOKEN_NEWLINE) || check(parser, TOKEN_EOF) || check(parser, TOKEN_RBRACE);
}

/* A statement starting with a name, 'yo' or 'super': an assignment to a variable ('x = 1'), an
 * array item ('a[i] = 1') or a field ('p.naam = 1'), a compound assignment ('x += 1', which means
 * 'x = x + 1'), or a function/method call used on its own. */
static AstNode *parse_assignment(Parser *parser) {
    Token start = parser->current;
    Parser saved_parser = *parser;
    Lexer saved_lexer = *parser->lexer;
    AstNode *target = parse_postfix(parser, parse_primary(parser));
    if (parser->had_error) return target;

    if (check(parser, TOKEN_EQUAL) || is_compound_assign(parser->current.type)) {
        if (target->type != AST_VARIABLE && target->type != AST_INDEX && target->type != AST_FIELD) {
            error_at_current(parser, "Can't assign to this. The left side of '=' must be a variable, an array "
                                     "item like 'a[0]', or a field like 'p.naam'.");
            return target;
        }
        Token op = parser->current;
        advance(parser);
        skip_newlines(parser);
        AstNode *value = parse_comparison(parser);

        if (op.type != TOKEN_EQUAL) {
            /* x += e  ->  x = x + e   (the target is read once more for the left side) */
            Token binop = op;
            binop.length = 1;
            switch (op.type) {
                case TOKEN_PLUS_EQUAL:  binop.type = TOKEN_PLUS; break;
                case TOKEN_MINUS_EQUAL: binop.type = TOKEN_MINUS; break;
                case TOKEN_STAR_EQUAL:  binop.type = TOKEN_STAR; break;
                case TOKEN_SLASH_EQUAL: binop.type = TOKEN_SLASH; break;
                default:                binop.type = TOKEN_PERCENT; break;
            }
            value = ast_new_binop(binop, ast_clone(target), value);
        }

        if (target->type == AST_INDEX) return ast_new_index_assign(target, value);
        if (target->type == AST_FIELD) return ast_new_field_assign(target, value);
        Token name = target->as.variable.name;
        ast_free(target);
        return ast_new_assign(name, value);
    }

    if (!parser->repl || parser->block_depth > 0) {
        if (target->type == AST_CALL) return target;
    } else {
        if (at_statement_end(parser)) return repl_echo(target);
        /* Something like 'x + 1': parse the whole line again as one expression. */
        ast_free(target);
        *parser = saved_parser;
        *parser->lexer = saved_lexer;
        return repl_echo(parse_comparison(parser));
    }

    error_at(parser, start,
             "Unknown statement. Use 'vana(...)' to print, 'name = value' to assign, or 'name(...)' to call a function.");
    ast_free(target);
    return ast_new_placeholder();
}

/* [const] TYPE NAME [= value]   or   const NAME = value
 * Without a value, a typed name starts at its type's empty value (0, 0.0, jutho, "", [], khali). */
static AstNode *parse_declaration(Parser *parser, int is_const) {
    TypeRef type = no_type();
    if (at_typed_name(parser)) type = parse_type(parser);
    if (parser->had_error) return ast_new_placeholder();
    if (type.kind == TYPE_VOID) {
        error_at(parser, type.name, "'void' is only for functions that give nothing back. A variable needs a "
                                    "real type, e.g. 'int' or 'string'.");
        return ast_new_placeholder();
    }

    if (!check(parser, TOKEN_IDENT)) {
        if (parser->current.type >= TOKEN_VANA && parser->current.type <= TOKEN_CONST) {
            char message[160];
            snprintf(message, sizeof(message), "'%.*s' is a reserved keyword and cannot be used as a variable name.",
                     parser->current.length, parser->current.start);
            error_at_current(parser, message);
        } else {
            error_at_current(parser, is_const ? "Expected a name after 'const', e.g. 'const PI = 3.14'."
                                              : "Expected a variable name after the type, e.g. 'int umar = 22'.");
        }
        return ast_new_placeholder();
    }
    Token name = parser->current;
    advance(parser);

    AstNode *value;
    if (match(parser, TOKEN_EQUAL)) {
        skip_newlines(parser);
        value = parse_comparison(parser);
    } else if (is_const || type.kind == TYPE_NONE) {
        char message[200];
        snprintf(message, sizeof(message), "A constant needs a value, e.g. 'const %.*s = 10'.", name.length,
                 name.start);
        error_at_current(parser, message);
        return ast_new_placeholder();
    } else {
        value = ast_default_for(type, name);
    }

    AstNode *assign = ast_new_assign(name, value);
    assign->as.assign.type = type;
    assign->as.assign.is_const = is_const;
    return assign;
}

static void skip_newlines(Parser *parser) {
    while (check(parser, TOKEN_NEWLINE)) {
        advance(parser);
    }
}

/* Parses statements until `end` (TOKEN_RBRACE or TOKEN_EOF), leaving `end` as the current token.
 * A statement must be followed by a newline, or directly by the closing '}' of its block. */
static void parse_statements(Parser *parser, AstNode *block, TokenType end) {
    skip_newlines(parser);
    while (!parser->had_error && !check(parser, end) && !check(parser, TOKEN_EOF)) {
        ast_block_append(block, parse_statement(parser));

        if (!parser->had_error && !check(parser, TOKEN_NEWLINE) && !check(parser, end) &&
            !check(parser, TOKEN_EOF)) {
            error_at_current(parser, "Unexpected extra input. Each statement must be on its own line.");
        }
        skip_newlines(parser);
    }
}

static AstNode *parse_block(Parser *parser, const char *after) {
    AstNode *block = ast_new_block();
    if (!check(parser, TOKEN_LBRACE)) {
        char message[128];
        snprintf(message, sizeof(message), "Expected '{' to start the block after %s.", after);
        error_at_current(parser, message);
        return block;
    }

    Token open = parser->current;
    advance(parser);
    parser->block_depth++;
    parse_statements(parser, block, TOKEN_RBRACE);
    parser->block_depth--;

    if (!parser->had_error && !check(parser, TOKEN_RBRACE)) {
        error_at(parser, open, "This '{' is never closed. Add a matching '}' to end the block.");
        return block;
    }
    advance(parser);
    return block;
}

static AstNode *parse_condition(Parser *parser, Token keyword) {
    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }
    if (check(parser, TOKEN_LBRACE)) {
        char message[128];
        snprintf(message, sizeof(message), "Expected a condition after '%.*s', e.g. '%.*s x > 0 {'.",
                 keyword.length, keyword.start, keyword.length, keyword.start);
        error_at_current(parser, message);
        return ast_new_placeholder();
    }

    AstNode *condition = parse_comparison(parser);
    if (!parser->had_error && check(parser, TOKEN_EQUAL)) {
        error_at_current(parser, "Use '==' to compare values. A single '=' is only for assignment.");
    }
    return condition;
}

static AstNode *parse_if(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    AstNode *condition = parse_condition(parser, keyword);
    AstNode *then_block = parse_block(parser, "the 'yedi' condition");
    AstNode *else_branch = NULL;

    if (!parser->had_error && check(parser, TOKEN_NATARA)) {
        advance(parser);
        if (check(parser, TOKEN_YEDI)) {
            else_branch = parse_if(parser);
        } else {
            else_branch = parse_block(parser, "'natara'");
        }
    }

    return ast_new_if(condition, then_block, else_branch);
}

static AstNode *parse_while(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    AstNode *condition = parse_condition(parser, keyword);
    parser->loop_depth++;
    AstNode *body = parse_block(parser, "the 'jabasamma' condition");
    parser->loop_depth--;
    return ast_new_while(condition, body);
}

/* ko_lagi NAME in START..END { ... }  -- END is exclusive.
 * ko_lagi NAME in ARRAY { ... }       -- one iteration per element. */
static AstNode *parse_for(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }

    Token var = expect(parser, TOKEN_IDENT, "Expected a loop variable name after 'ko_lagi', e.g. 'ko_lagi i in 0..10 {'.");
    expect(parser, TOKEN_IN, "Expected 'in' after the loop variable, e.g. 'ko_lagi i in 0..10 {'.");
    AstNode *start = parse_expr(parser);

    if (!parser->had_error && !check(parser, TOKEN_DOT_DOT)) {
        if (!check(parser, TOKEN_LBRACE)) {
            error_at_current(parser, "Expected '..' for a range like '0..10', or '{' to loop over an array.");
        }
        parser->loop_depth++;
        AstNode *body = parse_block(parser, "the 'ko_lagi' array");
        parser->loop_depth--;
        return ast_new_for_each(var, start, body);
    }

    expect(parser, TOKEN_DOT_DOT, "Expected '..' between the start and end of the range, e.g. '0..10'.");
    AstNode *end = parse_expr(parser);
    parser->loop_depth++;
    AstNode *body = parse_block(parser, "the 'ko_lagi' range");
    parser->loop_depth--;
    return ast_new_for(var, start, end, body);
}

/* kaam NAME([TYPE] PARAM, ...) [RETURN_TYPE] { ... }  -- only allowed at the top level of a file
 * (or as a method in a class). Inside an interface the method has no body. */
static AstNode *parse_function(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }
    if (parser->block_depth > 0) {
        error_at(parser, keyword,
                 "Functions must be defined at the top level of the file, not inside a block or another function.");
        return ast_new_placeholder();
    }

    Token name = expect(parser, TOKEN_IDENT, "Expected a function name after 'kaam', e.g. 'kaam jamma(a, b) {'.");
    AstNode *function = ast_new_function(name);
    function->as.function.return_type = no_type();
    expect(parser, TOKEN_LPAREN, "Expected '(' after the function name, e.g. 'kaam jamma(a, b) {'.");

    if (!parser->had_error && !check(parser, TOKEN_RPAREN)) {
        do {
            skip_newlines(parser);
            TypeRef type = no_type();
            if (at_typed_name(parser)) type = parse_type(parser);
            if (!parser->had_error && (type.kind == TYPE_VOID)) {
                error_at(parser, type.name, "A parameter can't be 'void'. Use a real type, e.g. 'int'.");
            }
            Token param = expect(parser, TOKEN_IDENT, "Expected a parameter name here.");
            if (parser->had_error) break;

            for (int i = 0; i < function->as.function.param_count; i++) {
                if (same_name(function->as.function.params[i], param)) {
                    char message[256];
                    snprintf(message, sizeof(message), "Parameter '%.*s' is listed twice. Each parameter needs a different name.",
                             param.length, param.start);
                    error_at(parser, param, message);
                    break;
                }
            }
            ast_function_add_param(function, param, type);
            skip_newlines(parser);
        } while (!parser->had_error && match(parser, TOKEN_COMMA));
    }
    expect(parser, TOKEN_RPAREN, "Expected ',' or ')' in the parameter list.");

    int is_constructor = parser->in_method && is_text(name, "banau");
    if (!parser->had_error && check(parser, TOKEN_IDENT)) {
        function->as.function.return_type = parse_type(parser);
        if (!parser->had_error && is_constructor) {
            error_at(parser, function->as.function.return_type.name,
                     "'banau' can't have a return type: it always gives back the new object.");
        }
    }
    if (parser->had_error) return function;

    if (parser->in_interface) {
        if (check(parser, TOKEN_LBRACE)) {
            error_at_current(parser, "Methods in an interface have no body: write only 'kaam name(params)'. "
                                     "Each class that implements the interface gives the body.");
        }
        return function;
    }

    parser->in_function = 1;
    parser->in_constructor = is_constructor;
    parser->return_void = function->as.function.return_type.kind == TYPE_VOID;
    function->as.function.body = parse_block(parser, "the function's parameter list");
    parser->in_function = 0;
    parser->in_constructor = 0;
    parser->return_void = 0;
    return function;
}

/* Reports `name` if a field or method of `class_decl` already uses it. */
static void check_member_name(Parser *parser, AstNode *class_decl, Token name) {
    char message[256];
    for (int i = 0; i < class_decl->as.class_decl.field_count; i++) {
        if (same_name(class_decl->as.class_decl.fields[i].name, name)) {
            snprintf(message, sizeof(message), "This class already has a field named '%.*s' (line %d).",
                     name.length, name.start, class_decl->as.class_decl.fields[i].name.line);
            error_at(parser, name, message);
            return;
        }
    }
    for (int i = 0; i < class_decl->as.class_decl.method_count; i++) {
        Token method = class_decl->as.class_decl.methods[i]->as.function.name;
        if (same_name(method, name)) {
            snprintf(message, sizeof(message), "This class already has a method named '%.*s' (line %d).",
                     name.length, name.start, method.line);
            error_at(parser, name, message);
            return;
        }
    }
}

/* The optional '(Parent, Interface, ...)' after a class or interface name. */
static void parse_bases(Parser *parser, AstNode *decl) {
    if (!match(parser, TOKEN_LPAREN)) return;
    do {
        Token base = expect(parser, TOKEN_IDENT, "Expected a class or interface name here, e.g. 'class Kukur(Janawar) {'.");
        if (parser->had_error) return;
        for (int i = 0; i < decl->as.class_decl.base_count; i++) {
            if (same_name(decl->as.class_decl.bases[i], base)) {
                error_at(parser, base, "This name is already listed.");
                return;
            }
        }
        ast_class_add_base(decl, base);
    } while (match(parser, TOKEN_COMMA));
    expect(parser, TOKEN_RPAREN, "Expected ',' or ')' after the parent class and interfaces.");
}

/* class NAME[(PARENT, INTERFACE, ...)] {
 *     [TYPE] FIELD = starting value      (or 'TYPE FIELD', which starts at the type's empty value)
 *     kaam METHOD(PARAM, ...) [TYPE] { ... }
 * }
 * interface NAME[(INTERFACE, ...)] {
 *     kaam METHOD(PARAM, ...) [TYPE]
 * }
 * Only allowed at the top level. A method named 'banau' is the constructor. */
static AstNode *parse_class(Parser *parser) {
    Token keyword = parser->current;
    int is_interface = keyword.type == TOKEN_INTERFACE;
    const char *what = is_interface ? "interface" : "class";
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }
    if (parser->block_depth > 0) {
        char message[160];
        snprintf(message, sizeof(message),
                 "A %s must be defined at the top level of the file, not inside a block or a function.", what);
        error_at(parser, keyword, message);
        return ast_new_placeholder();
    }

    Token name = expect(parser, TOKEN_IDENT, is_interface ? "Expected a name after 'interface', e.g. 'interface Bolne {'."
                                                          : "Expected a class name after 'class', e.g. 'class Manchhe {'.");
    AstNode *class_decl = is_interface ? ast_new_interface(name) : ast_new_class(name);
    if (parser->had_error) return class_decl;
    parse_bases(parser, class_decl);
    if (parser->had_error) return class_decl;

    Token open = parser->current;
    expect(parser, TOKEN_LBRACE, is_interface ? "Expected '{' after the interface name, e.g. 'interface Bolne {'."
                                              : "Expected '{' after the class name, e.g. 'class Manchhe {'.");
    skip_newlines(parser);

    while (!parser->had_error && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (check(parser, TOKEN_KAAM)) {
            parser->in_method = 1;
            parser->in_interface = is_interface;
            AstNode *method = parse_function(parser);
            parser->in_method = 0;
            parser->in_interface = 0;
            if (method->type != AST_FUNCTION) {
                ast_free(method);
                break;
            }
            check_member_name(parser, class_decl, method->as.function.name);
            if (is_interface && is_text(method->as.function.name, "banau")) {
                error_at(parser, method->as.function.name, "An interface can't have a constructor ('banau').");
            }
            ast_class_add_method(class_decl, method);
        } else if (is_interface) {
            error_at_current(parser, "An interface can only list methods, e.g. 'kaam bolau()'.");
            break;
        } else if (check(parser, TOKEN_IDENT)) {
            TypeRef type = no_type();
            if (at_typed_name(parser)) type = parse_type(parser);
            if (parser->had_error) break;
            if (type.kind == TYPE_VOID) {
                error_at(parser, type.name, "A field can't be 'void'. Use a real type, e.g. 'int'.");
                break;
            }
            Token field = expect(parser, TOKEN_IDENT, "Expected a field name after the type, e.g. 'string naam'.");
            if (parser->had_error) break;
            AstNode *value;
            if (match(parser, TOKEN_EQUAL)) {
                value = parse_comparison(parser);
            } else if (type.kind != TYPE_NONE) {
                value = ast_default_for(type, field);
            } else {
                char message[256];
                snprintf(message, sizeof(message),
                         "Expected '=' and a starting value for the field, e.g. '%.*s = 0' (or give it a type, e.g. "
                         "'int %.*s').", field.length, field.start, field.length, field.start);
                error_at_current(parser, message);
                break;
            }
            check_member_name(parser, class_decl, field);
            ast_class_add_field(class_decl, field, value, type);
        } else {
            error_at_current(parser, "A class can only contain fields (e.g. 'string naam' or 'naam = \"\"') and "
                                     "methods (e.g. 'kaam bolau() { ... }').");
            break;
        }

        if (!parser->had_error && !check(parser, TOKEN_NEWLINE) && !check(parser, TOKEN_RBRACE)) {
            error_at_current(parser, "Unexpected extra input. Each field and method must start on its own line.");
        }
        skip_newlines(parser);
    }

    if (!parser->had_error && !check(parser, TOKEN_RBRACE)) {
        char message[160];
        snprintf(message, sizeof(message), "This '{' is never closed. Add a matching '}' to end the %s.", what);
        error_at(parser, open, message);
    }
    if (!parser->had_error) advance(parser);
    return class_decl;
}

static AstNode *parse_return(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }
    if (!parser->in_function) {
        error_at(parser, keyword, "'farkau' can only be used inside a function ('kaam').");
        return ast_new_placeholder();
    }

    AstNode *value = NULL;
    if (!check(parser, TOKEN_NEWLINE) && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (parser->in_constructor) {
            error_at(parser, keyword, "'banau' can't return a value: it always gives back the new object. "
                                      "Use a plain 'farkau' to leave early.");
            return ast_new_placeholder();
        }
        if (parser->return_void) {
            error_at(parser, keyword, "This function is declared 'void', so it gives nothing back. "
                                      "Use a plain 'farkau' to leave early, or change its return type.");
            return ast_new_placeholder();
        }
        value = parse_comparison(parser);
    }
    return ast_new_return(keyword, value);
}

static AstNode *parse_loop_jump(Parser *parser) {
    Token keyword = parser->current;
    AstType type = keyword.type == TOKEN_RUK ? AST_BREAK : AST_CONTINUE;
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }
    if (parser->loop_depth == 0) {
        char message[128];
        snprintf(message, sizeof(message), "'%.*s' can only be used inside a loop ('jabasamma' or 'ko_lagi').",
                 keyword.length, keyword.start);
        error_at(parser, keyword, message);
        return ast_new_placeholder();
    }
    return ast_new_loop_jump(type, keyword);
}

/* import "path/to/file.np"   or   import math   (same as import "math.np") */
static AstNode *parse_import(Parser *parser) {
    Token keyword = parser->current;
    advance(parser);

    if (reject_keyword_as_name(parser, keyword)) {
        return ast_new_placeholder();
    }
    if (parser->block_depth > 0) {
        error_at(parser, keyword, "'import' must be at the top level of the file, not inside a block or function.");
        return ast_new_placeholder();
    }
    if (!check(parser, TOKEN_STRING) && !check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "Expected a module after 'import', e.g. 'import math' or 'import \"helpers.np\"'.");
        return ast_new_placeholder();
    }

    Token target = parser->current;
    int is_name = target.type == TOKEN_IDENT;
    const char *text = is_name ? target.start : target.start + 1;
    int length = is_name ? target.length : target.length - 2;
    char *path = malloc((size_t)length + 1);
    if (!path) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    memcpy(path, text, (size_t)length);
    path[length] = '\0';
    advance(parser);
    return ast_new_import(keyword, path, is_name);
}

static AstNode *parse_statement(Parser *parser) {
    if (check(parser, TOKEN_VANA)) {
        return parse_print(parser);
    }
    if (check(parser, TOKEN_YEDI)) {
        return parse_if(parser);
    }
    if (check(parser, TOKEN_JABASAMMA)) {
        return parse_while(parser);
    }
    if (check(parser, TOKEN_KO_LAGI)) {
        return parse_for(parser);
    }
    if (check(parser, TOKEN_KAAM)) {
        return parse_function(parser);
    }
    if (check(parser, TOKEN_IMPORT)) {
        return parse_import(parser);
    }
    if (check(parser, TOKEN_RUK) || check(parser, TOKEN_AGADI)) {
        return parse_loop_jump(parser);
    }
    if (check(parser, TOKEN_FARKAU)) {
        return parse_return(parser);
    }
    if (check(parser, TOKEN_CLASS) || check(parser, TOKEN_INTERFACE)) {
        return parse_class(parser);
    }
    if (check(parser, TOKEN_CONST)) {
        Token keyword = parser->current;
        advance(parser);
        if (reject_keyword_as_name(parser, keyword)) return ast_new_placeholder();
        return parse_declaration(parser, 1);
    }
    if (at_typed_name(parser)) {
        return parse_declaration(parser, 0);
    }
    if (check(parser, TOKEN_IN)) {
        Token keyword = parser->current;
        advance(parser);
        if (!reject_keyword_as_name(parser, keyword)) {
            error_at(parser, keyword, "'in' can only be used inside a loop, e.g. 'ko_lagi i in 0..10 {'.");
        }
        return ast_new_placeholder();
    }
    if (check(parser, TOKEN_NATARA)) {
        error_at_current(parser,
                         "'natara' must come right after the closing '}' of a 'yedi' block, on the same line: '} natara {'.");
        return ast_new_placeholder();
    }
    if (check(parser, TOKEN_RBRACE)) {
        error_at_current(parser, "Unexpected '}'. There is no open '{' block here to close.");
        return ast_new_placeholder();
    }
    if (check(parser, TOKEN_IDENT) || check(parser, TOKEN_YO) || check(parser, TOKEN_SUPER)) {
        return parse_assignment(parser);
    }
    if (parser->repl && parser->block_depth == 0) {
        return repl_echo(parse_comparison(parser));
    }

    error_at_current(parser, "Expected a statement here, such as 'vana(...)' or 'name = value'.");
    return ast_new_placeholder();
}

void parser_init(Parser *parser, Lexer *lexer, const char *source, const char *filename) {
    parser->lexer = lexer;
    parser->source = source;
    parser->filename = filename;
    parser->had_error = 0;
    parser->block_depth = 0;
    parser->loop_depth = 0;
    parser->in_function = 0;
    parser->in_method = 0;
    parser->in_constructor = 0;
    parser->in_interface = 0;
    parser->return_void = 0;
    parser->repl = 0;
    advance(parser);
}

AstNode *parser_parse_program(Parser *parser) {
    AstNode *program = ast_new_block();
    parse_statements(parser, program, TOKEN_EOF);
    return program;
}
