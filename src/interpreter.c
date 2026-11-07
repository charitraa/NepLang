#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interpreter.h"
#include "rt.h"
#include "value.h"

/* The tree-walking interpreter: `execute` runs statements and `eval` evaluates expressions,
 * directly on the AST. Every value operation (operators, indexing, fields, built-ins, type checks)
 * and every runtime error message comes from the shared runtime in rt.c, which the bytecode VM and
 * native programs use too -- so all three backends behave the same. */

/* Deep enough for normal recursion, shallow enough to fail with a clear message
 * long before the C stack overflows. */
#define MAX_CALL_DEPTH 1000

/* Variables live in hashed scopes (see Scope). Names are slices into the source buffer.
 * Each Variable owns one reference to its value. A declaration ('int x = 1', 'const N = 3')
 * gives the variable a type that later assignments must match, or makes it constant. */
typedef struct {
    const char *name;
    int length;
    Value value;
    TypeRef type;              /* TYPE_NONE: anything can be assigned */
    int is_const;
    const AstNode *decl;       /* the declaration that created it (NULL for a plain assignment) */
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

struct Interpreter {
    Scope globals;
    Scope *locals;              /* the current function call's scope; NULL at the top level */
    AstNode **functions;        /* AST_FUNCTION nodes, registered before execution starts */
    int function_count;
    int function_capacity;
    AstNode **classes;          /* AST_CLASS and AST_INTERFACE nodes, registered before execution */
    int class_count;
    int class_capacity;
    Object *self;               /* the object whose method is running ('yo'); NULL outside methods */
    const AstNode *self_class;  /* the class that declares the running method (privacy, 'super') */
    int call_depth;
    int breaking;               /* set by 'ruk'; unwinds blocks up to the enclosing loop */
    int continuing;             /* set by 'agadi'; unwinds blocks up to the enclosing loop */
    int returning;              /* set by 'farkau'; unwinds blocks and loops up to the call */
    Value return_value;         /* owned while `returning` is set */
    int repl;                   /* functions may be redefined on a later line */
};

#define UNWINDING(interp) ((interp)->returning || (interp)->breaking || (interp)->continuing)

static void out_of_memory(void) {
    fprintf(stderr, "neplang: out of memory\n");
    exit(1);
}

static int same_name(const char *name, int length, Token token) {
    return length == token.length && strncmp(name, token.start, (size_t)length) == 0;
}

static int same_token(Token a, Token b) {
    return same_name(a.start, a.length, b);
}

/* ---- scopes ---- */

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

static Variable *scope_find(Scope *scope, Token name) {
    if (scope->count == 0) return NULL;
    int slot = scope_probe(scope, name.start, name.length);
    return scope->slots[slot] == -1 ? NULL : &scope->vars[scope->slots[slot]];
}

/* Rebuilds the hash table with room for `slot_count` entries (a power of two). */
static void scope_rehash(Scope *scope, int slot_count) {
    int *slots = malloc(sizeof(int) * (size_t)slot_count);
    if (!slots) out_of_memory();
    for (int i = 0; i < slot_count; i++) slots[i] = -1;
    free(scope->slots);
    scope->slots = slots;
    scope->slot_count = slot_count;
    for (int i = 0; i < scope->count; i++) {
        scope->slots[scope_probe(scope, scope->vars[i].name, scope->vars[i].length)] = i;
    }
}

/* Creates a new, untyped variable. Takes ownership of `value`. */
static Variable *scope_add(Scope *scope, Token name, Value value) {
    if (scope->count == scope->capacity) {
        int capacity = scope->capacity == 0 ? 8 : scope->capacity * 2;
        Variable *vars = realloc(scope->vars, sizeof(Variable) * (size_t)capacity);
        if (!vars) out_of_memory();
        scope->vars = vars;
        scope->capacity = capacity;
    }
    /* Keep the table at most half full so probes stay short. */
    if (scope->slot_count < scope->capacity * 2) scope_rehash(scope, scope->capacity * 2);

    Variable *var = &scope->vars[scope->count];
    memset(var, 0, sizeof(*var));
    var->name = name.start;
    var->length = name.length;
    var->value = value;
    var->type.kind = TYPE_NONE;
    scope->slots[scope_probe(scope, name.start, name.length)] = scope->count;
    scope->count++;
    return var;
}

static void scope_free(Scope *scope) {
    for (int i = 0; i < scope->count; i++) {
        value_release(scope->vars[i].value);
    }
    free(scope->vars);
    free(scope->slots);
    memset(scope, 0, sizeof(*scope));
}

/* Reads look in the current function's locals first, then in globals. */
static Variable *find_variable(Interpreter *interp, Token name) {
    if (interp->locals) {
        Variable *var = scope_find(interp->locals, name);
        if (var) return var;
    }
    return scope_find(&interp->globals, name);
}

static Scope *current_scope(Interpreter *interp) {
    return interp->locals ? interp->locals : &interp->globals;
}

_Noreturn static void constant_error(Token name) {
    char message[256];
    snprintf(message, sizeof(message), "'%.*s' is a constant (declared with 'const'), so it can't be changed.",
             name.length, name.start);
    rt_error(name, message);
}

/* 'name = value' in the current scope: inside a function, assignment always creates/updates a
 * local (a function can read globals but never rebinds them). A typed variable only accepts
 * values of its type. Takes ownership of `value`. */
static void set_variable(Interpreter *interp, Token name, Value value) {
    Scope *scope = current_scope(interp);
    Variable *var = scope_find(scope, name);
    if (!var) {
        scope_add(scope, name, value);
        return;
    }
    if (var->is_const) constant_error(name);
    value = rt_coerce(value, &var->type, RT_CTX_VARIABLE, name, name);
    value_release(var->value);
    var->value = value;
}

/* 'int x = 1' / 'const N = 3': creates (or, when the same declaration runs again in a loop,
 * resets) a variable with a type. Takes ownership of `value`. */
static void declare_variable(Interpreter *interp, const AstNode *decl, Value value) {
    Token name = decl->as.assign.name;
    value = rt_coerce(value, &decl->as.assign.type, RT_CTX_VARIABLE, name, name);
    Scope *scope = current_scope(interp);
    Variable *var = scope_find(scope, name);
    if (var) {
        if (var->is_const && var->decl != decl) constant_error(name);
        value_release(var->value);
        var->value = value;
    } else {
        var = scope_add(scope, name, value);
    }
    var->type = decl->as.assign.type;
    var->is_const = decl->as.assign.is_const;
    var->decl = decl;
}

/* ---- functions and classes ---- */

static AstNode *find_function(Interpreter *interp, Token name) {
    for (int i = 0; i < interp->function_count; i++) {
        if (same_token(interp->functions[i]->as.function.name, name)) return interp->functions[i];
    }
    return NULL;
}

static AstNode *find_class(Interpreter *interp, Token name) {
    for (int i = 0; i < interp->class_count; i++) {
        if (same_token(interp->classes[i]->as.class_decl.name, name)) return interp->classes[i];
    }
    return NULL;
}

static int field_index(const AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        if (same_token(klass->as.class_decl.fields[i].name, name)) return i;
    }
    return -1;
}

static AstNode *find_method(const AstNode *klass, Token name) {
    for (int i = 0; i < klass->as.class_decl.method_count; i++) {
        if (same_token(klass->as.class_decl.methods[i]->as.function.name, name)) return klass->as.class_decl.methods[i];
    }
    return NULL;
}

static AstNode *find_constructor(const AstNode *klass) {
    return find_method(klass, (Token){ .start = "banau", .length = 5 });
}

/* Inside a method, a bare field name means that field of 'yo' (unless a parameter or local
 * variable has the same name), so the method can be written as in the class's own description.
 * Returns the field's index in `interp->self`, or -1. */
static int self_field_index(Interpreter *interp, Token name) {
    if (!interp->self) return -1;
    if (interp->locals && scope_find(interp->locals, name)) return -1;
    return field_index(interp->self->klass, name);
}

static Value self_value(Interpreter *interp) {
    return value_retain((Value){ .type = VAL_OBJECT, .as.object = interp->self });
}

/* Functions, classes and interfaces share one namespace: 'Manchhe(...)' must mean exactly one thing. */
static void check_new_global_name(Interpreter *interp, Token name, int is_function) {
    char message[256];

    if (rt_builtin_id(name.start, name.length) >= 0) {
        snprintf(message, sizeof(message), "'%.*s' is a built-in function and can't be redefined. Choose another name.",
                 name.length, name.start);
        rt_error(name, message);
    }

    AstNode *function = find_function(interp, name);
    if (function && !(interp->repl && is_function)) {
        snprintf(message, sizeof(message), "Function '%.*s' is already defined on line %d. Give this one a different name.",
                 name.length, name.start, function->as.function.name.line);
        rt_error(name, message);
    }

    AstNode *klass = find_class(interp, name);
    if (klass) {
        snprintf(message, sizeof(message), "%s '%.*s' is already defined on line %d. Give this one a different name.",
                 klass->type == AST_INTERFACE ? "Interface" : "Class", name.length, name.start,
                 klass->as.class_decl.name.line);
        rt_error(name, message);
    }
}

static void *grow(void *items, int *capacity, int count) {
    if (count < *capacity) return items;
    *capacity = *capacity == 0 ? 8 : *capacity * 2;
    void *grown = realloc(items, sizeof(AstNode *) * (size_t)*capacity);
    if (!grown) out_of_memory();
    return grown;
}

static void register_class(Interpreter *interp, AstNode *klass) {
    check_new_global_name(interp, klass->as.class_decl.name, 0);
    interp->classes = grow(interp->classes, &interp->class_capacity, interp->class_count);
    interp->classes[interp->class_count++] = klass;
}

static void register_function(Interpreter *interp, AstNode *function) {
    Token name = function->as.function.name;
    check_new_global_name(interp, name, 1);
    for (int i = 0; i < interp->function_count; i++) {
        if (same_token(interp->functions[i]->as.function.name, name)) {  /* REPL: a new definition */
            interp->functions[i] = function;
            return;
        }
    }
    interp->functions = grow(interp->functions, &interp->function_capacity, interp->function_count);
    interp->functions[interp->function_count++] = function;
}

/* ---- evaluation ---- */

static Value eval(Interpreter *interp, AstNode *node);
static void execute(Interpreter *interp, AstNode *node);

static int eval_condition(Interpreter *interp, AstNode *node) {
    return rt_condition(eval(interp, node), node->token);
}

_Noreturn static void arg_count_error(Token at, const char *before, Token label, const char *after, int expected,
                                      int got) {
    char message[300];
    snprintf(message, sizeof(message), "%s%.*s%s takes %d argument%s, but %d %s given here.", before, label.length,
             label.start, after, expected, expected == 1 ? "" : "s", got, got == 1 ? "was" : "were");
    rt_error(at, message);
}

/* Evaluates the arguments of `call` into args[offset...]; returns the array (caller frees). */
static Value *eval_args(Interpreter *interp, AstNode *call, int offset) {
    int argc = call->as.call.arg_count;
    Value *args = malloc(sizeof(Value) * (size_t)(argc + offset + 1));
    if (!args) out_of_memory();
    for (int i = 0; i < argc; i++) args[offset + i] = eval(interp, call->as.call.args[i]);
    return args;
}

/* Runs `function` with `args` (already counted; ownership passes to the new scope). `self` is the
 * object for a method, or NULL for a plain function. Returns the owned result. */
static Value invoke(Interpreter *interp, const AstNode *function, Value *args, Object *self, Token at) {
    Token name = function->as.function.name;
    if (interp->call_depth >= MAX_CALL_DEPTH) rt_too_deep(name.start, name.length, at);

    Scope frame;
    memset(&frame, 0, sizeof(frame));
    for (int i = 0; i < function->as.function.param_count; i++) {
        Token param = function->as.function.params[i];
        Value value = rt_coerce(args[i], &function->as.function.param_types[i], RT_CTX_PARAM, param, at);
        Variable *var = scope_add(&frame, param, value);
        var->type = function->as.function.param_types[i];
    }

    Scope *saved_locals = interp->locals;
    Object *saved_self = interp->self;
    const AstNode *saved_class = interp->self_class;
    interp->locals = &frame;
    interp->self = self;
    interp->self_class = self ? function->as.function.owner : NULL;
    interp->call_depth++;

    execute(interp, function->as.function.body);

    /* A function that ends without 'farkau' (or with a bare 'farkau') gives back khali. */
    Value result = interp->returning ? interp->return_value : value_null();
    interp->returning = 0;
    interp->return_value = value_null();

    interp->call_depth--;
    interp->locals = saved_locals;
    interp->self = saved_self;
    interp->self_class = saved_class;
    scope_free(&frame);

    const TypeRef *ret = &function->as.function.return_type;
    if (ret->kind != TYPE_NONE && ret->kind != TYPE_VOID) {
        result = rt_coerce(result, ret, RT_CTX_RETURN, name, at);
    }
    return result;
}

/* Klass(args): a new object with every field at its starting value, then 'banau' (if any). */
static Value construct(Interpreter *interp, AstNode *klass, AstNode *call) {
    Token name = klass->as.class_decl.name;
    char message[300];
    if (klass->type == AST_INTERFACE) {
        snprintf(message, sizeof(message),
                 "'%.*s' is an interface, so it can't make objects. Create an object of a class that implements it.",
                 name.length, name.start);
        rt_error(call->as.call.name, message);
    }

    AstNode *constructor = find_constructor(klass);
    int argc = call->as.call.arg_count;
    if (constructor) {
        if (argc != constructor->as.function.param_count) {
            arg_count_error(call->as.call.name, "'", name, "(...)'", constructor->as.function.param_count, argc);
        }
    } else if (argc != 0) {
        snprintf(message, sizeof(message),
                 "'%.*s(...)' takes no arguments, because class '%.*s' has no 'banau'. "
                 "Add 'kaam banau(...) { ... }' to the class to accept arguments.",
                 name.length, name.start, name.length, name.start);
        rt_error(call->as.call.name, message);
    }

    /* Starting values are evaluated afresh for every object (so 'items = []' isn't shared),
     * as if at the top level: they can see global variables, but not the caller's locals. */
    Value object = value_new_object(klass);
    Scope *saved_locals = interp->locals;
    Object *saved_self = interp->self;
    const AstNode *saved_class = interp->self_class;
    interp->locals = NULL;
    interp->self = NULL;
    interp->self_class = NULL;
    for (int i = 0; i < klass->as.class_decl.field_count; i++) {
        const AstField *field = &klass->as.class_decl.fields[i];
        rt_init_field(object, i, eval(interp, field->value), field->name);
    }
    interp->locals = saved_locals;
    interp->self = saved_self;
    interp->self_class = saved_class;

    if (constructor) {
        Value *args = eval_args(interp, call, 0);
        value_release(invoke(interp, constructor, args, object.as.object, call->as.call.name));
        free(args);
    }
    return object;
}

/* super.method(args) and super(args) inside a method: the parent class's version, on 'yo'. */
static Value call_super(Interpreter *interp, AstNode *call) {
    Token name = call->as.call.name;
    const AstNode *owner = interp->self_class;
    const AstNode *parent = owner ? owner->as.class_decl.parent : NULL;
    char message[300];
    if (!parent) {
        Token k = owner ? owner->as.class_decl.name : name;
        snprintf(message, sizeof(message), "Class '%.*s' has no parent class, so there is no 'super' to call.",
                 k.length, k.start);
        rt_error(name, message);
    }
    Token pname = parent->as.class_decl.name;
    int argc = call->as.call.arg_count;
    int is_constructor = same_name("super", 5, name);
    AstNode *method = is_constructor ? find_constructor(parent) : find_method(parent, name);
    if (!method) {
        if (is_constructor) {
            if (argc != 0) arg_count_error(name, "'super(...)' (", pname, "'s constructor)", 0, argc);
            return value_null();   /* the parent has no 'banau': its fields already have their values */
        }
        snprintf(message, sizeof(message), "The parent class '%.*s' has no method named '%.*s'.", pname.length,
                 pname.start, name.length, name.start);
        rt_error(name, message);
    }
    if (argc != method->as.function.param_count) {
        if (is_constructor) arg_count_error(name, "'super(...)' (", pname, "'s constructor)",
                                            method->as.function.param_count, argc);
        arg_count_error(name, "Method '", name, "'", method->as.function.param_count, argc);
    }
    Value *args = eval_args(interp, call, 0);
    Value result = invoke(interp, method, args, interp->self, name);
    free(args);
    if (is_constructor) {
        value_release(result);
        return value_null();
    }
    return result;
}

/* obj.name(args): a method of an object, or a method of an array or string ('a.push(1)'). */
static Value call_on_receiver(Interpreter *interp, AstNode *call) {
    Token name = call->as.call.name;
    int argc = call->as.call.arg_count;
    Value receiver = eval(interp, call->as.call.receiver);

    if (receiver.type != VAL_OBJECT) {
        Value *args = eval_args(interp, call, 1);
        args[0] = receiver;
        Value result = rt_value_method(args, argc + 1, name.start, name.length, name);
        free(args);
        return result;
    }

    const AstNode *klass;
    int index = rt_find_method(receiver, name.start, name.length, argc, interp->self_class, name, &klass);
    Value *args = eval_args(interp, call, 0);
    Value result = invoke(interp, klass->as.class_decl.methods[index], args, receiver.as.object, name);
    free(args);
    value_release(receiver);
    return result;
}

static Value call_builtin(Interpreter *interp, AstNode *call, int id) {
    Value *args = eval_args(interp, call, 0);
    Value result = rt_builtin(id, args, call->as.call.arg_count, call->as.call.name);
    free(args);
    return result;
}

static Value call_function(Interpreter *interp, AstNode *call) {
    Token name = call->as.call.name;
    char message[300];

    if (call->as.call.module.start) {
        Token module = call->as.call.module;
        int id = rt_module_builtin_id(module.start, module.length, name.start, name.length);
        if (id < 0) {
            snprintf(message, sizeof(message), "The '%.*s' module has no function named '%.*s'.", module.length,
                     module.start, name.length, name.start);
            rt_error(name, message);
        }
        return call_builtin(interp, call, id);
    }

    if (call->as.call.receiver) {
        if (call->as.call.receiver->type == AST_SUPER) return call_super(interp, call);
        return call_on_receiver(interp, call);
    }

    int builtin = rt_builtin_id(name.start, name.length);
    if (builtin >= 0) return call_builtin(interp, call, builtin);

    /* Inside a method, 'bolau()' calls the method 'bolau' of the same object. */
    if (interp->self && find_method(interp->self->klass, name)) {
        Value self = self_value(interp);
        const AstNode *klass;
        int index = rt_find_method(self, name.start, name.length, call->as.call.arg_count, interp->self_class, name,
                                   &klass);
        Value *args = eval_args(interp, call, 0);
        Value result = invoke(interp, klass->as.class_decl.methods[index], args, self.as.object, name);
        free(args);
        value_release(self);
        return result;
    }

    AstNode *klass = find_class(interp, name);
    if (klass) return construct(interp, klass, call);

    AstNode *function = find_function(interp, name);
    if (!function) {
        snprintf(message, sizeof(message), "Function '%.*s' is not defined. Define it with 'kaam %.*s(...) { ... }'.",
                 name.length, name.start, name.length, name.start);
        rt_error(name, message);
    }
    if (call->as.call.arg_count != function->as.function.param_count) {
        arg_count_error(name, "Function '", name, "'", function->as.function.param_count, call->as.call.arg_count);
    }
    Value *args = eval_args(interp, call, 0);
    Value result = invoke(interp, function, args, NULL, name);
    free(args);
    return result;
}

/* Returns an owned reference. */
static Value eval(Interpreter *interp, AstNode *node) {
    switch (node->type) {
        case AST_NUMBER: return value_int(node->as.number.value);
        case AST_FLOAT: return value_float(node->as.floating.value);
        case AST_BOOL: return value_bool(node->as.boolean.value);
        case AST_NULL: return value_null();
        case AST_STRING: return value_new_string(node->as.string.chars, node->as.string.length);

        case AST_VARIABLE: {
            Token name = node->as.variable.name;
            int index = self_field_index(interp, name);
            if (index >= 0) return value_retain(interp->self->fields[index]);
            Variable *var = find_variable(interp, name);
            if (!var) rt_undefined_variable(name.start, name.length, name);
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
            Value target = eval(interp, node->as.index.target);
            Value index = eval(interp, node->as.index.index);
            return rt_index(target, index, node->token);
        }

        case AST_CALL: return call_function(interp, node);

        case AST_FIELD: {
            Value object = eval(interp, node->as.field.object);
            Token name = node->as.field.name;
            return rt_get_field(object, name.start, name.length, interp->self_class, name);
        }

        case AST_SELF: return self_value(interp);

        case AST_UNARY: {
            AstNode *operand = node->as.unary.operand;
            Token at = operand->token.start ? operand->token : node->token;
            Value value = eval(interp, operand);
            if (node->as.unary.op == '!') return rt_not(value, at);
            if (node->as.unary.op == '-') return rt_neg(value, at);
            return rt_pos(value, at);
        }

        case AST_BINOP: {
            TokenType op = node->as.binop.op;
            if (op == TOKEN_RA || op == TOKEN_WA) {
                /* Short-circuit: the right side only runs when the left side doesn't decide the answer. */
                int left = eval_condition(interp, node->as.binop.left);
                if (op == TOKEN_RA ? !left : left) return value_bool(left);
                return value_bool(eval_condition(interp, node->as.binop.right));
            }
            Value left = eval(interp, node->as.binop.left);
            Value right = eval(interp, node->as.binop.right);
            return rt_binop(op, left, right, node->token);
        }

        default:
            fprintf(stderr, "Internal error: statement node inside expression.\n");
            exit(1);
    }
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
            if (!(node->as.print.repl && value.type == VAL_NULL)) rt_print(value);
            value_release(value);
            break;
        }

        case AST_ASSIGN: {
            Token name = node->as.assign.name;
            int is_declaration = node->as.assign.type.kind != TYPE_NONE || node->as.assign.is_const;
            Value value = eval(interp, node->as.assign.value);
            if (is_declaration) {
                declare_variable(interp, node, value);
                break;
            }
            if (self_field_index(interp, name) >= 0) {
                rt_set_field(self_value(interp), name.start, name.length, value, interp->self_class, name);
                break;
            }
            set_variable(interp, name, value);
            break;
        }

        case AST_FIELD_ASSIGN: {
            AstNode *target = node->as.field_assign.target;
            Token name = target->as.field.name;
            Value object = eval(interp, target->as.field.object);
            Value value = eval(interp, node->as.field_assign.value);
            rt_set_field(object, name.start, name.length, value, interp->self_class, name);
            break;
        }

        case AST_INDEX_ASSIGN: {
            AstNode *target = node->as.index_assign.target;
            Value container = eval(interp, target->as.index.target);
            Value index = eval(interp, target->as.index.index);
            Value value = eval(interp, node->as.index_assign.value);
            rt_set_index(container, index, value, target->token);
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
            AstNode *start_node = node->as.for_stmt.start, *end_node = node->as.for_stmt.end;
            Value start_value = eval(interp, start_node);
            long start = rt_number(start_value, "The start of a range", start_node->token);
            value_release(start_value);
            Value end_value = eval(interp, end_node);
            long end = rt_number(end_value, "The end of a range", end_node->token);
            value_release(end_value);
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
            rt_check_iterable(value, iterable->token);
            /* We hold a reference, so the array stays alive even if the body reassigns the variable
             * it came from. The length is re-read each time, so items pushed in the body are seen. */
            for (int i = 0; i < rt_length_of(value).as.integer && !interp->returning; i++) {
                Value item = value.type == VAL_STRING ? string_char_at(value.as.string, i)
                                                      : value_retain(value.as.array->items[i]);
                set_variable(interp, node->as.for_each.var, item);
                execute(interp, node->as.for_each.body);
                if (end_loop_iteration(interp)) break;
            }
            value_release(value);
            break;
        }

        case AST_IMPORT:  /* already spliced in by the module loader */
        case AST_FUNCTION:
        case AST_CLASS:
        case AST_INTERFACE:
            /* Already registered before execution started. */
            break;

        case AST_RETURN:
            interp->return_value = node->as.return_stmt.value ? eval(interp, node->as.return_stmt.value) : value_null();
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

/* ---- public interface ---- */

Interpreter *interpreter_new(int repl) {
    Interpreter *interp = calloc(1, sizeof(Interpreter));
    if (!interp) out_of_memory();
    interp->return_value = value_null();
    interp->repl = repl;
    return interp;
}

void interpreter_run(Interpreter *interp, AstNode *program) {
    if (program->type != AST_BLOCK) {
        fprintf(stderr, "Internal error: program root is not a block node.\n");
        exit(1);
    }
    /* Functions and classes are registered up front, so a function can be called before
     * (above) its definition, and two functions can call each other. */
    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *statement = program->as.block.statements[i];
        if (statement->type == AST_FUNCTION) {
            register_function(interp, statement);
        } else if (statement->type == AST_CLASS || statement->type == AST_INTERFACE) {
            register_class(interp, statement);
        }
    }
    execute(interp, program);
}

void interpreter_recover(Interpreter *interp) {
    /* After a runtime error in the REPL: back to the top level. (Values held by the frames that
     * were abandoned are leaked; that is acceptable for an interactive session.) */
    interp->locals = NULL;
    interp->self = NULL;
    interp->self_class = NULL;
    interp->call_depth = 0;
    interp->breaking = interp->continuing = interp->returning = 0;
    interp->return_value = value_null();
}

void interpreter_free(Interpreter *interp) {
    scope_free(&interp->globals);
    free(interp->functions);
    free(interp->classes);
    free(interp);
}

void interpret(AstNode *program) {
    Interpreter *interp = interpreter_new(0);
    interpreter_run(interp, program);
    interpreter_free(interp);
}
