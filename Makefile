CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
DEFS = -DNEPLANG_RUNTIME_DIR=\"$(CURDIR)/src\"
SRC = src/lexer.c src/value.c src/ast.c src/parser.c src/modules.c src/checker.c src/interpreter.c src/rt.c src/ir.c src/vm.c src/cgen.c src/diagnostics.c src/main.c
BIN = bin/neplang
PREFIX ?= /usr/local

RUNTIME_FILES = src/value.c src/value.h src/rt.c src/rt.h src/diagnostics.c src/diagnostics.h src/token.h src/ast.h

.PHONY: all debug test test-native test-all clean install uninstall

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p bin
	$(CC) $(CFLAGS) $(DEFS) -o $(BIN) $(SRC)

debug:
	mkdir -p bin
	$(CC) $(CFLAGS) $(DEFS) -fsanitize=address,undefined -o $(BIN) $(SRC)

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

# The installed binary looks for the runtime sources (needed by --build) in share/neplang/runtime.
install:
	$(MAKE) -B BIN=bin/neplang-install DEFS='-DNEPLANG_RUNTIME_DIR=\"$(PREFIX)/share/neplang/runtime\"' bin/neplang-install
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1 $(DESTDIR)$(PREFIX)/share/neplang/runtime
	install -m 755 bin/neplang-install $(DESTDIR)$(PREFIX)/bin/neplang
	install -m 644 $(RUNTIME_FILES) $(DESTDIR)$(PREFIX)/share/neplang/runtime/
	install -m 644 docs/neplang.1 $(DESTDIR)$(PREFIX)/share/man/man1/neplang.1
	rm -f bin/neplang-install

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/neplang $(DESTDIR)$(PREFIX)/share/man/man1/neplang.1
	rm -rf $(DESTDIR)$(PREFIX)/share/neplang
