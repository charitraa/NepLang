#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "classes.h"
#include "modules.h"
#include "ir.h"
#include "vm.h"
#include "cgen.h"
#include "diagnostics.h"
#include "interpreter.h"
#include "rt.h"

#define NEPLANG_VERSION "1.0.2"

static int has_np_extension(const char *path) {
    size_t len = strlen(path);
    return len > 3 && strcmp(path + len - 3, ".np") == 0;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "neplang: could not open file '%s'\n", path);
        exit(1);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }

    size_t read = fread(buffer, 1, (size_t)size, file);
    buffer[read] = '\0';
    fclose(file);
    return buffer;
}

static void usage(FILE *out) {
    fprintf(out,
            "NepLang %s\n"
            "usage: neplang [command] <file.np> [program arguments...]\n"
            "  neplang                 start the interactive REPL\n"
            "  neplang file.np         run with the tree-walking interpreter\n"
            "  neplang run file.np     the same\n"
            "  neplang check file.np   only check the program for errors (nothing runs)\n"
            "  neplang build file.np -o OUT\n"
            "                          compile to a native executable through the system C compiler\n"
            "options (before the file):\n"
            "  --vm             compile to bytecode and run it on the virtual machine\n"
            "  --ir             print the optimized intermediate representation\n"
            "  --emit-c         print the generated C source\n"
            "  --build -o OUT   same as 'build'\n"
            "  --version        print the version\n"
            "Arguments after the file are passed to the program: see os.args().\n",
            NEPLANG_VERSION);
}

typedef enum { MODE_INTERPRET, MODE_CHECK, MODE_VM, MODE_IR, MODE_EMIT_C, MODE_BUILD } Mode;

/* ---- REPL ---- */

/* Is the input so far unfinished: an open '{', '(' or '[', or an unclosed string? */
static int needs_more(const char *text) {
    int depth = 0, in_string = 0;
    for (const char *p = text; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_string = 0;
            else if (*p == '\n') in_string = 0;
            continue;
        }
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            if (!*p) break;
            continue;
        }
        if (*p == '"') in_string = 1;
        else if (*p == '{' || *p == '(' || *p == '[') depth++;
        else if (*p == '}' || *p == ')' || *p == ']') depth--;
    }
    return depth > 0;
}

static char *read_line(const char *prompt) {
    fputs(prompt, stdout);
    fflush(stdout);
    size_t cap = 256, len = 0;
    char *line = malloc(cap);
    if (!line) exit(1);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 2 >= cap) {
            cap *= 2;
            line = realloc(line, cap);
            if (!line) exit(1);
        }
        line[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        free(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

static int repl(void) {
    printf("NepLang %s REPL. Type 'exit' (or press Ctrl-D) to leave.\n", NEPLANG_VERSION);
    Interpreter *interp = interpreter_new(1);
    AstNode **decls = NULL;       /* every class/interface defined so far (for inheritance) */
    int decl_count = 0;
    AstNode **programs = NULL;    /* kept alive: functions, classes and variable names point into them */
    char **sources = NULL;
    int program_count = 0;
    jmp_buf recover;

    for (;;) {
        char *text = read_line("> ");
        if (!text) {
            printf("\n");
            break;
        }
        if (strcmp(text, "exit") == 0 || strcmp(text, "quit") == 0) {
            free(text);
            break;
        }
        while (needs_more(text)) {
            char *more = read_line("... ");
            if (!more) break;
            size_t n = strlen(text), m = strlen(more);
            char *joined = malloc(n + m + 2);
            if (!joined) exit(1);
            memcpy(joined, text, n);
            joined[n] = '\n';
            memcpy(joined + n + 1, more, m + 1);
            free(text);
            free(more);
            text = joined;
        }

        diagnostics_register_source(text, "<repl>");
        Lexer lexer;
        lexer_init(&lexer, text);
        Parser parser;
        parser_init(&parser, &lexer, text, "<repl>");
        parser.repl = 1;
        AstNode *program = parser_parse_program(&parser);

        programs = realloc(programs, sizeof(AstNode *) * (size_t)(program_count + 1));
        sources = realloc(sources, sizeof(char *) * (size_t)(program_count + 1));
        if (!programs || !sources) exit(1);
        programs[program_count] = program;
        sources[program_count++] = text;
        if (parser.had_error || modules_resolve(program, "<repl>") != 0) continue;

        int first = decl_count;
        for (int i = 0; i < program->as.block.count; i++) {
            AstNode *n = program->as.block.statements[i];
            if (n->type == AST_CLASS || n->type == AST_INTERFACE) {
                decls = realloc(decls, sizeof(AstNode *) * (size_t)(decl_count + 1));
                if (!decls) exit(1);
                decls[decl_count++] = n;
            }
        }
        if (decl_count > first && classes_resolve(decls, decl_count, first) > 0) {
            decl_count = first;
            continue;
        }

        rt_recover = &recover;
        if (setjmp(recover) == 0) {
            interpreter_run(interp, program);
        } else {
            interpreter_recover(interp);
        }
        rt_recover = NULL;
        fflush(stdout);
    }

    interpreter_free(interp);
    for (int i = 0; i < program_count; i++) {
        ast_free(programs[i]);
        free(sources[i]);
    }
    free(programs);
    free(sources);
    free(decls);
    modules_free();
    diagnostics_clear_sources();
    return 0;
}

int main(int argc, char **argv) {
    Mode mode = MODE_INTERPRET;
    const char *filename = NULL;
    const char *output = NULL;
    int program_args = argc;   /* index of the first argument that belongs to the program */

    int i = 1;
    if (i < argc && strcmp(argv[i], "run") == 0) i++;
    else if (i < argc && strcmp(argv[i], "check") == 0) { mode = MODE_CHECK; i++; }
    else if (i < argc && strcmp(argv[i], "build") == 0) { mode = MODE_BUILD; i++; }
    else if (i < argc && strcmp(argv[i], "repl") == 0) return repl();

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--vm") == 0) mode = MODE_VM;
        else if (strcmp(argv[i], "--ir") == 0) mode = MODE_IR;
        else if (strcmp(argv[i], "--emit-c") == 0) mode = MODE_EMIT_C;
        else if (strcmp(argv[i], "--build") == 0) mode = MODE_BUILD;
        else if (strcmp(argv[i], "--check") == 0) mode = MODE_CHECK;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (strcmp(argv[i], "--version") == 0) { printf("NepLang %s\n", NEPLANG_VERSION); return 0; }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(stdout); return 0; }
        else if (argv[i][0] == '-' && argv[i][1] == '-') { usage(stderr); return 1; }
        else {
            filename = argv[i];
            program_args = i + 1;
            /* 'build file.np -o out' */
            if (mode == MODE_BUILD && i + 2 < argc && strcmp(argv[i + 1], "-o") == 0) {
                output = argv[i + 2];
                program_args = i + 3;
            }
            break;
        }
    }
    if (!filename) {
        if (argc == 1) return repl();
        usage(stderr);
        return 1;
    }
    if (mode == MODE_BUILD && !output) {
        fprintf(stderr, "neplang: build needs an output file: neplang build file.np -o program\n");
        return 1;
    }

    if (!has_np_extension(filename)) {
        fprintf(stderr, "neplang: '%s' is not a NepLang source file. NepLang files must end in '.np'.\n", filename);
        return 1;
    }
    rt_set_args(argc - program_args, argv + program_args);
    char *source = read_file(filename);
    diagnostics_register_source(source, filename);

    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer, source, filename);

    AstNode *program = parser_parse_program(&parser);
    int status = 0;

    if (parser.had_error || modules_resolve(program, filename) != 0 || classes_resolve_program(program) > 0 ||
        check_program(program, source, filename) > 0) {
        status = 1;
    } else if (mode == MODE_CHECK) {
        printf("%s: no errors found.\n", filename);
    } else if (mode == MODE_INTERPRET) {
        interpret(program);
    } else {
        IrProgram *ir = ir_compile(program, source, filename);
        if (!ir) {
            status = 1;
        } else {
            ir_optimize(ir);
            ir_finalize(ir);
            if (mode == MODE_VM) vm_run(ir);
            else if (mode == MODE_IR) ir_dump(ir, stdout);
            else if (mode == MODE_EMIT_C) cgen_emit(ir, stdout);
            else status = cgen_build(ir, output, 0);
            ir_free(ir);
        }
    }

    fflush(stdout);
    ast_free(program);
    modules_free();
    diagnostics_clear_sources();
    free(source);
    return status;
}
