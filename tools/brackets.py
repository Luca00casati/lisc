#!/usr/bin/env python3
"""Rewrite lisc source to the bracket style, keeping comments and layout:
(defn f ((a T) (b U)) ...) -> (defn f [a T] [b U] ...), () params -> []
(let ((x 1) (y 2)) ...)    -> (let [x 1] [y 2] ...)
(for (i 0 n) ...)          -> (for [i 0 n] ...)
(struct s (x T) ...)       -> (struct s [x T] ...)
(aref a i)                 -> a[i]     (a a name, a.b, ,a or another index)
(make T ...)               -> {T ...}
Usage: brackets.py file.lsp ...   (rewrites in place)"""
import sys

DELIM = set(" \t\r\n()[]{}\";'`,")


class Node:
    def __init__(self, kind, start, end=None, text=None):
        self.kind, self.start, self.end, self.text = kind, start, end, text
        self.items = []


def parse(src):
    pos = 0
    n = len(src)

    def skip():
        nonlocal pos
        while pos < n:
            c = src[pos]
            if c == ';':
                while pos < n and src[pos] != '\n':
                    pos += 1
            elif c in ' \t\r\n':
                pos += 1
            else:
                return

    def form():
        nonlocal pos
        skip()
        start = pos
        if pos >= n:
            raise SyntaxError('unclosed bracket at the end')
        c = src[pos]
        if c in ')]}':
            raise SyntaxError(f'unexpected {c} at offset {pos}')
        if c in '([{':
            close = {'(': ')', '[': ']', '{': '}'}[c]
            node = Node(c, start)
            pos += 1
            while True:
                skip()
                if pos < n and src[pos] == close:
                    pos += 1
                    break
                node.items.append(form())
            node.end = pos
            return node
        if c in "'`,":
            pos += 1
            if c == ',' and src[pos] == '@':
                pos += 1
            node = Node('prefix', start)
            node.items.append(form())
            node.end = pos
            return node
        if c == '"' or (c == 'c' and src[pos + 1:pos + 2] == '"'):
            if c == 'c':
                pos += 1
            pos += 1
            while src[pos] != '"':
                pos += 2 if src[pos] == '\\' else 1
            pos += 1
            return Node('atom', start, pos, src[start:pos])
        if c == '#' and src[pos + 1:pos + 2] == '\\':
            pos += 3
        while pos < n and src[pos] not in DELIM:
            pos += 1
        return Node('atom', start, pos, src[start:pos])

    forms = []
    while True:
        skip()
        if pos >= n:
            return forms
        forms.append(form())


def convert(src):
    def head(node):
        return node.items[0].text if node.kind == '(' and node.items and node.items[0].kind == 'atom' else None

    def render(node, items=None, opener=None, closer=None, drop_outer=False):
        """the node's text with its children rendered (items: replacements by index)"""
        if node.kind == 'atom':
            return node.text
        out = []
        p = node.start
        for i, it in enumerate(node.items):
            out.append(src[p:it.start])
            out.append(items[i] if items and i in items else conv(it))
            p = it.end
        out.append(src[p:node.end])
        s = ''.join(out)
        if opener:
            s = opener + s[1:-1] + closer
        return s

    def inner(node):  # a list's rendered text without its parens
        return render(node)[1:-1]

    def splice(lst):
        """((a T) (b U)) -> [a T] [b U], keeping the gaps (one space less of indent)"""
        if not lst.items:
            return '[]'
        out = []
        p = lst.start + 1
        for i, b in enumerate(lst.items):
            gap = src[p:b.start]
            if i == 0:
                gap = ''
            elif '\n' in gap and gap.endswith(' '):
                gap = gap[:-1]
            out.append(gap)
            out.append('[' + inner(b) + ']' if b.kind == '(' else conv(b))
            p = b.end
        return ''.join(out)

    def indexable(node):
        if node.kind == 'atom':
            return node.text[:1] not in '"#' and not node.text[:1].isdigit()
        if node.kind == 'prefix':
            return src[node.start] == ',' and src[node.start + 1] != '@'
        return head(node) == 'aref'

    def conv(node):
        if node.kind == 'prefix':
            return render(node)
        if node.kind != '(':
            return render(node)
        h = head(node)
        k = len(node.items)
        if h == 'aref' and k == 3 and indexable(node.items[1]):
            return conv(node.items[1]) + '[' + conv(node.items[2]) + ']'
        if h == 'make' and k >= 2:
            return render(node, {0: ''}, '{', '}').replace('{ ', '{', 1)
        if h in ('defn', 'defn-', 'defop') and k >= 4 and node.items[2].kind == '(' and \
                all(x.kind == '(' for x in node.items[2].items):
            return render(node, {2: splice(node.items[2])})
        if h == 'let' and k >= 2 and node.items[1].kind == '(' and \
                all(x.kind == '(' for x in node.items[1].items):
            return render(node, {1: splice(node.items[1])})
        if h == 'for' and k >= 2 and node.items[1].kind == '(':
            return render(node, {1: '[' + inner(node.items[1]) + ']'})
        if h in ('struct', 'struct-', 'union', 'union-'):
            return render(node, {i: '[' + inner(x) + ']' for i, x in enumerate(node.items) if i >= 2 and x.kind == '('})
        return render(node)

    out = []
    p = 0
    for f in parse(src):
        out.append(src[p:f.start])
        # compile-time code (defmacro/defmeta) only gets its templates converted
        out.append(conv(f))
        p = f.end
    out.append(src[p:])
    return ''.join(out)


for path in sys.argv[1:]:
    src = open(path).read()
    try:
        new = convert(src)
    except (SyntaxError, IndexError) as e:
        print(f'{path}: not converted ({e})', file=sys.stderr)
        continue
    if new != src:
        open(path, 'w').write(new)
        print('rewrote', path)
