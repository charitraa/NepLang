#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"
#include "rt.h"
#include "diagnostics.h"

const char *const IR_WHAT[] = { "A condition", "An index", "The start of a range", "The end of a range" };

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    return q;
}

/* ---- compile-time state ---- */

typedef struct { Token name; int slot; } LocalName;

typedef struct {
    int break_label;
    int continue_label;
} Loop;

typedef struct {
    IrProgram *program;
    const char *source;
    const char *filename;
    int errors;
    AstNode *root;
    int top_functions;       /* functions[1..top_functions] are the top-level 'kaam's; methods follow */
    int cur_class;           /* class whose method is being compiled, or -1 */

    IrFunction *fn;
    int is_main;
    int depth;
    int next_label;
    LocalName *locals;
    int local_names;
    int local_names_cap;
    Loop loops[64];
    int loop_count;
} Compiler;

static int same(Token a, Token b) {
    return a.length == b.length && strncmp(a.start, b.start, (size_t)a.length) == 0;
}

static void unsupported(Compiler *c, Token at, const char *what) {
    char message[300];
    snprintf(message, sizeof(message),
             "%s can't be compiled yet: the bytecode VM and native backend only support the core language. "
             "Run this program with the normal interpreter instead.", what);
    c->errors++;
    report_error(c->source, c->filename, at, message);
}

static int emit(Compiler *c, IrOp op, long a, long b, Token token, int delta) {
    IrFunction *fn = c->fn;
    if (fn->count == fn->capacity) {
        fn->capacity = fn->capacity ? fn->capacity * 2 : 32;
        fn->code = xrealloc(fn->code, sizeof(IrInstr) * (size_t)fn->capacity);
    }
    fn->code[fn->count++] = (IrInstr){ op, a, b, token, c->depth };
    c->depth += delta;
    return fn->count - 1;
}

static Token no_token(void) {
    return (Token){ 0 };
}

static int new_label(Compiler *c) {
    return c->next_label++;
}

static void place_label(Compiler *c, int label) {
    emit(c, IR_LABEL, label, 0, no_token(), 0);
}

static int new_hidden(Compiler *c) {
    return c->fn->local_count++;
}

static int local_slot(Compiler *c, Token name) {
    for (int i = 0; i < c->local_names; i++) {
        if (same(c->locals[i].name, name)) return c->locals[i].slot;
    }
    return -1;
}

static int declare_local(Compiler *c, Token name) {
    int slot = local_slot(c, name);
    if (slot >= 0) return slot;
    if (c->local_names == c->local_names_cap) {
        c->local_names_cap = c->local_names_cap ? c->local_names_cap * 2 : 16;
        c->locals = xrealloc(c->locals, sizeof(LocalName) * (size_t)c->local_names_cap);
    }
    slot = c->fn->local_count++;
    c->locals[c->local_names++] = (LocalName){ name, slot };
    return slot;
}

static int global_id(Compiler *c, Token name) {
    IrProgram *p = c->program;
    for (int i = 0; i < p->global_count; i++) {
        if (same(p->globals[i], name)) return i;
    }
    p->globals = xrealloc(p->globals, sizeof(Token) * (size_t)(p->global_count + 1));
    p->globals[p->global_count] = name;
    return p->global_count++;
}

static int add_string(Compiler *c, const char *chars, int length) {
    IrProgram *p = c->program;
    p->strings = xrealloc(p->strings, sizeof(char *) * (size_t)(p->string_count + 1));
    p->string_lengths = xrealloc(p->string_lengths, sizeof(int) * (size_t)(p->string_count + 1));
    char *copy = xrealloc(NULL, (size_t)length + 1);
    memcpy(copy, chars, (size_t)length);
    copy[length] = '\0';
    p->strings[p->string_count] = copy;
    p->string_lengths[p->string_count] = length;
    return p->string_count++;
}

static int find_function(Compiler *c, Token name) {
    int end = c->top_functions + 1;
    for (int i = 1; i < end; i++) {
        if (same(c->program->functions[i].name, name)) return i;
    }
    return -1;
}

static int find_class(Compiler *c, Token name) {
    for (int i = 0; i < c->program->class_count; i++) {
        if (same(c->program->classes[i].decl->as.class_decl.name, name)) return i;
    }
    return -1;
}

static int class_field(const AstNode *decl, Token name) {
    for (int i = 0; i < decl->as.class_decl.field_count; i++) {
        if (same(decl->as.class_decl.fields[i].name, name)) return i;
    }
    return -1;
}

static int class_method(const AstNode *decl, Token name) {
    for (int i = 0; i < decl->as.class_decl.method_count; i++) {
        if (same(decl->as.class_decl.methods[i]->as.function.name, name)) return i;
    }
    return -1;
}

/* Inside a method, a bare name that is a field (and not a parameter or loop variable) means yo.name. */
static int is_bare_field(Compiler *c, Token name) {
    return c->cur_class >= 0 && !c->is_main && local_slot(c, name) < 0 &&
           class_field(c->program->classes[c->cur_class].decl, name) >= 0;
}

/* ---- expressions ---- */

static void compile_expr(Compiler *c, AstNode *node);
static void compile_stmt(Compiler *c, AstNode *node);

static void compile_construct(Compiler *c, int class_index, AstNode *node) {
    const AstNode *decl = c->program->classes[class_index].decl;
    int argc = node->as.call.arg_count;
    int ctor = class_method(decl, (Token){ .start = "banau", .length = 5 });
    int expected = ctor >= 0 ? decl->as.class_decl.methods[ctor]->as.function.param_count : 0;
    if (argc != expected) {
        c->errors++;
        report_error(c->source, c->filename, node->as.call.name, "Wrong number of arguments for this class.");
        emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
        return;
    }

    emit(c, IR_NEW_OBJECT, class_index, 0, node->as.call.name, 1);

    /* Starting values are evaluated afresh for every object, as if at the top level. */
    int saved_main = c->is_main, saved_class = c->cur_class;
    c->is_main = 1;
    c->cur_class = -1;
    for (int i = 0; i < decl->as.class_decl.field_count; i++) {
        compile_expr(c, decl->as.class_decl.fields[i].value);
        emit(c, IR_INIT_FIELD, i, 0, decl->as.class_decl.fields[i].name, -1);
    }
    c->is_main = saved_main;
    c->cur_class = saved_class;

    if (ctor >= 0) {
        emit(c, IR_DUP, 0, 0, no_token(), 1);
        for (int i = 0; i < argc; i++) compile_expr(c, node->as.call.args[i]);
        emit(c, IR_CALL, c->program->classes[class_index].method_fn[ctor], argc + 1, node->as.call.name, -argc);
        emit(c, IR_POP, 0, 0, no_token(), -1);
    }
}

static void compile_call(Compiler *c, AstNode *node) {
    Token name = node->as.call.name;
    int argc = node->as.call.arg_count;

    if (node->as.call.receiver) {
        compile_expr(c, node->as.call.receiver);
        for (int i = 0; i < argc; i++) compile_expr(c, node->as.call.args[i]);
        emit(c, IR_CALL_METHOD, c->cur_class, argc, name, -argc);
        return;
    }

    int builtin = rt_builtin_id(name.start, name.length);
    if (builtin >= 0) {
        for (int i = 0; i < argc; i++) compile_expr(c, node->as.call.args[i]);
        emit(c, IR_BUILTIN, builtin, argc, name, 1 - argc);
        return;
    }

    /* Inside a method, 'bolau()' calls a method of the same object. */
    if (c->cur_class >= 0 && !c->is_main && class_method(c->program->classes[c->cur_class].decl, name) >= 0) {
        emit(c, IR_LOAD_LOCAL, 0, 0, no_token(), 1);
        for (int i = 0; i < argc; i++) compile_expr(c, node->as.call.args[i]);
        emit(c, IR_CALL_METHOD, c->cur_class, argc, name, -argc);
        return;
    }

    int class_index = find_class(c, name);
    if (class_index >= 0) {
        compile_construct(c, class_index, node);
        return;
    }

    int fn_index = find_function(c, name);
    if (fn_index < 0) {
        unsupported(c, name, "An unknown function");
        emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
        return;
    }
    for (int i = 0; i < argc; i++) compile_expr(c, node->as.call.args[i]);
    emit(c, IR_CALL, fn_index, argc, name, 1 - argc);
}

static void store_var(Compiler *c, Token name) {
    if (c->is_main) {
        emit(c, IR_STORE_GLOBAL, global_id(c, name), 0, name, -1);
    } else {
        emit(c, IR_STORE_LOCAL, declare_local(c, name), 0, name, -1);
    }
}

static void compile_expr(Compiler *c, AstNode *node) {
    switch (node->type) {
        case AST_NUMBER:
            emit(c, IR_CONST_INT, node->as.number.value, 0, node->token, 1);
            break;
        case AST_STRING:
            emit(c, IR_CONST_STR, add_string(c, node->as.string.chars, node->as.string.length), 0, node->token, 1);
            break;
        case AST_VARIABLE: {
            Token name = node->as.variable.name;
            if (is_bare_field(c, name)) {
                emit(c, IR_LOAD_LOCAL, 0, 0, no_token(), 1);
                emit(c, IR_GET_FIELD, c->cur_class, 0, name, 0);
                break;
            }
            emit(c, IR_LOAD_NAME, c->is_main ? -1 : local_slot(c, name), global_id(c, name), name, 1);
            break;
        }
        case AST_ARRAY:
            for (int i = 0; i < node->as.array.count; i++) compile_expr(c, node->as.array.items[i]);
            emit(c, IR_NEW_ARRAY, node->as.array.count, 0, node->token, 1 - node->as.array.count);
            break;
        case AST_UNARY:
            compile_expr(c, node->as.unary.operand);
            emit(c, node->as.unary.op == '!' ? IR_NOT : node->as.unary.op == '-' ? IR_NEG : IR_POS, 0, 0,
                 node->as.unary.operand->token.start ? node->as.unary.operand->token : node->token, 0);
            break;
        case AST_BINOP: {
            TokenType op = node->as.binop.op;
            if (op == TOKEN_RA) {
                int false_label = new_label(c), end = new_label(c);
                compile_expr(c, node->as.binop.left);
                emit(c, IR_JUMP_IF_FALSE, false_label, 0, node->as.binop.left->token, -1);
                compile_expr(c, node->as.binop.right);
                emit(c, IR_TRUTH, 0, 0, node->as.binop.right->token, 0);
                emit(c, IR_JUMP, end, 0, no_token(), 0);
                c->depth--;
                place_label(c, false_label);
                emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
                place_label(c, end);
            } else if (op == TOKEN_WA) {
                int right_label = new_label(c), end = new_label(c);
                compile_expr(c, node->as.binop.left);
                emit(c, IR_JUMP_IF_FALSE, right_label, 0, node->as.binop.left->token, -1);
                emit(c, IR_CONST_INT, 1, 0, no_token(), 1);
                emit(c, IR_JUMP, end, 0, no_token(), 0);
                c->depth--;
                place_label(c, right_label);
                compile_expr(c, node->as.binop.right);
                emit(c, IR_TRUTH, 0, 0, node->as.binop.right->token, 0);
                place_label(c, end);
            } else {
                compile_expr(c, node->as.binop.left);
                compile_expr(c, node->as.binop.right);
                emit(c, IR_BINOP, op, 0, node->token, -1);
            }
            break;
        }
        case AST_INDEX:
            compile_expr(c, node->as.index.target);
            compile_expr(c, node->as.index.index);
            emit(c, IR_INDEX, 0, 0, node->token, -1);
            break;
        case AST_CALL:
            compile_call(c, node);
            break;
        case AST_FIELD:
            compile_expr(c, node->as.field.object);
            emit(c, IR_GET_FIELD, c->cur_class, 0, node->as.field.name, 0);
            break;
        case AST_SELF:
            emit(c, IR_LOAD_LOCAL, 0, 0, no_token(), 1);
            break;
        default:
            unsupported(c, node->token, "This expression");
            emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
            break;
    }
}

/* ---- statements ---- */

static void compile_block(Compiler *c, AstNode *block) {
    for (int i = 0; i < block->as.block.count; i++) compile_stmt(c, block->as.block.statements[i]);
}

static void check_num(Compiler *c, int what, Token at) {
    emit(c, IR_CHECK_NUM, what, 0, at, 0);
}

static void push_loop(Compiler *c, int break_label, int continue_label) {
    c->loops[c->loop_count++] = (Loop){ break_label, continue_label };
}

static void compile_stmt(Compiler *c, AstNode *node) {
    switch (node->type) {
        case AST_PRINT:
            compile_expr(c, node->as.print.expr);
            emit(c, IR_PRINT, 0, 0, node->token, -1);
            break;
        case AST_ASSIGN:
            if (is_bare_field(c, node->as.assign.name)) {
                emit(c, IR_LOAD_LOCAL, 0, 0, no_token(), 1);
                compile_expr(c, node->as.assign.value);
                emit(c, IR_SET_FIELD, c->cur_class, 0, node->as.assign.name, -2);
                break;
            }
            compile_expr(c, node->as.assign.value);
            store_var(c, node->as.assign.name);
            break;
        case AST_FIELD_ASSIGN: {
            AstNode *target = node->as.field_assign.target;
            compile_expr(c, target->as.field.object);
            compile_expr(c, node->as.field_assign.value);
            emit(c, IR_SET_FIELD, c->cur_class, 0, target->as.field.name, -2);
            break;
        }
        case AST_INDEX_ASSIGN: {
            AstNode *target = node->as.index_assign.target;
            compile_expr(c, target->as.index.target);
            compile_expr(c, target->as.index.index);
            compile_expr(c, node->as.index_assign.value);
            emit(c, IR_SET_INDEX, 0, 0, target->token, -3);
            break;
        }
        case AST_CALL:
            compile_call(c, node);
            emit(c, IR_POP, 0, 0, no_token(), -1);
            break;
        case AST_IF: {
            int else_label = new_label(c), end = new_label(c);
            compile_expr(c, node->as.if_stmt.condition);
            emit(c, IR_JUMP_IF_FALSE, else_label, 0, node->as.if_stmt.condition->token, -1);
            compile_stmt(c, node->as.if_stmt.then_block);
            if (node->as.if_stmt.else_branch) emit(c, IR_JUMP, end, 0, no_token(), 0);
            place_label(c, else_label);
            if (node->as.if_stmt.else_branch) {
                compile_stmt(c, node->as.if_stmt.else_branch);
                place_label(c, end);
            }
            break;
        }
        case AST_WHILE: {
            int start = new_label(c), end = new_label(c);
            place_label(c, start);
            compile_expr(c, node->as.while_stmt.condition);
            emit(c, IR_JUMP_IF_FALSE, end, 0, node->as.while_stmt.condition->token, -1);
            push_loop(c, end, start);
            compile_stmt(c, node->as.while_stmt.body);
            c->loop_count--;
            emit(c, IR_JUMP, start, 0, no_token(), 0);
            place_label(c, end);
            break;
        }
        case AST_FOR: {
            int counter = new_hidden(c), limit = new_hidden(c);
            int top = new_label(c), next = new_label(c), end = new_label(c);
            compile_expr(c, node->as.for_stmt.start);
            check_num(c, IR_WHAT_RANGE_START, node->as.for_stmt.start->token);
            emit(c, IR_STORE_LOCAL, counter, 0, no_token(), -1);
            compile_expr(c, node->as.for_stmt.end);
            check_num(c, IR_WHAT_RANGE_END, node->as.for_stmt.end->token);
            emit(c, IR_STORE_LOCAL, limit, 0, no_token(), -1);

            place_label(c, top);
            emit(c, IR_LOAD_LOCAL, counter, 0, no_token(), 1);
            emit(c, IR_LOAD_LOCAL, limit, 0, no_token(), 1);
            emit(c, IR_BINOP, TOKEN_LESS, 0, no_token(), -1);
            emit(c, IR_JUMP_IF_FALSE, end, 0, no_token(), -1);
            emit(c, IR_LOAD_LOCAL, counter, 0, no_token(), 1);
            store_var(c, node->as.for_stmt.var);
            push_loop(c, end, next);
            compile_stmt(c, node->as.for_stmt.body);
            c->loop_count--;
            place_label(c, next);
            emit(c, IR_LOAD_LOCAL, counter, 0, no_token(), 1);
            emit(c, IR_CONST_INT, 1, 0, no_token(), 1);
            emit(c, IR_BINOP, TOKEN_PLUS, 0, no_token(), -1);
            emit(c, IR_STORE_LOCAL, counter, 0, no_token(), -1);
            emit(c, IR_JUMP, top, 0, no_token(), 0);
            place_label(c, end);
            break;
        }
        case AST_FOR_EACH: {
            int items = new_hidden(c), index = new_hidden(c);
            int top = new_label(c), next = new_label(c), end = new_label(c);
            compile_expr(c, node->as.for_each.iterable);
            emit(c, IR_CHECK_ITER, 0, 0, node->as.for_each.iterable->token, 0);
            emit(c, IR_STORE_LOCAL, items, 0, no_token(), -1);
            emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
            emit(c, IR_STORE_LOCAL, index, 0, no_token(), -1);

            place_label(c, top);
            emit(c, IR_LOAD_LOCAL, index, 0, no_token(), 1);
            emit(c, IR_LOAD_LOCAL, items, 0, no_token(), 1);
            emit(c, IR_LEN, 0, 0, no_token(), 0);
            emit(c, IR_BINOP, TOKEN_LESS, 0, no_token(), -1);
            emit(c, IR_JUMP_IF_FALSE, end, 0, no_token(), -1);
            emit(c, IR_LOAD_LOCAL, items, 0, no_token(), 1);
            emit(c, IR_LOAD_LOCAL, index, 0, no_token(), 1);
            emit(c, IR_INDEX, 0, 0, no_token(), -1);
            store_var(c, node->as.for_each.var);
            push_loop(c, end, next);
            compile_stmt(c, node->as.for_each.body);
            c->loop_count--;
            place_label(c, next);
            emit(c, IR_LOAD_LOCAL, index, 0, no_token(), 1);
            emit(c, IR_CONST_INT, 1, 0, no_token(), 1);
            emit(c, IR_BINOP, TOKEN_PLUS, 0, no_token(), -1);
            emit(c, IR_STORE_LOCAL, index, 0, no_token(), -1);
            emit(c, IR_JUMP, top, 0, no_token(), 0);
            place_label(c, end);
            break;
        }
        case AST_BREAK:
            emit(c, IR_JUMP, c->loops[c->loop_count - 1].break_label, 0, no_token(), 0);
            break;
        case AST_CONTINUE:
            emit(c, IR_JUMP, c->loops[c->loop_count - 1].continue_label, 0, no_token(), 0);
            break;
        case AST_RETURN:
            if (node->as.return_stmt.value) {
                compile_expr(c, node->as.return_stmt.value);
            } else {
                emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
            }
            emit(c, IR_RETURN, 0, 0, no_token(), -1);
            break;
        case AST_BLOCK:
            compile_block(c, node);
            break;
        case AST_CLASS:
        case AST_FUNCTION:
        case AST_IMPORT:
            break;  /* functions are compiled separately; imports are already spliced in */
        default:
            unsupported(c, node->token, "This statement");
            break;
    }
}

/* Names assigned anywhere in a function body get a local slot up front, so a read that appears
 * before the assignment (e.g. inside a loop) still refers to the local, as in the interpreter. */
static void declare_assigned(Compiler *c, AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_ASSIGN:
            if (!is_bare_field(c, node->as.assign.name)) declare_local(c, node->as.assign.name);
            break;
        case AST_IF:
            declare_assigned(c, node->as.if_stmt.then_block);
            declare_assigned(c, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE: declare_assigned(c, node->as.while_stmt.body); break;
        case AST_FOR:
            declare_local(c, node->as.for_stmt.var);
            declare_assigned(c, node->as.for_stmt.body);
            break;
        case AST_FOR_EACH:
            declare_local(c, node->as.for_each.var);
            declare_assigned(c, node->as.for_each.body);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) declare_assigned(c, node->as.block.statements[i]);
            break;
        default: break;
    }
}

static void compile_function(Compiler *c, IrFunction *fn, AstNode *decl, AstNode *main_body, int class_index) {
    c->fn = fn;
    c->depth = 0;
    c->next_label = 0;
    c->local_names = 0;
    c->loop_count = 0;
    c->is_main = decl == NULL;
    c->cur_class = class_index;

    if (decl) {
        if (class_index >= 0) declare_local(c, (Token){ .start = "yo", .length = 2 });   /* slot 0 */
        for (int i = 0; i < decl->as.function.param_count; i++) declare_local(c, decl->as.function.params[i]);
        fn->param_count = decl->as.function.param_count + (class_index >= 0 ? 1 : 0);
        declare_assigned(c, decl->as.function.body);
        compile_stmt(c, decl->as.function.body);
    } else {
        compile_block(c, main_body);
    }
    emit(c, IR_CONST_INT, 0, 0, no_token(), 1);
    emit(c, IR_RETURN, 0, 0, no_token(), -1);
    fn->label_count = c->next_label;
}

IrProgram *ir_compile(AstNode *program, const char *source, const char *filename) {
    IrProgram *p = calloc(1, sizeof(IrProgram));
    if (!p) exit(1);
    Compiler c = { .program = p, .source = source, .filename = filename, .root = program, .cur_class = -1 };

    int top = 0, classes = 0, methods = 0;
    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *n = program->as.block.statements[i];
        if (n->type == AST_FUNCTION) top++;
        if (n->type == AST_CLASS) {
            classes++;
            methods += n->as.class_decl.method_count;
        }
    }
    int count = 1 + top + methods;
    p->function_count = count;
    p->functions = calloc((size_t)count, sizeof(IrFunction));
    p->classes = calloc((size_t)classes + 1, sizeof(IrClass));
    AstNode **decls = calloc((size_t)count, sizeof(AstNode *));
    int *owner = calloc((size_t)count, sizeof(int));
    if (!p->functions || !p->classes || !decls || !owner) exit(1);

    int k = 1;
    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *n = program->as.block.statements[i];
        if (n->type != AST_FUNCTION) continue;
        if (rt_builtin_id(n->as.function.name.start, n->as.function.name.length) >= 0 ||
            find_function(&c, n->as.function.name) >= 0) {
            unsupported(&c, n->as.function.name, "A function with a duplicate or built-in name");
        }
        p->functions[k].name = n->as.function.name;
        owner[k] = -1;
        decls[k++] = n;
        c.top_functions = k - 1;
    }

    int ci = 0;
    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *n = program->as.block.statements[i];
        if (n->type != AST_CLASS) continue;
        IrClass *cls = &p->classes[ci];
        cls->decl = n;
        Token cname = n->as.class_decl.name;
        if (rt_builtin_id(cname.start, cname.length) >= 0 || find_function(&c, cname) >= 0 || find_class(&c, cname) >= 0) {
            unsupported(&c, cname, "A class with a duplicate or built-in name");
        }
        c.program->class_count = ci + 1;
        cls->method_fn = calloc((size_t)n->as.class_decl.method_count + 1, sizeof(int));
        if (!cls->method_fn) exit(1);
        for (int m = 0; m < n->as.class_decl.method_count; m++) {
            AstNode *method = n->as.class_decl.methods[m];
            p->functions[k].name = method->as.function.name;
            cls->method_fn[m] = k;
            owner[k] = ci;
            decls[k++] = method;
        }
        ci++;
    }

    p->class_count = classes;
    p->functions[0].name = (Token){ .start = "<main>", .length = 6 };
    compile_function(&c, &p->functions[0], NULL, program, -1);
    for (int i = 1; i < count; i++) compile_function(&c, &p->functions[i], decls[i], NULL, owner[i]);

    free(decls);
    free(owner);
    free(c.locals);
    if (c.errors) {
        ir_free(p);
        return NULL;
    }
    return p;
}

/* ---- optimizer ---- */

static int is_terminator(IrOp op) {
    return op == IR_JUMP || op == IR_RETURN;
}

static int fold_binop(TokenType op, long l, long r, long *out) {
    switch (op) {
        case TOKEN_PLUS: *out = l + r; return 1;
        case TOKEN_MINUS: *out = l - r; return 1;
        case TOKEN_STAR: *out = l * r; return 1;
        case TOKEN_SLASH: if (r == 0) return 0; *out = l / r; return 1;
        case TOKEN_PERCENT: if (r == 0) return 0; *out = l % r; return 1;
        case TOKEN_EQUAL_EQUAL: *out = l == r; return 1;
        case TOKEN_BANG_EQUAL: *out = l != r; return 1;
        case TOKEN_LESS: *out = l < r; return 1;
        case TOKEN_LESS_EQUAL: *out = l <= r; return 1;
        case TOKEN_GREATER: *out = l > r; return 1;
        case TOKEN_GREATER_EQUAL: *out = l >= r; return 1;
        default: return 0;
    }
}

/* One optimizing pass over a function; returns 1 if anything changed.
 *  - constant folding:   CONST a, CONST b, BINOP  ->  CONST (a op b);   CONST a, NEG -> CONST -a
 *  - constant branches:  CONST k, JUMP_IF_FALSE L ->  nothing / JUMP L
 *  - jump to the next instruction is dropped
 *  - code after JUMP/RETURN up to the next label is unreachable and dropped */
static int optimize_function(IrFunction *fn) {
    IrInstr *out = xrealloc(NULL, sizeof(IrInstr) * (size_t)(fn->count + 1));
    int n = 0, changed = 0, dead = 0;

    for (int i = 0; i < fn->count; i++) {
        IrInstr in = fn->code[i];
        if (in.op == IR_LABEL) dead = 0;
        if (dead) { changed = 1; continue; }

        if (in.op == IR_BINOP && n >= 2 && out[n - 1].op == IR_CONST_INT && out[n - 2].op == IR_CONST_INT) {
            long value;
            if (fold_binop((TokenType)in.a, out[n - 2].a, out[n - 1].a, &value)) {
                out[n - 2].a = value;
                n--;
                changed = 1;
                continue;
            }
        }
        if ((in.op == IR_NEG || in.op == IR_POS) && n >= 1 && out[n - 1].op == IR_CONST_INT) {
            if (in.op == IR_NEG) out[n - 1].a = -out[n - 1].a;
            changed = 1;
            continue;
        }
        if (in.op == IR_JUMP_IF_FALSE && n >= 1 && out[n - 1].op == IR_CONST_INT) {
            long k = out[n - 1].a;
            n--;
            changed = 1;
            if (k == 0) {
                in.op = IR_JUMP;
                in.depth = out[n].depth;
                out[n++] = in;
                dead = 1;
            }
            continue;
        }
        if (in.op == IR_JUMP) {
            int j = i + 1;
            while (j < fn->count && fn->code[j].op == IR_LABEL && fn->code[j].a != in.a) j++;
            if (j < fn->count && fn->code[j].op == IR_LABEL && fn->code[j].a == in.a) {
                changed = 1;   /* jumps to a label that directly follows */
                continue;
            }
        }
        out[n++] = in;
        if (is_terminator(in.op)) dead = 1;
    }

    free(fn->code);
    fn->code = out;
    fn->count = n;
    fn->capacity = fn->count + 1;
    return changed;
}

void ir_optimize(IrProgram *program) {
    for (int f = 0; f < program->function_count; f++) {
        for (int round = 0; round < 8 && optimize_function(&program->functions[f]); round++) {}
    }
}

void ir_finalize(IrProgram *program) {
    for (int f = 0; f < program->function_count; f++) {
        IrFunction *fn = &program->functions[f];
        free(fn->label_pos);
        fn->label_pos = xrealloc(NULL, sizeof(int) * (size_t)(fn->label_count + 1));
        for (int i = 0; i <= fn->label_count; i++) fn->label_pos[i] = -1;
        fn->max_depth = 0;
        for (int i = 0; i < fn->count; i++) {
            if (fn->code[i].op == IR_LABEL) fn->label_pos[fn->code[i].a] = i;
            if (fn->code[i].depth + 2 > fn->max_depth) fn->max_depth = fn->code[i].depth + 2;
        }
    }
}

/* ---- dump ---- */

static const char *op_name(IrOp op) {
    static const char *const names[] = {
        "const_int", "const_str", "pop", "load_local", "store_local", "load_global", "store_global", "load_name",
        "binop", "neg", "pos", "not", "truth", "jump", "jump_if_false", "label", "new_array", "index", "set_index",
        "len", "check_iter", "check_num", "call", "builtin", "return", "print", "new_object", "init_field", "dup",
        "get_field", "set_field", "call_method",
    };
    return names[op];
}

static const char *binop_text(long op) {
    switch ((TokenType)op) {
        case TOKEN_PLUS: return "+"; case TOKEN_MINUS: return "-"; case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/"; case TOKEN_PERCENT: return "%"; case TOKEN_EQUAL_EQUAL: return "==";
        case TOKEN_BANG_EQUAL: return "!="; case TOKEN_LESS: return "<"; case TOKEN_LESS_EQUAL: return "<=";
        case TOKEN_GREATER: return ">"; case TOKEN_GREATER_EQUAL: return ">="; default: return "?";
    }
}

void ir_dump(const IrProgram *program, FILE *out) {
    for (int f = 0; f < program->function_count; f++) {
        const IrFunction *fn = &program->functions[f];
        fprintf(out, "function %d %.*s(params=%d, locals=%d)\n", f, fn->name.length, fn->name.start,
                fn->param_count, fn->local_count);
        for (int i = 0; i < fn->count; i++) {
            const IrInstr *in = &fn->code[i];
            if (in->op == IR_LABEL) {
                fprintf(out, "  L%ld:\n", in->a);
                continue;
            }
            fprintf(out, "    %-13s", op_name(in->op));
            switch (in->op) {
                case IR_BINOP: fprintf(out, " %s", binop_text(in->a)); break;
                case IR_CONST_STR: fprintf(out, " \"%s\"", program->strings[in->a]); break;
                case IR_LOAD_NAME:
                    fprintf(out, " %.*s (local %ld, global %ld)", in->token.length, in->token.start, in->a, in->b);
                    break;
                case IR_LOAD_GLOBAL: case IR_STORE_GLOBAL:
                    fprintf(out, " %.*s", program->globals[in->a].length, program->globals[in->a].start);
                    break;
                case IR_JUMP: case IR_JUMP_IF_FALSE: fprintf(out, " L%ld", in->a); break;
                case IR_CALL: fprintf(out, " %.*s/%ld", program->functions[in->a].name.length,
                                      program->functions[in->a].name.start, in->b); break;
                case IR_BUILTIN: case IR_CHECK_NUM: fprintf(out, " %ld", in->a); break;
                case IR_NEW_OBJECT: {
                    Token n = program->classes[in->a].decl->as.class_decl.name;
                    fprintf(out, " %.*s", n.length, n.start);
                    break;
                }
                case IR_GET_FIELD: case IR_SET_FIELD: case IR_CALL_METHOD:
                    fprintf(out, " .%.*s", in->token.length, in->token.start);
                    break;
                case IR_INIT_FIELD: fprintf(out, " %ld", in->a); break;
                case IR_CONST_INT: case IR_LOAD_LOCAL: case IR_STORE_LOCAL: case IR_NEW_ARRAY:
                    fprintf(out, " %ld", in->a); break;
                default: break;
            }
            fprintf(out, "\n");
        }
    }
}

void ir_free(IrProgram *program) {
    if (!program) return;
    for (int f = 0; f < program->function_count; f++) {
        free(program->functions[f].code);
        free(program->functions[f].label_pos);
    }
    free(program->functions);
    for (int i = 0; i < program->class_count; i++) free(program->classes[i].method_fn);
    free(program->classes);
    for (int i = 0; i < program->string_count; i++) free(program->strings[i]);
    free(program->strings);
    free(program->string_lengths);
    free(program->globals);
    free(program);
}
