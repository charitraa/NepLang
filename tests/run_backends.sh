#!/usr/bin/env bash
# Runs every test program through the bytecode VM and through the
# native backend (NepLang -> C -> executable), and checks they behave like the interpreter:
# same stdout for .expected tests, and a non-zero exit for .error.np tests.
# Usage: tests/run_backends.sh [vm|native|all]
BIN=${BIN:-./bin/neplang}
WHICH=${1:-all}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0; skipped=0

run_one() {   # $1=backend $2=file (output without the \r that Windows programs print)
    run_raw "$@" | tr -d '\r'
    return "${PIPESTATUS[0]}"
}

run_raw() {
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
        # Programs the compiler declines stay interpreter-only.
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
            # compared as strings, like run_tests.sh (no process substitution: fragile on MSYS2)
            actual=$(run_one "$backend" "$file")
            want=$(cat "$expected")
            if [ "$actual" == "$want" ]; then
                pass=$((pass + 1))
            else
                echo "FAIL  $file  (expected '${want:0:200}', got '${actual:0:200}')"; fail=$((fail + 1))
            fi
        fi
    done
done
if [ "$WHICH" = all ] || [ "$WHICH" = vm ]; then
    echo "== ir"
    printf 'x = 2 + 3 * 4\nvana(x)\njabasamma 0 {\n    vana(1)\n}\n' > "$TMP/ir.np"
    ir=$("$BIN" --ir "$TMP/ir.np")
    if echo "${ir//$'\r'/}" | grep -q "const_int     14" && ! echo "${ir//$'\r'/}" | grep -q "binop"; then
        pass=$((pass + 1))
    else
        echo "FAIL  constant folding"; fail=$((fail + 1))
    fi
    if ! echo "${ir//$'\r'/}" | grep -q "jump_if_false" && ! echo "${ir//$'\r'/}" | grep -q "const_int     1$"; then
        pass=$((pass + 1))
    else
        echo "FAIL  dead branch removal"; fail=$((fail + 1))
    fi
    # the VM and the interpreter agree on a stdin-driven program
    printf 'n = sankhya(padha())\nvana(n * 2)\n' > "$TMP/in.np"
    if [ "$(echo 21 | "$BIN" --vm "$TMP/in.np" | tr -d '\r')" = 42 ]; then pass=$((pass + 1)); else echo "FAIL  vm stdin"; fail=$((fail + 1)); fi
fi
if [ "$WHICH" = all ] || [ "$WHICH" = native ]; then
    echo "== runtime location"
    # the normal runs above use the runtime built into neplang; NEPLANG_RUNTIME overrides it
    printf 'vana("ok")\n' > "$TMP/rt.np"
    if NEPLANG_RUNTIME=src "$BIN" --build "$TMP/rt.np" -o "$TMP/rt" 2>/dev/null && [ "$("$TMP/rt" | tr -d '\r')" = ok ]; then
        pass=$((pass + 1))
    else
        echo "FAIL  NEPLANG_RUNTIME=src"; fail=$((fail + 1))
    fi
    if NEPLANG_RUNTIME="$TMP/missing" "$BIN" --build "$TMP/rt.np" -o "$TMP/rt2" 2>/dev/null; then
        echo "FAIL  missing NEPLANG_RUNTIME folder (expected a failure)"; fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
fi
echo ""
echo "$pass passed, $fail failed ($skipped skipped: interpreter-only)"
[ "$fail" -eq 0 ]
