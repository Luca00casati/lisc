#!/bin/sh
# Each tests/NAME.lsp is compiled and run; its output must equal tests/NAME.out.
# Each tests/errors/NAME.lsp must fail to compile with the message in its
# first line ("; error: ...").
cd "$(dirname "$0")"
mkdir -p out
pass=0 fail=0
for src in *.lsp; do
    name=${src%.lsp}
    extra=; [ -e "$name.c" ] && extra="$name.c"   # C code linked into the test
    [ "$name" = defines ] && extra="-DWIDTH=640 -DNAME=teapot -DFAST"
    if ../lisc "$src" $extra -o "out/$name" 2>"out/$name.err" && "./out/$name" >"out/$name.txt" 2>&1 && cmp -s "out/$name.txt" "$name.out"; then
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
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
