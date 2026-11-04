#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diagnostics.h"

typedef struct {
    const char *source;
    size_t length;
    const char *filename;
} SourceEntry;

static SourceEntry *sources;
static int source_count;
static int source_capacity;

void diagnostics_register_source(const char *source, const char *filename) {
    if (source_count == source_capacity) {
        source_capacity = source_capacity ? source_capacity * 2 : 8;
        sources = realloc(sources, sizeof(SourceEntry) * (size_t)source_capacity);
        if (!sources) {
            fprintf(stderr, "neplang: out of memory\n");
            exit(1);
        }
    }
    sources[source_count++] = (SourceEntry){ source, strlen(source), filename };
}

void diagnostics_clear_sources(void) {
    free(sources);
    sources = NULL;
    source_count = source_capacity = 0;
}

void report_error(const char *source, const char *filename, Token token, const char *message) {
    for (int i = 0; i < source_count; i++) {
        if (token.start >= sources[i].source && token.start <= sources[i].source + sources[i].length) {
            source = sources[i].source;
            filename = sources[i].filename;
            break;
        }
    }

    const char *line_start = token.start;
    while (line_start > source && line_start[-1] != '\n') {
        line_start--;
    }

    const char *line_end = token.start;
    while (*line_end != '\n' && *line_end != '\0') {
        line_end++;
    }

    fprintf(stderr, "Error in %s at line %d, column %d:\n\n", filename, token.line, token.col);
    fprintf(stderr, "    %.*s\n", (int)(line_end - line_start), line_start);
    fprintf(stderr, "    %*s^\n\n", token.col - 1, "");
    fprintf(stderr, "%s\n", message);
}
