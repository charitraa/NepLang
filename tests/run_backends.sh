#!/usr/bin/env bash
# Runs every test program that doesn't use classes through the bytecode VM and through the
# native backend (NepLang -> C -> executable), and checks they behave like the interpreter:
# same stdout for .expected tests, and a non-zero exit for .error.np tests.
# Usage: tests/run_backends.sh [vm|native|all]
BIN=${BIN:-./bin/neplang}
WHICH=${1:-all}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0; skipped=0

run_one() {   # $1=backend $2=file
    local backend=$1 file=$2 exe="$TMP/prog"
    if [ "$backend" = vm ]; then
        "$BIN" --vm "$file" 2>/dev/null </dev/null
    else
        rm -f "$exe"
        "$BIN" --build "$file" -o "$exe" 2>/dev/null || return 1
        "$exe" 2>/dev/null </dev/null
    fi
}

for backend in vm native; do
    [ "$WHICH" = all ] || [ "$WHICH" = "$backend" ] || continue
    echo "== $backend backend"
    for file in tests/*/*.np; do
        case "$file" in tests/*/lib/*|*/cyc_*.np) continue ;; esac
        # Programs the compiler declines (classes) stay interpreter-only.
        if "$BIN" --ir "$file" 2>&1 >/dev/null | grep -q "can't be compiled yet"; then
            skipped=$((skipped + 1)); continue
        fi
        if [[ "$file" == *.error.np ]]; then
            if run_one "$backend" "$file" >/dev/null; then
                echo "FAIL  $file (expected a failure)"; fail=$((fail + 1))
            else
                pass=$((pass + 1))
            fi
        else
            expected="${file%.np}.expected"
            [ -f "$expected" ] || continue
            if diff <(run_one "$backend" "$file") "$expected" >/dev/null; then
                pass=$((pass + 1))
            else
                echo "FAIL  $file"; fail=$((fail + 1))
            fi
        fi
    done
done
if [ "$WHICH" = all ] || [ "$WHICH" = vm ]; then
    echo "== ir"
    printf 'x = 2 + 3 * 4\nvana(x)\njabasamma 0 {\n    vana(1)\n}\n' > "$TMP/ir.np"
    ir=$("$BIN" --ir "$TMP/ir.np")
    if echo "$ir" | grep -q "const_int     14" && ! echo "$ir" | grep -q "binop"; then
        pass=$((pass + 1))
    else
        echo "FAIL  constant folding"; fail=$((fail + 1))
    fi
    if ! echo "$ir" | grep -q "jump_if_false" && ! echo "$ir" | grep -q "const_int     1$"; then
        pass=$((pass + 1))
    else
        echo "FAIL  dead branch removal"; fail=$((fail + 1))
    fi
    # classes are refused by the compiled backends with a clear message
    printf 'class A {\n    x = 1\n}\n' > "$TMP/cls.np"
    if "$BIN" --vm "$TMP/cls.np" 2>&1 | grep -q "can't be compiled yet"; then
        pass=$((pass + 1))
    else
        echo "FAIL  class refusal"; fail=$((fail + 1))
    fi
    # the VM and the interpreter agree on a stdin-driven program
    printf 'n = sankhya(padha())\nvana(n * 2)\n' > "$TMP/in.np"
    if [ "$(echo 21 | "$BIN" --vm "$TMP/in.np")" = 42 ]; then pass=$((pass + 1)); else echo "FAIL  vm stdin"; fail=$((fail + 1)); fi
fi
echo ""
echo "$pass passed, $fail failed ($skipped skipped: classes are interpreter-only)"
[ "$fail" -eq 0 ]
