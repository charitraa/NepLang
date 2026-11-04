#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "rt.h"
#include "diagnostics.h"

_Noreturn void rt_error(Token at, const char *message) {
    fflush(stdout);
    if (at.start) {
        report_error("", "<program>", at, message);
    } else {
        fprintf(stderr, "Error at line %d, column %d:\n\n%s\n", at.line, at.col, message);
    }
    exit(1);
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

static Value string_binop(TokenType type, Value left, Value right, Token at) {
    char message[256];
    const char *text = op_text(type);

    if (type == TOKEN_PLUS) {
        Value other = left.type == VAL_STRING ? right : left;
        if (other.type != VAL_STRING && other.type != VAL_INT) {
            snprintf(message, sizeof(message),
                     "Can't use '+' with %s. '+' adds two numbers, or joins text when one side is a string.",
                     value_type_name(other));
            rt_error(at, message);
        }
        return value_concat(left, right);
    }
    if (is_comparison(type)) {
        if (left.type != right.type) {
            snprintf(message, sizeof(message),
                     "Can't compare %s with %s using '%s'. Both sides must be numbers, or both must be strings.",
                     value_type_name(left), value_type_name(right), text);
            rt_error(at, message);
        }
        int order = compare_strings(left.as.string, right.as.string);
        switch (type) {
            case TOKEN_EQUAL_EQUAL: return value_int(order == 0);
            case TOKEN_BANG_EQUAL:  return value_int(order != 0);
            case TOKEN_LESS:        return value_int(order < 0);
            case TOKEN_LESS_EQUAL:  return value_int(order <= 0);
            case TOKEN_GREATER:     return value_int(order > 0);
            default:                return value_int(order >= 0);
        }
    }
    snprintf(message, sizeof(message), "Can't use '%s' with a string. It only works on numbers.", text);
    rt_error(at, message);
}

Value rt_binop(TokenType op, Value left, Value right, Token at) {
    Value result;
    if (left.type == VAL_STRING || right.type == VAL_STRING) {
        result = string_binop(op, left, right, at);
        value_release(left);
        value_release(right);
        return result;
    }

    if (left.type != VAL_INT || right.type != VAL_INT) {
        char message[256];
        const char *operand = value_type_name(left.type != VAL_INT ? left : right);
        if (is_comparison(op)) {
            snprintf(message, sizeof(message), "Can't compare %s using '%s'. Only numbers and strings can be compared.",
                     operand, op_text(op));
        } else {
            snprintf(message, sizeof(message), "Can't use '%s' with %s. It only works on numbers.", op_text(op), operand);
        }
        rt_error(at, message);
    }

    long l = left.as.integer;
    long r = right.as.integer;
    switch (op) {
        case TOKEN_PLUS:          return value_int(l + r);
        case TOKEN_MINUS:         return value_int(l - r);
        case TOKEN_STAR:          return value_int(l * r);
        case TOKEN_EQUAL_EQUAL:   return value_int(l == r);
        case TOKEN_BANG_EQUAL:    return value_int(l != r);
        case TOKEN_LESS:          return value_int(l < r);
        case TOKEN_LESS_EQUAL:    return value_int(l <= r);
        case TOKEN_GREATER:       return value_int(l > r);
        case TOKEN_GREATER_EQUAL: return value_int(l >= r);
        case TOKEN_SLASH:
            if (r == 0) rt_error(at, "Division by zero.");
            return value_int(l / r);
        case TOKEN_PERCENT:
            if (r == 0) rt_error(at, "Modulo by zero.");
            return value_int(l % r);
        default:
            rt_error(at, "Internal error: unknown binary operator.");
    }
}

long rt_number(Value v, const char *what, Token at) {
    if (v.type != VAL_INT) {
        char message[256];
        snprintf(message, sizeof(message), "%s must be a number, but this is %s.", what, value_type_name(v));
        rt_error(at, message);
    }
    return v.as.integer;
}

Value rt_neg(Value v, Token at) {
    return value_int(-rt_number(v, "The value after a sign", at));
}

Value rt_pos(Value v, Token at) {
    return value_int(rt_number(v, "The value after a sign", at));
}

int rt_condition(Value v, Token at) {
    long n = rt_number(v, "A condition", at);
    return n != 0;
}

Value rt_not(Value v, Token at) {
    return value_int(!rt_condition(v, at));
}

Value rt_truth(Value v, Token at) {
    return value_int(rt_condition(v, at));
}

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

static void check_bounds(Value target, long index, Token at) {
    int count = target.type == VAL_STRING ? target.as.string->count : target.as.array->count;
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
    return value_int(v.type == VAL_STRING ? v.as.string->count : v.as.array->count);
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

static const struct { const char *name; int arity; } BUILTINS[RT_BUILTIN_COUNT] = {
    { "lambai", 1 }, { "thapa", 2 }, { "hata", 1 }, { "sankhya", 1 }, { "padha", 0 },
};

int rt_builtin_id(const char *name, int length) {
    for (int i = 0; i < RT_BUILTIN_COUNT; i++) {
        if ((int)strlen(BUILTINS[i].name) == length && strncmp(BUILTINS[i].name, name, (size_t)length) == 0) return i;
    }
    return -1;
}

int rt_builtin_arity(int id) {
    return BUILTINS[id].arity;
}

Value rt_builtin(int id, Value *args, Token at) {
    char message[256];
    switch (id) {
        case RT_LAMBAI: {
            Value arg = args[0];
            if (arg.type != VAL_ARRAY && arg.type != VAL_STRING) {
                snprintf(message, sizeof(message), "'lambai' needs an array or a string, but this is %s.",
                         value_type_name(arg));
                rt_error(at, message);
            }
            long length = arg.type == VAL_STRING ? arg.as.string->count : arg.as.array->count;
            value_release(arg);
            return value_int(length);
        }
        case RT_THAPA: {
            if (args[0].type != VAL_ARRAY) {
                snprintf(message, sizeof(message), "'thapa' needs an array as its first argument, but this is %s.",
                         value_type_name(args[0]));
                rt_error(at, message);
            }
            array_append(args[0].as.array, args[1]);
            value_release(args[0]);
            return value_int(0);
        }
        case RT_HATA: {
            if (args[0].type != VAL_ARRAY) {
                snprintf(message, sizeof(message), "'hata' needs an array, but this is %s.", value_type_name(args[0]));
                rt_error(at, message);
            }
            if (args[0].as.array->count == 0) rt_error(at, "Can't use 'hata' on an empty array.");
            Value item = args[0].as.array->items[--args[0].as.array->count];
            value_release(args[0]);
            return item;
        }
        case RT_SANKHYA: {
            Value arg = args[0];
            if (arg.type == VAL_INT) return arg;
            if (arg.type != VAL_STRING) {
                snprintf(message, sizeof(message), "'sankhya' needs a string, but this is %s.", value_type_name(arg));
                rt_error(at, message);
            }
            char *end = NULL;
            errno = 0;
            const char *text = arg.as.string->chars;
            while (*text == ' ' || *text == '\t') text++;
            long number = strtol(text, &end, 10);
            while (end && (*end == ' ' || *end == '\t')) end++;
            if (end == text || *end != '\0' || errno == ERANGE) {
                snprintf(message, sizeof(message), "Can't turn \"%s\" into a number.", arg.as.string->chars);
                rt_error(at, message);
            }
            value_release(arg);
            return value_int(number);
        }
        default: {
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
    }
}
