#!/bin/sh
# Each tests/NAME.lsp is compiled (with debug info) and run; its output must equal tests/NAME.out.
# Each tests/errors/NAME.lsp must fail to compile with the message in its
# first line ("; error: ...").
cd "$(dirname "$0")"
mkdir -p out
pass=0 fail=0
for src in *.lsp; do
    name=${src%.lsp}
    extra=; [ -e "$name.c" ] && extra="$name.c"   # C code linked into the test
    [ "$name" = defines ] && extra="-DWIDTH=640 -DNAME=teapot -DFAST"
    if ../lisc "$src" $extra -g -o "out/$name" 2>"out/$name.err" && "./out/$name" >"out/$name.txt" 2>&1 && cmp -s "out/$name.txt" "$name.out"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); echo "FAIL $src"; cat "out/$name.err"; diff "$name.out" "out/$name.txt" | head -20
    fi
done
for src in errors/*.lsp; do
    [ -e "$src" ] || continue
    want=$(head -1 "$src" | sed 's/^; error: //')
    got=$(../lisc "$src" -o out/err 2>&1)
    case "$got" in
    *"$want"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); echo "FAIL $src: want '$want', got '$got'" ;;
    esac
done
# Each tests/checks/NAME.lsp, built with --checks, must stop with the message in
# its first line ("; abort: ...").
for src in checks/*.lsp; do
    [ -e "$src" ] || continue
    want=$(head -1 "$src" | sed 's/^; abort: //')
    if ../lisc "$src" --checks -o out/check 2>out/check.err && ! ./out/check 2>out/check.txt >/dev/null && grep -qF -- "$want" out/check.txt; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); echo "FAIL $src: want '$want', got '$(cat out/check.err out/check.txt)'"
    fi
done
# Each tests/warnings/NAME.lsp must compile with the warning in its first line.
for src in warnings/*.lsp; do
    [ -e "$src" ] || continue
    want=$(head -1 "$src" | sed 's/^; warning: //')
    got=$(../lisc "$src" -o out/warn 2>&1)
    case "$got" in
    *"warning: $want"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); echo "FAIL $src: want warning '$want', got '$got'" ;;
    esac
done
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
