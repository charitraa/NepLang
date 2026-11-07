#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include "cgen.h"
#include "rt.h"

#ifndef NEPLANG_RUNTIME_DIR
#define NEPLANG_RUNTIME_DIR "src"
#endif

static void emit_c_string(FILE *out, const char *s, int length) {
    fputc('"', out);
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
            c == '.' || c == ',' || c == '-' || c == ':' || c == '+' || c == '*' || c == '/') {
            fputc(c, out);
        } else {
            fprintf(out, "\\%03o", c);   /* octal escapes are never ambiguous, even next to digits */
        }
    }
    fputc('"', out);
}

static void emit_token_init(FILE *out, Token t);

static void args_array(FILE *out, int depth, int argc) {
    fprintf(out, "Value args[] = {");
    for (int i = 0; i < argc; i++) fprintf(out, "%ss%d", i ? ", " : "", depth - argc + i);
    fprintf(out, "};");
}

static const char *caller_expr(long class_index) {
    static char buf[32];
    if (class_index < 0) return "NULL";
    snprintf(buf, sizeof(buf), "&K%ld", class_index);
    return buf;
}

static void emit_function(const IrProgram *p, int index, FILE *out) {
    const IrFunction *fn = &p->functions[index];
    int locals = fn->local_count > 0 ? fn->local_count : 1;

    fprintf(out, "static Value fn_%d(Value *args) {\n", index);
    fprintf(out, "    Value L[%d];\n    for (int i = 0; i < %d; i++) L[i] = rt_unset();\n", locals, locals);
    fprintf(out, "    for (int i = 0; i < %d; i++) L[i] = args[i];\n", fn->param_count);
    fprintf(out, "    Value ");
    for (int i = 0; i < fn->max_depth + 1; i++) fprintf(out, "%ss%d", i ? ", " : "", i);
    fprintf(out, ";\n");

    for (int i = 0; i < fn->count; i++) {
        const IrInstr *in = &fn->code[i];
        int d = in->depth;
        char at[64];
        snprintf(at, sizeof(at), "T(%d, %d)", in->token.line, in->token.col);

        fprintf(out, "    ");
        switch (in->op) {
            case IR_CONST_INT:
                if (in->a == LONG_MIN) fprintf(out, "s%d = value_int(-%ldL - 1L);\n", d, LONG_MAX);
                else fprintf(out, "s%d = value_int(%ldL);\n", d, in->a);
                break;
            case IR_CONST_FLOAT:
                if (isinf(in->f)) fprintf(out, "s%d = value_float(%sHUGE_VAL);\n", d, in->f < 0 ? "-" : "");
                else if (isnan(in->f)) fprintf(out, "s%d = value_float(NAN);\n", d);
                else fprintf(out, "s%d = value_float(%.17g);\n", d, in->f);
                break;
            case IR_CONST_BOOL: fprintf(out, "s%d = value_bool(%d);\n", d, in->a ? 1 : 0); break;
            case IR_CONST_NULL: fprintf(out, "s%d = value_null();\n", d); break;
            case IR_COERCE:
                fprintf(out, "s%d = rt_coerce(s%d, &TY[%ld], %ld, (Token)", d - 1, d - 1, in->a, in->b);
                emit_token_init(out, in->token);
                fprintf(out, ", %s);\n", at);
                break;
            case IR_CONST_STR:
                fprintf(out, "s%d = value_new_string(", d);
                emit_c_string(out, p->strings[in->a], p->string_lengths[in->a]);
                fprintf(out, ", %d);\n", p->string_lengths[in->a]);
                break;
            case IR_POP: fprintf(out, "value_release(s%d);\n", d - 1); break;
            case IR_LOAD_LOCAL: fprintf(out, "s%d = value_retain(L[%ld]);\n", d, in->a); break;
            case IR_STORE_LOCAL: fprintf(out, "value_release(L[%ld]); L[%ld] = s%d;\n", in->a, in->a, d - 1); break;
            case IR_LOAD_GLOBAL: fprintf(out, "s%d = value_retain(G[%ld]);\n", d, in->a); break;
            case IR_STORE_GLOBAL: fprintf(out, "value_release(G[%ld]); G[%ld] = s%d;\n", in->a, in->a, d - 1); break;
            case IR_LOAD_NAME:
                if (in->a >= 0) {
                    fprintf(out, "if (!rt_is_unset(L[%ld])) s%d = value_retain(L[%ld]); else ", in->a, d, in->a);
                }
                fprintf(out, "if (!rt_is_unset(G[%ld])) s%d = value_retain(G[%ld]); else rt_undefined_variable(",
                        in->b, d, in->b);
                emit_c_string(out, in->token.start, in->token.length);
                fprintf(out, ", %d, %s);\n", in->token.length, at);
                break;
            case IR_BINOP:
                fprintf(out, "s%d = rt_binop((TokenType)%ld, s%d, s%d, %s);\n", d - 2, in->a, d - 2, d - 1, at);
                break;
            case IR_NEG: fprintf(out, "s%d = rt_neg(s%d, %s);\n", d - 1, d - 1, at); break;
            case IR_POS: fprintf(out, "s%d = rt_pos(s%d, %s);\n", d - 1, d - 1, at); break;
            case IR_NOT: fprintf(out, "s%d = rt_not(s%d, %s);\n", d - 1, d - 1, at); break;
            case IR_TRUTH: fprintf(out, "s%d = rt_truth(s%d, %s);\n", d - 1, d - 1, at); break;
            case IR_JUMP: fprintf(out, "goto L%ld;\n", in->a); break;
            case IR_JUMP_IF_FALSE: fprintf(out, "if (!rt_condition(s%d, %s)) goto L%ld;\n", d - 1, at, in->a); break;
            case IR_LABEL: fprintf(out, "L%ld: ;\n", in->a); break;
            case IR_NEW_ARRAY:
                if (in->a == 0) {
                    fprintf(out, "s%d = rt_new_array(NULL, 0);\n", d);
                } else {
                    fprintf(out, "{ ");
                    args_array(out, d, (int)in->a);
                    fprintf(out, " s%d = rt_new_array(args, %ld); }\n", d - (int)in->a, in->a);
                }
                break;
            case IR_INDEX: fprintf(out, "s%d = rt_index(s%d, s%d, %s);\n", d - 2, d - 2, d - 1, at); break;
            case IR_SET_INDEX: fprintf(out, "rt_set_index(s%d, s%d, s%d, %s);\n", d - 3, d - 2, d - 1, at); break;
            case IR_LEN:
                fprintf(out, "{ Value t = rt_length_of(s%d); value_release(s%d); s%d = t; }\n", d - 1, d - 1, d - 1);
                break;
            case IR_CHECK_ITER: fprintf(out, "rt_check_iterable(s%d, %s);\n", d - 1, at); break;
            case IR_CHECK_NUM: fprintf(out, "rt_number(s%d, \"%s\", %s);\n", d - 1, IR_WHAT[in->a], at); break;
            case IR_CALL: {
                const IrFunction *callee = &p->functions[in->a];
                int argc = (int)in->b;
                fprintf(out, "{ if (rt_call_depth >= 1000) rt_too_deep(");
                emit_c_string(out, callee->name.start, callee->name.length);
                fprintf(out, ", %d, %s); ", callee->name.length, at);
                if (argc == 0) {
                    fprintf(out, "rt_call_depth++; s%d = fn_%ld(NULL); rt_call_depth--; }\n", d, in->a);
                } else {
                    args_array(out, d, argc);
                    fprintf(out, " rt_call_depth++; s%d = fn_%ld(args); rt_call_depth--; }\n", d - argc, in->a);
                }
                break;
            }
            case IR_BUILTIN: {
                int argc = (int)in->b;
                if (argc == 0) {
                    fprintf(out, "{ Value args[1]; s%d = rt_builtin(%ld, args, 0, %s); }\n", d, in->a, at);
                } else {
                    fprintf(out, "{ ");
                    args_array(out, d, argc);
                    fprintf(out, " s%d = rt_builtin(%ld, args, %d, %s); }\n", d - argc, in->a, argc, at);
                }
                break;
            }
            case IR_RETURN:
                fprintf(out, "{ Value r = s%d; for (int i = 0; i < %d; i++) value_release(L[i]); return r; }\n", d - 1,
                        locals);
                break;
            case IR_PRINT: fprintf(out, "rt_print(s%d); value_release(s%d);\n", d - 1, d - 1); break;
            case IR_NEW_OBJECT: fprintf(out, "s%d = value_new_object(&K%ld);\n", d, in->a); break;
            case IR_INIT_FIELD:
                fprintf(out, "rt_init_field(s%d, %ld, s%d, %s);\n", d - 2, in->a, d - 1, at);
                break;
            case IR_DUP: fprintf(out, "s%d = value_retain(s%d);\n", d, d - 1); break;
            case IR_GET_FIELD:
                fprintf(out, "s%d = rt_get_field(s%d, ", d - 1, d - 1);
                emit_c_string(out, in->token.start, in->token.length);
                fprintf(out, ", %d, %s, %s);\n", in->token.length, caller_expr(in->a), at);
                break;
            case IR_SET_FIELD:
                fprintf(out, "rt_set_field(s%d, ", d - 2);
                emit_c_string(out, in->token.start, in->token.length);
                fprintf(out, ", %d, s%d, %s, %s);\n", in->token.length, d - 1, caller_expr(in->a), at);
                break;
            case IR_CALL_METHOD: {
                int argc = (int)in->b;
                fprintf(out, "if (s%d.type != VAL_OBJECT) { ", d - argc - 1);
                args_array(out, d, argc + 1);
                fprintf(out, " s%d = rt_value_method(args, %d, ", d - argc - 1, argc + 1);
                emit_c_string(out, in->token.start, in->token.length);
                fprintf(out, ", %d, %s); } else ", in->token.length, at);
                fprintf(out, "{ const AstNode *k; int m = rt_find_method(s%d, ", d - argc - 1);
                emit_c_string(out, in->token.start, in->token.length);
                fprintf(out, ", %d, %d, %s, %s, &k); int f = method_fn(k, m); if (rt_call_depth >= 1000) rt_too_deep(",
                        in->token.length, argc, caller_expr(in->a), at);
                emit_c_string(out, in->token.start, in->token.length);
                fprintf(out, ", %d, %s); ", in->token.length, at);
                args_array(out, d, argc + 1);
                fprintf(out, " rt_call_depth++; s%d = FN[f](args); rt_call_depth--; }\n", d - argc - 1);
                break;
            }
        }
    }
    fprintf(out, "}\n\n");
}

static void emit_token_init(FILE *out, Token t) {
    fprintf(out, "{ .start = ");
    emit_c_string(out, t.start, t.length);
    fprintf(out, ", .length = %d }", t.length);
}

static void emit_type_init(FILE *out, const TypeRef *t) {
    fprintf(out, "{ .kind = (TypeKind)%d, .nullable = %d, .name = ", (int)t->kind, t->nullable);
    emit_token_init(out, t->name);
    fprintf(out, " }");
}

/* The written types used by IR_COERCE. */
static void emit_types(const IrProgram *p, FILE *out) {
    fprintf(out, "static const TypeRef TY[] = {");
    for (int i = 0; i < p->type_count; i++) {
        fprintf(out, "%s\n    ", i ? "," : "");
        emit_type_init(out, p->types[i]);
    }
    fprintf(out, "%s};\n\n", p->type_count ? "\n" : "{ 0 }");
}

static int class_index(const IrProgram *p, const AstNode *decl) {
    for (int i = 0; i < p->class_count; i++) {
        if (p->classes[i].decl == decl) return i;
    }
    return -1;
}

/* Class descriptions as static AST nodes, so the shared runtime (value.c, rt.c) can print objects,
 * look up fields and methods, check field types and 'is a' relations exactly as it does for the
 * interpreter and VM. */
static void emit_classes(const IrProgram *p, FILE *out) {
    if (p->class_count == 0) {
        fprintf(out, "static Value (*const FN[])(Value *) = {");
        for (int i = 0; i < p->function_count; i++) fprintf(out, "%sfn_%d", i ? ", " : "", i);
        fprintf(out, "};\nstatic int method_fn(const AstNode *k, int m) { (void)k; return m; }\n\n");
        return;
    }
    for (int c = 0; c < p->class_count; c++) fprintf(out, "static AstNode K%d;\n", c);
    for (int c = 0; c < p->class_count; c++) {
        const AstNode *d = p->classes[c].decl;
        int nf = d->as.class_decl.field_count, nm = d->as.class_decl.method_count;
        int ni = d->as.class_decl.interface_count;
        fprintf(out, "static AstField CF%d[] = {", c);
        for (int i = 0; i < nf; i++) {
            fprintf(out, "%s{ .name = ", i ? ", " : "");
            emit_token_init(out, d->as.class_decl.fields[i].name);
            fprintf(out, ", .type = ");
            emit_type_init(out, &d->as.class_decl.fields[i].type);
            fprintf(out, " }");
        }
        fprintf(out, "%s};\n", nf ? "" : "{ { 0 } }");
        for (int m = 0; m < nm; m++) {
            const AstNode *f = d->as.class_decl.methods[m];
            fprintf(out, "static AstNode CM%d_%d = { .type = AST_FUNCTION, .as.function = { .name = ", c, m);
            emit_token_init(out, f->as.function.name);
            fprintf(out, ", .param_count = %d } };\n", f->as.function.param_count);
        }
        fprintf(out, "static AstNode *CMS%d[] = {", c);
        for (int m = 0; m < nm; m++) fprintf(out, "%s&CM%d_%d", m ? ", " : "", c, m);
        fprintf(out, "%s};\n", nm ? "" : "NULL");
        fprintf(out, "static const AstNode *KI%d[] = {", c);
        for (int i = 0; i < ni; i++) fprintf(out, "%s&K%d", i ? ", " : "", class_index(p, d->as.class_decl.interfaces[i]));
        fprintf(out, "%s};\n", ni ? "" : "NULL");
        fprintf(out, "static AstNode K%d = { .type = %s, .as.class_decl = { .name = ", c,
                d->type == AST_INTERFACE ? "AST_INTERFACE" : "AST_CLASS");
        emit_token_init(out, d->as.class_decl.name);
        fprintf(out, ", .fields = CF%d, .field_count = %d, .methods = CMS%d, .method_count = %d", c, nf, c, nm);
        if (d->as.class_decl.parent) fprintf(out, ", .parent = &K%d", class_index(p, d->as.class_decl.parent));
        fprintf(out, ", .interfaces = KI%d, .interface_count = %d } };\n\n", c, ni);
    }
    fprintf(out, "static Value (*const FN[])(Value *) = {");
    for (int i = 0; i < p->function_count; i++) fprintf(out, "%sfn_%d", i ? ", " : "", i);
    fprintf(out, "};\n");
    fprintf(out, "static int method_fn(const AstNode *k, int m) {\n");
    for (int c = 0; c < p->class_count; c++) {
        const AstNode *d = p->classes[c].decl;
        fprintf(out, "    if (k == &K%d) { static const int t[] = {", c);
        for (int m = 0; m < d->as.class_decl.method_count; m++) {
            fprintf(out, "%s%d", m ? ", " : "", p->classes[c].method_fn[m]);
        }
        fprintf(out, "%s}; return t[m]; }\n", d->as.class_decl.method_count ? "" : "0");
    }
    fprintf(out, "    return 0;\n}\n\n");
}

#ifdef NEPLANG_EMBED_RUNTIME
/* bin/runtime_embed.c, generated by tools/embed_runtime.c: the runtime headers and sources
 * joined into one translation unit, so `build` works without a runtime folder on disk. */
extern const unsigned char neplang_runtime_source[];
extern const unsigned long neplang_runtime_source_size;
#endif

/* With `embedded_runtime` the runtime is pasted in front of the program instead of included,
 * making the output a single self-contained C file. */
static void emit_program(const IrProgram *program, FILE *out, int embedded_runtime) {
    fprintf(out, "/* Generated by neplang. Do not edit. */\n");
#ifdef NEPLANG_EMBED_RUNTIME
    if (embedded_runtime) fwrite(neplang_runtime_source, 1, neplang_runtime_source_size, out);
#endif
    fprintf(out, "#include <stddef.h>\n#include <math.h>\n%s#define T(l, c) ((Token){ .line = (l), .col = (c) })\n\n",
            embedded_runtime ? "" : "#include \"rt.h\"\n");
    fprintf(out, "static Value G[%d];\n\n", program->global_count > 0 ? program->global_count : 1);
    for (int i = 0; i < program->function_count; i++) fprintf(out, "static Value fn_%d(Value *args);\n", i);
    fprintf(out, "\n");
    emit_types(program, out);
    emit_classes(program, out);
    for (int i = 0; i < program->function_count; i++) emit_function(program, i, out);
    fprintf(out, "int main(int argc, char **argv) {\n    rt_set_args(argc - 1, argv + 1);\n");
    fprintf(out, "    for (int i = 0; i < %d; i++) G[i] = rt_unset();\n", program->global_count);
    fprintf(out, "    value_release(fn_0(NULL));\n    return 0;\n}\n");
}

void cgen_emit(const IrProgram *program, FILE *out) {
    emit_program(program, out, 0);
}

/* The runtime folder from NEPLANG_RUNTIME, or NULL when it isn't set. */
static const char *runtime_env(void) {
    const char *env = getenv("NEPLANG_RUNTIME");
    return env && *env ? env : NULL;
}

static int runtime_dir_ok(const char *dir) {
    size_t check_size = strlen(dir) + 16;
    char *check = malloc(check_size);
    if (!check) return 0;
    snprintf(check, check_size, "%s/rt.c", dir);
    FILE *probe = fopen(check, "r");
    free(check);
    if (!probe) return 0;
    fclose(probe);
    return 1;
}

int cgen_build(const IrProgram *program, const char *output_path, int keep_c) {
    /* NEPLANG_RUNTIME wins (for working on the runtime itself); otherwise the copy built into
     * this binary; otherwise the runtime folder baked in at compile time. */
    const char *dir = runtime_env();
#ifdef NEPLANG_EMBED_RUNTIME
    int embedded = dir == NULL;
#else
    int embedded = 0;
    if (!dir) dir = NEPLANG_RUNTIME_DIR;
#endif
    if (!embedded && !runtime_dir_ok(dir)) {
        fprintf(stderr,
                "neplang: can't find the runtime sources for --build in '%s'.\n"
                "Set NEPLANG_RUNTIME to the 'src' folder of a NepLang checkout (or the installed "
                "share/neplang/runtime folder).\n", dir);
        return 1;
    }

    size_t n = strlen(output_path);
    char *c_path = malloc(n + 3);
    if (!c_path) return 1;
    sprintf(c_path, "%s.c", output_path);

    FILE *out = fopen(c_path, "w");
    if (!out) {
        fprintf(stderr, "neplang: could not write '%s'\n", c_path);
        free(c_path);
        return 1;
    }
    emit_program(program, out, embedded);
    fclose(out);

    const char *cc = getenv("CC");
    size_t size = 400 + (dir ? strlen(dir) * 4 : 0) + n * 2;
    char *command = malloc(size);
    if (!command) {
        free(c_path);
        return 1;
    }
#ifdef _WIN32
    /* cmd.exe only understands double quotes; MinGW provides gcc rather than cc */
    if (embedded)
        snprintf(command, size, "%s -std=c11 -O2 -w -o \"%s\" \"%s\" -lm", cc && *cc ? cc : "gcc", output_path, c_path);
    else
        snprintf(command, size, "%s -std=c11 -O2 -w -I\"%s\" -o \"%s\" \"%s\" \"%s/value.c\" \"%s/rt.c\" \"%s/diagnostics.c\" -lm",
                 cc && *cc ? cc : "gcc", dir, output_path, c_path, dir, dir, dir);
#else
    if (embedded)
        snprintf(command, size, "%s -std=c11 -O2 -w -o '%s' '%s' -lm", cc && *cc ? cc : "cc", output_path, c_path);
    else
        snprintf(command, size, "%s -std=c11 -O2 -w -I'%s' -o '%s' '%s' '%s/value.c' '%s/rt.c' '%s/diagnostics.c' -lm",
                 cc && *cc ? cc : "cc", dir, output_path, c_path, dir, dir, dir);
#endif
    int status = system(command);
    if (status != 0) {
        fprintf(stderr, "neplang: the C compiler failed (command: %s)\n", command);
    }
    if (!keep_c) remove(c_path);
    free(command);
    free(c_path);
    return status == 0 ? 0 : 1;
}
