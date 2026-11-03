#!/bin/sh
# Each tests/NAME.lsp is compiled (with debug info) and run; its output must equal tests/NAME.out.
# Each tests/errors/NAME.lsp must fail to compile with the message in its
# first line ("; error: ..."), and any "; note: ..." lines after it.
cd "$(dirname "$0")"
mkdir -p out
pass=0 fail=0
# C imports are cached in a fresh directory: the first test of each module
# imports it with libclang, the others read it back
rm -rf out/cache
export XDG_CACHE_HOME="$PWD/out/cache"
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
    ok=1
    case "$got" in *"$want"*) ;; *) ok=0 ;; esac
    # "; note: ..." lines after it: notes the output must have too
    notes=$(sed -n '2,$p' "$src" | grep '^; note: ' | sed 's/^; note: //')
    if [ -n "$notes" ]; then
        nl='
'
        old_ifs=$IFS; IFS=$nl
        for note in $notes; do
            case "$got" in *"$note"*) ;; *) ok=0; want="$want / $note" ;; esac
        done
        IFS=$old_ifs
    fi
    if [ $ok = 1 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $src: want '$want', got '$got'"; fi
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
# The C import cache must notice a header that changed (even at the same size).
mkdir -p out/hc
printf '#define ANSWER 41\nstruct pt { int x, y; };\n' >out/hc/h.h
printf '(import "libc")\n(import-c "h.h" :as h)\n(defn main [] i32 (libc::printf c"%%d %%lu\\n" h::ANSWER (sizeof h::pt)) 0)\n' >out/hc/t.lsp
got=$(../lisc out/hc/t.lsp -o out/hc/t && ./out/hc/t && ../lisc out/hc/t.lsp -o out/hc/t && ./out/hc/t)
sleep 0.01
printf '#define ANSWER 42\nstruct pt { int x, y; };\n' >out/hc/h.h
got="$got $(../lisc out/hc/t.lsp -o out/hc/t && ./out/hc/t)"
printf '#define ANSWER 42\nstruct pt { long x, y; };\n' >out/hc/h.h
got="$got $(../lisc out/hc/t.lsp -o out/hc/t && ./out/hc/t)"
if [ "$got" = "41 8
41 8 42 8 42 16" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL C import cache: got '$got'"; fi
# The object cache: a rebuild with nothing changed compiles nothing; a change
# in one module compiles that module (and the program's output follows it).
mkdir -p out/oc
printf '(defn twice [x i32] i32 (* 2 x))\n(defn big [x i32] i32 (let [s 0] (for [i 0 x] (+= s (* i i))) s))\n' >out/oc/b.lsp
printf '(import "b")\n(defn main [] i32 (println (b::twice 21) " " (b::big 10)) 0)\n' >out/oc/a.lsp
oc() { ../lisc out/oc/a.lsp -o out/oc/a --time 2>&1 | sed -n 's/.*parts, \([0-9]*\) compiled.*/\1/p'; }
got="$(oc) $(./out/oc/a) $(oc)"
printf '(defn twice [x i32] i32 (* 3 x))\n(defn big [x i32] i32 (let [s 0] (for [i 0 x] (+= s (* i i))) s))\n' >out/oc/b.lsp
got="$got $(oc) $(./out/oc/a)"
if [ "$got" = "2 42 285 0 2 63 285" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL object cache: got '$got'"; fi
# lisc fmt: tests/fmt/in.lsp formatted is tests/fmt/out.lsp (and that stays as it is);
# the standard library and the tests are formatted already
cp fmt/in.lsp out/fmt.lsp
if ../lisc fmt out/fmt.lsp && cmp -s out/fmt.lsp fmt/out.lsp && ../lisc fmt --check fmt/out.lsp ../lib/std/*.lsp *.lsp mods/*.lsp >out/fmt.txt; then
    pass=$((pass + 1))
else
    fail=$((fail + 1)); echo "FAIL lisc fmt"; diff fmt/out.lsp out/fmt.lsp | head -20; cat out/fmt.txt
fi
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
