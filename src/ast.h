#ifndef NEPLANG_AST_H
#define NEPLANG_AST_H

#include "token.h"

typedef enum {
    AST_NUMBER,
    AST_STRING,
    AST_BINOP,
    AST_UNARY,
    AST_VARIABLE,
    AST_ASSIGN,
    AST_PRINT,
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_FOR_EACH,
    AST_ARRAY,
    AST_INDEX,
    AST_INDEX_ASSIGN,
    AST_FUNCTION,
    AST_CALL,
    AST_RETURN,
    AST_CLASS,
    AST_FIELD,
    AST_FIELD_ASSIGN,
    AST_SELF,
    AST_BREAK,
    AST_CONTINUE,
    AST_IMPORT,
    AST_BLOCK
} AstType;

/* A field declared in a class body: 'name = default'. */
typedef struct {
    Token name;
    struct AstNode *value;
} AstField;

typedef struct AstNode {
    AstType type;
    Token token;  /* where runtime errors about this node point; zeroed (start == NULL) if unknown */
    union {
        struct { long value; } number;
        struct { char *chars; int length; } string;  /* escapes already decoded; owned by the node */
        struct { TokenType op; struct AstNode *left; struct AstNode *right; } binop;
        struct { char op; struct AstNode *operand; } unary;
        struct { Token name; } variable;
        struct { Token name; struct AstNode *value; } assign;
        struct { struct AstNode *expr; } print;
        struct { struct AstNode *condition; struct AstNode *then_block; struct AstNode *else_branch; } if_stmt;
        struct { struct AstNode *condition; struct AstNode *body; } while_stmt;
        struct { Token var; struct AstNode *start; struct AstNode *end; struct AstNode *body; } for_stmt;
        struct { Token var; struct AstNode *iterable; struct AstNode *body; } for_each;
        struct { struct AstNode **items; int count; int capacity; } array;
        struct { struct AstNode *target; struct AstNode *index; } index;
        struct { struct AstNode *target; struct AstNode *value; } index_assign;  /* target is an AST_INDEX */
        struct { Token name; Token *params; int param_count; int param_capacity; struct AstNode *body; } function;
        /* receiver is NULL for 'name(args)', or the object for a method call 'obj.name(args)' */
        struct { Token name; struct AstNode *receiver; struct AstNode **args; int arg_count; int arg_capacity; } call;
        struct { Token keyword; struct AstNode *value; } return_stmt;  /* value may be NULL */
        struct {
            Token name;
            AstField *fields; int field_count; int field_capacity;
            struct AstNode **methods; int method_count; int method_capacity;  /* AST_FUNCTION nodes */
        } class_decl;
        struct { struct AstNode *object; Token name; } field;
        struct { struct AstNode *target; struct AstNode *value; } field_assign;  /* target is an AST_FIELD */
        struct { char *path; int is_name; } import_stmt;  /* path owned; is_name: "import math" (no quotes) */
        struct { struct AstNode **statements; int count; int capacity; } block;
    } as;
} AstNode;

AstNode *ast_new_number(Token literal);
AstNode *ast_new_string(Token literal, char *chars, int length);  /* takes ownership of chars */
AstNode *ast_new_placeholder(void);
AstNode *ast_new_binop(Token op, AstNode *left, AstNode *right);
AstNode *ast_new_unary(Token op, AstNode *operand);
AstNode *ast_new_variable(Token name);
AstNode *ast_new_assign(Token name, AstNode *value);
AstNode *ast_new_print(AstNode *expr);
AstNode *ast_new_if(AstNode *condition, AstNode *then_block, AstNode *else_branch);
AstNode *ast_new_while(AstNode *condition, AstNode *body);
AstNode *ast_new_for(Token var, AstNode *start, AstNode *end, AstNode *body);
AstNode *ast_new_for_each(Token var, AstNode *iterable, AstNode *body);
AstNode *ast_new_array(Token open);
void ast_array_add_item(AstNode *array, AstNode *item);
AstNode *ast_new_index(Token open, AstNode *target, AstNode *index);
AstNode *ast_new_index_assign(AstNode *target, AstNode *value);
AstNode *ast_new_function(Token name);
void ast_function_add_param(AstNode *function, Token param);
AstNode *ast_new_call(Token name);
void ast_call_add_arg(AstNode *call, AstNode *arg);
AstNode *ast_new_return(Token keyword, AstNode *value);
AstNode *ast_new_class(Token name);
void ast_class_add_field(AstNode *class_decl, Token name, AstNode *value);
void ast_class_add_method(AstNode *class_decl, AstNode *method);
AstNode *ast_new_field(AstNode *object, Token name);
AstNode *ast_new_field_assign(AstNode *target, AstNode *value);
AstNode *ast_new_self(Token keyword);
AstNode *ast_new_import(Token keyword, char *path, int is_name);  /* takes ownership of path */
AstNode *ast_new_loop_jump(AstType type, Token keyword);  /* AST_BREAK or AST_CONTINUE */
AstNode *ast_new_block(void);
void ast_block_append(AstNode *block, AstNode *statement);
void ast_free(AstNode *node);

#endif
