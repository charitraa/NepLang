#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

static AstNode *ast_alloc(AstType type, Token token) {
    AstNode *node = malloc(sizeof(AstNode));
    if (!node) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    memset(node, 0, sizeof(AstNode));
    node->type = type;
    node->token = token;
    return node;
}

static Token no_token(void) {
    Token token;
    memset(&token, 0, sizeof(token));
    return token;
}

/* Grows `*items` (an array of `size`-byte elements) so it can hold one more than `count`. */
static void *grow_array(void *items, int count, int *capacity, size_t size) {
    if (count < *capacity) return items;

    int new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    void *grown = realloc(items, size * (size_t)new_capacity);
    if (!grown) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    *capacity = new_capacity;
    return grown;
}

AstNode *ast_new_number(Token literal) {
    AstNode *node = ast_alloc(AST_NUMBER, literal);
    node->as.number.value = literal.value;
    return node;
}

AstNode *ast_new_string(Token literal, char *chars, int length) {
    AstNode *node = ast_alloc(AST_STRING, literal);
    node->as.string.chars = chars;
    node->as.string.length = length;
    return node;
}

/* Stand-in node returned by the parser after a syntax error; never executed. */
AstNode *ast_new_placeholder(void) {
    return ast_alloc(AST_NUMBER, no_token());
}

AstNode *ast_new_binop(Token op, AstNode *left, AstNode *right) {
    AstNode *node = ast_alloc(AST_BINOP, op);
    node->as.binop.op = op.type;
    node->as.binop.left = left;
    node->as.binop.right = right;
    return node;
}

AstNode *ast_new_unary(Token op, AstNode *operand) {
    AstNode *node = ast_alloc(AST_UNARY, op);
    node->as.unary.op = op.type == TOKEN_HOINA ? '!' : op.start[0];
    node->as.unary.operand = operand;
    return node;
}

AstNode *ast_new_variable(Token name) {
    AstNode *node = ast_alloc(AST_VARIABLE, name);
    node->as.variable.name = name;
    return node;
}

AstNode *ast_new_assign(Token name, AstNode *value) {
    AstNode *node = ast_alloc(AST_ASSIGN, name);
    node->as.assign.name = name;
    node->as.assign.value = value;
    return node;
}

AstNode *ast_new_print(AstNode *expr) {
    AstNode *node = ast_alloc(AST_PRINT, no_token());
    node->as.print.expr = expr;
    return node;
}

AstNode *ast_new_if(AstNode *condition, AstNode *then_block, AstNode *else_branch) {
    AstNode *node = ast_alloc(AST_IF, no_token());
    node->as.if_stmt.condition = condition;
    node->as.if_stmt.then_block = then_block;
    node->as.if_stmt.else_branch = else_branch;
    return node;
}

AstNode *ast_new_while(AstNode *condition, AstNode *body) {
    AstNode *node = ast_alloc(AST_WHILE, no_token());
    node->as.while_stmt.condition = condition;
    node->as.while_stmt.body = body;
    return node;
}

AstNode *ast_new_for(Token var, AstNode *start, AstNode *end, AstNode *body) {
    AstNode *node = ast_alloc(AST_FOR, var);
    node->as.for_stmt.var = var;
    node->as.for_stmt.start = start;
    node->as.for_stmt.end = end;
    node->as.for_stmt.body = body;
    return node;
}

AstNode *ast_new_for_each(Token var, AstNode *iterable, AstNode *body) {
    AstNode *node = ast_alloc(AST_FOR_EACH, var);
    node->as.for_each.var = var;
    node->as.for_each.iterable = iterable;
    node->as.for_each.body = body;
    return node;
}

AstNode *ast_new_array(Token open) {
    return ast_alloc(AST_ARRAY, open);
}

void ast_array_add_item(AstNode *array, AstNode *item) {
    array->as.array.items = grow_array(array->as.array.items, array->as.array.count,
                                       &array->as.array.capacity, sizeof(AstNode *));
    array->as.array.items[array->as.array.count++] = item;
}

AstNode *ast_new_index(Token open, AstNode *target, AstNode *index) {
    AstNode *node = ast_alloc(AST_INDEX, open);
    node->as.index.target = target;
    node->as.index.index = index;
    return node;
}

AstNode *ast_new_index_assign(AstNode *target, AstNode *value) {
    AstNode *node = ast_alloc(AST_INDEX_ASSIGN, target->token);
    node->as.index_assign.target = target;
    node->as.index_assign.value = value;
    return node;
}

AstNode *ast_new_function(Token name) {
    AstNode *node = ast_alloc(AST_FUNCTION, name);
    node->as.function.name = name;
    node->as.function.params = NULL;
    node->as.function.param_count = 0;
    node->as.function.param_capacity = 0;
    node->as.function.body = NULL;
    return node;
}

void ast_function_add_param(AstNode *function, Token param) {
    function->as.function.params = grow_array(function->as.function.params, function->as.function.param_count,
                                              &function->as.function.param_capacity, sizeof(Token));
    function->as.function.params[function->as.function.param_count++] = param;
}

AstNode *ast_new_call(Token name) {
    AstNode *node = ast_alloc(AST_CALL, name);
    node->as.call.name = name;
    node->as.call.receiver = NULL;
    node->as.call.args = NULL;
    node->as.call.arg_count = 0;
    node->as.call.arg_capacity = 0;
    return node;
}

void ast_call_add_arg(AstNode *call, AstNode *arg) {
    call->as.call.args = grow_array(call->as.call.args, call->as.call.arg_count,
                                    &call->as.call.arg_capacity, sizeof(AstNode *));
    call->as.call.args[call->as.call.arg_count++] = arg;
}

AstNode *ast_new_return(Token keyword, AstNode *value) {
    AstNode *node = ast_alloc(AST_RETURN, keyword);
    node->as.return_stmt.keyword = keyword;
    node->as.return_stmt.value = value;
    return node;
}

AstNode *ast_new_import(Token keyword, char *path, int is_name) {
    AstNode *node = ast_alloc(AST_IMPORT, keyword);
    node->as.import_stmt.path = path;
    node->as.import_stmt.is_name = is_name;
    return node;
}

AstNode *ast_new_loop_jump(AstType type, Token keyword) {
    return ast_alloc(type, keyword);
}

AstNode *ast_new_class(Token name) {
    AstNode *node = ast_alloc(AST_CLASS, name);
    node->as.class_decl.name = name;
    return node;
}

void ast_class_add_field(AstNode *class_decl, Token name, AstNode *value) {
    class_decl->as.class_decl.fields = grow_array(class_decl->as.class_decl.fields, class_decl->as.class_decl.field_count,
                                                  &class_decl->as.class_decl.field_capacity, sizeof(AstField));
    AstField *field = &class_decl->as.class_decl.fields[class_decl->as.class_decl.field_count++];
    field->name = name;
    field->value = value;
}

void ast_class_add_method(AstNode *class_decl, AstNode *method) {
    class_decl->as.class_decl.methods = grow_array(class_decl->as.class_decl.methods, class_decl->as.class_decl.method_count,
                                                   &class_decl->as.class_decl.method_capacity, sizeof(AstNode *));
    class_decl->as.class_decl.methods[class_decl->as.class_decl.method_count++] = method;
}

AstNode *ast_new_field(AstNode *object, Token name) {
    AstNode *node = ast_alloc(AST_FIELD, name);
    node->as.field.object = object;
    node->as.field.name = name;
    return node;
}

AstNode *ast_new_field_assign(AstNode *target, AstNode *value) {
    AstNode *node = ast_alloc(AST_FIELD_ASSIGN, target->token);
    node->as.field_assign.target = target;
    node->as.field_assign.value = value;
    return node;
}

AstNode *ast_new_self(Token keyword) {
    return ast_alloc(AST_SELF, keyword);
}

AstNode *ast_new_block(void) {
    AstNode *node = ast_alloc(AST_BLOCK, no_token());
    node->as.block.statements = NULL;
    node->as.block.count = 0;
    node->as.block.capacity = 0;
    return node;
}

void ast_block_append(AstNode *block, AstNode *statement) {
    block->as.block.statements = grow_array(block->as.block.statements, block->as.block.count,
                                            &block->as.block.capacity, sizeof(AstNode *));
    block->as.block.statements[block->as.block.count++] = statement;
}

void ast_free(AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case AST_NUMBER:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_VARIABLE:
        case AST_SELF:
            break;
        case AST_IMPORT:
            free(node->as.import_stmt.path);
            break;
        case AST_STRING:
            free(node->as.string.chars);
            break;
        case AST_BINOP:
            ast_free(node->as.binop.left);
            ast_free(node->as.binop.right);
            break;
        case AST_UNARY:
            ast_free(node->as.unary.operand);
            break;
        case AST_ASSIGN:
            ast_free(node->as.assign.value);
            break;
        case AST_PRINT:
            ast_free(node->as.print.expr);
            break;
        case AST_IF:
            ast_free(node->as.if_stmt.condition);
            ast_free(node->as.if_stmt.then_block);
            ast_free(node->as.if_stmt.else_branch);
            break;
        case AST_WHILE:
            ast_free(node->as.while_stmt.condition);
            ast_free(node->as.while_stmt.body);
            break;
        case AST_FOR:
            ast_free(node->as.for_stmt.start);
            ast_free(node->as.for_stmt.end);
            ast_free(node->as.for_stmt.body);
            break;
        case AST_FOR_EACH:
            ast_free(node->as.for_each.iterable);
            ast_free(node->as.for_each.body);
            break;
        case AST_ARRAY:
            for (int i = 0; i < node->as.array.count; i++) {
                ast_free(node->as.array.items[i]);
            }
            free(node->as.array.items);
            break;
        case AST_INDEX:
            ast_free(node->as.index.target);
            ast_free(node->as.index.index);
            break;
        case AST_INDEX_ASSIGN:
            ast_free(node->as.index_assign.target);
            ast_free(node->as.index_assign.value);
            break;
        case AST_FUNCTION:
            free(node->as.function.params);
            ast_free(node->as.function.body);
            break;
        case AST_CALL:
            ast_free(node->as.call.receiver);
            for (int i = 0; i < node->as.call.arg_count; i++) {
                ast_free(node->as.call.args[i]);
            }
            free(node->as.call.args);
            break;
        case AST_RETURN:
            ast_free(node->as.return_stmt.value);
            break;
        case AST_CLASS:
            for (int i = 0; i < node->as.class_decl.field_count; i++) {
                ast_free(node->as.class_decl.fields[i].value);
            }
            free(node->as.class_decl.fields);
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                ast_free(node->as.class_decl.methods[i]);
            }
            free(node->as.class_decl.methods);
            break;
        case AST_FIELD:
            ast_free(node->as.field.object);
            break;
        case AST_FIELD_ASSIGN:
            ast_free(node->as.field_assign.target);
            ast_free(node->as.field_assign.value);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                ast_free(node->as.block.statements[i]);
            }
            free(node->as.block.statements);
            break;
    }
    free(node);
}
