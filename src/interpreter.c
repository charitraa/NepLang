#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include "interpreter.h"
#include "diagnostics.h"
#include "value.h"

/* Deep enough for normal recursion, shallow enough to fail with a clear message
 * long before the C stack overflows. */
#define MAX_CALL_DEPTH 1000

/* Variables live in hashed scopes (see Scope). Names are slices into the source buffer.
 * Each Variable owns one reference to its value. */
typedef struct {
    const char *name;
    int length;
    Value value;
} Variable;

/* A scope is a hashed symbol table: `vars` holds the variables densely in creation order, and
 * `slots` is an open-addressing hash table (power-of-two size, linear probing) of indexes into it. */
typedef struct {
    Variable *vars;
    int count;
    int capacity;
    int *slots;
    int slot_count;
} Scope;

typedef struct {
    Scope globals;
    Scope *locals;          /* the current function call's scope; NULL at the top level */
    AstNode **functions;    /* AST_FUNCTION nodes, registered before execution starts */
    int function_count;
    int function_capacity;
    AstNode **classes;      /* AST_CLASS nodes, registered before execution starts */
    int class_count;
    int class_capacity;
    Object *self;           /* the object whose method is running ('yo'); NULL outside methods */
    int call_depth;
    int breaking;           /* set by 'ruk'; unwinds blocks up to the enclosing loop */
    int continuing;         /* set by 'agadi'; unwinds blocks up to the enclosing loop */
    int returning;          /* set by 'farkau'; unwinds blocks and loops up to the call */
    Value return_value;     /* owned while `returning` is set */
    const char *source;
    const char *filename;
} Interpreter;

#define UNWINDING(interp) ((interp)->returning || (interp)->breaking || (interp)->continuing)

static void runtime_error(Interpreter *interp, Token token, const char *message) {
    if (token.start) {
        report_error(interp->source, interp->filename, token, message);
    } else {
        fprintf(stderr, "Runtime error: %s\n", message);
    }
    exit(1);
}

static int same_name(const char *name, int length, Token token) {
    return length == token.length && strncmp(name, token.start, (size_t)length) == 0;
}

/* FNV-1a hash of a name. */
static unsigned hash_name(const char *name, int length) {
    unsigned h = 2166136261u;
    for (int i = 0; i < length; i++) {
        h ^= (unsigned char)name[i];
        h *= 16777619u;
    }
    return h;
}

/* Lookup: probe the open-addressing table `slots` (each entry is an index into `vars`, or -1).
 * Returns the slot that holds the name, or the empty slot where it would be inserted. */
static int scope_probe(const Scope *scope, const char *name, int length) {
    unsigned mask = (unsigned)scope->slot_count - 1;
    unsigned i = hash_name(name, length) & mask;
    while (scope->slots[i] != -1) {
        const Variable *var = &scope->vars[scope->slots[i]];
        if (var->length == length && strncmp(var->name, name, (size_t)length) == 0) break;
        i = (i + 1) & mask;
    }
    return (int)i;
}

static void scope_out_of_memory(void) {
    fprintf(stderr, "neplang: out of memory\n");
    exit(1);
}

static Variable *scope_find(Scope *scope, Token name) {
    if (scope->count == 0) return NULL;
    int slot = scope_probe(scope, name.start, name.length);
    return scope->slots[slot] == -1 ? NULL : &scope->vars[scope->slots[slot]];
}

/* Rebuilds the hash table with room for `slot_count` entries (a power of two). */
static void scope_rehash(Scope *scope, int slot_count) {
    int *slots = malloc(sizeof(int) * (size_t)slot_count);
    if (!slots) scope_out_of_memory();
    for (int i = 0; i < slot_count; i++) slots[i] = -1;
    free(scope->slots);
    scope->slots = slots;
    scope->slot_count = slot_count;
    for (int i = 0; i < scope->count; i++) {
        scope->slots[scope_probe(scope, scope->vars[i].name, scope->vars[i].length)] = i;
    }
}

/* Takes ownership of `value`. */
static void scope_set(Scope *scope, Token name, Value value) {
    Variable *var = scope_find(scope, name);
    if (var) {
        value_release(var->value);
        var->value = value;
        return;
    }

    if (scope->count == scope->capacity) {
        int capacity = scope->capacity == 0 ? 8 : scope->capacity * 2;
        Variable *vars = realloc(scope->vars, sizeof(Variable) * (size_t)capacity);
        if (!vars) scope_out_of_memory();
        scope->vars = vars;
        scope->capacity = capacity;
    }
    /* Keep the table at most half full so probes stay short. */
    if (scope->slot_count < scope->capacity * 2) scope_rehash(scope, scope->capacity * 2);

    var = &scope->vars[scope->count];
    var->name = name.start;
    var->length = name.length;
    var->value = value;
    scope->slots[scope_probe(scope, name.start, name.length)] = scope->count;
    scope->count++;
}

static void scope_free(Scope *scope) {
    for (int i = 0; i < scope->count; i++) {
        value_release(scope->vars[i].value);
    }
    free(scope->vars);
    free(scope->slots);
}

/* Reads look in the current function's locals first, then in globals. */
static Variable *find_variable(Interpreter *interp, Token name) {
    if (interp->locals) {
        Variable *var = scope_find(interp->locals, name);
        if (var) return var;
    }
    return scope_find(&interp->globals, name);
}

/* Inside a function, assignment always creates/updates a local: a function can read
 * globals but never rebinds them. Takes ownership of `value`. */
static void set_variable(Interpreter *interp, Token name, Value value) {
    scope_set(interp->locals ? interp->locals : &interp->globals, name, value);
}

/* Inside a method, a bare field name means that field of 'yo' (unless a parameter or
 * loop variable has the same name), so the method can be written as in the class's
 * own description. Returns the field's index in `interp->self`, or -1. */
static int field_index(const AstNode *klass, Token name);

static int self_field_index(Interpreter *interp, Token name) {
    if (!interp->self) return -1;
    if (interp->locals && scope_find(interp->locals, name)) return -1;
    return field_index(interp->self->klass, name);
}

/* 'name = value' -- takes ownership of `value`. */
static void assign_variable(Interpreter *interp, Token name, Value value) {
    int index = self_field_index(interp, name);
    if (index >= 0) {
        value_release(interp->self->fields[index]);
        interp->self->fields[index] = value;
        return;
    }
    set_variable(interp, name, value);
}

static int is_comparison(TokenType type) {
    return type == TOKEN_EQUAL_EQUAL || type == TOKEN_BANG_EQUAL ||
           type == TOKEN_LESS || type == TOKEN_LESS_EQUAL ||
           type == TOKEN_GREATER || type == TOKEN_GREATER_EQUAL;
}

static const char *const BUILTINS[] = { "lambai", "thapa", "hata", "sankhya", "padha" };

static int is_builtin(Token name) {
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        if (same_name(BUILTINS[i], (int)strlen(BUILTINS[i]), name)) return 1;
    }
    return 0;
}

static AstNode *find_function(Interpreter *interp, Token name) {
    for (int i = 0; i < interp->function_count; i++) {
        Token fn = interp->functions[i]->as.function.name;
        if (same_name(fn.start, fn.length, name)) {
            return interp->functions[i];
        }
    }
    return NULL;
}

static AstNode *find_class(Interpreter *interp, Token name) {
    for (int i = 0; i < interp->class_count; i++) {
        Token klass = interp->classes[i]->as.class_decl.name;
        if (same_name(klass.start, klass.length, name)) {
            return interp->classes[i];
        }
    }
    return NULL;
}

static int field_index(const AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        Token field = klass->as.class_decl.fields[i].name;
        if (same_name(field.start, field.length, name)) return i;
    }
    return -1;
}

static AstNode *find_method(const AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.method_count; i++) {
        Token method = klass->as.class_decl.methods[i]->as.function.name;
        if (same_name(method.start, method.length, name)) return klass->as.class_decl.methods[i];
    }
    return NULL;
}

/* Functions and classes share one namespace: 'Manchhe(...)' must mean exactly one thing. */
static void check_new_global_name(Interpreter *interp, Token name) {
    char message[256];

    if (is_builtin(name)) {
        snprintf(message, sizeof(message), "'%.*s' is a built-in function and can't be redefined. Choose another name.",
                 name.length, name.start);
        runtime_error(interp, name, message);
    }

    AstNode *function = find_function(interp, name);
    if (function) {
        snprintf(message, sizeof(message), "Function '%.*s' is already defined on line %d. Give this one a different name.",
                 name.length, name.start, function->as.function.name.line);
        runtime_error(interp, name, message);
    }

    AstNode *klass = find_class(interp, name);
    if (klass) {
        snprintf(message, sizeof(message), "Class '%.*s' is already defined on line %d. Give this one a different name.",
                 name.length, name.start, klass->as.class_decl.name.line);
        runtime_error(interp, name, message);
    }
}

static void register_class(Interpreter *interp, AstNode *klass) {
    check_new_global_name(interp, klass->as.class_decl.name);

    if (interp->class_count == interp->class_capacity) {
        int capacity = interp->class_capacity == 0 ? 8 : interp->class_capacity * 2;
        AstNode **classes = realloc(interp->classes, sizeof(AstNode *) * (size_t)capacity);
        if (!classes) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
        interp->classes = classes;
        interp->class_capacity = capacity;
    }
    interp->classes[interp->class_count++] = klass;
}

static void register_function(Interpreter *interp, AstNode *function) {
    check_new_global_name(interp, function->as.function.name);

    if (interp->function_count == interp->function_capacity) {
        int capacity = interp->function_capacity == 0 ? 8 : interp->function_capacity * 2;
        AstNode **functions = realloc(interp->functions, sizeof(AstNode *) * (size_t)capacity);
        if (!functions) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
        interp->functions = functions;
        interp->function_capacity = capacity;
    }
    interp->functions[interp->function_count++] = function;
}

static Value eval(Interpreter *interp, AstNode *node);
static void execute(Interpreter *interp, AstNode *node);

/* Evaluates `node` and requires a number. `what` completes "... must be a number". */
static long eval_int(Interpreter *interp, AstNode *node, const char *what) {
    Value value = eval(interp, node);
    if (value.type != VAL_INT) {
        char message[256];
        snprintf(message, sizeof(message), "%s must be a number, but this is %s.", what, value_type_name(value));
        runtime_error(interp, node->token, message);
    }
    return value.as.integer;
}

static long eval_condition(Interpreter *interp, AstNode *node) {
    /* No boolean type yet: any non-zero number counts as true. */
    return eval_int(interp, node, "A condition") != 0;
}

/* Bounds-checks `index` against an array or string of `count` items/characters;
 * `at` is the '[' token. */
static void check_index(Interpreter *interp, Value target, long index, Token at) {
    int count = target.type == VAL_STRING ? target.as.string->count : target.as.array->count;
    if (index >= 0 && index < count) return;

    const char *kind = target.type == VAL_STRING ? "string" : "array";
    const char *unit = target.type == VAL_STRING ? "character" : "item";
    char message[256];
    if (count == 0) {
        snprintf(message, sizeof(message), "Index %ld is out of range: this %s is empty.", index, kind);
    } else {
        snprintf(message, sizeof(message),
                 "Index %ld is out of range. This %s has %d %s%s, so valid indexes are 0 to %d.",
                 index, kind, count, unit, count == 1 ? "" : "s", count - 1);
    }
    runtime_error(interp, at, message);
}

/* Evaluates the value being indexed in `index_node` (an AST_INDEX): an array or a string.
 * Returns an owned reference. */
static Value eval_indexed(Interpreter *interp, AstNode *index_node) {
    Value target = eval(interp, index_node->as.index.target);
    if (target.type != VAL_ARRAY && target.type != VAL_STRING) {
        char message[256];
        snprintf(message, sizeof(message), "Can't use [ ] on %s. Only arrays and strings can be indexed.",
                 value_type_name(target));
        runtime_error(interp, index_node->token, message);
    }
    return target;
}

static void builtin_arity(Interpreter *interp, AstNode *call, int expected, const char *usage) {
    if (call->as.call.arg_count == expected) return;
    char message[256];
    snprintf(message, sizeof(message), "'%.*s' takes %d argument%s (%s), but %d %s given here.",
             call->as.call.name.length, call->as.call.name.start, expected, expected == 1 ? "" : "s", usage,
             call->as.call.arg_count, call->as.call.arg_count == 1 ? "was" : "were");
    runtime_error(interp, call->as.call.name, message);
}

/* Evaluates argument `i` and insists on a type; the caller releases the result. */
static Value builtin_arg(Interpreter *interp, AstNode *call, int i, ValueType type, const char *what) {
    AstNode *arg_node = call->as.call.args[i];
    Value arg = eval(interp, arg_node);
    if (arg.type != type) {
        char message[256];
        snprintf(message, sizeof(message), "'%.*s' needs %s, but this is %s.", call->as.call.name.length,
                 call->as.call.name.start, what, value_type_name(arg));
        runtime_error(interp, arg_node->token, message);
    }
    return arg;
}

static Value call_builtin(Interpreter *interp, AstNode *call) {
    Token name = call->as.call.name;
    char message[256];

    if (same_name("lambai", 6, name)) {
        /* lambai(array) -> number of items; lambai(string) -> number of characters */
        builtin_arity(interp, call, 1, "an array or a string");
        AstNode *arg_node = call->as.call.args[0];
        Value arg = eval(interp, arg_node);
        if (arg.type != VAL_ARRAY && arg.type != VAL_STRING) {
            snprintf(message, sizeof(message), "'lambai' needs an array or a string, but this is %s.",
                     value_type_name(arg));
            runtime_error(interp, arg_node->token, message);
        }
        long length = arg.type == VAL_STRING ? arg.as.string->count : arg.as.array->count;
        value_release(arg);
        return value_int(length);
    }

    if (same_name("thapa", 5, name)) {
        /* thapa(array, value): add to the end (the array is shared, so callers see it) */
        builtin_arity(interp, call, 2, "an array and a value");
        Value array = builtin_arg(interp, call, 0, VAL_ARRAY, "an array as its first argument");
        Value item = eval(interp, call->as.call.args[1]);
        array_append(array.as.array, item);
        value_release(array);
        return value_int(0);
    }

    if (same_name("hata", 4, name)) {
        /* hata(array): remove and return the last item */
        builtin_arity(interp, call, 1, "an array");
        Value array = builtin_arg(interp, call, 0, VAL_ARRAY, "an array");
        if (array.as.array->count == 0) {
            runtime_error(interp, name, "Can't use 'hata' on an empty array.");
        }
        Value item = array.as.array->items[--array.as.array->count];
        value_release(array);
        return item;
    }

    if (same_name("sankhya", 7, name)) {
        /* sankhya(string) or sankhya(number): convert text such as "42" to a number */
        builtin_arity(interp, call, 1, "a string");
        AstNode *arg_node = call->as.call.args[0];
        Value arg = eval(interp, arg_node);
        if (arg.type == VAL_INT) return arg;
        if (arg.type != VAL_STRING) {
            snprintf(message, sizeof(message), "'sankhya' needs a string, but this is %s.", value_type_name(arg));
            runtime_error(interp, arg_node->token, message);
        }
        char *end = NULL;
        errno = 0;
        const char *text = arg.as.string->chars;
        while (*text == ' ' || *text == '\t') text++;
        long number = strtol(text, &end, 10);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end == text || *end != '\0' || errno == ERANGE) {
            snprintf(message, sizeof(message), "Can't turn \"%s\" into a number.", arg.as.string->chars);
            runtime_error(interp, arg_node->token, message);
        }
        value_release(arg);
        return value_int(number);
    }

    /* padha(): read one line from the keyboard as a string (empty at end of input) */
    builtin_arity(interp, call, 0, "nothing");
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

/* Errors unless `call` passes exactly `param_count` arguments. The callee is described as
 * before + label + after, e.g. "Function '" "jamma" "'" or "'" "Manchhe" "(...)'". */
static void check_arg_count(Interpreter *interp, AstNode *call, int param_count,
                            const char *before, Token label, const char *after) {
    int arg_count = call->as.call.arg_count;
    if (arg_count == param_count) return;

    char message[256];
    snprintf(message, sizeof(message), "%s%.*s%s takes %d argument%s, but %d %s given here.",
             before, label.length, label.start, after, param_count, param_count == 1 ? "" : "s",
             arg_count, arg_count == 1 ? "was" : "were");
    runtime_error(interp, call->as.call.name, message);
}

/* Runs `function` with the arguments of `call` (already arity-checked). `self` is the
 * object for a method, or NULL for a plain function. */
static Value invoke(Interpreter *interp, AstNode *function, AstNode *call, Object *self) {
    Token name = function->as.function.name;
    if (interp->call_depth >= MAX_CALL_DEPTH) {
        char message[256];
        snprintf(message, sizeof(message),
                 "Too many nested function calls (limit %d). Does '%.*s' call itself without ever stopping?",
                 MAX_CALL_DEPTH, name.length, name.start);
        runtime_error(interp, call->as.call.name, message);
    }

    /* Arguments are evaluated in the caller's scope, before the new scope exists. */
    Scope frame = { NULL, 0, 0, NULL, 0 };
    for (int i = 0; i < call->as.call.arg_count; i++) {
        Value value = eval(interp, call->as.call.args[i]);
        scope_set(&frame, function->as.function.params[i], value);
    }

    Scope *saved_locals = interp->locals;
    Object *saved_self = interp->self;
    interp->locals = &frame;
    interp->self = self;
    interp->call_depth++;

    execute(interp, function->as.function.body);

    /* A function that ends without 'farkau' (or with a bare 'farkau') returns 0. */
    Value result = interp->returning ? interp->return_value : value_int(0);
    interp->returning = 0;
    interp->return_value = value_int(0);

    interp->call_depth--;
    interp->locals = saved_locals;
    interp->self = saved_self;
    scope_free(&frame);
    return result;
}

static int is_constructor(Token name) {
    return same_name("banau", 5, name);
}

static int is_private(Token name) {
    return name.start[0] == '_';
}

/* Members whose names start with '_' can only be used inside the methods of their own class. */
static void check_access(Interpreter *interp, Object *object, Token member, const char *kind) {
    if (!is_private(member) || (interp->self && interp->self->klass == object->klass)) return;

    Token klass = object->klass->as.class_decl.name;
    char message[256];
    snprintf(message, sizeof(message),
             "The %s '%.*s' is private to class '%.*s' (its name starts with '_'), so it can only be used "
             "inside %.*s's own methods.",
             kind, member.length, member.start, klass.length, klass.start, klass.length, klass.start);
    runtime_error(interp, member, message);
}

/* Errors unless `value` is an object; `what` is "fields" or "methods". */
static Object *expect_object(Interpreter *interp, Value value, Token member, const char *what) {
    if (value.type == VAL_OBJECT) return value.as.object;

    char message[256];
    snprintf(message, sizeof(message), "Can't use '.%.*s' on %s. Only objects have %s.",
             member.length, member.start, value_type_name(value), what);
    runtime_error(interp, member, message);
    return NULL;
}

static Value call_method(Interpreter *interp, Object *object, AstNode *method, AstNode *call) {
    Token name = call->as.call.name;
    Token klass = object->klass->as.class_decl.name;
    if (is_constructor(name)) {
        char message[256];
        snprintf(message, sizeof(message),
                 "'banau' runs by itself when an object is created with '%.*s(...)'. It can't be called directly.",
                 klass.length, klass.start);
        runtime_error(interp, name, message);
    }
    check_arg_count(interp, call, method->as.function.param_count, "Method '", name, "'");
    return invoke(interp, method, call, object);
}

/* obj.name(args) */
static Value call_on_receiver(Interpreter *interp, AstNode *call) {
    Token name = call->as.call.name;
    Value receiver = eval(interp, call->as.call.receiver);
    Object *object = expect_object(interp, receiver, name, "methods");

    AstNode *method = find_method(object->klass, name);
    if (!method) {
        Token klass = object->klass->as.class_decl.name;
        char message[256];
        if (field_index(object->klass, name) >= 0) {
            snprintf(message, sizeof(message), "'%.*s' is a field of '%.*s', not a method. Use it without '( )'.",
                     name.length, name.start, klass.length, klass.start);
        } else {
            snprintf(message, sizeof(message), "Class '%.*s' has no method named '%.*s'.",
                     klass.length, klass.start, name.length, name.start);
        }
        runtime_error(interp, name, message);
    }
    check_access(interp, object, name, "method");

    Value result = call_method(interp, object, method, call);
    value_release(receiver);
    return result;
}

/* Klass(args): a new object with every field at its starting value, then 'banau' (if any). */
static Value construct(Interpreter *interp, AstNode *klass, AstNode *call) {
    Token name = klass->as.class_decl.name;
    AstNode *constructor = find_method(klass, (Token){ .start = "banau", .length = 5 });

    if (constructor) {
        check_arg_count(interp, call, constructor->as.function.param_count, "'", name, "(...)'");
    } else if (call->as.call.arg_count != 0) {
        char message[256];
        snprintf(message, sizeof(message),
                 "'%.*s(...)' takes no arguments, because class '%.*s' has no 'banau'. "
                 "Add 'kaam banau(...) { ... }' to the class to accept arguments.",
                 name.length, name.start, name.length, name.start);
        runtime_error(interp, call->as.call.name, message);
    }

    /* Starting values are evaluated afresh for every object (so 'items = []' isn't shared),
     * as if at the top level: they can see global variables, but not the caller's locals. */
    Value value = value_new_object(klass);
    Object *object = value.as.object;
    Scope *saved_locals = interp->locals;
    Object *saved_self = interp->self;
    interp->locals = NULL;
    interp->self = NULL;
    for (int i = 0; i < object->field_count; i++) {
        object->fields[i] = eval(interp, klass->as.class_decl.fields[i].value);
    }
    interp->locals = saved_locals;
    interp->self = saved_self;

    if (constructor) {
        value_release(invoke(interp, constructor, call, object));
    }
    return value;
}

static Value call_function(Interpreter *interp, AstNode *call) {
    Token name = call->as.call.name;

    if (call->as.call.receiver) {
        return call_on_receiver(interp, call);
    }

    if (is_builtin(name)) {
        return call_builtin(interp, call);
    }

    /* Inside a method, 'bolau()' calls the method 'bolau' of the same object. */
    if (interp->self) {
        AstNode *method = find_method(interp->self->klass, name);
        if (method) return call_method(interp, interp->self, method, call);
    }

    AstNode *klass = find_class(interp, name);
    if (klass) {
        return construct(interp, klass, call);
    }

    AstNode *function = find_function(interp, name);
    if (!function) {
        char message[256];
        snprintf(message, sizeof(message), "Function '%.*s' is not defined. Define it with 'kaam %.*s(...) { ... }'.",
                 name.length, name.start, name.length, name.start);
        runtime_error(interp, name, message);
    }

    check_arg_count(interp, call, function->as.function.param_count, "Function '", name, "'");
    return invoke(interp, function, call, NULL);
}

/* Evaluates the object in `field_node` (an AST_FIELD) and finds the field's index.
 * Returns an owned reference to the object. */
static Object *eval_field_target(Interpreter *interp, AstNode *field_node, int *index) {
    Token name = field_node->as.field.name;
    Value value = eval(interp, field_node->as.field.object);
    Object *object = expect_object(interp, value, name, "fields");

    *index = field_index(object->klass, name);
    if (*index < 0) {
        Token klass = object->klass->as.class_decl.name;
        char message[256];
        if (find_method(object->klass, name)) {
            snprintf(message, sizeof(message), "'%.*s' is a method of '%.*s'. Call it with '( )', e.g. '.%.*s()'.",
                     name.length, name.start, klass.length, klass.start, name.length, name.start);
        } else {
            snprintf(message, sizeof(message),
                     "Class '%.*s' has no field named '%.*s'. Fields must be listed in the class, e.g. '%.*s = 0'.",
                     klass.length, klass.start, name.length, name.start, name.length, name.start);
        }
        runtime_error(interp, name, message);
    }
    check_access(interp, object, name, "field");
    return object;
}

/* Byte-wise comparison: dictionary order for English text ("apple" < "banana"),
 * with every uppercase letter before every lowercase one. */
static int compare_strings(String *a, String *b) {
    int shorter = a->length < b->length ? a->length : b->length;
    int result = memcmp(a->chars, b->chars, (size_t)shorter);
    if (result != 0) return result;
    return (a->length > b->length) - (a->length < b->length);
}

static Value eval_string_binop(Interpreter *interp, AstNode *node, Value left, Value right) {
    Token op = node->token;
    TokenType type = node->as.binop.op;
    char message[256];

    if (type == TOKEN_PLUS) {
        Value other = left.type == VAL_STRING ? right : left;
        if (other.type != VAL_STRING && other.type != VAL_INT) {
            snprintf(message, sizeof(message),
                     "Can't use '+' with %s. '+' adds two numbers, or joins text when one side is a string.",
                     value_type_name(other));
            runtime_error(interp, op, message);
        }
        return value_concat(left, right);
    }

    if (is_comparison(type)) {
        if (left.type != right.type) {
            snprintf(message, sizeof(message),
                     "Can't compare %s with %s using '%.*s'. Both sides must be numbers, or both must be strings.",
                     value_type_name(left), value_type_name(right), op.length, op.start);
            runtime_error(interp, op, message);
        }
        int order = compare_strings(left.as.string, right.as.string);
        switch (type) {
            case TOKEN_EQUAL_EQUAL:   return value_int(order == 0);
            case TOKEN_BANG_EQUAL:    return value_int(order != 0);
            case TOKEN_LESS:          return value_int(order < 0);
            case TOKEN_LESS_EQUAL:    return value_int(order <= 0);
            case TOKEN_GREATER:       return value_int(order > 0);
            default:                  return value_int(order >= 0);
        }
    }

    snprintf(message, sizeof(message), "Can't use '%.*s' with a string. It only works on numbers.", op.length, op.start);
    runtime_error(interp, op, message);
    return value_int(0);
}

static Value eval_binop(Interpreter *interp, AstNode *node) {
    if (node->as.binop.op == TOKEN_RA || node->as.binop.op == TOKEN_WA) {
        /* Short-circuit: the right side only runs when the left side doesn't decide the answer. */
        int left_true = eval_condition(interp, node->as.binop.left);
        if (node->as.binop.op == TOKEN_RA ? !left_true : left_true) {
            return value_int(left_true);
        }
        return value_int(eval_condition(interp, node->as.binop.right) != 0);
    }

    Value left = eval(interp, node->as.binop.left);
    Value right = eval(interp, node->as.binop.right);
    Token op = node->token;

    if (left.type == VAL_STRING || right.type == VAL_STRING) {
        Value result = eval_string_binop(interp, node, left, right);
        value_release(left);
        value_release(right);
        return result;
    }

    if (left.type != VAL_INT || right.type != VAL_INT) {
        char message[256];
        const char *operand = value_type_name(left.type != VAL_INT ? left : right);
        if (is_comparison(node->as.binop.op)) {
            snprintf(message, sizeof(message), "Can't compare %s using '%.*s'. Only numbers and strings can be compared.",
                     operand, op.length, op.start);
        } else {
            snprintf(message, sizeof(message), "Can't use '%.*s' with %s. It only works on numbers.",
                     op.length, op.start, operand);
        }
        runtime_error(interp, op, message);
    }

    long l = left.as.integer;
    long r = right.as.integer;
    switch (node->as.binop.op) {
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
            if (r == 0) runtime_error(interp, op, "Division by zero.");
            return value_int(l / r);
        case TOKEN_PERCENT:
            if (r == 0) runtime_error(interp, op, "Modulo by zero.");
            return value_int(l % r);
        default:
            break;
    }

    fprintf(stderr, "Internal error: unknown binary operator.\n");
    exit(1);
}

/* Returns an owned reference. */
static Value eval(Interpreter *interp, AstNode *node) {
    switch (node->type) {
        case AST_NUMBER:
            return value_int(node->as.number.value);

        case AST_STRING:
            return value_new_string(node->as.string.chars, node->as.string.length);

        case AST_VARIABLE: {
            Token name = node->as.variable.name;
            int index = self_field_index(interp, name);
            if (index >= 0) {
                return value_retain(interp->self->fields[index]);
            }
            Variable *var = find_variable(interp, name);
            if (!var) {
                char message[256];
                snprintf(message, sizeof(message),
                         "Variable '%.*s' is not defined. Assign it a value first, e.g. '%.*s = 0'.",
                         name.length, name.start, name.length, name.start);
                runtime_error(interp, name, message);
            }
            return value_retain(var->value);
        }

        case AST_ARRAY: {
            Value array = value_new_array();
            for (int i = 0; i < node->as.array.count; i++) {
                array_append(array.as.array, eval(interp, node->as.array.items[i]));
            }
            return array;
        }

        case AST_INDEX: {
            Value target = eval_indexed(interp, node);
            long index = eval_int(interp, node->as.index.index, "An index");
            check_index(interp, target, index, node->token);

            /* Take the item before releasing the target: it may be a temporary. */
            Value item = target.type == VAL_STRING ? string_char_at(target.as.string, (int)index)
                                                   : value_retain(target.as.array->items[index]);
            value_release(target);
            return item;
        }

        case AST_CALL:
            return call_function(interp, node);

        case AST_FIELD: {
            int index;
            Object *object = eval_field_target(interp, node, &index);
            Value field = value_retain(object->fields[index]);
            value_release((Value){ .type = VAL_OBJECT, .as.object = object });
            return field;
        }

        case AST_SELF:
            return value_retain((Value){ .type = VAL_OBJECT, .as.object = interp->self });

        case AST_UNARY: {
            if (node->as.unary.op == '!') {
                return value_int(!eval_condition(interp, node->as.unary.operand));
            }
            long value = eval_int(interp, node->as.unary.operand, "The value after a sign");
            return value_int(node->as.unary.op == '-' ? -value : value);
        }

        case AST_BINOP:
            return eval_binop(interp, node);

        case AST_ASSIGN:
        case AST_INDEX_ASSIGN:
        case AST_FIELD_ASSIGN:
        case AST_CLASS:
        case AST_PRINT:
        case AST_IF:
        case AST_WHILE:
        case AST_FOR:
        case AST_FOR_EACH:
        case AST_FUNCTION:
        case AST_RETURN:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_IMPORT:
        case AST_BLOCK:
            fprintf(stderr, "Internal error: statement node inside expression.\n");
            exit(1);
    }

    fprintf(stderr, "Internal error: unknown AST node.\n");
    exit(1);
}

/* Called after a loop body ran: consumes 'agadi', and returns 1 if the loop must stop
 * ('ruk' or 'farkau'). */
static int end_loop_iteration(Interpreter *interp) {
    interp->continuing = 0;
    if (interp->breaking) {
        interp->breaking = 0;
        return 1;
    }
    return interp->returning;
}

static void execute(Interpreter *interp, AstNode *node) {
    switch (node->type) {
        case AST_PRINT: {
            Value value = eval(interp, node->as.print.expr);
            value_print(value);
            printf("\n");
            value_release(value);
            break;
        }

        case AST_ASSIGN:
            assign_variable(interp, node->as.assign.name, eval(interp, node->as.assign.value));
            break;

        case AST_FIELD_ASSIGN: {
            int index;
            Object *object = eval_field_target(interp, node->as.field_assign.target, &index);
            Value value = eval(interp, node->as.field_assign.value);
            value_release(object->fields[index]);
            object->fields[index] = value;
            value_release((Value){ .type = VAL_OBJECT, .as.object = object });
            break;
        }

        case AST_INDEX_ASSIGN: {
            AstNode *target = node->as.index_assign.target;
            Value container = eval_indexed(interp, target);
            if (container.type == VAL_STRING) {
                runtime_error(interp, target->token,
                              "Strings can't be changed one character at a time. Build a new string with '+' instead.");
            }
            Array *array = container.as.array;
            long index = eval_int(interp, target->as.index.index, "An index");
            Value value = eval(interp, node->as.index_assign.value);
            check_index(interp, container, index, target->token);

            value_release(array->items[index]);
            array->items[index] = value;
            value_release(container);
            break;
        }

        case AST_CALL:
            value_release(eval(interp, node));  /* call used as a statement: result is discarded */
            break;

        case AST_IF:
            if (eval_condition(interp, node->as.if_stmt.condition)) {
                execute(interp, node->as.if_stmt.then_block);
            } else if (node->as.if_stmt.else_branch) {
                execute(interp, node->as.if_stmt.else_branch);
            }
            break;

        case AST_WHILE:
            while (!interp->returning && eval_condition(interp, node->as.while_stmt.condition)) {
                execute(interp, node->as.while_stmt.body);
                if (end_loop_iteration(interp)) break;
            }
            break;

        case AST_FOR: {
            /* Bounds are evaluated once. The loop variable is reassigned from an internal
             * counter each iteration, so changing it inside the body doesn't skip iterations. */
            long start = eval_int(interp, node->as.for_stmt.start, "The start of a range");
            long end = eval_int(interp, node->as.for_stmt.end, "The end of a range");
            for (long i = start; i < end && !interp->returning; i++) {
                set_variable(interp, node->as.for_stmt.var, value_int(i));
                execute(interp, node->as.for_stmt.body);
                if (end_loop_iteration(interp)) break;
            }
            break;
        }

        case AST_FOR_EACH: {
            AstNode *iterable = node->as.for_each.iterable;
            Value value = eval(interp, iterable);
            if (value.type == VAL_STRING) {
                /* One iteration per character; strings are immutable, so no aliasing concerns. */
                for (int i = 0; i < value.as.string->count && !interp->returning; i++) {
                    set_variable(interp, node->as.for_each.var, string_char_at(value.as.string, i));
                    execute(interp, node->as.for_each.body);
                    if (end_loop_iteration(interp)) break;
                }
                value_release(value);
                break;
            }
            if (value.type != VAL_ARRAY) {
                char message[256];
                snprintf(message, sizeof(message),
                         "'ko_lagi ... in' needs an array, a string, or a range like '0..10', but this is %s.",
                         value_type_name(value));
                runtime_error(interp, iterable->token, message);
            }

            /* We hold a reference, so the array stays alive even if the body reassigns
             * the variable it came from. */
            Array *array = value.as.array;
            for (int i = 0; i < array->count && !interp->returning; i++) {
                set_variable(interp, node->as.for_each.var, value_retain(array->items[i]));
                execute(interp, node->as.for_each.body);
                if (end_loop_iteration(interp)) break;
            }
            value_release(value);
            break;
        }

        case AST_IMPORT:  /* already spliced in by the module loader */
        case AST_FUNCTION:
        case AST_CLASS:
            /* Already registered before execution started. */
            break;

        case AST_RETURN:
            interp->return_value = node->as.return_stmt.value ? eval(interp, node->as.return_stmt.value) : value_int(0);
            interp->returning = 1;
            break;

        case AST_BREAK:
            interp->breaking = 1;
            break;

        case AST_CONTINUE:
            interp->continuing = 1;
            break;

        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count && !UNWINDING(interp); i++) {
                execute(interp, node->as.block.statements[i]);
            }
            break;

        default:
            fprintf(stderr, "Internal error: expression node used as a statement.\n");
            exit(1);
    }
}

void interpret(AstNode *program, const char *source, const char *filename) {
    if (program->type != AST_BLOCK) {
        fprintf(stderr, "Internal error: program root is not a block node.\n");
        exit(1);
    }

    Interpreter interp;
    memset(&interp, 0, sizeof(interp));
    interp.return_value = value_int(0);
    interp.source = source;
    interp.filename = filename;

    /* Functions are registered up front, so a function can be called before
     * (above) its definition, and two functions can call each other. */
    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *statement = program->as.block.statements[i];
        if (statement->type == AST_FUNCTION) {
            register_function(&interp, statement);
        } else if (statement->type == AST_CLASS) {
            register_class(&interp, statement);
        }
    }

    execute(&interp, program);

    scope_free(&interp.globals);
    free(interp.functions);
    free(interp.classes);
}
