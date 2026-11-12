CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
DEFS = -DNEPLANG_RUNTIME_DIR=\"$(CURDIR)/src\" -DNEPLANG_EMBED_RUNTIME
SRC = src/lexer.c src/value.c src/ast.c src/parser.c src/modules.c src/classes.c src/checker.c src/interpreter.c src/rt.c src/ir.c src/vm.c src/cgen.c src/diagnostics.c src/main.c
BIN = bin/neplang
PREFIX ?= /usr/local
LDLIBS = -lm

RUNTIME_FILES = src/value.c src/value.h src/rt.c src/rt.h src/diagnostics.c src/diagnostics.h src/token.h src/ast.h
# Order matters: the runtime is joined into one C file (headers before the code that uses them).
EMBED_ORDER = src/token.h src/ast.h src/value.h src/diagnostics.h src/rt.h src/value.c src/rt.c src/diagnostics.c
EMBED = bin/runtime_embed.c

.PHONY: all debug test test-native test-all clean install uninstall wasm

all: $(BIN)

$(BIN): $(SRC) $(EMBED)
	mkdir -p bin
	$(CC) $(CFLAGS) $(DEFS) -o $(BIN) $(SRC) $(EMBED) $(LDLIBS)

# The runtime sources, built into neplang so `neplang build` needs no runtime folder.
# The generator is compiled without the -static etc. a release build may pass in CFLAGS.
$(EMBED): tools/embed_runtime.c $(EMBED_ORDER)
	mkdir -p bin
	$(CC) -std=c11 -O2 -o bin/embed_runtime tools/embed_runtime.c
	./bin/embed_runtime $(EMBED) $(EMBED_ORDER)

debug: $(EMBED)
	mkdir -p bin
	$(CC) $(CFLAGS) $(DEFS) -fsanitize=address,undefined -o $(BIN) $(SRC) $(EMBED) $(LDLIBS)

# Interpreter suites, then the same programs through the bytecode VM and the IR checks.
test: all
	bash tests/run_tests.sh
	bash tests/run_backends.sh vm

# Compiles every program to a native executable too (slower: runs the C compiler per test).
test-native: all
	bash tests/run_backends.sh native

test-all: test test-native

clean:
	rm -rf bin

# The website playground: the same interpreter compiled to WebAssembly with Emscripten (emcc).
# Programs run in a Web Worker; each run makes a fresh module instance (callMain once).
WASM_OUT = website/wasm
wasm:
	mkdir -p $(WASM_OUT)
	emcc -std=c11 -O2 -o $(WASM_OUT)/neplang.js $(SRC) -lm \
		-sMODULARIZE=1 -sEXPORT_NAME=createNepLang -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
		-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB -sFORCE_FILESYSTEM=1 \
		-sENVIRONMENT=worker -sEXPORTED_RUNTIME_METHODS=callMain,FS

# The installed binary looks for the runtime sources (needed by --build) in share/neplang/runtime.
install:
	$(MAKE) -B BIN=bin/neplang-install DEFS='-DNEPLANG_RUNTIME_DIR=\"$(PREFIX)/share/neplang/runtime\" -DNEPLANG_EMBED_RUNTIME' bin/neplang-install
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1 $(DESTDIR)$(PREFIX)/share/neplang/runtime
	install -m 755 bin/neplang-install $(DESTDIR)$(PREFIX)/bin/neplang
	install -m 644 $(RUNTIME_FILES) $(DESTDIR)$(PREFIX)/share/neplang/runtime/
	install -m 644 docs/neplang.1 $(DESTDIR)$(PREFIX)/share/man/man1/neplang.1
	rm -f bin/neplang-install

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/neplang $(DESTDIR)$(PREFIX)/share/man/man1/neplang.1
	rm -rf $(DESTDIR)$(PREFIX)/share/neplang
