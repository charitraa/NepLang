#ifndef NEPLANG_VALUE_H
#define NEPLANG_VALUE_H

typedef enum {
    VAL_INT,
    VAL_ARRAY,
    VAL_STRING,
    VAL_OBJECT,
    VAL_FLOAT,  /* a decimal number (double) */
    VAL_BOOL,   /* sacho / jutho; stored in as.integer as 1 / 0 */
    VAL_NULL,   /* khali */
    VAL_UNSET   /* a compiled-code variable slot that has not been assigned yet; never a user-visible value */
} ValueType;

typedef struct Array Array;
typedef struct String String;
typedef struct Object Object;
struct AstNode;

typedef struct {
    ValueType type;
    union {
        long integer;
        double number;
        Array *array;
        String *string;
        Object *object;
    } as;
} Value;

/* Arrays are reference-counted and shared: assigning an array or passing it to a
 * function shares the same Array (like Python/Java), it does not copy it.
 * A cycle (an array that contains itself) is never freed; there is no cycle collector. */
struct Array {
    int refcount;
    int count;
    int capacity;
    Value *items;
};

/* Strings are immutable and reference-counted. `chars` holds UTF-8 and is NUL-terminated
 * (the NUL is not counted in `length`). Indexing and lambai() work on characters
 * (code points), not bytes, so "नमस्ते"[0] is "न". */
struct String {
    int refcount;
    int length;     /* in bytes */
    int count;      /* in characters (UTF-8 code points) */
    char chars[];
};

/* An instance of a 'class'. Shared by reference, like arrays. `klass` is the class's
 * AST_CLASS declaration, which outlives every object; fields[i] holds the value of the
 * class's i-th declared field. */
struct Object {
    int refcount;
    const struct AstNode *klass;
    int field_count;
    Value fields[];
};

/* Ownership convention: a function returning a Value hands the caller one reference.
 * Whoever holds a Value must eventually pass it to value_release(). */
Value value_int(long integer);
Value value_float(double number);
Value value_bool(int truth);
Value value_null(void);
Value value_new_array(void);
void array_append(Array *array, Value item);  /* takes ownership of item */
Value value_new_string(const char *chars, int length);  /* copies `chars` */
Value value_concat(Value left, Value right);  /* a string from two strings, or a string and a number */
Value string_char_at(String *string, int index);  /* 0 <= index < count; one-character string */
Value value_new_object(const struct AstNode *klass);  /* every field starts as 0 */

Value value_retain(Value value);
void value_release(Value value);

const char *value_type_name(Value value);  /* "a number", "an array", "a string", ... for error messages */
void value_print(Value value);
/* Shortest text that reads back as the same double, always with a '.' or exponent ("2.0", "0.1"). */
void value_format_float(double number, char *buffer, int size);
/* Text of a value as '+' and string() show it (strings unquoted); a new string. Borrows `value`. */
Value value_to_string(Value value);
/* Structural equality used by ==, contains() and index_of(): numbers by value (1 == 1.0), strings by
 * content, bools, khali, and arrays item by item; objects only by identity. */
int value_equals(Value a, Value b);
int value_is_number(Value value);   /* VAL_INT or VAL_FLOAT */
double value_as_double(Value value); /* VAL_INT or VAL_FLOAT */

#endif
