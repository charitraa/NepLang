#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checker.h"
#include "diagnostics.h"

/* The checker does type *inference*, not type declarations: it knows the type of literals,
 * of variables that are only ever assigned one type within their scope, and of the results of
 * operators and a few built-ins. Anything else is T_UNKNOWN, and unknown never causes an
 * error — so a program the checker rejects is one that would definitely fail at runtime. */
typedef enum { T_UNKNOWN, T_NUMBER, T_STRING, T_ARRAY, T_OBJECT } Type;

typedef struct {
    Token name;
    Type type;
} Symbol;

typedef struct {
    Symbol *items;
    int count;
    int capacity;
} SymbolTable;

typedef struct {
    const char *source;
    const char *filename;
    int errors;
    AstNode *program;
    SymbolTable globals;
    SymbolTable locals;          /* current function/method body; only valid when in_function */
    int in_function;
    AstNode *klass;              /* class whose method is being checked, or NULL */
} Checker;

static const struct { const char *name; int arity; } BUILTINS[] = {
    { "lambai", 1 }, { "thapa", 2 }, { "hata", 1 }, { "sankhya", 1 }, { "padha", 0 },
};

static int same(Token a, Token b) {
    return a.length == b.length && strncmp(a.start, b.start, (size_t)a.length) == 0;
}

static int is_name(Token t, const char *name) {
    return t.length == (int)strlen(name) && strncmp(t.start, name, (size_t)t.length) == 0;
}

static const char *type_name(Type t) {
    switch (t) {
        case T_NUMBER: return "a number";
        case T_STRING: return "a string";
        case T_ARRAY:  return "an array";
        case T_OBJECT: return "an object";
        default:       return "a value";
    }
}

static void error(Checker *c, Token token, const char *format, ...) __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static void error(Checker *c, Token token, const char *format, ...) {
    char message[400];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    c->errors++;
    if (token.start) {
        report_error(c->source, c->filename, token, message);
    } else {
        fprintf(stderr, "Error in %s:\n\n%s\n", c->filename, message);
    }
}

/* ---- symbol tables ---- */

static Symbol *table_find(SymbolTable *table, Token name) {
    for (int i = 0; i < table->count; i++) {
        if (same(table->items[i].name, name)) return &table->items[i];
    }
    return NULL;
}

/* Records an assignment: the first one fixes the type, a different one makes it unknown. */
static void table_assign(SymbolTable *table, Token name, Type type) {
    Symbol *sym = table_find(table, name);
    if (sym) {
        if (sym->type != type) sym->type = T_UNKNOWN;
        return;
    }
    if (table->count == table->capacity) {
        table->capacity = table->capacity ? table->capacity * 2 : 16;
        table->items = realloc(table->items, sizeof(Symbol) * (size_t)table->capacity);
        if (!table->items) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
    }
    table->items[table->count++] = (Symbol){ name, type };
}

static void table_free(SymbolTable *table) {
    free(table->items);
    table->items = NULL;
    table->count = table->capacity = 0;
}

/* ---- lookups ---- */

static AstNode *find_class(Checker *c, Token name) {
    for (int i = 0; i < c->program->as.block.count; i++) {
        AstNode *n = c->program->as.block.statements[i];
        if (n->type == AST_CLASS && same(n->as.class_decl.name, name)) return n;
    }
    return NULL;
}

static AstNode *find_function(Checker *c, Token name) {
    for (int i = 0; i < c->program->as.block.count; i++) {
        AstNode *n = c->program->as.block.statements[i];
        if (n->type == AST_FUNCTION && same(n->as.function.name, name)) return n;
    }
    return NULL;
}

static AstNode *find_method(AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.method_count; i++) {
        AstNode *m = klass->as.class_decl.methods[i];
        if (same(m->as.function.name, name)) return m;
    }
    return NULL;
}

static int field_index(AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        if (same(klass->as.class_decl.fields[i].name, name)) return i;
    }
    return -1;
}

/* ---- inference ---- */

static Type check_expr(Checker *c, AstNode *node, int use_tables);

/* Type of a variable read; reports a name that is never assigned anywhere it could come from. */
static Type variable_type(Checker *c, AstNode *node, int report) {
    Token name = node->as.variable.name;
    if (c->in_function) {
        Symbol *sym = table_find(&c->locals, name);
        if (sym) return sym->type;
        if (c->klass && field_index(c->klass, name) >= 0) return T_UNKNOWN;
    }
    Symbol *sym = table_find(&c->globals, name);
    if (sym) return c->in_function ? T_UNKNOWN : sym->type;
    if (report) {
        error(c, name, "'%.*s' is used here but it is never given a value. Assign it first, e.g. '%.*s = 0'.",
              name.length, name.start, name.length, name.start);
    }
    return T_UNKNOWN;
}

static void check_condition(Checker *c, AstNode *cond) {
    Type t = check_expr(c, cond, 1);
    if (t == T_STRING || t == T_ARRAY || t == T_OBJECT) {
        error(c, cond->token.start ? cond->token : (Token){ 0 },
              "A condition must be a number, but this is %s.", type_name(t));
    }
}

static void check_number(Checker *c, AstNode *node, const char *what) {
    Type t = check_expr(c, node, 1);
    if (t == T_STRING || t == T_ARRAY || t == T_OBJECT) {
        error(c, node->token, "%s must be a number, but this is %s.", what, type_name(t));
    }
}

static void check_arity(Checker *c, AstNode *call, int expected) {
    int got = call->as.call.arg_count;
    if (got != expected) {
        Token name = call->as.call.name;
        error(c, name, "'%.*s' takes %d argument%s, but %d %s given here.", name.length, name.start, expected,
              expected == 1 ? "" : "s", got, got == 1 ? "was" : "were");
    }
}

static Type check_call(Checker *c, AstNode *node) {
    Token name = node->as.call.name;
    Type types[8] = { T_UNKNOWN };

    for (int i = 0; i < node->as.call.arg_count; i++) {
        Type t = check_expr(c, node->as.call.args[i], 1);
        if (i < 8) types[i] = t;
    }

    if (node->as.call.receiver) {
        Type rt = check_expr(c, node->as.call.receiver, 1);
        if (rt == T_NUMBER || rt == T_STRING || rt == T_ARRAY) {
            error(c, name, "Can't call method '%.*s' on %s. Only objects have methods.", name.length, name.start,
                  type_name(rt));
        } else if (node->as.call.receiver->type == AST_SELF && c->klass) {
            AstNode *m = find_method(c->klass, name);
            if (!m) {
                error(c, name, "Class '%.*s' has no method '%.*s'.", c->klass->as.class_decl.name.length,
                      c->klass->as.class_decl.name.start, name.length, name.start);
            } else {
                check_arity(c, node, m->as.function.param_count);
            }
        }
        return T_UNKNOWN;
    }

    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        if (!is_name(name, BUILTINS[i].name)) continue;
        check_arity(c, node, BUILTINS[i].arity);
        if (node->as.call.arg_count == BUILTINS[i].arity && BUILTINS[i].arity > 0) {
            Type a = types[0];
            if (is_name(name, "lambai") && (a == T_NUMBER || a == T_OBJECT)) {
                error(c, name, "'lambai' needs an array or a string, but this is %s.", type_name(a));
            } else if ((is_name(name, "thapa") || is_name(name, "hata")) && a != T_ARRAY && a != T_UNKNOWN) {
                error(c, name, "'%.*s' needs an array as its first argument, but this is %s.", name.length,
                      name.start, type_name(a));
            } else if (is_name(name, "sankhya") && (a == T_ARRAY || a == T_OBJECT)) {
                error(c, name, "'sankhya' needs a string, but this is %s.", type_name(a));
            }
        }
        if (is_name(name, "lambai") || is_name(name, "sankhya")) return T_NUMBER;
        if (is_name(name, "padha")) return T_STRING;
        return T_UNKNOWN;
    }

    if (c->klass) {
        AstNode *m = find_method(c->klass, name);
        if (m) {
            check_arity(c, node, m->as.function.param_count);
            return T_UNKNOWN;
        }
    }

    AstNode *klass = find_class(c, name);
    if (klass) {
        AstNode *ctor = find_method(klass, (Token){ .start = "banau", .length = 5 });
        check_arity(c, node, ctor ? ctor->as.function.param_count : 0);
        return T_OBJECT;
    }

    AstNode *fn = find_function(c, name);
    if (!fn) {
        error(c, name, "Function '%.*s' is not defined. Define it with 'kaam %.*s(...) { ... }'.", name.length,
              name.start, name.length, name.start);
        return T_UNKNOWN;
    }
    check_arity(c, node, fn->as.function.param_count);
    return T_UNKNOWN;
}

static int is_comparison_op(TokenType op) {
    return op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL || op == TOKEN_LESS || op == TOKEN_LESS_EQUAL ||
           op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL;
}

static Type check_binop(Checker *c, AstNode *node) {
    TokenType op = node->as.binop.op;
    Token tok = node->token;

    if (op == TOKEN_RA || op == TOKEN_WA) {
        check_condition(c, node->as.binop.left);
        check_condition(c, node->as.binop.right);
        return T_NUMBER;
    }

    Type l = check_expr(c, node->as.binop.left, 1);
    Type r = check_expr(c, node->as.binop.right, 1);
    int composite = (l == T_ARRAY || l == T_OBJECT) ? 1 : (r == T_ARRAY || r == T_OBJECT) ? 2 : 0;

    if (is_comparison_op(op)) {
        if (composite) {
            error(c, tok, "Can't compare %s using '%.*s'. Only numbers and strings can be compared.",
                  type_name(composite == 1 ? l : r), tok.length, tok.start);
        } else if (l != T_UNKNOWN && r != T_UNKNOWN && l != r) {
            error(c, tok, "Can't compare %s with %s using '%.*s'.", type_name(l), type_name(r), tok.length,
                  tok.start);
        }
        return T_NUMBER;
    }

    if (composite) {
        error(c, tok, "Can't use '%.*s' with %s.", tok.length, tok.start, type_name(composite == 1 ? l : r));
        return T_UNKNOWN;
    }
    if (op == TOKEN_PLUS) {
        if (l == T_STRING || r == T_STRING) return T_STRING;
        return (l == T_NUMBER && r == T_NUMBER) ? T_NUMBER : T_UNKNOWN;
    }
    if (l == T_STRING || r == T_STRING) {
        error(c, tok, "Can't use '%.*s' with a string. Only '+' (joining) and comparisons work on strings.",
              tok.length, tok.start);
    }
    return T_NUMBER;
}

static Type check_expr(Checker *c, AstNode *node, int use_tables) {
    (void)use_tables;
    switch (node->type) {
        case AST_NUMBER: return T_NUMBER;
        case AST_STRING: return T_STRING;
        case AST_VARIABLE: return variable_type(c, node, 1);
        case AST_SELF: return T_OBJECT;
        case AST_ARRAY:
            for (int i = 0; i < node->as.array.count; i++) check_expr(c, node->as.array.items[i], 1);
            return T_ARRAY;
        case AST_UNARY:
            if (node->as.unary.op == '!') {
                check_condition(c, node->as.unary.operand);
            } else {
                check_number(c, node->as.unary.operand, "The value after a sign");
            }
            return T_NUMBER;
        case AST_BINOP: return check_binop(c, node);
        case AST_INDEX: {
            Type t = check_expr(c, node->as.index.target, 1);
            check_number(c, node->as.index.index, "An index");
            if (t == T_NUMBER || t == T_OBJECT) {
                error(c, node->token, "Can't use [ ] on %s. Only arrays and strings can be indexed.", type_name(t));
                return T_UNKNOWN;
            }
            return t == T_STRING ? T_STRING : T_UNKNOWN;
        }
        case AST_FIELD: {
            Type t = check_expr(c, node->as.field.object, 1);
            Token name = node->as.field.name;
            if (t == T_NUMBER || t == T_STRING || t == T_ARRAY) {
                error(c, name, "Can't read field '%.*s' of %s. Only objects have fields.", name.length, name.start,
                      type_name(t));
            } else if (node->as.field.object->type == AST_SELF && c->klass && field_index(c->klass, name) < 0) {
                error(c, name, "Class '%.*s' has no field '%.*s'.", c->klass->as.class_decl.name.length,
                      c->klass->as.class_decl.name.start, name.length, name.start);
            }
            return T_UNKNOWN;
        }
        case AST_CALL: return check_call(c, node);
        default: return T_UNKNOWN;
    }
}

/* ---- statements ---- */

static void check_stmt(Checker *c, AstNode *node);

/* Table-free type of an assigned value: used when collecting what type each variable holds.
 * Never looks at other variables, so it can't be fooled by assignments that happen later. */
static Type simple_type(Checker *c, AstNode *node) {
    switch (node->type) {
        case AST_NUMBER: case AST_UNARY: return T_NUMBER;
        case AST_STRING: return T_STRING;
        case AST_ARRAY: return T_ARRAY;
        case AST_BINOP:
            if (is_comparison_op(node->as.binop.op) || node->as.binop.op == TOKEN_RA ||
                node->as.binop.op == TOKEN_WA) return T_NUMBER;
            if (node->as.binop.op != TOKEN_PLUS) return T_NUMBER;
            if (simple_type(c, node->as.binop.left) == T_STRING || simple_type(c, node->as.binop.right) == T_STRING)
                return T_STRING;
            return (simple_type(c, node->as.binop.left) == T_NUMBER &&
                    simple_type(c, node->as.binop.right) == T_NUMBER) ? T_NUMBER : T_UNKNOWN;
        case AST_CALL: {
            Token n = node->as.call.name;
            if (node->as.call.receiver) return T_UNKNOWN;
            if (is_name(n, "lambai") || is_name(n, "sankhya")) return T_NUMBER;
            if (is_name(n, "padha")) return T_STRING;
            if (find_class(c, n)) return T_OBJECT;
            return T_UNKNOWN;
        }
        default: return T_UNKNOWN;
    }
}

/* Walks a body (without entering functions or classes) recording every name it assigns. */
static void collect(Checker *c, SymbolTable *table, AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSIGN:
            table_assign(table, node->as.assign.name, simple_type(c, node->as.assign.value));
            break;
        case AST_IF:
            collect(c, table, node->as.if_stmt.then_block);
            collect(c, table, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE: collect(c, table, node->as.while_stmt.body); break;
        case AST_FOR:
            table_assign(table, node->as.for_stmt.var, T_NUMBER);
            collect(c, table, node->as.for_stmt.body);
            break;
        case AST_FOR_EACH:
            table_assign(table, node->as.for_each.var,
                         simple_type(c, node->as.for_each.iterable) == T_STRING ? T_STRING : T_UNKNOWN);
            collect(c, table, node->as.for_each.body);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) collect(c, table, node->as.block.statements[i]);
            break;
        default: break;
    }
}

static void check_function(Checker *c, AstNode *fn, AstNode *klass) {
    SymbolTable saved = c->locals;
    c->locals = (SymbolTable){ NULL, 0, 0 };
    for (int i = 0; i < fn->as.function.param_count; i++) {
        table_assign(&c->locals, fn->as.function.params[i], T_UNKNOWN);
    }
    collect(c, &c->locals, fn->as.function.body);

    int saved_in = c->in_function;
    AstNode *saved_class = c->klass;
    c->in_function = 1;
    c->klass = klass;
    check_stmt(c, fn->as.function.body);
    c->in_function = saved_in;
    c->klass = saved_class;

    table_free(&c->locals);
    c->locals = saved;
}

static void check_stmt(Checker *c, AstNode *node) {
    switch (node->type) {
        case AST_PRINT: check_expr(c, node->as.print.expr, 1); break;
        case AST_ASSIGN: check_expr(c, node->as.assign.value, 1); break;
        case AST_INDEX_ASSIGN: {
            AstNode *target = node->as.index_assign.target;
            Type t = check_expr(c, target->as.index.target, 1);
            check_number(c, target->as.index.index, "An index");
            check_expr(c, node->as.index_assign.value, 1);
            if (t == T_STRING) {
                error(c, target->token, "Strings can't be changed in place. Build a new string instead.");
            } else if (t == T_NUMBER || t == T_OBJECT) {
                error(c, target->token, "Can't use [ ] on %s. Only arrays and strings can be indexed.", type_name(t));
            }
            break;
        }
        case AST_FIELD_ASSIGN: {
            AstNode *target = node->as.field_assign.target;
            check_expr(c, target, 1);
            check_expr(c, node->as.field_assign.value, 1);
            break;
        }
        case AST_IF:
            check_condition(c, node->as.if_stmt.condition);
            check_stmt(c, node->as.if_stmt.then_block);
            if (node->as.if_stmt.else_branch) check_stmt(c, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE:
            check_condition(c, node->as.while_stmt.condition);
            check_stmt(c, node->as.while_stmt.body);
            break;
        case AST_FOR:
            check_number(c, node->as.for_stmt.start, "The start of a range");
            check_number(c, node->as.for_stmt.end, "The end of a range");
            check_stmt(c, node->as.for_stmt.body);
            break;
        case AST_FOR_EACH: {
            Type t = check_expr(c, node->as.for_each.iterable, 1);
            if (t == T_NUMBER || t == T_OBJECT) {
                error(c, node->as.for_each.iterable->token,
                      "'ko_lagi ... in' needs an array, a string, or a range like '0..10', but this is %s.",
                      type_name(t));
            }
            check_stmt(c, node->as.for_each.body);
            break;
        }
        case AST_RETURN:
            if (node->as.return_stmt.value) check_expr(c, node->as.return_stmt.value, 1);
            break;
        case AST_CALL: check_call(c, node); break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) check_stmt(c, node->as.block.statements[i]);
            break;
        default: break;
    }
}

static void check_class(Checker *c, AstNode *klass) {
    /* Field defaults are evaluated in a globals-only context. */
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        check_expr(c, klass->as.class_decl.fields[i].value, 1);
    }
    for (int i = 0; i < klass->as.class_decl.method_count; i++) {
        check_function(c, klass->as.class_decl.methods[i], klass);
    }
}

int check_program(AstNode *program, const char *source, const char *filename) {
    Checker c = { .source = source, .filename = filename, .program = program };
    collect(&c, &c.globals, program);

    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *n = program->as.block.statements[i];
        if (n->type == AST_FUNCTION) {
            check_function(&c, n, NULL);
        } else if (n->type == AST_CLASS) {
            check_class(&c, n);
        } else {
            check_stmt(&c, n);
        }
    }

    table_free(&c.globals);
    return c.errors;
}
