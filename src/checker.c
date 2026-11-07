#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checker.h"
#include "diagnostics.h"
#include "rt.h"

/* The checker runs after parsing, imports and class resolution, before any backend. It combines
 * the types the programmer wrote ('int umar', 'kaam f(string s) bool') with type *inference*: it
 * knows the type of literals, of variables that are only ever assigned one type within their
 * scope, and of the results of operators and many built-ins. Anything else is T_UNKNOWN, and
 * unknown never causes an error -- so a program the checker rejects is one that would definitely
 * fail when it runs (or breaks a rule such as changing a constant). */
typedef enum { T_UNKNOWN, T_INT, T_FLOAT, T_BOOL, T_STRING, T_ARRAY, T_OBJECT, T_NULL } Type;

typedef struct {
    Token name;
    Type type;
    TypeRef declared;          /* TYPE_NONE unless the variable was declared with a type */
    int is_const;
    const AstNode *decl;       /* the declaring assignment, if any */
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
    const AstNode *function;     /* function or method being checked */
    const AstNode *klass;        /* class whose method is being checked, or NULL */
} Checker;

static int same(Token a, Token b) {
    return a.length == b.length && strncmp(a.start, b.start, (size_t)a.length) == 0;
}

static int is_name(Token t, const char *name) {
    return t.length == (int)strlen(name) && strncmp(t.start, name, (size_t)t.length) == 0;
}

static const char *type_name(Type t) {
    switch (t) {
        case T_INT:    return "a whole number";
        case T_FLOAT:  return "a decimal number";
        case T_BOOL:   return "a bool (sacho/jutho)";
        case T_STRING: return "a string";
        case T_ARRAY:  return "an array";
        case T_OBJECT: return "an object";
        case T_NULL:   return "khali";
        default:       return "a value";
    }
}

/* the short form used in "expected int but received string" */
static const char *short_name(Type t) {
    switch (t) {
        case T_INT:    return "int";
        case T_FLOAT:  return "float";
        case T_BOOL:   return "bool";
        case T_STRING: return "string";
        case T_ARRAY:  return "array";
        case T_OBJECT: return "an object";
        case T_NULL:   return "khali";
        default:       return "a value";
    }
}

static int is_number(Type t) {
    return t == T_INT || t == T_FLOAT;
}

static void error(Checker *c, Token token, const char *format, ...) __attribute__((format(printf, 3, 4)));

static void error(Checker *c, Token token, const char *format, ...) {
    char message[600];
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

static Symbol *table_add(SymbolTable *table, Token name, Type type) {
    if (table->count == table->capacity) {
        table->capacity = table->capacity ? table->capacity * 2 : 16;
        table->items = realloc(table->items, sizeof(Symbol) * (size_t)table->capacity);
        if (!table->items) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
    }
    Symbol *sym = &table->items[table->count++];
    memset(sym, 0, sizeof(*sym));
    sym->name = name;
    sym->type = type;
    sym->declared.kind = TYPE_NONE;
    return sym;
}

/* Records an assignment: the first one fixes the type, a different one makes it unknown. A
 * declared variable keeps its declared type. */
static void table_assign(SymbolTable *table, Token name, Type type) {
    Symbol *sym = table_find(table, name);
    if (!sym) {
        table_add(table, name, type);
        return;
    }
    if (sym->declared.kind != TYPE_NONE) return;
    if (sym->type != type) sym->type = T_UNKNOWN;
}

static void table_free(SymbolTable *table) {
    free(table->items);
    table->items = NULL;
    table->count = table->capacity = 0;
}

static SymbolTable *scope_table(Checker *c) {
    return c->in_function ? &c->locals : &c->globals;
}

/* ---- lookups ---- */

static const AstNode *find_class(Checker *c, Token name) {
    for (int i = 0; i < c->program->as.block.count; i++) {
        AstNode *n = c->program->as.block.statements[i];
        if ((n->type == AST_CLASS || n->type == AST_INTERFACE) && same(n->as.class_decl.name, name)) return n;
    }
    return NULL;
}

static const AstNode *find_function(Checker *c, Token name) {
    for (int i = 0; i < c->program->as.block.count; i++) {
        AstNode *n = c->program->as.block.statements[i];
        if (n->type == AST_FUNCTION && same(n->as.function.name, name)) return n;
    }
    return NULL;
}

static const AstNode *find_method(const AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.method_count; i++) {
        const AstNode *m = klass->as.class_decl.methods[i];
        if (same(m->as.function.name, name)) return m;
    }
    return NULL;
}

static const AstNode *find_constructor(const AstNode *klass) {
    return find_method(klass, (Token){ .start = "banau", .length = 5 });
}

static int field_index(const AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        if (same(klass->as.class_decl.fields[i].name, name)) return i;
    }
    return -1;
}

/* ---- "did you mean" ---- */

static int edit_distance(Token a, Token b) {
    if (a.length > 40 || b.length > 40) return 99;
    int row[41];
    for (int j = 0; j <= b.length; j++) row[j] = j;
    for (int i = 1; i <= a.length; i++) {
        int diagonal = row[0];
        row[0] = i;
        for (int j = 1; j <= b.length; j++) {
            int above = row[j];
            int cost = a.start[i - 1] == b.start[j - 1] ? 0 : 1;
            int best = diagonal + cost;
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diagonal = above;
        }
    }
    return row[b.length];
}

typedef struct {
    Token target;
    Token best;
    int distance;
} Suggestion;

static void consider(Suggestion *s, Token candidate) {
    if (!candidate.start || same(candidate, s->target)) return;
    int limit = s->target.length <= 3 ? 1 : 2;
    int d = edit_distance(s->target, candidate);
    if (d <= limit && d < s->distance) {
        s->distance = d;
        s->best = candidate;
    }
}

/* Writes " Did you mean 'umar'?" into `out` (or nothing). */
static void suggest_variable(Checker *c, Token name, char *out, int size) {
    Suggestion s = { name, { 0 }, 99 };
    if (c->in_function) {
        for (int i = 0; i < c->locals.count; i++) consider(&s, c->locals.items[i].name);
        if (c->klass) {
            for (int i = 0; i < c->klass->as.class_decl.field_count; i++) consider(&s, c->klass->as.class_decl.fields[i].name);
        }
    }
    for (int i = 0; i < c->globals.count; i++) consider(&s, c->globals.items[i].name);
    out[0] = '\0';
    if (s.best.start) snprintf(out, (size_t)size, " Did you mean '%.*s'?", s.best.length, s.best.start);
}

static void suggest_function(Checker *c, Token name, char *out, int size) {
    Suggestion s = { name, { 0 }, 99 };
    for (int i = 0; i < c->program->as.block.count; i++) {
        AstNode *n = c->program->as.block.statements[i];
        if (n->type == AST_FUNCTION) consider(&s, n->as.function.name);
        if (n->type == AST_CLASS) consider(&s, n->as.class_decl.name);
    }
    static const char *const builtins[] = { "lambai", "thapa", "hata", "sankhya", "padha", "int", "float",
                                            "string", "bool", "prakar" };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        consider(&s, (Token){ .start = builtins[i], .length = (int)strlen(builtins[i]) });
    }
    if (c->klass) {
        for (int i = 0; i < c->klass->as.class_decl.method_count; i++) {
            consider(&s, c->klass->as.class_decl.methods[i]->as.function.name);
        }
    }
    out[0] = '\0';
    if (s.best.start) snprintf(out, (size_t)size, " Did you mean '%.*s'?", s.best.length, s.best.start);
}

/* ---- written types ---- */

/* The inferred type a value of declared type `t` is known to have (unknown if it may be khali). */
static Type type_of_ref(const TypeRef *t) {
    if (t->nullable) return T_UNKNOWN;
    switch (t->kind) {
        case TYPE_INT: return T_INT;
        case TYPE_FLOAT: return T_FLOAT;
        case TYPE_BOOL: return T_BOOL;
        case TYPE_STRING: return T_STRING;
        case TYPE_ARRAY: return T_ARRAY;
        case TYPE_VOID: return T_NULL;
        default: return T_UNKNOWN;   /* objects of a class may be khali */
    }
}

/* Reports a class type that names no class or interface. */
static void check_type_name(Checker *c, const TypeRef *t) {
    if (t->kind != TYPE_CLASS) return;
    if (!find_class(c, t->name)) {
        Token n = t->name;
        error(c, n, "Unknown type '%.*s'. Types are int, float, bool, string, array, object, any, or the name of a "
                    "class or interface.", n.length, n.start);
    }
}

/* The class a value is statically known to be an object of (a constructor call), or NULL. */
static const AstNode *static_class(Checker *c, const AstNode *value) {
    if (value->type == AST_CALL && !value->as.call.receiver && !value->as.call.module.start) {
        const AstNode *k = find_class(c, value->as.call.name);
        if (k && k->type == AST_CLASS) return k;
    }
    if (value->type == AST_SELF) return c->klass;
    return NULL;
}

/* Does a value of inferred type `vt` (from expression `value`) certainly not fit `t`? */
static int mismatch(Checker *c, const TypeRef *t, Type vt, const AstNode *value) {
    if (t->kind == TYPE_NONE || t->kind == TYPE_ANY || t->kind == TYPE_VOID || vt == T_UNKNOWN) return 0;
    if (vt == T_NULL) return !(t->nullable || t->kind == TYPE_CLASS || t->kind == TYPE_OBJECT);
    switch (t->kind) {
        case TYPE_INT: return vt != T_INT;
        case TYPE_FLOAT: return !is_number(vt);
        case TYPE_BOOL: return vt != T_BOOL;
        case TYPE_STRING: return vt != T_STRING;
        case TYPE_ARRAY: return vt != T_ARRAY;
        case TYPE_OBJECT: return vt != T_OBJECT;
        case TYPE_CLASS: {
            if (vt != T_OBJECT) return 1;
            /* only a constructor call tells the exact class ('yo' may be an object of a subclass) */
            const AstNode *k = value && value->type == AST_CALL ? static_class(c, value) : NULL;
            return k && !rt_is_a(k, t->name.start, t->name.length);
        }
        default: return 0;
    }
}

static void received(Checker *c, Type vt, const AstNode *value, char *out, int size) {
    const AstNode *k = value && vt == T_OBJECT ? static_class(c, value) : NULL;
    if (k) snprintf(out, (size_t)size, "a '%.*s' object", k->as.class_decl.name.length, k->as.class_decl.name.start);
    else snprintf(out, (size_t)size, "%s", short_name(vt));
}

/* `what` completes the sentence: "Variable 'x' is declared as int." */
static void check_fits(Checker *c, const TypeRef *t, Type vt, const AstNode *value, Token at, const char *what,
                       Token name) {
    if (!mismatch(c, t, vt, value)) return;
    char expected[80], got[120];
    rt_type_text(t, expected, (int)sizeof(expected));
    received(c, vt, value, got, (int)sizeof(got));
    if (strcmp(what, "return") == 0) {
        error(c, at, "Type error: expected %s but received %s. Function '%.*s' must give back %s with 'farkau'.",
              expected, got, name.length, name.start, expected);
    } else {
        error(c, at, "Type error: expected %s but received %s. %s '%.*s' is declared as %s.", expected, got, what,
              name.length, name.start, expected);
    }
}

/* ---- inference ---- */

static Type check_expr(Checker *c, AstNode *node);

static void module_hint(Token name, char *out, int size) {
    out[0] = '\0';
    if (rt_is_module(name.start, name.length)) {
        snprintf(out, (size_t)size, " '%.*s' is a built-in module: add 'import %.*s' at the top of the file to use it.",
                 name.length, name.start, name.length, name.start);
    }
}

/* Type of a variable read; reports a name that is never assigned anywhere it could come from. */
static Type variable_type(Checker *c, AstNode *node) {
    Token name = node->as.variable.name;
    if (c->in_function) {
        Symbol *sym = table_find(&c->locals, name);
        if (sym) return sym->type;
        if (c->klass && field_index(c->klass, name) >= 0) {
            return type_of_ref(&c->klass->as.class_decl.fields[field_index(c->klass, name)].type);
        }
    }
    Symbol *sym = table_find(&c->globals, name);
    if (sym) {
        /* a function may run before the global is assigned; only a declared type is certain */
        if (!c->in_function) return sym->type;
        return sym->declared.kind != TYPE_NONE ? type_of_ref(&sym->declared) : T_UNKNOWN;
    }
    char hint[200];
    module_hint(name, hint, (int)sizeof(hint));
    if (hint[0]) {
        error(c, name, "%s", hint + 1);
        return T_UNKNOWN;
    }
    suggest_variable(c, name, hint, (int)sizeof(hint));
    error(c, name, "'%.*s' is used here but it is never given a value. Assign it first, e.g. '%.*s = 0'.%s",
          name.length, name.start, name.length, name.start, hint);
    return T_UNKNOWN;
}

static Token where(AstNode *node) {
    return node->token;
}

static void check_condition(Checker *c, AstNode *cond) {
    Type t = check_expr(c, cond);
    if (t == T_STRING || t == T_ARRAY || t == T_OBJECT || t == T_NULL) {
        error(c, where(cond), "A condition must be sacho/jutho (or a number), but this is %s.", type_name(t));
    }
}

static void check_whole(Checker *c, AstNode *node, const char *what) {
    Type t = check_expr(c, node);
    if (t == T_FLOAT) {
        error(c, where(node), "%s must be a whole number, but this is a decimal number. Use int(x) to drop the "
                              "fraction.", what);
    } else if (t != T_UNKNOWN && t != T_INT) {
        error(c, where(node), "%s must be a number, but this is %s.", what, type_name(t));
    }
}

static void arity_error(Checker *c, Token name, const char *label, int min, int max, int got) {
    char expected[40];
    if (min == max) snprintf(expected, sizeof(expected), "%d", min);
    else snprintf(expected, sizeof(expected), "%d to %d", min, max);
    error(c, name, "%s takes %s argument%s, but %d %s given here.", label, expected, max == 1 ? "" : "s", got,
          got == 1 ? "was" : "were");
}

static void check_arity(Checker *c, AstNode *call, const char *kind, int expected) {
    int got = call->as.call.arg_count;
    if (got != expected) {
        Token name = call->as.call.name;
        char label[160];
        snprintf(label, sizeof(label), "%s'%.*s'", kind, name.length, name.start);
        arity_error(c, name, label, expected, expected, got);
    }
}

/* Checks each argument against the parameter types of `fn` (skipping `skip` leading params). */
static void check_params(Checker *c, AstNode *call, const AstNode *fn, Type *types) {
    int n = call->as.call.arg_count < fn->as.function.param_count ? call->as.call.arg_count : fn->as.function.param_count;
    for (int i = 0; i < n; i++) {
        check_fits(c, &fn->as.function.param_types[i], i < 16 ? types[i] : T_UNKNOWN, call->as.call.args[i],
                   where(call->as.call.args[i]), "Parameter", fn->as.function.params[i]);
    }
}

static Type returns(const AstNode *fn) {
    return fn->as.function.return_type.kind == TYPE_NONE ? T_UNKNOWN : type_of_ref(&fn->as.function.return_type);
}

/* What a built-in gives back, when it's always the same type. */
static Type builtin_result(int id) {
    const char *name = rt_builtin_name(id);
    static const struct { const char *name; Type type; } table[] = {
        { "lambai", T_INT }, { "padha", T_STRING }, { "leu", T_STRING }, { "int", T_INT }, { "float", T_FLOAT },
        { "string", T_STRING }, { "bool", T_BOOL }, { "prakar", T_STRING }, { "thapa", T_NULL },
        { "math.sqrt", T_FLOAT }, { "math.sin", T_FLOAT }, { "math.cos", T_FLOAT }, { "math.tan", T_FLOAT },
        { "math.exp", T_FLOAT }, { "math.log", T_FLOAT }, { "math.log10", T_FLOAT }, { "math.PI", T_FLOAT },
        { "math.E", T_FLOAT }, { "math.random", T_FLOAT }, { "math.randint", T_INT }, { "math.floor", T_INT },
        { "math.ceil", T_INT },
        { "string.length", T_INT }, { "string.upper", T_STRING }, { "string.lower", T_STRING },
        { "string.trim", T_STRING }, { "string.split", T_ARRAY }, { "string.join", T_STRING },
        { "string.contains", T_BOOL }, { "string.find", T_INT }, { "string.replace", T_STRING },
        { "string.starts_with", T_BOOL }, { "string.ends_with", T_BOOL }, { "string.repeat", T_STRING },
        { "string.chars", T_ARRAY }, { "string.code", T_INT }, { "string.from_code", T_STRING },
        { "array.length", T_INT }, { "array.contains", T_BOOL }, { "array.index_of", T_INT },
        { "array.join", T_STRING }, { "array.copy", T_ARRAY }, { "array.range", T_ARRAY }, { "array.slice", T_ARRAY },
        { "io.read", T_STRING }, { "io.read_file", T_STRING }, { "io.exists", T_BOOL },
        { "time.now", T_FLOAT }, { "time.millis", T_INT }, { "os.args", T_ARRAY },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].name, name) == 0) return table[i].type;
    }
    return T_UNKNOWN;
}

static void check_builtin_args(Checker *c, AstNode *node, int id, int offset, Type *types) {
    Token name = node->as.call.name;
    int argc = node->as.call.arg_count + offset;
    int min = rt_builtin_min_args(id) - offset, max = rt_builtin_max_args(id) - offset;
    if (argc - offset < min || argc - offset > max) {
        char label[120];
        if (offset) snprintf(label, sizeof(label), "'.%.*s()'", name.length, name.start);
        else snprintf(label, sizeof(label), "'%s'", rt_builtin_name(id));
        arity_error(c, name, label, min, max, argc - offset);
        return;
    }
    if (offset) return;
    Type a = node->as.call.arg_count > 0 ? types[0] : T_UNKNOWN;
    const char *bname = rt_builtin_name(id);
    if (a == T_UNKNOWN) return;
    if ((strcmp(bname, "lambai") == 0) && a != T_ARRAY && a != T_STRING) {
        error(c, name, "'lambai' needs an array or a string, but this is %s.", type_name(a));
    } else if ((strcmp(bname, "thapa") == 0 || strcmp(bname, "hata") == 0) && a != T_ARRAY) {
        error(c, name, "'%s' needs an array as its first argument, but this is %s.", bname, type_name(a));
    } else if (strcmp(bname, "sankhya") == 0 && !is_number(a) && a != T_STRING) {
        error(c, name, "'sankhya' needs a string, but this is %s.", type_name(a));
    } else if (strncmp(bname, "math.", 5) == 0 && strcmp(bname, "math.seed") != 0 && strcmp(bname, "math.randint") != 0 &&
               !is_number(a)) {
        error(c, name, "'%s' needs a number, but this is %s.", bname, type_name(a));
    }
}

static Type check_call(Checker *c, AstNode *node) {
    Token name = node->as.call.name;
    Type types[16];
    for (int i = 0; i < 16; i++) types[i] = T_UNKNOWN;

    AstNode *receiver = node->as.call.receiver;
    Type receiver_type = T_UNKNOWN;
    if (receiver && receiver->type != AST_SUPER) receiver_type = check_expr(c, receiver);
    for (int i = 0; i < node->as.call.arg_count; i++) {
        Type t = check_expr(c, node->as.call.args[i]);
        if (i < 16) types[i] = t;
    }

    if (node->as.call.module.start) {
        Token module = node->as.call.module;
        int id = rt_module_builtin_id(module.start, module.length, name.start, name.length);
        if (id < 0) {
            error(c, name, "The '%.*s' module has no function named '%.*s'.", module.length, module.start, name.length,
                  name.start);
            return T_UNKNOWN;
        }
        check_builtin_args(c, node, id, 0, types);
        return builtin_result(id);
    }

    if (receiver && receiver->type == AST_SUPER) {
        const AstNode *parent = c->klass ? c->klass->as.class_decl.parent : NULL;
        if (!parent) {
            Token k = c->klass ? c->klass->as.class_decl.name : name;
            error(c, receiver->token, "Class '%.*s' has no parent class, so there is no 'super' to call.", k.length,
                  k.start);
            return T_UNKNOWN;
        }
        Token pname = parent->as.class_decl.name;
        if (is_name(name, "super")) {
            const AstNode *ctor = find_constructor(parent);
            int expected = ctor ? ctor->as.function.param_count : 0;
            if (node->as.call.arg_count != expected) {
                char label[160];
                snprintf(label, sizeof(label), "'super(...)' (%.*s's constructor)", pname.length, pname.start);
                arity_error(c, name, label, expected, expected, node->as.call.arg_count);
            } else if (ctor) {
                check_params(c, node, ctor, types);
            }
            return T_NULL;
        }
        const AstNode *m = find_method(parent, name);
        if (!m) {
            error(c, name, "The parent class '%.*s' has no method named '%.*s'.", pname.length, pname.start, name.length,
                  name.start);
            return T_UNKNOWN;
        }
        check_arity(c, node, "Method ", m->as.function.param_count);
        check_params(c, node, m, types);
        return returns(m);
    }

    if (receiver) {
        if (receiver_type == T_ARRAY || receiver_type == T_STRING) {
            int id = rt_value_method_id(receiver_type == T_ARRAY ? VAL_ARRAY : VAL_STRING, name.start, name.length);
            if (id < 0) {
                error(c, name, "%s have no method '%.*s'.", receiver_type == T_ARRAY ? "Arrays" : "Strings", name.length,
                      name.start);
                return T_UNKNOWN;
            }
            check_builtin_args(c, node, id, 1, types);
            return builtin_result(id);
        }
        if (receiver_type == T_INT || receiver_type == T_FLOAT || receiver_type == T_BOOL || receiver_type == T_NULL) {
            error(c, name, "Can't call method '%.*s' on %s. Only objects, arrays and strings have methods.", name.length,
                  name.start, type_name(receiver_type));
            return T_UNKNOWN;
        }
        const AstNode *k = static_class(c, receiver);
        if (k) {
            const AstNode *m = find_method(k, name);
            Token kname = k->as.class_decl.name;
            if (!m) {
                if (field_index(k, name) >= 0) {
                    error(c, name, "'%.*s' is a field of '%.*s', not a method. Use it without '( )'.", name.length,
                          name.start, kname.length, kname.start);
                } else {
                    error(c, name, "Class '%.*s' has no method '%.*s'.", kname.length, kname.start, name.length,
                          name.start);
                }
                return T_UNKNOWN;
            }
            if (!is_name(name, "banau")) {
                check_arity(c, node, "Method ", m->as.function.param_count);
                check_params(c, node, m, types);
            }
            return returns(m);
        }
        return T_UNKNOWN;
    }

    int builtin = rt_builtin_id(name.start, name.length);
    if (builtin >= 0) {
        check_builtin_args(c, node, builtin, 0, types);
        return builtin_result(builtin);
    }

    if (c->klass && c->in_function) {
        const AstNode *m = find_method(c->klass, name);
        if (m) {
            check_arity(c, node, "Method ", m->as.function.param_count);
            check_params(c, node, m, types);
            return returns(m);
        }
    }

    const AstNode *klass = find_class(c, name);
    if (klass) {
        if (klass->type == AST_INTERFACE) {
            error(c, name, "'%.*s' is an interface, so it can't make objects. Create an object of a class that "
                           "implements it.", name.length, name.start);
            return T_UNKNOWN;
        }
        const AstNode *ctor = find_constructor(klass);
        if (ctor) {
            check_arity(c, node, "", ctor->as.function.param_count);
            check_params(c, node, ctor, types);
        } else if (node->as.call.arg_count != 0) {
            error(c, name, "'%.*s(...)' takes no arguments, because class '%.*s' has no 'banau'.", name.length,
                  name.start, name.length, name.start);
        }
        return T_OBJECT;
    }

    const AstNode *fn = find_function(c, name);
    if (!fn) {
        char hint[160];
        suggest_function(c, name, hint, (int)sizeof(hint));
        error(c, name, "Function '%.*s' is not defined. Define it with 'kaam %.*s(...) { ... }'.%s", name.length,
              name.start, name.length, name.start, hint);
        return T_UNKNOWN;
    }
    check_arity(c, node, "Function ", fn->as.function.param_count);
    check_params(c, node, fn, types);
    return returns(fn);
}

static int is_comparison_op(TokenType op) {
    return op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL || op == TOKEN_LESS || op == TOKEN_LESS_EQUAL ||
           op == TOKEN_GREATER || op == TOKEN_GREATER_EQUAL;
}

static int is_text_part(Type t) {
    return t == T_STRING || is_number(t) || t == T_BOOL || t == T_NULL;
}

static Type check_binop(Checker *c, AstNode *node) {
    TokenType op = node->as.binop.op;
    Token tok = node->token;

    if (op == TOKEN_RA || op == TOKEN_WA) {
        check_condition(c, node->as.binop.left);
        check_condition(c, node->as.binop.right);
        return T_BOOL;
    }

    Type l = check_expr(c, node->as.binop.left);
    Type r = check_expr(c, node->as.binop.right);
    int known = l != T_UNKNOWN && r != T_UNKNOWN;

    if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) {
        if (!known || l == T_NULL || r == T_NULL) return T_BOOL;
        if (is_number(l) && is_number(r)) return T_BOOL;
        if (l == r && l != T_OBJECT) return T_BOOL;
        if (l == T_OBJECT && r == T_OBJECT) {
            error(c, tok, "Can't compare two objects with '%.*s'. Compare their fields instead (or compare with khali).",
                  tok.length, tok.start);
        } else {
            error(c, tok, "Can't compare %s with %s using '%.*s'.", type_name(l), type_name(r), tok.length, tok.start);
        }
        return T_BOOL;
    }
    if (is_comparison_op(op)) {
        Type bad = (l != T_UNKNOWN && !is_number(l) && l != T_STRING) ? l
                 : (r != T_UNKNOWN && !is_number(r) && r != T_STRING) ? r : T_UNKNOWN;
        if (bad != T_UNKNOWN) {
            error(c, tok, "Can't compare %s using '%.*s'. Only numbers and strings can be ordered.", type_name(bad),
                  tok.length, tok.start);
        } else if (known && (l == T_STRING) != (r == T_STRING)) {
            error(c, tok, "Can't compare %s with %s using '%.*s'. Both sides must be numbers, or both must be strings.",
                  type_name(l), type_name(r), tok.length, tok.start);
        }
        return T_BOOL;
    }

    if (l == T_STRING || r == T_STRING) {
        Type other = l == T_STRING ? r : l;
        if (op == TOKEN_PLUS) {
            if (other != T_UNKNOWN && !is_text_part(other)) {
                error(c, tok, "Can't use '+' with %s. '+' adds two numbers, or joins text when one side is a string. "
                              "Use string(x) to turn it into text first.", type_name(other));
            }
            return T_STRING;
        }
        if (op == TOKEN_STAR && (other == T_INT || other == T_UNKNOWN)) return other == T_INT ? T_STRING : T_UNKNOWN;
        error(c, tok, "Can't use '%.*s' with a string. Only '+' (joining), '*' (repeating) and comparisons work on "
                      "strings.", tok.length, tok.start);
        return T_UNKNOWN;
    }

    Type bad = (l != T_UNKNOWN && !is_number(l)) ? l : (r != T_UNKNOWN && !is_number(r)) ? r : T_UNKNOWN;
    if (bad != T_UNKNOWN) {
        error(c, tok, "Can't use '%.*s' with %s. It only works on numbers.", tok.length, tok.start, type_name(bad));
        return T_UNKNOWN;
    }
    if (l == T_INT && r == T_INT) return T_INT;
    if (known) return T_FLOAT;
    return T_UNKNOWN;
}

static Type check_expr(Checker *c, AstNode *node) {
    switch (node->type) {
        case AST_NUMBER: return T_INT;
        case AST_FLOAT: return T_FLOAT;
        case AST_BOOL: return T_BOOL;
        case AST_NULL: return T_NULL;
        case AST_STRING: return T_STRING;
        case AST_VARIABLE: return variable_type(c, node);
        case AST_SELF: return T_OBJECT;
        case AST_ARRAY:
            for (int i = 0; i < node->as.array.count; i++) check_expr(c, node->as.array.items[i]);
            return T_ARRAY;
        case AST_UNARY: {
            if (node->as.unary.op == '!') {
                check_condition(c, node->as.unary.operand);
                return T_BOOL;
            }
            Type t = check_expr(c, node->as.unary.operand);
            if (t != T_UNKNOWN && !is_number(t)) {
                error(c, node->token, "The value after a sign must be a number, but this is %s.", type_name(t));
                return T_UNKNOWN;
            }
            return t;
        }
        case AST_BINOP: return check_binop(c, node);
        case AST_INDEX: {
            Type t = check_expr(c, node->as.index.target);
            check_whole(c, node->as.index.index, "An index");
            if (t != T_UNKNOWN && t != T_ARRAY && t != T_STRING) {
                error(c, node->token, "Can't use [ ] on %s. Only arrays and strings can be indexed.", type_name(t));
                return T_UNKNOWN;
            }
            return t == T_STRING ? T_STRING : T_UNKNOWN;
        }
        case AST_FIELD: {
            AstNode *object = node->as.field.object;
            Token name = node->as.field.name;
            if (object->type == AST_VARIABLE && rt_is_module(object->as.variable.name.start, object->as.variable.name.length) &&
                !table_find(&c->globals, object->as.variable.name) &&
                !(c->in_function && table_find(&c->locals, object->as.variable.name))) {
                Token m = object->as.variable.name;
                error(c, m, "'%.*s' is a built-in module: add 'import %.*s' at the top of the file to use it.", m.length,
                      m.start, m.length, m.start);
                return T_UNKNOWN;
            }
            Type t = check_expr(c, object);
            if (t != T_UNKNOWN && t != T_OBJECT) {
                error(c, name, "Can't read field '%.*s' of %s. Only objects have fields.", name.length, name.start,
                      type_name(t));
                return T_UNKNOWN;
            }
            const AstNode *k = static_class(c, object);
            if (k) {
                int index = field_index(k, name);
                if (index < 0) {
                    Token kn = k->as.class_decl.name;
                    if (find_method(k, name)) {
                        error(c, name, "'%.*s' is a method of '%.*s'. Call it with '( )', e.g. '.%.*s()'.", name.length,
                              name.start, kn.length, kn.start, name.length, name.start);
                    } else {
                        error(c, name, "Class '%.*s' has no field '%.*s'.", kn.length, kn.start, name.length, name.start);
                    }
                    return T_UNKNOWN;
                }
                return type_of_ref(&k->as.class_decl.fields[index].type);
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
        case AST_NUMBER: return T_INT;
        case AST_FLOAT: return T_FLOAT;
        case AST_BOOL: return T_BOOL;
        case AST_NULL: return T_NULL;
        case AST_STRING: return T_STRING;
        case AST_ARRAY: return T_ARRAY;
        case AST_UNARY:
            if (node->as.unary.op == '!') return T_BOOL;
            return simple_type(c, node->as.unary.operand) == T_FLOAT ? T_FLOAT
                 : simple_type(c, node->as.unary.operand) == T_INT ? T_INT : T_UNKNOWN;
        case AST_BINOP: {
            TokenType op = node->as.binop.op;
            if (is_comparison_op(op) || op == TOKEN_RA || op == TOKEN_WA) return T_BOOL;
            Type l = simple_type(c, node->as.binop.left), r = simple_type(c, node->as.binop.right);
            if (op == TOKEN_PLUS && (l == T_STRING || r == T_STRING)) return T_STRING;
            if (op == TOKEN_STAR && (l == T_STRING || r == T_STRING)) return T_STRING;
            if (l == T_INT && r == T_INT) return T_INT;
            if (is_number(l) && is_number(r)) return T_FLOAT;
            return T_UNKNOWN;
        }
        case AST_CALL: {
            Token n = node->as.call.name;
            if (node->as.call.module.start) {
                int id = rt_module_builtin_id(node->as.call.module.start, node->as.call.module.length, n.start, n.length);
                return id >= 0 ? builtin_result(id) : T_UNKNOWN;
            }
            if (node->as.call.receiver) return T_UNKNOWN;
            int id = rt_builtin_id(n.start, n.length);
            if (id >= 0) return builtin_result(id);
            const AstNode *k = find_class(c, n);
            if (k) return k->type == AST_CLASS ? T_OBJECT : T_UNKNOWN;
            const AstNode *fn = find_function(c, n);
            return fn ? returns(fn) : T_UNKNOWN;
        }
        default: return T_UNKNOWN;
    }
}

static int is_declaration(const AstNode *node) {
    return node->type == AST_ASSIGN && (node->as.assign.type.kind != TYPE_NONE || node->as.assign.is_const);
}

/* Walks a body (without entering functions or classes) recording every name it assigns.
 * Declarations must come first: 'int x' can't follow an earlier 'x = ...' in the same scope. */
static void collect(Checker *c, SymbolTable *table, AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSIGN:
            if (is_declaration(node)) {
                Token name = node->as.assign.name;
                Symbol *old = table_find(table, name);
                if (old) {
                    error(c, name, "'%.*s' already exists (line %d). A declaration like '%s%.*s ...' must be the "
                                   "first time a name is given a value; after that, use '%.*s = ...'.",
                          name.length, name.start, old->name.line, node->as.assign.is_const ? "const " : "type ",
                          name.length, name.start, name.length, name.start);
                    break;
                }
                Symbol *sym = table_add(table, name, T_UNKNOWN);
                sym->declared = node->as.assign.type;
                sym->is_const = node->as.assign.is_const;
                sym->decl = node;
                sym->type = node->as.assign.type.kind != TYPE_NONE ? type_of_ref(&node->as.assign.type)
                                                                    : simple_type(c, node->as.assign.value);
            } else {
                table_assign(table, node->as.assign.name, simple_type(c, node->as.assign.value));
            }
            break;
        case AST_IF:
            collect(c, table, node->as.if_stmt.then_block);
            collect(c, table, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE: collect(c, table, node->as.while_stmt.body); break;
        case AST_FOR:
            table_assign(table, node->as.for_stmt.var, T_INT);
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

static void check_function(Checker *c, const AstNode *fn, const AstNode *klass) {
    SymbolTable saved = c->locals;
    c->locals = (SymbolTable){ NULL, 0, 0 };
    for (int i = 0; i < fn->as.function.param_count; i++) {
        const TypeRef *t = &fn->as.function.param_types[i];
        check_type_name(c, t);
        Symbol *sym = table_add(&c->locals, fn->as.function.params[i], type_of_ref(t));
        sym->declared = *t;
    }
    check_type_name(c, &fn->as.function.return_type);
    if (!fn->as.function.body) {   /* an interface's method */
        table_free(&c->locals);
        c->locals = saved;
        return;
    }

    int saved_in = c->in_function;
    const AstNode *saved_class = c->klass, *saved_fn = c->function;
    c->in_function = 1;
    c->klass = klass;
    c->function = fn;
    collect(c, &c->locals, fn->as.function.body);
    check_stmt(c, fn->as.function.body);
    c->in_function = saved_in;
    c->klass = saved_class;
    c->function = saved_fn;

    table_free(&c->locals);
    c->locals = saved;
}

static void check_assign(Checker *c, AstNode *node) {
    Token name = node->as.assign.name;
    Type vt = check_expr(c, node->as.assign.value);
    if (is_declaration(node)) {
        check_type_name(c, &node->as.assign.type);
        check_fits(c, &node->as.assign.type, vt, node->as.assign.value, name, "Variable", name);
        return;
    }
    Symbol *sym = table_find(scope_table(c), name);
    if (!sym && c->in_function && c->klass && field_index(c->klass, name) >= 0) {
        const AstField *field = &c->klass->as.class_decl.fields[field_index(c->klass, name)];
        check_fits(c, &field->type, vt, node->as.assign.value, name, "Field", name);
        return;
    }
    if (!sym) return;
    if (sym->is_const) {
        error(c, name, "'%.*s' is a constant (declared with 'const' on line %d), so it can't be changed.", name.length,
              name.start, sym->name.line);
        return;
    }
    check_fits(c, &sym->declared, vt, node->as.assign.value, name, "Variable", name);
}

static void check_stmt(Checker *c, AstNode *node) {
    switch (node->type) {
        case AST_PRINT: check_expr(c, node->as.print.expr); break;
        case AST_ASSIGN: check_assign(c, node); break;
        case AST_INDEX_ASSIGN: {
            AstNode *target = node->as.index_assign.target;
            Type t = check_expr(c, target->as.index.target);
            check_whole(c, target->as.index.index, "An index");
            check_expr(c, node->as.index_assign.value);
            if (t == T_STRING) {
                error(c, target->token, "Strings can't be changed one character at a time. Build a new string with '+' "
                                        "instead.");
            } else if (t != T_UNKNOWN && t != T_ARRAY) {
                error(c, target->token, "Can't use [ ] on %s. Only arrays and strings can be indexed.", type_name(t));
            }
            break;
        }
        case AST_FIELD_ASSIGN: {
            AstNode *target = node->as.field_assign.target;
            check_expr(c, target);
            Type vt = check_expr(c, node->as.field_assign.value);
            const AstNode *k = static_class(c, target->as.field.object);
            int index = k ? field_index(k, target->as.field.name) : -1;
            if (index >= 0) {
                check_fits(c, &k->as.class_decl.fields[index].type, vt, node->as.field_assign.value,
                           target->as.field.name, "Field", target->as.field.name);
            }
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
        case AST_FOR: {
            check_whole(c, node->as.for_stmt.start, "The start of a range");
            check_whole(c, node->as.for_stmt.end, "The end of a range");
            Symbol *sym = table_find(scope_table(c), node->as.for_stmt.var);
            if (sym && sym->is_const) {
                error(c, node->as.for_stmt.var, "'%.*s' is a constant, so it can't be used as a loop variable.",
                      node->as.for_stmt.var.length, node->as.for_stmt.var.start);
            }
            check_stmt(c, node->as.for_stmt.body);
            break;
        }
        case AST_FOR_EACH: {
            Type t = check_expr(c, node->as.for_each.iterable);
            if (t != T_UNKNOWN && t != T_ARRAY && t != T_STRING) {
                error(c, node->as.for_each.iterable->token,
                      "'ko_lagi ... in' needs an array, a string, or a range like '0..10', but this is %s.",
                      type_name(t));
            }
            Symbol *sym = table_find(scope_table(c), node->as.for_each.var);
            if (sym && sym->is_const) {
                error(c, node->as.for_each.var, "'%.*s' is a constant, so it can't be used as a loop variable.",
                      node->as.for_each.var.length, node->as.for_each.var.start);
            }
            check_stmt(c, node->as.for_each.body);
            break;
        }
        case AST_RETURN: {
            const AstNode *fn = c->function;
            const TypeRef *rt = fn ? &fn->as.function.return_type : NULL;
            if (node->as.return_stmt.value) {
                Type vt = check_expr(c, node->as.return_stmt.value);
                if (rt) check_fits(c, rt, vt, node->as.return_stmt.value, node->as.return_stmt.keyword, "return",
                                   fn->as.function.name);
            } else if (rt && rt->kind != TYPE_NONE && rt->kind != TYPE_VOID && rt->kind != TYPE_ANY && !rt->nullable &&
                       rt->kind != TYPE_CLASS && rt->kind != TYPE_OBJECT) {
                char expected[80];
                rt_type_text(rt, expected, (int)sizeof(expected));
                error(c, node->as.return_stmt.keyword,
                      "Function '%.*s' must give back %s, but this 'farkau' gives nothing. Write 'farkau value'.",
                      fn->as.function.name.length, fn->as.function.name.start, expected);
            }
            break;
        }
        case AST_CALL: check_call(c, node); break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) check_stmt(c, node->as.block.statements[i]);
            break;
        default: break;
    }
}

static void check_class(Checker *c, const AstNode *klass) {
    int resolved = klass->as.class_decl.resolved;
    int first_field = resolved ? klass->as.class_decl.inherited_fields : 0;
    int own_methods = resolved ? klass->as.class_decl.own_methods : klass->as.class_decl.method_count;
    /* Field defaults are evaluated in a globals-only context. */
    for (int i = first_field; i < klass->as.class_decl.field_count; i++) {
        const AstField *field = &klass->as.class_decl.fields[i];
        check_type_name(c, &field->type);
        Type vt = check_expr(c, field->value);
        check_fits(c, &field->type, vt, field->value, field->name, "Field", field->name);
    }
    for (int i = 0; i < own_methods; i++) {
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
        } else if (n->type == AST_CLASS || n->type == AST_INTERFACE) {
            check_class(&c, n);
        } else {
            check_stmt(&c, n);
        }
    }

    table_free(&c.globals);
    return c.errors;
}
