#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules.h"
#include "lexer.h"
#include "parser.h"
#include "diagnostics.h"
#include "rt.h"

#define MAX_IMPORT_DEPTH 32

typedef struct {
    char *path;      /* as joined, used to skip files that were already imported */
    char *source;    /* kept alive: AST tokens point into it */
} LoadedModule;

static LoadedModule *loaded;
static int loaded_count;
static int loaded_capacity;

/* Standard-library modules named by 'import math' (built into the runtime, no .np file). */
static char **std_imports;
static int std_count;

static int std_imported(const char *name, int length) {
    for (int i = 0; i < std_count; i++) {
        if ((int)strlen(std_imports[i]) == length && strncmp(std_imports[i], name, (size_t)length) == 0) return 1;
    }
    return 0;
}

static void add_std_import(const char *name) {
    if (std_imported(name, (int)strlen(name))) return;
    char **grown = realloc(std_imports, sizeof(char *) * (size_t)(std_count + 1));
    if (!grown) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    std_imports = grown;
    size_t n = strlen(name);
    std_imports[std_count] = malloc(n + 1);
    if (!std_imports[std_count]) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    memcpy(std_imports[std_count++], name, n + 1);
}

/* Is this expression a standard-library module name, like the 'math' in 'math.sqrt(x)'? */
static int is_module_ref(const AstNode *node) {
    return node && node->type == AST_VARIABLE &&
           std_imported(node->as.variable.name.start, node->as.variable.name.length);
}

/* Turns 'math.sqrt(x)' into a module call (call.module = "math", no receiver) and the constant
 * 'math.PI' into the call 'math.PI()', everywhere in the tree. */
static void rewrite_module_uses(AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_CALL:
            if (is_module_ref(node->as.call.receiver)) {
                node->as.call.module = node->as.call.receiver->as.variable.name;
                ast_free(node->as.call.receiver);
                node->as.call.receiver = NULL;
            }
            rewrite_module_uses(node->as.call.receiver);
            for (int i = 0; i < node->as.call.arg_count; i++) rewrite_module_uses(node->as.call.args[i]);
            break;
        case AST_FIELD:
            if (is_module_ref(node->as.field.object)) {
                Token module = node->as.field.object->as.variable.name;
                Token name = node->as.field.name;
                ast_free(node->as.field.object);
                memset(&node->as, 0, sizeof(node->as));
                node->type = AST_CALL;
                node->token = name;
                node->as.call.name = name;
                node->as.call.module = module;
                break;
            }
            rewrite_module_uses(node->as.field.object);
            break;
        case AST_BINOP:
            rewrite_module_uses(node->as.binop.left);
            rewrite_module_uses(node->as.binop.right);
            break;
        case AST_UNARY: rewrite_module_uses(node->as.unary.operand); break;
        case AST_ASSIGN: rewrite_module_uses(node->as.assign.value); break;
        case AST_PRINT: rewrite_module_uses(node->as.print.expr); break;
        case AST_IF:
            rewrite_module_uses(node->as.if_stmt.condition);
            rewrite_module_uses(node->as.if_stmt.then_block);
            rewrite_module_uses(node->as.if_stmt.else_branch);
            break;
        case AST_WHILE:
            rewrite_module_uses(node->as.while_stmt.condition);
            rewrite_module_uses(node->as.while_stmt.body);
            break;
        case AST_FOR:
            rewrite_module_uses(node->as.for_stmt.start);
            rewrite_module_uses(node->as.for_stmt.end);
            rewrite_module_uses(node->as.for_stmt.body);
            break;
        case AST_FOR_EACH:
            rewrite_module_uses(node->as.for_each.iterable);
            rewrite_module_uses(node->as.for_each.body);
            break;
        case AST_ARRAY:
            for (int i = 0; i < node->as.array.count; i++) rewrite_module_uses(node->as.array.items[i]);
            break;
        case AST_INDEX:
            rewrite_module_uses(node->as.index.target);
            rewrite_module_uses(node->as.index.index);
            break;
        case AST_INDEX_ASSIGN:
            rewrite_module_uses(node->as.index_assign.target);
            rewrite_module_uses(node->as.index_assign.value);
            break;
        case AST_FIELD_ASSIGN:
            rewrite_module_uses(node->as.field_assign.target);
            rewrite_module_uses(node->as.field_assign.value);
            break;
        case AST_FUNCTION: rewrite_module_uses(node->as.function.body); break;
        case AST_RETURN: rewrite_module_uses(node->as.return_stmt.value); break;
        case AST_CLASS:
            /* before inheritance is resolved, every field and method here is the class's own */
            for (int i = 0; i < node->as.class_decl.field_count; i++) rewrite_module_uses(node->as.class_decl.fields[i].value);
            for (int i = 0; i < node->as.class_decl.method_count; i++) rewrite_module_uses(node->as.class_decl.methods[i]);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) rewrite_module_uses(node->as.block.statements[i]);
            break;
        default: break;
    }
}

static char *copy_string(const char *s) {
    size_t n = strlen(s);
    char *copy = malloc(n + 1);
    if (!copy) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    memcpy(copy, s, n + 1);
    return copy;
}

static int is_separator(char c) {
    return c == '/' || c == '\\';
}

/* "dir/prog.np" + "util.np" -> "dir/util.np" */
static char *join_next_to(const char *file, const char *name) {
    size_t dir_length = 0;
    for (size_t i = 0; file[i]; i++) {
        if (is_separator(file[i])) dir_length = i + 1;
    }
    char *path = malloc(dir_length + strlen(name) + 1);
    if (!path) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    memcpy(path, file, dir_length);
    strcpy(path + dir_length, name);
    return path;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fprintf(stderr, "neplang: out of memory\n");
        exit(1);
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    buffer[got] = '\0';
    fclose(file);
    return buffer;
}

/* Finds the file an import refers to: next to the importing file, then in NEPLANG_PATH. */
static char *locate(const char *importer, const char *name, char **source_out) {
    char *candidate = join_next_to(importer, name);
    *source_out = read_file(candidate);
    if (*source_out) return candidate;
    free(candidate);

    const char *search = getenv("NEPLANG_PATH");
    while (search && *search) {
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        const char *end = strchr(search, separator);
        size_t length = end ? (size_t)(end - search) : strlen(search);
        if (length > 0) {
            char *path = malloc(length + strlen(name) + 2);
            if (!path) {
                fprintf(stderr, "neplang: out of memory\n");
                exit(1);
            }
            memcpy(path, search, length);
            path[length] = '/';
            strcpy(path + length + 1, name);
            *source_out = read_file(path);
            if (*source_out) return path;
            free(path);
        }
        search = end ? end + 1 : NULL;
    }
    return NULL;
}

static int already_loaded(const char *path) {
    for (int i = 0; i < loaded_count; i++) {
        if (strcmp(loaded[i].path, path) == 0) return 1;
    }
    return 0;
}

static void remember(char *path, char *source) {
    if (loaded_count == loaded_capacity) {
        loaded_capacity = loaded_capacity ? loaded_capacity * 2 : 8;
        loaded = realloc(loaded, sizeof(LoadedModule) * (size_t)loaded_capacity);
        if (!loaded) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
    }
    loaded[loaded_count++] = (LoadedModule){ path, source };
}

static void append_statement(AstNode *block, AstNode *statement) {
    ast_block_append(block, statement);
}

static int resolve_into(AstNode *program, const char *file, int depth);

/* Parses `path` (already read into `source`) and returns its statements, imports resolved. */
static AstNode *load_module(char *path, char *source, int depth) {
    diagnostics_register_source(source, path);

    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer, source, path);
    AstNode *module = parser_parse_program(&parser);
    if (parser.had_error || resolve_into(module, path, depth + 1) != 0) {
        ast_free(module);
        return NULL;
    }
    return module;
}

/* Rebuilds program->statements with each AST_IMPORT replaced by the imported statements. */
static int resolve_into(AstNode *program, const char *file, int depth) {
    AstNode *result = ast_new_block();
    int i;

    for (i = 0; i < program->as.block.count; i++) {
        AstNode *stmt = program->as.block.statements[i];
        if (stmt->type != AST_IMPORT) {
            append_statement(result, stmt);
            continue;
        }

        const char *name = stmt->as.import_stmt.path;
        char *filename = stmt->as.import_stmt.is_name
                             ? malloc(strlen(name) + 4)
                             : copy_string(name);
        if (!filename) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
        if (stmt->as.import_stmt.is_name) sprintf(filename, "%s.np", name);

        char message[400];
        if (depth >= MAX_IMPORT_DEPTH) {
            snprintf(message, sizeof(message), "Imports are nested too deeply while importing '%s'.", filename);
            report_error("", file, stmt->token, message);
            free(filename);
            goto fail;
        }

        if (stmt->as.import_stmt.is_name && rt_is_module(name, (int)strlen(name))) {
            /* 'import math': a module of the standard library, built into the runtime. (A file of
             * the same name can still be imported with quotes: import "math.np".) */
            add_std_import(name);
            free(filename);
            ast_free(stmt);
            continue;
        }

        char *source = NULL;
        char *path = locate(file, filename, &source);
        if (!path) {
            snprintf(message, sizeof(message),
                     "Could not find module '%s'. It is looked up next to this file, then in NEPLANG_PATH. "
                     "The built-in modules are math, string, array, io, time and os.",
                     filename);
            report_error("", file, stmt->token, message);
            free(filename);
            goto fail;
        }
        free(filename);

        if (already_loaded(path)) {  /* also breaks import cycles */
            free(path);
            free(source);
            ast_free(stmt);
            continue;
        }
        remember(path, source);

        AstNode *module = load_module(path, source, depth);
        if (!module) goto fail;
        for (int j = 0; j < module->as.block.count; j++) {
            append_statement(result, module->as.block.statements[j]);
        }
        module->as.block.count = 0;  /* statements now belong to `result` */
        ast_free(module);
        ast_free(stmt);
    }

    /* Swap the rebuilt statement list into `program`. */
    free(program->as.block.statements);
    program->as.block = result->as.block;
    result->as.block.statements = NULL;
    result->as.block.count = 0;
    ast_free(result);
    return 0;

fail:
    /* `result` holds statements already moved out of `program`; give them back for freeing. */
    for (int i = 0; i < result->as.block.count; i++) ast_free(result->as.block.statements[i]);
    result->as.block.count = 0;
    ast_free(result);
    /* Statements before `i` were moved into `result`; the rest are still owned by `program`. */
    for (int j = i; j < program->as.block.count; j++) ast_free(program->as.block.statements[j]);
    program->as.block.count = 0;
    return 1;
}

int modules_resolve(AstNode *program, const char *main_path) {
    int has_import = 0;
    for (int i = 0; i < program->as.block.count; i++) {
        if (program->as.block.statements[i]->type == AST_IMPORT) has_import = 1;
    }
    if (has_import) {
        remember(copy_string(main_path), NULL);
        if (resolve_into(program, main_path, 0) != 0) return 1;
    }
    if (std_count > 0) rewrite_module_uses(program);
    return 0;
}

void modules_free(void) {
    for (int i = 0; i < loaded_count; i++) {
        free(loaded[i].path);
        free(loaded[i].source);
    }
    free(loaded);
    loaded = NULL;
    loaded_count = loaded_capacity = 0;
    for (int i = 0; i < std_count; i++) free(std_imports[i]);
    free(std_imports);
    std_imports = NULL;
    std_count = 0;
}
