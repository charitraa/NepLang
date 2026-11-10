#!/usr/bin/env bash
set -u

BIN="bin/neplang"
pass=0
fail=0

for DIR in tests/*/; do
DIR="${DIR%/}"
echo "== $(basename "$DIR")"

for src in "$DIR"/*.error.np; do
    [ -e "$src" ] || continue
    name=$(basename "$src")
    "$BIN" "$src" >/dev/null 2>/dev/null
    code=$?
    if [ "$code" -ne 0 ]; then
        echo "PASS  $name (correctly failed)"
        pass=$((pass + 1))
    else
        echo "FAIL  $name (expected failure, got exit code 0)"
        fail=$((fail + 1))
    fi
done

for src in "$DIR"/*.np; do
    [ -e "$src" ] || continue
    case "$src" in
        *.error.np) continue ;;
    esac
    name=$(basename "$src" .np)
    expected_file="$DIR/$name.expected"
    [ -e "$expected_file" ] || continue

    actual=$("$BIN" "$src" 2>/dev/null)
    actual=${actual//$'\r'/}   # Windows programs print \r\n
    expected=$(cat "$expected_file")

    if [ "$actual" == "$expected" ]; then
        echo "PASS  $name.np"
        pass=$((pass + 1))
    else
        echo "FAIL  $name.np  (expected '$expected', got '$actual')"
        fail=$((fail + 1))
    fi
done

done

echo "== cli"
if "$BIN" tests/calculator/basic.expected >/dev/null 2>&1; then
    echo "FAIL  non-.np file was accepted"
    fail=$((fail + 1))
else
    echo "PASS  non-.np file rejected"
    pass=$((pass + 1))
fi

# cli_check NAME EXPECTED ACTUAL
cli_check() {
    if [ "${3//$'\r'/}" == "$2" ]; then
        echo "PASS  $1"
        pass=$((pass + 1))
    else
        echo "FAIL  $1  (expected '$2', got '$3')"
        fail=$((fail + 1))
    fi
}

TMPDIR_CLI=$(mktemp -d)
trap 'rm -rf "$TMPDIR_CLI"' EXIT
printf 'import os\nvana(os.args())\n' > "$TMPDIR_CLI/args.np"
cli_check "program arguments" '["a", "b c"]' "$("$BIN" "$TMPDIR_CLI/args.np" a "b c" 2>&1)"
cli_check "run subcommand" '["x"]' "$("$BIN" run "$TMPDIR_CLI/args.np" x 2>&1)"
# relative path: on Windows (MSYS2) an absolute /tmp path reaches neplang rewritten as D:/...
cli_check "check subcommand" "args.np: no errors found." "$(cd "$TMPDIR_CLI" && "$OLDPWD/$BIN" check args.np 2>&1)"
"$BIN" check tests/types/int_gets_string.error.np >/dev/null 2>&1
cli_check "check reports errors" "1" "$?"

# the REPL: expressions are echoed, state persists, errors don't end the session
repl_out=$(printf 'x = 5\nx * 2\nkaam sq(n) {\n    farkau n * n\n}\nsq(x)\nvana(nope)\n"after error"\nthapa([1], 2)\nexit\n' | "$BIN" 2>/dev/null | tr -d '\r' | tail -n +2 | sed -E -e 's/(> |\.\.\. )//g' | tr '\n' '|')
cli_check "repl session" "10|25|after error|" "$repl_out"

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
