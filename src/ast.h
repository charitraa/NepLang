#ifndef NEPLANG_AST_H
#define NEPLANG_AST_H

#include "token.h"

typedef enum {
    AST_NUMBER,
    AST_FLOAT,
    AST_BOOL,
    AST_NULL,
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
    AST_INTERFACE,
    AST_SUPER,
    AST_BLOCK
} AstType;

/* A written type: 'int', 'string?', 'Manchhe'. TYPE_NONE means no annotation (anything goes).
 * Class and interface types (TYPE_CLASS) also accept khali, like object references in Java; the
 * other types only do when written with '?'. */
typedef enum {
    TYPE_NONE, TYPE_INT, TYPE_FLOAT, TYPE_BOOL, TYPE_STRING, TYPE_ARRAY, TYPE_OBJECT, TYPE_CLASS, TYPE_ANY,
    TYPE_VOID   /* only as a function's return type: it gives nothing back */
} TypeKind;

typedef struct {
    TypeKind kind;
    int nullable;
    Token name;   /* the type as written; for TYPE_CLASS, the class or interface name */
} TypeRef;

/* A field declared in a class body: '[type] name = default' (or 'type name', default by type). */
typedef struct {
    Token name;
    struct AstNode *value;
    TypeRef type;
} AstField;

typedef struct AstNode {
    AstType type;
    Token token;  /* where runtime errors about this node point; zeroed (start == NULL) if unknown */
    union {
        struct { long value; } number;
        struct { double value; } floating;
        struct { int value; } boolean;
        struct { char *chars; int length; } string;  /* escapes already decoded; owned by the node */
        struct { TokenType op; struct AstNode *left; struct AstNode *right; } binop;
        struct { char op; struct AstNode *operand; } unary;
        struct { Token name; } variable;
        /* type.kind != TYPE_NONE for a declaration 'int x = 1'; is_const for 'const X = 1' */
        struct { Token name; struct AstNode *value; TypeRef type; int is_const; } assign;
        struct { struct AstNode *expr; int repl; } print;  /* repl: a REPL echo, which skips khali */
        struct { struct AstNode *condition; struct AstNode *then_block; struct AstNode *else_branch; } if_stmt;
        struct { struct AstNode *condition; struct AstNode *body; } while_stmt;
        struct { Token var; struct AstNode *start; struct AstNode *end; struct AstNode *body; } for_stmt;
        struct { Token var; struct AstNode *iterable; struct AstNode *body; } for_each;
        struct { struct AstNode **items; int count; int capacity; } array;
        struct { struct AstNode *target; struct AstNode *index; } index;
        struct { struct AstNode *target; struct AstNode *value; } index_assign;  /* target is an AST_INDEX */
        /* param_types[i] is TYPE_NONE when untyped. owner is the class that declares a method (NULL for
         * functions); with inheritance the same method node can be listed in several classes. body is
         * NULL for an interface's method. */
        struct {
            Token name; Token *params; TypeRef *param_types; int param_count; int param_capacity;
            struct AstNode *body; TypeRef return_type; const struct AstNode *owner;
        } function;
        /* receiver is NULL for 'name(args)', or the object for a method call 'obj.name(args)'.
         * module is set (start != NULL) for a standard-library call such as 'math.sqrt(x)'. */
        struct {
            Token name; struct AstNode *receiver; struct AstNode **args; int arg_count; int arg_capacity;
            Token module;
        } call;
        struct { Token keyword; struct AstNode *value; } return_stmt;  /* value may be NULL */
        /* After inheritance is resolved (classes.c), fields[0..inherited_fields) are the parent's
         * (borrowed: their nodes belong to the parent) and the rest are this class's own; methods
         * [0..own_methods) are its own and the rest are inherited, not overridden ones (borrowed).
         * `bases` are the names written in 'class Kukur(Janawar, Bolne)'; `parent` is the base that is
         * a class and `interfaces` the ones that are interfaces. An AST_INTERFACE uses only name,
         * methods (bodiless AST_FUNCTIONs) and bases (interfaces it extends). */
        struct {
            Token name;
            AstField *fields; int field_count; int field_capacity;
            struct AstNode **methods; int method_count; int method_capacity;  /* AST_FUNCTION nodes */
            Token *bases; int base_count; int base_capacity;
            const struct AstNode *parent;
            const struct AstNode **interfaces; int interface_count;
            int inherited_fields;
            int own_methods;
            int resolved;
        } class_decl;
        struct { struct AstNode *object; Token name; } field;
        struct { struct AstNode *target; struct AstNode *value; } field_assign;  /* target is an AST_FIELD */
        struct { char *path; int is_name; } import_stmt;  /* path owned; is_name: "import math" (no quotes) */
        struct { struct AstNode **statements; int count; int capacity; } block;
    } as;
} AstNode;

AstNode *ast_new_number(Token literal);
AstNode *ast_new_float(Token literal, double value);
AstNode *ast_new_bool(Token literal, int value);
AstNode *ast_new_null(Token literal);
AstNode *ast_default_for(TypeRef type, Token at);  /* the starting value of a typed name: 0, 0.0, jutho, "", [], khali */
AstNode *ast_clone(const AstNode *expr);          /* deep copy of an expression */
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
void ast_function_add_param(AstNode *function, Token param, TypeRef type);
AstNode *ast_new_call(Token name);
void ast_call_add_arg(AstNode *call, AstNode *arg);
AstNode *ast_new_return(Token keyword, AstNode *value);
AstNode *ast_new_class(Token name);
void ast_class_add_field(AstNode *class_decl, Token name, AstNode *value, TypeRef type);
void ast_class_add_base(AstNode *class_decl, Token base);
AstNode *ast_new_interface(Token name);
void ast_class_add_method(AstNode *class_decl, AstNode *method);
AstNode *ast_new_field(AstNode *object, Token name);
AstNode *ast_new_field_assign(AstNode *target, AstNode *value);
AstNode *ast_new_self(Token keyword);
AstNode *ast_new_super(Token keyword);
AstNode *ast_new_import(Token keyword, char *path, int is_name);  /* takes ownership of path */
AstNode *ast_new_loop_jump(AstType type, Token keyword);  /* AST_BREAK or AST_CONTINUE */
AstNode *ast_new_block(void);
void ast_block_append(AstNode *block, AstNode *statement);
void ast_free(AstNode *node);

#endif
