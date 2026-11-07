#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "rt.h"
#include "diagnostics.h"

jmp_buf *rt_recover = NULL;

_Noreturn void rt_error(Token at, const char *message) {
    fflush(stdout);
    if (at.start) {
        report_error("", "<program>", at, message);
    } else if (at.line > 0) {
        fprintf(stderr, "Error at line %d, column %d:\n\n%s\n", at.line, at.col, message);
    } else {
        fprintf(stderr, "Error: %s\n", message);
    }
    if (rt_recover) longjmp(*rt_recover, 1);
    exit(1);
}

static int program_argc;
static char **program_argv;

void rt_set_args(int argc, char **argv) {
    program_argc = argc;
    program_argv = argv;
}

int rt_call_depth = 0;

_Noreturn void rt_too_deep(const char *name, int length, Token at) {
    char message[256];
    snprintf(message, sizeof(message),
             "Too many nested function calls (limit 1000). Does '%.*s' call itself without ever stopping?", length,
             name);
    rt_error(at, message);
}

Value rt_unset(void) {
    return (Value){ .type = VAL_UNSET };
}

int rt_is_unset(Value v) {
    return v.type == VAL_UNSET;
}

_Noreturn void rt_undefined_variable(const char *name, int length, Token at) {
    char message[256];
    snprintf(message, sizeof(message), "Variable '%.*s' is not defined. Assign it a value first, e.g. '%.*s = 0'.",
             length, name, length, name);
    rt_error(at, message);
}

static void *rt_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    return p;
}

/* ---- operators ---- */

static const char *op_text(TokenType op) {
    switch (op) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_EQUAL_EQUAL: return "==";
        case TOKEN_BANG_EQUAL: return "!=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        default: return "?";
    }
}

static int is_comparison(TokenType t) {
    return t == TOKEN_EQUAL_EQUAL || t == TOKEN_BANG_EQUAL || t == TOKEN_LESS || t == TOKEN_LESS_EQUAL ||
           t == TOKEN_GREATER || t == TOKEN_GREATER_EQUAL;
}

static int compare_strings(String *a, String *b) {
    int shorter = a->length < b->length ? a->length : b->length;
    int result = memcmp(a->chars, b->chars, (size_t)shorter);
    if (result != 0) return result;
    return (a->length > b->length) - (a->length < b->length);
}

static Value ordered(TokenType type, int order) {
    switch (type) {
        case TOKEN_EQUAL_EQUAL: return value_bool(order == 0);
        case TOKEN_BANG_EQUAL:  return value_bool(order != 0);
        case TOKEN_LESS:        return value_bool(order < 0);
        case TOKEN_LESS_EQUAL:  return value_bool(order <= 0);
        case TOKEN_GREATER:     return value_bool(order > 0);
        default:                return value_bool(order >= 0);
    }
}

static int is_text_part(Value v) {
    return v.type == VAL_STRING || v.type == VAL_INT || v.type == VAL_FLOAT || v.type == VAL_BOOL ||
           v.type == VAL_NULL;
}

/* Does not consume. */
static Value binop_values(TokenType op, Value left, Value right, Token at) {
    char message[300];
    const char *text = op_text(op);

    /* == and != work between any two values of the same kind, and with khali on either side. */
    if ((op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) &&
        (left.type == VAL_NULL || right.type == VAL_NULL ||
         (left.type == VAL_BOOL && right.type == VAL_BOOL))) {
        int same = value_equals(left, right);
        return value_bool(op == TOKEN_EQUAL_EQUAL ? same : !same);
    }

    if (left.type == VAL_STRING || right.type == VAL_STRING) {
        if (op == TOKEN_PLUS) {
            Value other = left.type == VAL_STRING ? right : left;
            if (!is_text_part(other)) {
                snprintf(message, sizeof(message),
                         "Can't use '+' with %s. '+' adds two numbers, or joins text when one side is a string. "
                         "Use string(x) to turn it into text first.",
                         value_type_name(other));
                rt_error(at, message);
            }
            return value_concat(left, right);
        }
        if (op == TOKEN_STAR && (left.type == VAL_INT || right.type == VAL_INT)) {
            /* "ab" * 3 -> "ababab" */
            String *s = left.type == VAL_STRING ? left.as.string : right.as.string;
            long n = left.type == VAL_INT ? left.as.integer : right.as.integer;
            if (n < 0) n = 0;
            if (s->length > 0 && n > (long)(1 << 30) / s->length) rt_error(at, "This repeated string is too long.");
            char *chars = rt_alloc((size_t)s->length * (size_t)n + 1);
            for (long i = 0; i < n; i++) memcpy(chars + i * s->length, s->chars, (size_t)s->length);
            Value result = value_new_string(chars, (int)(s->length * n));
            free(chars);
            return result;
        }
        if (is_comparison(op)) {
            if (left.type != right.type) {
                snprintf(message, sizeof(message),
                         "Can't compare %s with %s using '%s'. Both sides must be numbers, or both must be strings.",
                         value_type_name(left), value_type_name(right), text);
                rt_error(at, message);
            }
            return ordered(op, compare_strings(left.as.string, right.as.string));
        }
        snprintf(message, sizeof(message), "Can't use '%s' with a string. It only works on numbers.", text);
        rt_error(at, message);
    }

    if ((op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) && left.type == VAL_ARRAY && right.type == VAL_ARRAY) {
        int same = value_equals(left, right);
        return value_bool(op == TOKEN_EQUAL_EQUAL ? same : !same);
    }

    if (!value_is_number(left) || !value_is_number(right)) {
        const char *operand = value_type_name(!value_is_number(left) ? left : right);
        if (is_comparison(op)) {
            snprintf(message, sizeof(message),
                     "Can't compare %s using '%s'. Only numbers and strings can be ordered; "
                     "'==' also works on bools, arrays and khali.",
                     operand, text);
        } else {
            snprintf(message, sizeof(message), "Can't use '%s' with %s. It only works on numbers.", text, operand);
        }
        rt_error(at, message);
    }

    if (left.type == VAL_INT && right.type == VAL_INT) {
        long l = left.as.integer;
        long r = right.as.integer;
        switch (op) {
            case TOKEN_PLUS:  return value_int((long)((unsigned long)l + (unsigned long)r));
            case TOKEN_MINUS: return value_int((long)((unsigned long)l - (unsigned long)r));
            case TOKEN_STAR:  return value_int((long)((unsigned long)l * (unsigned long)r));
            case TOKEN_SLASH:
                if (r == 0) rt_error(at, "Division by zero.");
                if (r == -1) return value_int((long)(0UL - (unsigned long)l));
                return value_int(l / r);
            case TOKEN_PERCENT:
                if (r == 0) rt_error(at, "Modulo by zero.");
                if (r == -1) return value_int(0);
                return value_int(l % r);
            default:
                return ordered(op, (l > r) - (l < r));
        }
    }

    double l = value_as_double(left);
    double r = value_as_double(right);
    switch (op) {
        case TOKEN_PLUS:  return value_float(l + r);
        case TOKEN_MINUS: return value_float(l - r);
        case TOKEN_STAR:  return value_float(l * r);
        case TOKEN_SLASH:
            if (r == 0.0) rt_error(at, "Division by zero.");
            return value_float(l / r);
        case TOKEN_PERCENT:
            if (r == 0.0) rt_error(at, "Modulo by zero.");
            return value_float(fmod(l, r));
        default:
            return ordered(op, (l > r) - (l < r));
    }
}

Value rt_binop(TokenType op, Value left, Value right, Token at) {
    Value result = binop_values(op, left, right, at);
    value_release(left);
    value_release(right);
    return result;
}

long rt_number(Value v, const char *what, Token at) {
    if (v.type != VAL_INT) {
        char message[256];
        if (v.type == VAL_FLOAT) {
            snprintf(message, sizeof(message), "%s must be a whole number, but this is a decimal number. "
                     "Use int(x) to drop the fraction.", what);
        } else {
            snprintf(message, sizeof(message), "%s must be a number, but this is %s.", what, value_type_name(v));
        }
        rt_error(at, message);
    }
    return v.as.integer;
}

static void expect_number(Value v, Token at) {
    if (!value_is_number(v)) {
        char message[256];
        snprintf(message, sizeof(message), "The value after a sign must be a number, but this is %s.",
                 value_type_name(v));
        rt_error(at, message);
    }
}

Value rt_neg(Value v, Token at) {
    expect_number(v, at);
    if (v.type == VAL_FLOAT) return value_float(-v.as.number);
    return value_int((long)(0UL - (unsigned long)v.as.integer));
}

Value rt_pos(Value v, Token at) {
    expect_number(v, at);
    return v;
}

int rt_condition(Value v, Token at) {
    int truth;
    switch (v.type) {
        case VAL_BOOL: truth = v.as.integer != 0; break;
        case VAL_INT: truth = v.as.integer != 0; break;
        case VAL_FLOAT: truth = v.as.number != 0.0; break;
        default: {
            char message[256];
            snprintf(message, sizeof(message),
                     "A condition must be sacho/jutho (or a number), but this is %s.", value_type_name(v));
            rt_error(at, message);
        }
    }
    value_release(v);
    return truth;
}

Value rt_not(Value v, Token at) {
    return value_bool(!rt_condition(v, at));
}

Value rt_truth(Value v, Token at) {
    return value_bool(rt_condition(v, at));
}

/* ---- arrays and strings ---- */

Value rt_new_array(Value *items, int count) {
    Value array = value_new_array();
    for (int i = 0; i < count; i++) array_append(array.as.array, items[i]);
    return array;
}

static void check_target(Value target, Token at) {
    if (target.type != VAL_ARRAY && target.type != VAL_STRING) {
        char message[256];
        snprintf(message, sizeof(message), "Can't use [ ] on %s. Only arrays and strings can be indexed.",
                 value_type_name(target));
        rt_error(at, message);
    }
}

static int count_of(Value target) {
    return target.type == VAL_STRING ? target.as.string->count : target.as.array->count;
}

static void check_bounds(Value target, long index, Token at) {
    int count = count_of(target);
    if (index >= 0 && index < count) return;

    const char *kind = target.type == VAL_STRING ? "string" : "array";
    const char *unit = target.type == VAL_STRING ? "character" : "item";
    char message[256];
    if (count == 0) {
        snprintf(message, sizeof(message), "Index %ld is out of range: this %s is empty.", index, kind);
    } else {
        snprintf(message, sizeof(message),
                 "Index %ld is out of range. This %s has %d %s%s, so valid indexes are 0 to %d.", index, kind, count,
                 unit, count == 1 ? "" : "s", count - 1);
    }
    rt_error(at, message);
}

Value rt_index(Value target, Value index, Token at) {
    check_target(target, at);
    long i = rt_number(index, "An index", at);
    check_bounds(target, i, at);
    Value item = target.type == VAL_STRING ? string_char_at(target.as.string, (int)i)
                                           : value_retain(target.as.array->items[i]);
    value_release(target);
    return item;
}

void rt_set_index(Value target, Value index, Value value, Token at) {
    check_target(target, at);
    if (target.type == VAL_STRING) {
        rt_error(at, "Strings can't be changed one character at a time. Build a new string with '+' instead.");
    }
    long i = rt_number(index, "An index", at);
    check_bounds(target, i, at);
    Array *array = target.as.array;
    value_release(array->items[i]);
    array->items[i] = value;
    value_release(target);
}

Value rt_length_of(Value v) {
    return value_int(count_of(v));
}

void rt_check_iterable(Value v, Token at) {
    if (v.type != VAL_ARRAY && v.type != VAL_STRING) {
        char message[256];
        snprintf(message, sizeof(message),
                 "'ko_lagi ... in' needs an array, a string, or a range like '0..10', but this is %s.",
                 value_type_name(v));
        rt_error(at, message);
    }
}

void rt_print(Value v) {
    value_print(v);
    printf("\n");
}

/* ---- types ---- */

static int same_text(Token t, const char *name, int length) {
    return t.length == length && strncmp(t.start, name, (size_t)length) == 0;
}

int rt_is_a(const AstNode *klass, const char *name, int length) {
    for (const AstNode *k = klass; k; k = k->as.class_decl.parent) {
        if (same_text(k->as.class_decl.name, name, length)) return 1;
        for (int i = 0; i < k->as.class_decl.interface_count; i++) {
            if (rt_is_a(k->as.class_decl.interfaces[i], name, length)) return 1;
        }
    }
    return 0;
}

void rt_type_text(const TypeRef *type, char *buffer, int size) {
    snprintf(buffer, (size_t)size, "%.*s%s", type->name.length, type->name.start, type->nullable ? "?" : "");
}

static int matches(Value v, const TypeRef *type) {
    if (type->kind == TYPE_ANY || type->kind == TYPE_NONE) return 1;
    if (v.type == VAL_NULL) return type->nullable || type->kind == TYPE_CLASS || type->kind == TYPE_OBJECT;
    switch (type->kind) {
        case TYPE_INT: return v.type == VAL_INT;
        case TYPE_FLOAT: return v.type == VAL_FLOAT || v.type == VAL_INT;
        case TYPE_BOOL: return v.type == VAL_BOOL;
        case TYPE_STRING: return v.type == VAL_STRING;
        case TYPE_ARRAY: return v.type == VAL_ARRAY;
        case TYPE_OBJECT: return v.type == VAL_OBJECT;
        case TYPE_CLASS:
            return v.type == VAL_OBJECT && rt_is_a(v.as.object->klass, type->name.start, type->name.length);
        default: return 1;
    }
}

static void received_text(Value v, char *buffer, int size) {
    if (v.type == VAL_OBJECT) {
        Token k = v.as.object->klass->as.class_decl.name;
        snprintf(buffer, (size_t)size, "a '%.*s' object", k.length, k.start);
    } else {
        snprintf(buffer, (size_t)size, "%s", value_type_name(v));
    }
}

Value rt_coerce(Value v, const TypeRef *type, int context, Token name, Token at) {
    if (!type || type->kind == TYPE_NONE) return v;
    if (!matches(v, type)) {
        char expected[80], got[120], message[400];
        rt_type_text(type, expected, (int)sizeof(expected));
        received_text(v, got, (int)sizeof(got));
        switch (context) {
            case RT_CTX_PARAM:
                snprintf(message, sizeof(message),
                         "Type error: expected %s but received %s. Parameter '%.*s' is declared as %s.", expected,
                         got, name.length, name.start, expected);
                break;
            case RT_CTX_RETURN:
                snprintf(message, sizeof(message),
                         "Type error: expected %s but received %s. Function '%.*s' must give back %s with 'farkau'.",
                         expected, got, name.length, name.start, expected);
                break;
            case RT_CTX_FIELD:
                snprintf(message, sizeof(message),
                         "Type error: expected %s but received %s. Field '%.*s' is declared as %s.", expected, got,
                         name.length, name.start, expected);
                break;
            default:
                snprintf(message, sizeof(message),
                         "Type error: expected %s but received %s. Variable '%.*s' is declared as %s.", expected, got,
                         name.length, name.start, expected);
                break;
        }
        rt_error(at, message);
    }
    if (type->kind == TYPE_FLOAT && v.type == VAL_INT) return value_float((double)v.as.integer);
    return v;
}

/* ---- objects ---- */

static int rt_field_index(const AstNode *klass, const char *name, int length) {
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        if (same_text(klass->as.class_decl.fields[i].name, name, length)) return i;
    }
    return -1;
}

static int rt_method_index(const AstNode *klass, const char *name, int length) {
    for (int i = 0; i < klass->as.class_decl.method_count; i++) {
        if (same_text(klass->as.class_decl.methods[i]->as.function.name, name, length)) return i;
    }
    return -1;
}

static void rt_expect_object(Value v, const char *name, int length, const char *what, Token at) {
    if (v.type == VAL_OBJECT) return;
    char message[256];
    if (v.type == VAL_NULL) {
        snprintf(message, sizeof(message), "Can't use '.%.*s' on khali. This variable doesn't hold an object yet.",
                 length, name);
    } else {
        snprintf(message, sizeof(message), "Can't use '.%.*s' on %s. Only objects have %s.", length, name,
                 value_type_name(v), what);
    }
    rt_error(at, message);
}

static int inherits(const AstNode *klass, const AstNode *ancestor) {
    for (const AstNode *k = klass; k; k = k->as.class_decl.parent) {
        if (k == ancestor) return 1;
    }
    return 0;
}

static void rt_check_access(const AstNode *klass, const AstNode *caller, const char *name, int length,
                            const char *kind, Token at) {
    if (name[0] != '_' || (caller && (inherits(klass, caller) || inherits(caller, klass)))) return;
    Token k = klass->as.class_decl.name;
    char message[300];
    snprintf(message, sizeof(message),
             "The %s '%.*s' is private to class '%.*s' (its name starts with '_'), so it can only be used "
             "inside %.*s's own methods.",
             kind, length, name, k.length, k.start, k.length, k.start);
    rt_error(at, message);
}

static int rt_field_slot(Value v, const char *name, int length, const AstNode *caller, Token at) {
    rt_expect_object(v, name, length, "fields", at);
    const AstNode *klass = v.as.object->klass;
    int index = rt_field_index(klass, name, length);
    if (index < 0) {
        Token k = klass->as.class_decl.name;
        char message[300];
        if (rt_method_index(klass, name, length) >= 0) {
            snprintf(message, sizeof(message), "'%.*s' is a method of '%.*s'. Call it with '( )', e.g. '.%.*s()'.",
                     length, name, k.length, k.start, length, name);
        } else {
            snprintf(message, sizeof(message),
                     "Class '%.*s' has no field named '%.*s'. Fields must be listed in the class, e.g. '%.*s = 0'.",
                     k.length, k.start, length, name, length, name);
        }
        rt_error(at, message);
    }
    rt_check_access(klass, caller, name, length, "field", at);
    return index;
}

Value rt_get_field(Value object, const char *name, int length, const AstNode *caller, Token at) {
    int index = rt_field_slot(object, name, length, caller, at);
    Value field = value_retain(object.as.object->fields[index]);
    value_release(object);
    return field;
}

void rt_set_field(Value object, const char *name, int length, Value value, const AstNode *caller, Token at) {
    int index = rt_field_slot(object, name, length, caller, at);
    Object *o = object.as.object;
    const AstField *field = &o->klass->as.class_decl.fields[index];
    value = rt_coerce(value, &field->type, RT_CTX_FIELD, field->name, at);
    value_release(o->fields[index]);
    o->fields[index] = value;
    value_release(object);
}

void rt_init_field(Value object, int index, Value value, Token at) {
    Object *o = object.as.object;
    const AstField *field = &o->klass->as.class_decl.fields[index];
    value = rt_coerce(value, &field->type, RT_CTX_FIELD, field->name, at);
    value_release(o->fields[index]);
    o->fields[index] = value;
}

int rt_find_method(Value receiver, const char *name, int length, int argc, const AstNode *caller, Token at,
                   const AstNode **klass) {
    rt_expect_object(receiver, name, length, "methods", at);
    const AstNode *k = receiver.as.object->klass;
    int index = rt_method_index(k, name, length);
    Token kname = k->as.class_decl.name;
    char message[300];
    if (index < 0) {
        if (rt_field_index(k, name, length) >= 0) {
            snprintf(message, sizeof(message), "'%.*s' is a field of '%.*s', not a method. Use it without '( )'.",
                     length, name, kname.length, kname.start);
        } else {
            snprintf(message, sizeof(message), "Class '%.*s' has no method named '%.*s'.", kname.length, kname.start,
                     length, name);
        }
        rt_error(at, message);
    }
    rt_check_access(k, caller, name, length, "method", at);
    if (length == 5 && strncmp(name, "banau", 5) == 0) {
        snprintf(message, sizeof(message),
                 "'banau' runs by itself when an object is created with '%.*s(...)'. It can't be called directly.",
                 kname.length, kname.start);
        rt_error(at, message);
    }
    int params = k->as.class_decl.methods[index]->as.function.param_count;
    if (argc != params) {
        snprintf(message, sizeof(message), "Method '%.*s' takes %d argument%s, but %d %s given here.", length, name,
                 params, params == 1 ? "" : "s", argc, argc == 1 ? "was" : "were");
        rt_error(at, message);
    }
    *klass = k;
    return index;
}

/* ---- built-in functions ---- */

typedef Value (*BuiltinFn)(int id, Value *args, int argc, Token at);   /* borrows args */

typedef struct {
    const char *module;   /* NULL for a global built-in */
    const char *name;
    int min_args, max_args;
    BuiltinFn fn;
} Builtin;

static const Builtin *info(int id);   /* the table is at the end of the file */

const char *rt_builtin_name(int id) {
    static char buffer[64];
    if (!info(id)->module) return info(id)->name;
    snprintf(buffer, sizeof(buffer), "%s.%s", info(id)->module, info(id)->name);
    return buffer;
}

_Noreturn static void arg_error(int id, int i, const char *what, Value got, Token at) {
    char message[300];
    char name[64];
    snprintf(name, sizeof(name), "%s", rt_builtin_name(id));
    if (info(id)->max_args == 1) {
        snprintf(message, sizeof(message), "'%s' needs %s, but this is %s.", name, what, value_type_name(got));
    } else {
        snprintf(message, sizeof(message), "'%s' needs %s as argument %d, but this is %s.", name, what, i + 1,
                 value_type_name(got));
    }
    rt_error(at, message);
}

static long arg_int(int id, Value *args, int i, Token at) {
    if (args[i].type != VAL_INT) arg_error(id, i, "a whole number", args[i], at);
    return args[i].as.integer;
}

static double arg_num(int id, Value *args, int i, Token at) {
    if (!value_is_number(args[i])) arg_error(id, i, "a number", args[i], at);
    return value_as_double(args[i]);
}

static String *arg_str(int id, Value *args, int i, Token at) {
    if (args[i].type != VAL_STRING) arg_error(id, i, "a string", args[i], at);
    return args[i].as.string;
}

static Array *arg_arr(int id, Value *args, int i, Token at) {
    if (args[i].type != VAL_ARRAY) arg_error(id, i, "an array", args[i], at);
    return args[i].as.array;
}

static Value from_double(double d) {
    return value_float(d);
}

/* Rounds a double to a long, refusing values that don't fit. */
static Value to_whole(int id, double d, Token at) {
    if (isnan(d) || d >= 9.2e18 || d <= -9.2e18) {
        char message[200];
        snprintf(message, sizeof(message), "'%s' got a number too large to turn into a whole number.",
                 rt_builtin_name(id));
        rt_error(at, message);
    }
    return value_int((long)d);
}

/* -- global -- */

static Value b_lambai(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type != VAL_ARRAY && args[0].type != VAL_STRING) arg_error(id, 0, "an array or a string", args[0], at);
    return value_int(count_of(args[0]));
}

static Value b_push(int id, Value *args, int argc, Token at) {
    (void)argc;
    array_append(arg_arr(id, args, 0, at), value_retain(args[1]));
    return value_null();
}

static Value b_pop(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    if (a->count == 0) {
        char message[200];
        snprintf(message, sizeof(message), "Can't use '%s' on an empty array.", rt_builtin_name(id));
        rt_error(at, message);
    }
    return a->items[--a->count];   /* the array's reference moves to the caller */
}

/* Parses "42", " -7 ", "3.5", "1e3". Returns 0 if the text isn't a number. */
static int parse_number(const char *text, Value *out) {
    while (*text == ' ' || *text == '\t') text++;
    char *end = NULL;
    errno = 0;
    long whole = strtol(text, &end, 10);
    const char *rest = end;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (end != text && *rest == '\0' && errno != ERANGE) {
        *out = value_int(whole);
        return 1;
    }
    errno = 0;
    double d = strtod(text, &end);
    rest = end;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (end != text && *rest == '\0' && errno != ERANGE && strpbrk(text, "0123456789") &&
        !strpbrk(text, "xXnNiI")) {
        *out = value_float(d);
        return 1;
    }
    return 0;
}

static Value b_sankhya(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (value_is_number(args[0])) return args[0];
    String *s = arg_str(id, args, 0, at);
    Value number;
    if (!parse_number(s->chars, &number)) {
        char message[300];
        snprintf(message, sizeof(message), "Can't turn \"%.200s\" into a number.", s->chars);
        rt_error(at, message);
    }
    return number;
}

static Value read_line(void) {
    fflush(stdout);
    char *line = NULL;
    size_t cap = 0;
    ssize_t got = getline(&line, &cap, stdin);
    if (got < 0) got = 0;
    while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) got--;
    Value result = value_new_string(line ? line : "", (int)got);
    free(line);
    return result;
}

static Value b_padha(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    return read_line();
}

static Value b_int(int id, Value *args, int argc, Token at) {
    (void)argc;
    Value v = args[0];
    switch (v.type) {
        case VAL_INT: return v;
        case VAL_FLOAT: return to_whole(id, trunc(v.as.number), at);
        case VAL_BOOL: return value_int(v.as.integer);
        case VAL_STRING: {
            Value number;
            if (!parse_number(v.as.string->chars, &number)) {
                char message[300];
                snprintf(message, sizeof(message), "Can't turn \"%.200s\" into a whole number.", v.as.string->chars);
                rt_error(at, message);
            }
            if (number.type == VAL_FLOAT) return to_whole(id, trunc(number.as.number), at);
            return number;
        }
        default: arg_error(id, 0, "a number, a bool or a string", v, at);
    }
}

static Value b_float(int id, Value *args, int argc, Token at) {
    (void)argc;
    Value v = args[0];
    switch (v.type) {
        case VAL_INT: return value_float((double)v.as.integer);
        case VAL_FLOAT: return v;
        case VAL_BOOL: return value_float((double)v.as.integer);
        case VAL_STRING: {
            Value number;
            if (!parse_number(v.as.string->chars, &number)) {
                char message[300];
                snprintf(message, sizeof(message), "Can't turn \"%.200s\" into a number.", v.as.string->chars);
                rt_error(at, message);
            }
            return value_float(value_as_double(number));
        }
        default: arg_error(id, 0, "a number, a bool or a string", v, at);
    }
}

static Value b_string(int id, Value *args, int argc, Token at) {
    (void)id, (void)argc, (void)at;
    return value_to_string(args[0]);
}

static Value b_bool(int id, Value *args, int argc, Token at) {
    (void)id, (void)argc, (void)at;
    Value v = args[0];
    switch (v.type) {
        case VAL_BOOL: case VAL_INT: return value_bool(v.as.integer != 0);
        case VAL_FLOAT: return value_bool(v.as.number != 0.0);
        case VAL_STRING: return value_bool(v.as.string->length > 0);
        case VAL_ARRAY: return value_bool(v.as.array->count > 0);
        case VAL_NULL: return value_bool(0);
        default: return value_bool(1);
    }
}

static Value b_prakar(int id, Value *args, int argc, Token at) {
    (void)id, (void)argc, (void)at;
    const char *name;
    switch (args[0].type) {
        case VAL_INT: name = "int"; break;
        case VAL_FLOAT: name = "float"; break;
        case VAL_BOOL: name = "bool"; break;
        case VAL_STRING: name = "string"; break;
        case VAL_ARRAY: name = "array"; break;
        case VAL_NULL: name = "khali"; break;
        case VAL_OBJECT: {
            Token k = args[0].as.object->klass->as.class_decl.name;
            return value_new_string(k.start, k.length);
        }
        default: name = "?"; break;
    }
    return value_new_string(name, (int)strlen(name));
}

/* -- math -- */

static Value b_sqrt(int id, Value *args, int argc, Token at) {
    (void)argc;
    double x = arg_num(id, args, 0, at);
    if (x < 0) rt_error(at, "'math.sqrt' can't take the square root of a negative number.");
    return from_double(sqrt(x));
}

static Value b_pow(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_INT && args[1].type == VAL_INT && args[1].as.integer >= 0) {
        unsigned long result = 1, base = (unsigned long)args[0].as.integer;
        for (long e = args[1].as.integer; e > 0; e >>= 1) {
            if (e & 1) result *= base;
            base *= base;
        }
        return value_int((long)result);
    }
    return from_double(pow(arg_num(id, args, 0, at), arg_num(id, args, 1, at)));
}

static Value b_abs(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_INT) return value_int(args[0].as.integer < 0 ? -args[0].as.integer : args[0].as.integer);
    return from_double(fabs(arg_num(id, args, 0, at)));
}

static Value b_floor(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_INT) return args[0];
    return to_whole(id, floor(arg_num(id, args, 0, at)), at);
}

static Value b_ceil(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_INT) return args[0];
    return to_whole(id, ceil(arg_num(id, args, 0, at)), at);
}

static Value b_round(int id, Value *args, int argc, Token at) {
    if (argc == 2) {
        /* math.round(x, digits) keeps a decimal number */
        double x = arg_num(id, args, 0, at);
        long digits = arg_int(id, args, 1, at);
        double scale = pow(10.0, (double)digits);
        return from_double(round(x * scale) / scale);
    }
    if (args[0].type == VAL_INT) return args[0];
    return to_whole(id, round(arg_num(id, args, 0, at)), at);
}

static Value b_min(int id, Value *args, int argc, Token at) {
    (void)argc;
    double a = arg_num(id, args, 0, at), b = arg_num(id, args, 1, at);
    return value_retain(b < a ? args[1] : args[0]);
}

static Value b_max(int id, Value *args, int argc, Token at) {
    (void)argc;
    double a = arg_num(id, args, 0, at), b = arg_num(id, args, 1, at);
    return value_retain(b > a ? args[1] : args[0]);
}

static Value b_trig(int id, Value *args, int argc, Token at) {
    (void)argc;
    double x = arg_num(id, args, 0, at);
    const char *name = info(id)->name;
    if (strcmp(name, "sin") == 0) return from_double(sin(x));
    if (strcmp(name, "cos") == 0) return from_double(cos(x));
    if (strcmp(name, "tan") == 0) return from_double(tan(x));
    if (strcmp(name, "exp") == 0) return from_double(exp(x));
    if (x <= 0) {
        char message[200];
        snprintf(message, sizeof(message), "'%s' needs a number greater than 0.", rt_builtin_name(id));
        rt_error(at, message);
    }
    if (strcmp(name, "log10") == 0) return from_double(log10(x));
    return from_double(log(x));
}

static Value b_pi(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    return value_float(3.14159265358979323846);
}

static Value b_e(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    return value_float(2.71828182845904523536);
}

/* xorshift64*: small, fast and good enough for games and simulations (not for cryptography). */
static unsigned long long random_state;

static unsigned long long next_random(void) {
    if (random_state == 0) random_state = (unsigned long long)time(NULL) * 2654435761ULL + 88172645463325252ULL;
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return random_state * 2685821657736338717ULL;
}

static Value b_random(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    return value_float((double)(next_random() >> 11) / 9007199254740992.0);
}

static Value b_randint(int id, Value *args, int argc, Token at) {
    (void)argc;
    long lo = arg_int(id, args, 0, at), hi = arg_int(id, args, 1, at);
    if (hi < lo) rt_error(at, "'math.randint(a, b)' needs a <= b.");
    unsigned long span = (unsigned long)hi - (unsigned long)lo + 1UL;
    return value_int(span == 0 ? (long)next_random() : (long)((unsigned long)lo + next_random() % span));
}

static Value b_seed(int id, Value *args, int argc, Token at) {
    (void)argc;
    random_state = (unsigned long long)arg_int(id, args, 0, at) * 2654435761ULL + 1ULL;
    return value_null();
}

/* -- strings -- */

static int is_cont(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

/* Byte offset of character `index` (0 <= index <= count). */
static int char_offset(String *s, int index) {
    if (s->count == s->length) return index;
    int offset = 0;
    for (int seen = 0; seen < index && offset < s->length; seen++) {
        offset++;
        while (offset < s->length && is_cont(s->chars[offset])) offset++;
    }
    return offset;
}

static int char_index_of_offset(String *s, int offset) {
    int index = 0;
    for (int i = 0; i < offset; i++) {
        if (!is_cont(s->chars[i])) index++;
    }
    return index;
}

static Value b_upper(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *s = arg_str(id, args, 0, at);
    Value result = value_new_string(s->chars, s->length);
    int upper = strcmp(info(id)->name, "upper") == 0;
    for (int i = 0; i < s->length; i++) {
        char c = result.as.string->chars[i];
        if (upper && c >= 'a' && c <= 'z') result.as.string->chars[i] = (char)(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') result.as.string->chars[i] = (char)(c - 'A' + 'a');
    }
    return result;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static Value b_trim(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *s = arg_str(id, args, 0, at);
    int start = 0, end = s->length;
    while (start < end && is_space(s->chars[start])) start++;
    while (end > start && is_space(s->chars[end - 1])) end--;
    return value_new_string(s->chars + start, end - start);
}

static const char *find_bytes(String *hay, int from, String *needle) {
    if (needle->length == 0) return hay->chars + from;
    for (int i = from; i + needle->length <= hay->length; i++) {
        if (memcmp(hay->chars + i, needle->chars, (size_t)needle->length) == 0) return hay->chars + i;
    }
    return NULL;
}

static Value b_split(int id, Value *args, int argc, Token at) {
    String *s = arg_str(id, args, 0, at);
    Value result = value_new_array();
    if (argc < 2) {
        /* split(s): on runs of whitespace */
        int i = 0;
        while (i < s->length) {
            while (i < s->length && is_space(s->chars[i])) i++;
            int start = i;
            while (i < s->length && !is_space(s->chars[i])) i++;
            if (i > start) array_append(result.as.array, value_new_string(s->chars + start, i - start));
        }
        return result;
    }
    String *sep = arg_str(id, args, 1, at);
    if (sep->length == 0) {
        for (int i = 0; i < s->count; i++) array_append(result.as.array, string_char_at(s, i));
        return result;
    }
    int from = 0;
    for (;;) {
        const char *hit = find_bytes(s, from, sep);
        int end = hit ? (int)(hit - s->chars) : s->length;
        array_append(result.as.array, value_new_string(s->chars + from, end - from));
        if (!hit) break;
        from = end + sep->length;
    }
    return result;
}

static Value b_join(int id, Value *args, int argc, Token at) {
    Array *a = arg_arr(id, args, 0, at);
    if (argc > 1) arg_str(id, args, 1, at);
    Value result = value_new_string("", 0);
    for (int i = 0; i < a->count; i++) {
        if (i > 0 && argc > 1) {
            Value joined = value_concat(result, args[1]);
            value_release(result);
            result = joined;
        }
        Value text = value_to_string(a->items[i]);
        Value joined = value_concat(result, text);
        value_release(text);
        value_release(result);
        result = joined;
    }
    return result;
}

static Value b_contains(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_STRING) {
        return value_bool(find_bytes(args[0].as.string, 0, arg_str(id, args, 1, at)) != NULL);
    }
    Array *a = arg_arr(id, args, 0, at);
    for (int i = 0; i < a->count; i++) {
        if (value_equals(a->items[i], args[1])) return value_bool(1);
    }
    return value_bool(0);
}

static Value b_find(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_STRING) {
        String *s = args[0].as.string;
        const char *hit = find_bytes(s, 0, arg_str(id, args, 1, at));
        return value_int(hit ? char_index_of_offset(s, (int)(hit - s->chars)) : -1);
    }
    Array *a = arg_arr(id, args, 0, at);
    for (int i = 0; i < a->count; i++) {
        if (value_equals(a->items[i], args[1])) return value_int(i);
    }
    return value_int(-1);
}

static Value b_replace(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *s = arg_str(id, args, 0, at);
    String *old = arg_str(id, args, 1, at);
    arg_str(id, args, 2, at);
    if (old->length == 0) return value_retain(args[0]);
    Value result = value_new_string("", 0);
    int from = 0;
    for (;;) {
        const char *hit = find_bytes(s, from, old);
        int end = hit ? (int)(hit - s->chars) : s->length;
        Value piece = value_new_string(s->chars + from, end - from);
        Value joined = value_concat(result, piece);
        value_release(piece);
        value_release(result);
        result = joined;
        if (!hit) break;
        joined = value_concat(result, args[2]);
        value_release(result);
        result = joined;
        from = end + old->length;
    }
    return result;
}

static Value b_starts(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *s = arg_str(id, args, 0, at), *p = arg_str(id, args, 1, at);
    int starts = strcmp(info(id)->name, "starts_with") == 0;
    if (p->length > s->length) return value_bool(0);
    const char *where = starts ? s->chars : s->chars + s->length - p->length;
    return value_bool(memcmp(where, p->chars, (size_t)p->length) == 0);
}

/* Clamps a [from, to) range of `count` items; negative positions count from the end. */
static void clamp_range(long *from, long *to, int count) {
    if (*from < 0) *from += count;
    if (*to < 0) *to += count;
    if (*from < 0) *from = 0;
    if (*to > count) *to = count;
    if (*from > *to) *from = *to;
}

static Value b_slice(int id, Value *args, int argc, Token at) {
    if (args[0].type != VAL_STRING && args[0].type != VAL_ARRAY) arg_error(id, 0, "an array or a string", args[0], at);
    int count = count_of(args[0]);
    long from = arg_int(id, args, 1, at);
    long to = argc > 2 ? arg_int(id, args, 2, at) : count;
    clamp_range(&from, &to, count);
    if (args[0].type == VAL_STRING) {
        String *s = args[0].as.string;
        int a = char_offset(s, (int)from), b = char_offset(s, (int)to);
        return value_new_string(s->chars + a, b - a);
    }
    Array *src = args[0].as.array;
    Value result = value_new_array();
    for (long i = from; i < to; i++) array_append(result.as.array, value_retain(src->items[i]));
    return result;
}

static Value b_repeat(int id, Value *args, int argc, Token at) {
    (void)argc;
    arg_str(id, args, 0, at);
    arg_int(id, args, 1, at);
    return binop_values(TOKEN_STAR, args[0], args[1], at);
}

static Value b_chars(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *s = arg_str(id, args, 0, at);
    Value result = value_new_array();
    for (int i = 0; i < s->count; i++) array_append(result.as.array, string_char_at(s, i));
    return result;
}

static Value b_code(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *s = arg_str(id, args, 0, at);
    if (s->length == 0) rt_error(at, "'string.code' needs a string with at least one character.");
    const unsigned char *p = (const unsigned char *)s->chars;
    long code;
    if (p[0] < 0x80) code = p[0];
    else if (p[0] < 0xE0) code = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    else if (p[0] < 0xF0) code = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else code = ((long)(p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return value_int(code);
}

static Value b_from_code(int id, Value *args, int argc, Token at) {
    (void)argc;
    long c = arg_int(id, args, 0, at);
    char out[4];
    int n;
    if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) rt_error(at, "That is not a valid character code.");
    if (c < 0x80) { out[0] = (char)c; n = 1; }
    else if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F)); n = 3;
    } else {
        out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F)); n = 4;
    }
    return value_new_string(out, n);
}

static Value b_reverse(int id, Value *args, int argc, Token at) {
    (void)argc;
    if (args[0].type == VAL_STRING) {
        String *s = args[0].as.string;
        char *out = rt_alloc((size_t)s->length + 1);
        int w = s->length;
        for (int i = 0; i < s->length;) {
            int n = 1;
            while (i + n < s->length && is_cont(s->chars[i + n])) n++;
            w -= n;
            memcpy(out + w, s->chars + i, (size_t)n);
            i += n;
        }
        Value result = value_new_string(out, s->length);
        free(out);
        return result;
    }
    Array *a = arg_arr(id, args, 0, at);
    for (int i = 0, j = a->count - 1; i < j; i++, j--) {
        Value t = a->items[i];
        a->items[i] = a->items[j];
        a->items[j] = t;
    }
    return value_null();
}

/* -- arrays -- */

static Value b_insert(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    long i = arg_int(id, args, 1, at);
    if (i < 0 || i > a->count) {
        char message[200];
        snprintf(message, sizeof(message), "Can't insert at index %ld: valid positions are 0 to %d.", i, a->count);
        rt_error(at, message);
    }
    array_append(a, value_null());
    memmove(a->items + i + 1, a->items + i, sizeof(Value) * (size_t)(a->count - 1 - i));
    a->items[i] = value_retain(args[2]);
    return value_null();
}

static Value b_remove(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    long i = arg_int(id, args, 1, at);
    check_bounds(args[0], i, at);
    Value item = a->items[i];
    memmove(a->items + i, a->items + i + 1, sizeof(Value) * (size_t)(a->count - 1 - i));
    a->count--;
    return item;
}

static Value b_clear(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    for (int i = 0; i < a->count; i++) value_release(a->items[i]);
    a->count = 0;
    return value_null();
}

static Value b_copy(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    Value result = value_new_array();
    for (int i = 0; i < a->count; i++) array_append(result.as.array, value_retain(a->items[i]));
    return result;
}

static int sort_order(Value a, Value b) {
    if (a.type == VAL_STRING) return compare_strings(a.as.string, b.as.string);
    double x = value_as_double(a), y = value_as_double(b);
    return (x > y) - (x < y);
}

static Value b_sort(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    int strings = a->count > 0 && a->items[0].type == VAL_STRING;
    for (int i = 0; i < a->count; i++) {
        Value v = a->items[i];
        if (strings ? v.type != VAL_STRING : !value_is_number(v)) {
            rt_error(at, "'sort' needs an array of only numbers or only strings.");
        }
    }
    /* bottom-up merge sort: stable, O(n log n) */
    Value *tmp = rt_alloc(sizeof(Value) * (size_t)(a->count ? a->count : 1));
    for (int width = 1; width < a->count; width *= 2) {
        for (int lo = 0; lo < a->count; lo += 2 * width) {
            int mid = lo + width < a->count ? lo + width : a->count;
            int hi = lo + 2 * width < a->count ? lo + 2 * width : a->count;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = sort_order(a->items[j], a->items[i]) < 0 ? a->items[j++] : a->items[i++];
            while (i < mid) tmp[k++] = a->items[i++];
            while (j < hi) tmp[k++] = a->items[j++];
        }
        memcpy(a->items, tmp, sizeof(Value) * (size_t)a->count);
    }
    free(tmp);
    return value_null();
}

static Value b_sum(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    Value total = value_int(0);
    for (int i = 0; i < a->count; i++) {
        if (!value_is_number(a->items[i])) rt_error(at, "'sum' needs an array of numbers.");
        total = binop_values(TOKEN_PLUS, total, a->items[i], at);
    }
    return total;
}

static Value b_array_minmax(int id, Value *args, int argc, Token at) {
    (void)argc;
    Array *a = arg_arr(id, args, 0, at);
    int want_max = strcmp(info(id)->name, "max") == 0;
    if (a->count == 0) {
        char message[200];
        snprintf(message, sizeof(message), "Can't use '%s' on an empty array.", rt_builtin_name(id));
        rt_error(at, message);
    }
    int best = 0;
    for (int i = 0; i < a->count; i++) {
        Value v = a->items[i];
        if (v.type != a->items[0].type && !(value_is_number(v) && value_is_number(a->items[0]))) {
            rt_error(at, "'min' and 'max' need an array of only numbers or only strings.");
        }
        if (!value_is_number(v) && v.type != VAL_STRING) {
            rt_error(at, "'min' and 'max' need an array of only numbers or only strings.");
        }
        int order = sort_order(v, a->items[best]);
        if (want_max ? order > 0 : order < 0) best = i;
    }
    return value_retain(a->items[best]);
}

static Value b_range(int id, Value *args, int argc, Token at) {
    long from = argc == 1 ? 0 : arg_int(id, args, 0, at);
    long to = arg_int(id, args, argc == 1 ? 0 : 1, at);
    if (to - from > 100000000L) rt_error(at, "This range is too large to make into an array.");
    Value result = value_new_array();
    for (long i = from; i < to; i++) array_append(result.as.array, value_int(i));
    return result;
}

/* -- io -- */

static Value b_write(int id, Value *args, int argc, Token at) {
    (void)id, (void)argc, (void)at;
    value_print(args[0]);
    fflush(stdout);
    return value_null();
}

static Value b_read_file(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *path = arg_str(id, args, 0, at);
    FILE *file = fopen(path->chars, "rb");
    if (!file) {
        char message[400];
        snprintf(message, sizeof(message), "Could not open the file '%.300s' for reading.", path->chars);
        rt_error(at, message);
    }
    char *data = NULL;
    size_t length = 0, cap = 0;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (length + got > cap) {
            cap = (length + got) * 2;
            data = realloc(data, cap);
            if (!data) {
                fprintf(stderr, "neplang: out of memory\n");
                exit(1);
            }
        }
        memcpy(data + length, chunk, got);
        length += got;
    }
    fclose(file);
    if (length > (size_t)1 << 30) rt_error(at, "This file is too large to read.");
    Value result = value_new_string(data ? data : "", (int)length);
    free(data);
    return result;
}

static Value b_write_file(int id, Value *args, int argc, Token at) {
    (void)argc;
    String *path = arg_str(id, args, 0, at);
    Value text = value_to_string(args[1]);
    FILE *file = fopen(path->chars, strcmp(info(id)->name, "append_file") == 0 ? "ab" : "wb");
    if (!file) {
        char message[400];
        snprintf(message, sizeof(message), "Could not open the file '%.300s' for writing.", path->chars);
        rt_error(at, message);
    }
    fwrite(text.as.string->chars, 1, (size_t)text.as.string->length, file);
    fclose(file);
    value_release(text);
    return value_null();
}

static Value b_exists(int id, Value *args, int argc, Token at) {
    (void)argc;
    FILE *file = fopen(arg_str(id, args, 0, at)->chars, "rb");
    if (file) fclose(file);
    return value_bool(file != NULL);
}

/* -- time -- */

static Value b_now(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return value_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static Value b_millis(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return value_int((long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static Value b_sleep(int id, Value *args, int argc, Token at) {
    (void)argc;
    long ms = arg_int(id, args, 0, at);
    if (ms > 0) {
        fflush(stdout);
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
#endif
    }
    return value_null();
}

/* -- os -- */

static Value b_args(int id, Value *args, int argc, Token at) {
    (void)id, (void)args, (void)argc, (void)at;
    Value result = value_new_array();
    for (int i = 0; i < program_argc; i++) {
        array_append(result.as.array, value_new_string(program_argv[i], (int)strlen(program_argv[i])));
    }
    return result;
}

static Value b_env(int id, Value *args, int argc, Token at) {
    (void)argc;
    const char *v = getenv(arg_str(id, args, 0, at)->chars);
    return v ? value_new_string(v, (int)strlen(v)) : value_null();
}

static Value b_exit(int id, Value *args, int argc, Token at) {
    long code = argc > 0 ? arg_int(id, args, 0, at) : 0;
    fflush(stdout);
    exit((int)code);
}

static const Builtin BUILTINS[] = {
    /* the first five keep their old ids (RT_LAMBAI ...) */
    { NULL, "lambai", 1, 1, b_lambai },
    { NULL, "thapa", 2, 2, b_push },
    { NULL, "hata", 1, 1, b_pop },
    { NULL, "sankhya", 1, 1, b_sankhya },
    { NULL, "padha", 0, 0, b_padha },
    { NULL, "leu", 0, 0, b_padha },
    { NULL, "int", 1, 1, b_int },
    { NULL, "float", 1, 1, b_float },
    { NULL, "string", 1, 1, b_string },
    { NULL, "bool", 1, 1, b_bool },
    { NULL, "prakar", 1, 1, b_prakar },

    { "math", "sqrt", 1, 1, b_sqrt },
    { "math", "pow", 2, 2, b_pow },
    { "math", "abs", 1, 1, b_abs },
    { "math", "floor", 1, 1, b_floor },
    { "math", "ceil", 1, 1, b_ceil },
    { "math", "round", 1, 2, b_round },
    { "math", "min", 2, 2, b_min },
    { "math", "max", 2, 2, b_max },
    { "math", "sin", 1, 1, b_trig },
    { "math", "cos", 1, 1, b_trig },
    { "math", "tan", 1, 1, b_trig },
    { "math", "exp", 1, 1, b_trig },
    { "math", "log", 1, 1, b_trig },
    { "math", "log10", 1, 1, b_trig },
    { "math", "PI", 0, 0, b_pi },
    { "math", "E", 0, 0, b_e },
    { "math", "random", 0, 0, b_random },
    { "math", "randint", 2, 2, b_randint },
    { "math", "seed", 1, 1, b_seed },

    { "string", "length", 1, 1, b_lambai },
    { "string", "upper", 1, 1, b_upper },
    { "string", "lower", 1, 1, b_upper },
    { "string", "trim", 1, 1, b_trim },
    { "string", "split", 1, 2, b_split },
    { "string", "join", 1, 2, b_join },
    { "string", "contains", 2, 2, b_contains },
    { "string", "find", 2, 2, b_find },
    { "string", "replace", 3, 3, b_replace },
    { "string", "starts_with", 2, 2, b_starts },
    { "string", "ends_with", 2, 2, b_starts },
    { "string", "slice", 2, 3, b_slice },
    { "string", "repeat", 2, 2, b_repeat },
    { "string", "chars", 1, 1, b_chars },
    { "string", "code", 1, 1, b_code },
    { "string", "from_code", 1, 1, b_from_code },
    { "string", "reverse", 1, 1, b_reverse },

    { "array", "length", 1, 1, b_lambai },
    { "array", "push", 2, 2, b_push },
    { "array", "pop", 1, 1, b_pop },
    { "array", "insert", 3, 3, b_insert },
    { "array", "remove", 2, 2, b_remove },
    { "array", "clear", 1, 1, b_clear },
    { "array", "contains", 2, 2, b_contains },
    { "array", "index_of", 2, 2, b_find },
    { "array", "reverse", 1, 1, b_reverse },
    { "array", "sort", 1, 1, b_sort },
    { "array", "slice", 2, 3, b_slice },
    { "array", "join", 1, 2, b_join },
    { "array", "copy", 1, 1, b_copy },
    { "array", "sum", 1, 1, b_sum },
    { "array", "min", 1, 1, b_array_minmax },
    { "array", "max", 1, 1, b_array_minmax },
    { "array", "range", 1, 2, b_range },

    { "io", "write", 1, 1, b_write },
    { "io", "read", 0, 0, b_padha },
    { "io", "read_file", 1, 1, b_read_file },
    { "io", "write_file", 2, 2, b_write_file },
    { "io", "append_file", 2, 2, b_write_file },
    { "io", "exists", 1, 1, b_exists },

    { "time", "now", 0, 0, b_now },
    { "time", "millis", 0, 0, b_millis },
    { "time", "sleep", 1, 1, b_sleep },

    { "os", "args", 0, 0, b_args },
    { "os", "env", 1, 1, b_env },
    { "os", "exit", 0, 1, b_exit },
};

static const int BUILTIN_COUNT = (int)(sizeof(BUILTINS) / sizeof(BUILTINS[0]));

static const Builtin *info(int id) {
    return &BUILTINS[id];
}

static int same_cstr(const char *a, const char *b, int length) {
    return a && (int)strlen(a) == length && strncmp(a, b, (size_t)length) == 0;
}

int rt_builtin_id(const char *name, int length) {
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (!BUILTINS[i].module && same_cstr(BUILTINS[i].name, name, length)) return i;
    }
    return -1;
}

int rt_module_builtin_id(const char *module, int module_length, const char *name, int length) {
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (same_cstr(BUILTINS[i].module, module, module_length) && same_cstr(BUILTINS[i].name, name, length)) return i;
    }
    return -1;
}

int rt_is_module(const char *name, int length) {
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (same_cstr(BUILTINS[i].module, name, length)) return 1;
    }
    return 0;
}

int rt_builtin_min_args(int id) {
    return BUILTINS[id].min_args;
}

int rt_builtin_max_args(int id) {
    return BUILTINS[id].max_args;
}

Value rt_builtin(int id, Value *args, int argc, Token at) {
    const Builtin *b = &BUILTINS[id];
    if (argc < b->min_args || argc > b->max_args) {
        char message[300], expected[40];
        if (b->min_args == b->max_args) snprintf(expected, sizeof(expected), "%d", b->min_args);
        else snprintf(expected, sizeof(expected), "%d to %d", b->min_args, b->max_args);
        snprintf(message, sizeof(message), "'%s' takes %s argument%s, but %d %s given here.", rt_builtin_name(id),
                 expected, b->max_args == 1 ? "" : "s", argc, argc == 1 ? "was" : "were");
        rt_error(at, message);
    }
    Value result = b->fn(id, args, argc, at);
    for (int i = 0; i < argc; i++) value_release(args[i]);
    return result;
}

int rt_value_method_id(ValueType kind, const char *name, int length) {
    const char *module = kind == VAL_ARRAY ? "array" : kind == VAL_STRING ? "string" : NULL;
    if (!module) return -1;
    int id = rt_module_builtin_id(module, (int)strlen(module), name, length);
    if (id >= 0 && kind == VAL_ARRAY && same_cstr("range", name, length)) return -1;
    if (id >= 0 && kind == VAL_STRING && same_cstr("from_code", name, length)) return -1;
    return id;
}

Value rt_value_method(Value *args, int argc, const char *name, int length, Token at) {
    Value receiver = args[0];
    int id = rt_value_method_id(receiver.type, name, length);
    if (id < 0) {
        char message[400];
        if (receiver.type == VAL_ARRAY) {
            snprintf(message, sizeof(message),
                     "Arrays have no method '%.*s'. Array methods: push, pop, insert, remove, clear, contains, "
                     "index_of, reverse, sort, slice, join, copy, sum, min, max, length.",
                     length, name);
        } else if (receiver.type == VAL_STRING) {
            snprintf(message, sizeof(message),
                     "Strings have no method '%.*s'. String methods: upper, lower, trim, split, contains, find, "
                     "replace, starts_with, ends_with, slice, repeat, chars, code, reverse, length.",
                     length, name);
        } else {
            rt_expect_object(receiver, name, length, "methods", at);
            snprintf(message, sizeof(message), "Can't call '%.*s' here.", length, name);
        }
        rt_error(at, message);
    }
    return rt_builtin(id, args, argc, at);
}
