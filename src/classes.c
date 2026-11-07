#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "classes.h"
#include "diagnostics.h"

typedef struct {
    AstNode **decls;
    int count;
    int errors;
    char *state;   /* per decl: 0 = not started, 1 = in progress (cycle check), 2 = done */
} Resolver;

static int same(Token a, Token b) {
    return a.length == b.length && strncmp(a.start, b.start, (size_t)a.length) == 0;
}

static int is_text(Token t, const char *text) {
    return t.length == (int)strlen(text) && strncmp(t.start, text, (size_t)t.length) == 0;
}

static void error(Resolver *r, Token at, const char *message) {
    r->errors++;
    report_error("", "<program>", at, message);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    return p;
}

static int find(Resolver *r, Token name) {
    for (int i = 0; i < r->count; i++) {
        if (same(r->decls[i]->as.class_decl.name, name)) return i;
    }
    return -1;
}

static const char *kind_name(const AstNode *decl) {
    return decl->type == AST_INTERFACE ? "interface" : "class";
}

static AstNode *find_method(const AstNode *decl, Token name) {
    for (int i = 0; i < decl->as.class_decl.method_count; i++) {
        if (same(decl->as.class_decl.methods[i]->as.function.name, name)) return decl->as.class_decl.methods[i];
    }
    return NULL;
}

static void resolve(Resolver *r, int index);

/* Adds `iface` and the interfaces it extends to the class's list of required interfaces. */
static void check_interface(Resolver *r, AstNode *klass, const AstNode *iface, Token at) {
    for (int i = 0; i < iface->as.class_decl.method_count; i++) {
        const AstNode *required = iface->as.class_decl.methods[i];
        Token mname = required->as.function.name;
        AstNode *method = find_method(klass, mname);
        Token k = klass->as.class_decl.name, in = iface->as.class_decl.name;
        char message[400];
        if (!method) {
            snprintf(message, sizeof(message),
                     "Class '%.*s' says it is a '%.*s', but it has no method '%.*s'. Add 'kaam %.*s(...) { ... }' "
                     "to the class.",
                     k.length, k.start, in.length, in.start, mname.length, mname.start, mname.length, mname.start);
            error(r, at, message);
        } else if (method->as.function.param_count != required->as.function.param_count) {
            int need = required->as.function.param_count;
            snprintf(message, sizeof(message),
                     "Method '%.*s' must take %d parameter%s, as interface '%.*s' says (line %d).", mname.length,
                     mname.start, need, need == 1 ? "" : "s", in.length, in.start, mname.line);
            error(r, method->as.function.name, message);
        }
    }
    for (int i = 0; i < iface->as.class_decl.interface_count; i++) {
        check_interface(r, klass, iface->as.class_decl.interfaces[i], at);
    }
}

static void resolve(Resolver *r, int index) {
    if (r->state[index] == 2) return;
    AstNode *decl = r->decls[index];
    Token name = decl->as.class_decl.name;
    char message[400];

    if (r->state[index] == 1) {
        snprintf(message, sizeof(message), "'%.*s' inherits from itself (through its parents). Remove the cycle.",
                 name.length, name.start);
        error(r, name, message);
        return;
    }
    r->state[index] = 1;

    const AstNode *parent = NULL;
    const AstNode **interfaces = xmalloc(sizeof(AstNode *) * (size_t)(decl->as.class_decl.base_count + 1));
    int interface_count = 0;

    for (int b = 0; b < decl->as.class_decl.base_count; b++) {
        Token base = decl->as.class_decl.bases[b];
        int at = find(r, base);
        if (at < 0) {
            snprintf(message, sizeof(message), "There is no class or interface named '%.*s'.", base.length, base.start);
            error(r, base, message);
            continue;
        }
        if (at == index) {
            snprintf(message, sizeof(message), "'%.*s' can't inherit from itself.", base.length, base.start);
            error(r, base, message);
            continue;
        }
        resolve(r, at);
        if (r->state[at] != 2) continue;   /* a cycle, already reported */
        AstNode *b_decl = r->decls[at];
        if (b_decl->type == AST_CLASS) {
            if (decl->type == AST_INTERFACE) {
                snprintf(message, sizeof(message), "An interface can only extend other interfaces, but '%.*s' is a class.",
                         base.length, base.start);
                error(r, base, message);
            } else if (parent) {
                snprintf(message, sizeof(message),
                         "A class can have only one parent class, but '%.*s' and '%.*s' are both classes. "
                         "The others must be interfaces.",
                         parent->as.class_decl.name.length, parent->as.class_decl.name.start, base.length, base.start);
                error(r, base, message);
            } else {
                parent = b_decl;
            }
        } else {
            interfaces[interface_count++] = b_decl;
        }
    }

    int own_fields = decl->as.class_decl.field_count;
    int own_methods = decl->as.class_decl.method_count;
    for (int m = 0; m < own_methods; m++) decl->as.class_decl.methods[m]->as.function.owner = decl;

    if (parent) {
        /* fields: the parent's (borrowed) first, then this class's own */
        int inherited = parent->as.class_decl.field_count;
        for (int f = 0; f < own_fields; f++) {
            for (int p = 0; p < inherited; p++) {
                Token fname = decl->as.class_decl.fields[f].name;
                if (same(fname, parent->as.class_decl.fields[p].name)) {
                    snprintf(message, sizeof(message),
                             "Class '%.*s' already gets the field '%.*s' from its parent '%.*s'. Set its value in "
                             "'banau' instead of declaring it again.",
                             name.length, name.start, fname.length, fname.start, parent->as.class_decl.name.length,
                             parent->as.class_decl.name.start);
                    error(r, fname, message);
                }
            }
        }
        int total = inherited + own_fields;
        AstField *fields = xmalloc(sizeof(AstField) * (size_t)total);
        if (inherited) memcpy(fields, parent->as.class_decl.fields, sizeof(AstField) * (size_t)inherited);
        if (own_fields) memcpy(fields + inherited, decl->as.class_decl.fields, sizeof(AstField) * (size_t)own_fields);
        free(decl->as.class_decl.fields);
        decl->as.class_decl.fields = fields;
        decl->as.class_decl.field_count = decl->as.class_decl.field_capacity = total;
        decl->as.class_decl.inherited_fields = inherited;

        /* methods: own ones, then the parent's that this class doesn't override */
        for (int m = 0; m < own_methods; m++) {
            AstNode *method = decl->as.class_decl.methods[m];
            AstNode *base = find_method(parent, method->as.function.name);
            if (base && !is_text(method->as.function.name, "banau") &&
                base->as.function.param_count != method->as.function.param_count) {
                Token mname = method->as.function.name;
                int need = base->as.function.param_count;
                snprintf(message, sizeof(message),
                         "Method '%.*s' replaces the parent's '%.*s', so it must take the same number of parameters "
                         "(%d).",
                         mname.length, mname.start, mname.length, mname.start, need);
                error(r, mname, message);
            }
        }
        for (int m = 0; m < parent->as.class_decl.method_count; m++) {
            AstNode *method = parent->as.class_decl.methods[m];
            if (!find_method(decl, method->as.function.name)) ast_class_add_method(decl, method);
        }
    }
    decl->as.class_decl.parent = parent;
    decl->as.class_decl.interfaces = interfaces;
    decl->as.class_decl.interface_count = interface_count;
    decl->as.class_decl.own_methods = own_methods;
    decl->as.class_decl.resolved = 1;

    if (decl->type == AST_CLASS) {
        /* the parent's interfaces are already checked for the parent, and methods are inherited */
        for (int i = 0; i < interface_count; i++) check_interface(r, decl, interfaces[i], name);
    }
    r->state[index] = 2;
}

int classes_resolve(AstNode **decls, int count, int first) {
    Resolver r = { decls, count, 0, xmalloc((size_t)count + 1) };
    for (int i = 0; i < count; i++) r.state[i] = i < first ? 2 : 0;

    /* the same name used twice */
    for (int i = first; i < count; i++) {
        for (int j = 0; j < i; j++) {
            if (same(decls[i]->as.class_decl.name, decls[j]->as.class_decl.name)) {
                Token n = decls[i]->as.class_decl.name;
                char message[300];
                snprintf(message, sizeof(message), "A %s named '%.*s' is already defined on line %d.",
                         kind_name(decls[j]), n.length, n.start, decls[j]->as.class_decl.name.line);
                error(&r, n, message);
                break;
            }
        }
    }
    for (int i = first; i < count; i++) resolve(&r, i);
    free(r.state);
    return r.errors;
}

int classes_resolve_program(AstNode *program) {
    int count = 0;
    for (int i = 0; i < program->as.block.count; i++) {
        AstType t = program->as.block.statements[i]->type;
        if (t == AST_CLASS || t == AST_INTERFACE) count++;
    }
    if (count == 0) return 0;
    AstNode **decls = xmalloc(sizeof(AstNode *) * (size_t)count);
    int k = 0;
    for (int i = 0; i < program->as.block.count; i++) {
        AstNode *n = program->as.block.statements[i];
        if (n->type == AST_CLASS || n->type == AST_INTERFACE) decls[k++] = n;
    }
    int errors = classes_resolve(decls, count, 0);
    free(decls);
    return errors;
}
