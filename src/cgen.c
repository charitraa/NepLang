#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void args_array(FILE *out, int depth, int argc) {
    fprintf(out, "Value args[] = {");
    for (int i = 0; i < argc; i++) fprintf(out, "%ss%d", i ? ", " : "", depth - argc + i);
    fprintf(out, "};");
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
            case IR_CONST_INT: fprintf(out, "s%d = value_int(%ldL);\n", d, in->a); break;
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
                    fprintf(out, "{ Value args[1]; s%d = rt_builtin(%ld, args, %s); }\n", d, in->a, at);
                } else {
                    fprintf(out, "{ ");
                    args_array(out, d, argc);
                    fprintf(out, " s%d = rt_builtin(%ld, args, %s); }\n", d - argc, in->a, at);
                }
                break;
            }
            case IR_RETURN:
                fprintf(out, "{ Value r = s%d; for (int i = 0; i < %d; i++) value_release(L[i]); return r; }\n", d - 1,
                        locals);
                break;
            case IR_PRINT: fprintf(out, "rt_print(s%d); value_release(s%d);\n", d - 1, d - 1); break;
        }
    }
    fprintf(out, "}\n\n");
}

void cgen_emit(const IrProgram *program, FILE *out) {
    fprintf(out, "/* Generated by neplang. Do not edit. */\n");
    fprintf(out, "#include <stddef.h>\n#include \"rt.h\"\n#define T(l, c) ((Token){ .line = (l), .col = (c) })\n\n");
    fprintf(out, "static Value G[%d];\n\n", program->global_count > 0 ? program->global_count : 1);
    for (int i = 0; i < program->function_count; i++) fprintf(out, "static Value fn_%d(Value *args);\n", i);
    fprintf(out, "\n");
    for (int i = 0; i < program->function_count; i++) emit_function(program, i, out);
    fprintf(out, "int main(void) {\n    for (int i = 0; i < %d; i++) G[i] = rt_unset();\n", program->global_count);
    fprintf(out, "    value_release(fn_0(NULL));\n    return 0;\n}\n");
}

static const char *runtime_dir(void) {
    const char *env = getenv("NEPLANG_RUNTIME");
    return env && *env ? env : NEPLANG_RUNTIME_DIR;
}

int cgen_build(const IrProgram *program, const char *output_path, int keep_c) {
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
    cgen_emit(program, out);
    fclose(out);

    const char *cc = getenv("CC");
    const char *dir = runtime_dir();
    size_t check_size = strlen(dir) + 16;
    char *check = malloc(check_size);
    if (!check) return 1;
    snprintf(check, check_size, "%s/rt.c", dir);
    FILE *probe = fopen(check, "r");
    free(check);
    if (!probe) {
        fprintf(stderr,
                "neplang: can't find the runtime sources for --build in '%s'.\n"
                "Set NEPLANG_RUNTIME to the 'src' folder of a NepLang checkout (or the installed "
                "share/neplang/runtime folder).\n", dir);
        remove(c_path);
        free(c_path);
        return 1;
    }
    fclose(probe);
    size_t size = 400 + strlen(dir) * 4 + n * 2;
    char *command = malloc(size);
    if (!command) return 1;
    snprintf(command, size, "%s -std=c11 -O2 -w -I'%s' -o '%s' '%s' '%s/value.c' '%s/rt.c' '%s/diagnostics.c'",
             cc && *cc ? cc : "cc", dir, output_path, c_path, dir, dir, dir);
    int status = system(command);
    if (status != 0) {
        fprintf(stderr, "neplang: the C compiler failed (command: %s)\n", command);
    }
    if (!keep_c) remove(c_path);
    free(command);
    free(c_path);
    return status == 0 ? 0 : 1;
}
