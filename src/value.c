#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include "value.h"
#include "ast.h"

/* Deeply nested (or self-containing) arrays print as "[...]" past this depth. */
#define MAX_PRINT_DEPTH 32

Value value_int(long integer) {
    Value value;
    value.type = VAL_INT;
    value.as.integer = integer;
    return value;
}

Value value_float(double number) {
    Value value;
    value.type = VAL_FLOAT;
    value.as.number = number;
    return value;
}

Value value_bool(int truth) {
    Value value;
    value.type = VAL_BOOL;
    value.as.integer = truth ? 1 : 0;
    return value;
}

Value value_null(void) {
    Value value;
    value.type = VAL_NULL;
    value.as.integer = 0;
    return value;
}

Value value_new_array(void) {
    Array *array = malloc(sizeof(Array));
    if (!array) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    array->refcount = 1;
    array->count = 0;
    array->capacity = 0;
    array->items = NULL;

    Value value;
    value.type = VAL_ARRAY;
    value.as.array = array;
    return value;
}

void array_append(Array *array, Value item) {
    if (array->count == array->capacity) {
        int capacity = array->capacity == 0 ? 8 : array->capacity * 2;
        Value *items = realloc(array->items, sizeof(Value) * (size_t)capacity);
        if (!items) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
        array->items = items;
        array->capacity = capacity;
    }
    array->items[array->count++] = item;
}

static int is_continuation_byte(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

Value value_new_string(const char *chars, int length) {
    String *string = malloc(sizeof(String) + (size_t)length + 1);
    if (!string) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    string->refcount = 1;
    string->length = length;
    memcpy(string->chars, chars, (size_t)length);
    string->chars[length] = '\0';

    string->count = 0;
    for (int i = 0; i < length; i++) {
        if (!is_continuation_byte(chars[i])) string->count++;
    }

    Value value;
    value.type = VAL_STRING;
    value.as.string = string;
    return value;
}

/* ---- text of values: one string builder shared by printing, '+' and string() ---- */

typedef struct {
    char *chars;
    int length;
    int capacity;
} Buf;

static void buf_add(Buf *b, const char *chars, int length) {
    if (length > INT_MAX - b->length - 1) {
        fprintf(stderr, "neplang: string too long\n");
        exit(1);
    }
    if (b->length + length + 1 > b->capacity) {
        int capacity = b->capacity ? b->capacity : 64;
        while (capacity < b->length + length + 1) capacity = capacity > INT_MAX / 2 ? INT_MAX : capacity * 2;
        char *grown = realloc(b->chars, (size_t)capacity);
        if (!grown) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
        b->chars = grown;
        b->capacity = capacity;
    }
    memcpy(b->chars + b->length, chars, (size_t)length);
    b->length += length;
    b->chars[b->length] = '\0';
}

static void buf_text(Buf *b, const char *text) {
    buf_add(b, text, (int)strlen(text));
}

void value_format_float(double number, char *buffer, int size) {
    if (isnan(number)) {
        snprintf(buffer, (size_t)size, "nan");
        return;
    }
    if (isinf(number)) {
        snprintf(buffer, (size_t)size, number < 0 ? "-inf" : "inf");
        return;
    }
    double magnitude = fabs(number);
    if (magnitude == 0.0 || (magnitude >= 1e-5 && magnitude < 1e16)) {
        /* Plain decimal notation with the fewest digits that read back exactly: 1000.0, 0.1, 3.14 */
        for (int decimals = 1; decimals <= 20; decimals++) {
            snprintf(buffer, (size_t)size, "%.*f", decimals, number);
            if (strtod(buffer, NULL) == number) return;
        }
        return;
    }
    /* Very large or very small: the shortest exponent form that reads back exactly (1e+20). */
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(buffer, (size_t)size, "%.*g", precision, number);
        if (strtod(buffer, NULL) == number) break;
    }
}

static void quote_into(Buf *b, String *string) {
    buf_add(b, "\"", 1);
    for (int i = 0; i < string->length; i++) {
        char c = string->chars[i];
        switch (c) {
            case '\n': buf_text(b, "\\n"); break;
            case '\t': buf_text(b, "\\t"); break;
            case '"':  buf_text(b, "\\\""); break;
            case '\\': buf_text(b, "\\\\"); break;
            default:   buf_add(b, &c, 1); break;
        }
    }
    buf_add(b, "\"", 1);
}

/* Strings nested inside an array or object are quoted, so ["a, b"] and ["a", "b"] look different. */
static void text_of(Buf *b, Value value, int depth) {
    char number[64];
    switch (value.type) {
        case VAL_INT:
            snprintf(number, sizeof(number), "%ld", value.as.integer);
            buf_text(b, number);
            break;
        case VAL_FLOAT:
            value_format_float(value.as.number, number, (int)sizeof(number));
            buf_text(b, number);
            break;
        case VAL_BOOL:
            buf_text(b, value.as.integer ? "sacho" : "jutho");
            break;
        case VAL_NULL:
            buf_text(b, "khali");
            break;
        case VAL_UNSET:
            break;
        case VAL_STRING:
            if (depth == 0) buf_add(b, value.as.string->chars, value.as.string->length);
            else quote_into(b, value.as.string);
            break;
        case VAL_OBJECT: {
            /* Manchhe(naam="Sita", umar=22) */
            Object *object = value.as.object;
            Token name = object->klass->as.class_decl.name;
            buf_add(b, name.start, name.length);
            buf_add(b, "(", 1);
            if (depth >= MAX_PRINT_DEPTH) {
                buf_text(b, "...)");
                break;
            }
            for (int i = 0; i < object->field_count; i++) {
                Token field = object->klass->as.class_decl.fields[i].name;
                if (i > 0) buf_text(b, ", ");
                buf_add(b, field.start, field.length);
                buf_add(b, "=", 1);
                text_of(b, object->fields[i], depth + 1);
            }
            buf_add(b, ")", 1);
            break;
        }
        case VAL_ARRAY:
            if (depth >= MAX_PRINT_DEPTH) {
                buf_text(b, "[...]");
                break;
            }
            buf_add(b, "[", 1);
            for (int i = 0; i < value.as.array->count; i++) {
                if (i > 0) buf_text(b, ", ");
                text_of(b, value.as.array->items[i], depth + 1);
            }
            buf_add(b, "]", 1);
            break;
    }
}

Value value_to_string(Value value) {
    if (value.type == VAL_STRING) return value_retain(value);
    Buf b = { NULL, 0, 0 };
    text_of(&b, value, 0);
    Value result = value_new_string(b.chars ? b.chars : "", b.length);
    free(b.chars);
    return result;
}

Value value_concat(Value left, Value right) {
    Buf b = { NULL, 0, 0 };
    text_of(&b, left, 0);
    text_of(&b, right, 0);
    Value result = value_new_string(b.chars ? b.chars : "", b.length);
    free(b.chars);
    return result;
}

Value string_char_at(String *string, int index) {
    /* All-ASCII strings (the common case) can be indexed directly. */
    if (string->count == string->length) {
        return value_new_string(string->chars + index, 1);
    }

    int offset = 0;
    for (int seen = 0; seen < index; seen++) {
        offset++;
        while (is_continuation_byte(string->chars[offset])) offset++;
    }
    int end = offset + 1;
    while (end < string->length && is_continuation_byte(string->chars[end])) end++;
    return value_new_string(string->chars + offset, end - offset);
}

Value value_new_object(const AstNode *klass) {
    int field_count = klass->as.class_decl.field_count;
    Object *object = malloc(sizeof(Object) + sizeof(Value) * (size_t)field_count);
    if (!object) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    object->refcount = 1;
    object->klass = klass;
    object->field_count = field_count;
    for (int i = 0; i < field_count; i++) {
        object->fields[i] = value_int(0);
    }

    Value value;
    value.type = VAL_OBJECT;
    value.as.object = object;
    return value;
}

Value value_retain(Value value) {
    if (value.type == VAL_ARRAY) {
        value.as.array->refcount++;
    } else if (value.type == VAL_STRING) {
        value.as.string->refcount++;
    } else if (value.type == VAL_OBJECT) {
        value.as.object->refcount++;
    }
    return value;
}

void value_release(Value value) {
    if (value.type == VAL_STRING) {
        if (--value.as.string->refcount == 0) free(value.as.string);
        return;
    }
    if (value.type == VAL_OBJECT) {
        Object *object = value.as.object;
        if (--object->refcount > 0) return;
        for (int i = 0; i < object->field_count; i++) {
            value_release(object->fields[i]);
        }
        free(object);
        return;
    }
    if (value.type != VAL_ARRAY) return;

    Array *array = value.as.array;
    if (--array->refcount > 0) return;

    for (int i = 0; i < array->count; i++) {
        value_release(array->items[i]);
    }
    free(array->items);
    free(array);
}

const char *value_type_name(Value value) {
    switch (value.type) {
        case VAL_INT:    return "a number";
        case VAL_ARRAY:  return "an array";
        case VAL_STRING: return "a string";
        case VAL_OBJECT: return "an object";
        case VAL_FLOAT:  return "a decimal number";
        case VAL_BOOL:   return "a bool (sacho/jutho)";
        case VAL_NULL:   return "khali";
        case VAL_UNSET:  return "an unassigned variable";
    }
    return "value";
}

void value_print(Value value) {
    if (value.type == VAL_STRING) {
        fwrite(value.as.string->chars, 1, (size_t)value.as.string->length, stdout);
        return;
    }
    Buf b = { NULL, 0, 0 };
    text_of(&b, value, 0);
    if (b.length > 0) fwrite(b.chars, 1, (size_t)b.length, stdout);
    free(b.chars);
}

int value_is_number(Value value) {
    return value.type == VAL_INT || value.type == VAL_FLOAT;
}

double value_as_double(Value value) {
    return value.type == VAL_INT ? (double)value.as.integer : value.as.number;
}

int value_equals(Value a, Value b) {
    if (value_is_number(a) && value_is_number(b)) {
        if (a.type == VAL_INT && b.type == VAL_INT) return a.as.integer == b.as.integer;
        return value_as_double(a) == value_as_double(b);
    }
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_BOOL: return a.as.integer == b.as.integer;
        case VAL_NULL: return 1;
        case VAL_STRING:
            return a.as.string->length == b.as.string->length &&
                   memcmp(a.as.string->chars, b.as.string->chars, (size_t)a.as.string->length) == 0;
        case VAL_ARRAY:
            if (a.as.array == b.as.array) return 1;
            if (a.as.array->count != b.as.array->count) return 0;
            for (int i = 0; i < a.as.array->count; i++) {
                if (!value_equals(a.as.array->items[i], b.as.array->items[i])) return 0;
            }
            return 1;
        case VAL_OBJECT: return a.as.object == b.as.object;
        default: return 0;
    }
}
