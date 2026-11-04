#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "modules.h"
#include "ir.h"
#include "vm.h"
#include "cgen.h"
#include "diagnostics.h"
#include "interpreter.h"

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

static void usage(void) {
    fprintf(stderr,
            "usage: neplang [mode] <file.np>\n"
            "  (no mode)        run with the tree-walking interpreter (supports everything)\n"
            "  --vm             compile to bytecode and run it on the virtual machine\n"
            "  --ir             print the optimized intermediate representation\n"
            "  --emit-c         print the generated C source\n"
            "  --build -o OUT   compile to a native executable through the system C compiler\n"
            "The compiled modes (--vm, --ir, --emit-c, --build) don't support classes yet.\n");
}

typedef enum { MODE_INTERPRET, MODE_VM, MODE_IR, MODE_EMIT_C, MODE_BUILD } Mode;

int main(int argc, char **argv) {
    Mode mode = MODE_INTERPRET;
    const char *filename = NULL;
    const char *output = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--vm") == 0) mode = MODE_VM;
        else if (strcmp(argv[i], "--ir") == 0) mode = MODE_IR;
        else if (strcmp(argv[i], "--emit-c") == 0) mode = MODE_EMIT_C;
        else if (strcmp(argv[i], "--build") == 0) mode = MODE_BUILD;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-') { usage(); return 1; }
        else if (!filename) filename = argv[i];
        else { usage(); return 1; }
    }
    if (!filename) {
        usage();
        return 1;
    }
    if (mode == MODE_BUILD && !output) {
        fprintf(stderr, "neplang: --build needs an output file: neplang --build file.np -o program\n");
        return 1;
    }

    if (!has_np_extension(filename)) {
        fprintf(stderr, "neplang: '%s' is not a NepLang source file. NepLang files must end in '.np'.\n", filename);
        return 1;
    }
    char *source = read_file(filename);
    diagnostics_register_source(source, filename);

    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer, source, filename);

    AstNode *program = parser_parse_program(&parser);
    int status = 0;

    if (parser.had_error || modules_resolve(program, filename) != 0 ||
        check_program(program, source, filename) > 0) {
        status = 1;
    } else if (mode == MODE_INTERPRET) {
        interpret(program, source, filename);
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

    ast_free(program);
    modules_free();
    free(source);
    return status;
}
