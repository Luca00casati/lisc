#!/usr/bin/env python3
"""Rewrite lisc functions to the explicit-return style, keeping comments and layout:
(defn f [a T] R body... e)  -> (defn f [a T] -> R body... (return e))
(defn f [a T] void body...) -> (defn f [a T] body...)
the same for defop and (fn [a T] R ...) literals. The return goes into the
tail of let / do / if / cond / case / match / unchecked, else around the last form.
Run it once per file (a void function, once converted, has no result type
to tell it apart), then lisc fmt (the indentation of wrapped forms). Prints
what needs a hand: a body that ends in a template's ,@splice.
Usage: returns.py file.lsp ...   (rewrites in place)"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from brackets import parse  # noqa: E402


def head(n):
    return n.items[0].text if n.kind == '(' and n.items and n.items[0].kind == 'atom' else None


def convert(src, path):
    edits = []  # (position, text to insert, length to delete)
    notes = []

    def line(pos):
        return src.count('\n', 0, pos) + 1

    def wrap(n):
        edits.append((n.start, '(return ', 0))
        edits.append((n.end, ')', 0))

    def tail(n):
        h = head(n)
        if h == 'return':
            return
        if n.kind == 'prefix':
            if src.startswith(',@', n.start):
                notes.append(f'{path}:{line(n.start)}: the body ends in {src[n.start:n.end]}: add the return by hand')
                return
            return wrap(n)
        if h in ('let', 'do', 'unchecked', '%alias') and len(n.items) > 1 and n.items[-1].kind != '[':
            return tail(n.items[-1])
        if h == 'if' and len(n.items) == 4:
            tail(n.items[2])
            tail(n.items[3])
            return
        if h == 'cond' and all(c.kind == '(' and len(c.items) >= 2 for c in n.items[1:]):
            for c in n.items[1:]:
                tail(c.items[-1])
            return
        if h in ('case', 'match') and len(n.items) > 2 and all(c.kind == '(' and len(c.items) >= 2 for c in n.items[2:]):
            for c in n.items[2:]:
                tail(c.items[-1])
            return
        wrap(n)

    def last_expr(items, first):
        """the last expression of items[first:], an index chain like v.data[i][j] as one"""
        j = len(items) - 1
        while j > first and items[j].kind == '[' and items[j].start == items[j - 1].end:
            j -= 1
        last = items[-1]
        if j == len(items) - 1:
            return last
        chain = type(last)('chain', items[j].start, last.end)
        return chain

    def fn_def(n, params):
        k = params
        while k < len(n.items) and n.items[k].kind == '[':
            k += 1
        if k == params or k >= len(n.items):
            return  # (not the old shape: no parameters in [ ], or nothing after them)
        r = n.items[k]
        if r.kind == 'atom' and r.text == '->':
            return  # already converted
        void = r.kind == 'atom' and r.text == 'void'
        if void:  # drop "void" and the space before it
            s = r.start
            while s > 0 and src[s - 1] in ' \t':
                s -= 1
            edits.append((s, '', r.end - s))
        else:
            edits.append((r.start, '-> ', 0))
        b = k + 1
        while b < len(n.items) and n.items[b].kind == 'atom' and n.items[b].text.startswith(':') and len(n.items[b].text) > 1:
            b += 1
        if not void and b < len(n.items):
            tail(last_expr(n.items, b))
        elif not void:
            notes.append(f'{path}:{line(n.start)}: a function with a result and no body')

    def walk(n):
        h = head(n)
        if h in ('defn', 'defop') and len(n.items) > 2:
            fn_def(n, 2)
        elif h == 'fn' and len(n.items) > 1 and n.items[1].kind == '[':
            fn_def(n, 1)
        elif h == 'fn' and len(n.items) > 2 and n.items[1].kind == 'atom' and n.items[1].text == ':heap' and n.items[2].kind == '[':
            fn_def(n, 2)
        for it in n.items:
            walk(it)

    for f in parse(src):
        walk(f)
    out = src
    for pos, text, dl in sorted(edits, key=lambda e: (e[0], e[2]), reverse=True):
        out = out[:pos] + text + out[pos + dl:]
    return out, notes


if __name__ == '__main__':
    for path in sys.argv[1:]:
        try:
            src = open(path).read()
            new, notes = convert(src, path)
        except (SyntaxError, IndexError, UnicodeDecodeError) as e:
            print(f'{path}: not converted ({e})', file=sys.stderr)
            continue
        for m in notes:
            print(m)
        if new != src:
            open(path, 'w').write(new)
            print('rewrote', path)
