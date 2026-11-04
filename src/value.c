#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
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

/* Writes the text of a number or string operand of '+' into `buffer` (numbers) or
 * points at the string's own bytes. */
static const char *concat_operand(Value value, char *buffer, size_t size, int *length) {
    if (value.type == VAL_STRING) {
        *length = value.as.string->length;
        return value.as.string->chars;
    }
    *length = snprintf(buffer, size, "%ld", value.as.integer);
    return buffer;
}

Value value_concat(Value left, Value right) {
    char left_buffer[32], right_buffer[32];
    int left_length, right_length;
    const char *left_chars = concat_operand(left, left_buffer, sizeof(left_buffer), &left_length);
    const char *right_chars = concat_operand(right, right_buffer, sizeof(right_buffer), &right_length);

    if (left_length > INT_MAX - right_length) {
        fprintf(stderr, "neplang: string too long\n");
        exit(1);
    }
    char *chars = malloc((size_t)left_length + (size_t)right_length);
    if (!chars && left_length + right_length > 0) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    if (left_length > 0) memcpy(chars, left_chars, (size_t)left_length);
    if (right_length > 0) memcpy(chars + left_length, right_chars, (size_t)right_length);

    Value result = value_new_string(chars, left_length + right_length);
    free(chars);
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
        case VAL_UNSET:  return "an unassigned variable";
    }
    return "value";
}

/* Strings inside an array print quoted, with escapes, so ["a, b"] and ["a", "b"] look different. */
static void print_quoted(String *string) {
    putchar('"');
    for (int i = 0; i < string->length; i++) {
        char c = string->chars[i];
        switch (c) {
            case '\n': printf("\\n"); break;
            case '\t': printf("\\t"); break;
            case '"':  printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            default:   putchar(c); break;
        }
    }
    putchar('"');
}

static void print_value(Value value, int depth) {
    switch (value.type) {
        case VAL_INT:
            printf("%ld", value.as.integer);
            break;

        case VAL_STRING:
            if (depth == 0) {
                fwrite(value.as.string->chars, 1, (size_t)value.as.string->length, stdout);
            } else {
                print_quoted(value.as.string);
            }
            break;

        case VAL_UNSET:
            break;

        case VAL_OBJECT: {
            /* Manchhe(naam="Sita", umar=22) */
            Object *object = value.as.object;
            Token name = object->klass->as.class_decl.name;
            printf("%.*s(", name.length, name.start);
            if (depth >= MAX_PRINT_DEPTH) {
                printf("...)");
                break;
            }
            for (int i = 0; i < object->field_count; i++) {
                Token field = object->klass->as.class_decl.fields[i].name;
                if (i > 0) printf(", ");
                printf("%.*s=", field.length, field.start);
                print_value(object->fields[i], depth + 1);
            }
            printf(")");
            break;
        }

        case VAL_ARRAY:
            if (depth >= MAX_PRINT_DEPTH) {
                printf("[...]");
                break;
            }
            printf("[");
            for (int i = 0; i < value.as.array->count; i++) {
                if (i > 0) printf(", ");
                print_value(value.as.array->items[i], depth + 1);
            }
            printf("]");
            break;
    }
}

void value_print(Value value) {
    print_value(value, 0);
}
