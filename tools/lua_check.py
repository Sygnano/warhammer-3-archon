"""Static structure check for Lua 5.1 sources (no Lua toolchain needed).

Tokenizes each file (long strings/comments, quoted strings with escapes, numbers, names, operators) and checks:
- block balance: function/if/do ... end, repeat ... until, with if-then / elseif-then / else placement;
- bracket balance ( ) { } [ ], and that no bracket stays open across a block end;
- unterminated strings / long comments;
- operators that do not exist in Lua 5.1 (!=, +=, -=, *=, /=, ++, //, &&, ||) outside strings and comments.
It is not a full parser; it catches the classic slips. Usage: python -I lua_check.py <file-or-dir> ...
Exit code 0 when every file passes.
"""
import os, re, sys

KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local",
            "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"}
BAD_OPS = ["!=", "+=", "-=", "*=", "/=", "++", "&&", "||"]


class LuaError(Exception):
    pass


def tokenize(src):
    i, n, line = 0, len(src), 1
    toks = []
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1; i += 1; continue
        if c in " \t\r":
            i += 1; continue
        if src.startswith("--", i):
            m = re.match(r"--\[(=*)\[", src[i:])
            if m:
                close = "]" + m.group(1) + "]"
                j = src.find(close, i + len(m.group(0)))
                if j < 0: raise LuaError("line %d: unterminated long comment" % line)
                line += src.count("\n", i, j); i = j + len(close); continue
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        m = re.match(r"\[(=*)\[", src[i:])
        if m:
            close = "]" + m.group(1) + "]"
            j = src.find(close, i + len(m.group(0)))
            if j < 0: raise LuaError("line %d: unterminated long string" % line)
            line += src.count("\n", i, j); toks.append(("str", "", line)); i = j + len(close); continue
        if c in "\"'":
            j = i + 1
            while True:
                if j >= n: raise LuaError("line %d: unterminated string" % line)
                d = src[j]
                if d == "\\":
                    if j + 1 < n and src[j + 1] == "\n": line += 1
                    j += 2; continue
                if d == "\n": raise LuaError("line %d: newline in string" % line)
                if d == c: break
                j += 1
            toks.append(("str", "", line)); i = j + 1; continue
        m = re.match(r"0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?", src[i:])
        if m and (c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit())):
            toks.append(("num", m.group(0), line)); i += len(m.group(0)); continue
        m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", src[i:])
        if m:
            w = m.group(0)
            toks.append(("kw" if w in KEYWORDS else "name", w, line)); i += len(w); continue
        for op in BAD_OPS:
            if src.startswith(op, i):
                raise LuaError("line %d: operator %r is not Lua 5.1" % (line, op))
        m = re.match(r"\.\.\.|\.\.|==|~=|<=|>=|[-+*/%^#<>=(){}\[\];:,.]", src[i:])
        if not m: raise LuaError("line %d: unexpected character %r" % (line, c))
        toks.append(("op", m.group(0), line)); i += len(m.group(0))
    return toks


def check(src):
    toks = tokenize(src)
    stack = []  # entries: (kind, line)
    pairs = {")": "(", "}": "{", "]": "["}
    for k, v, line in toks:
        if k == "op" and v in "({[":
            stack.append((v, line))
        elif k == "op" and v in ")}]":
            if not stack or stack[-1][0] != pairs[v]:
                raise LuaError("line %d: unbalanced %r (open: %r)" % (line, v, stack[-1] if stack else None))
            stack.pop()
        elif k == "kw":
            if v in ("function", "do", "repeat"):
                stack.append((v, line))
            elif v == "if":
                stack.append(("if?", line))           # waiting for then
            elif v == "then":
                if not stack or stack[-1][0] not in ("if?", "elseif?"):
                    raise LuaError("line %d: 'then' without if/elseif" % line)
                stack[-1] = ("if", stack[-1][1])
            elif v == "elseif":
                if not stack or stack[-1][0] != "if":
                    raise LuaError("line %d: 'elseif' outside if" % line)
                stack[-1] = ("elseif?", stack[-1][1])
            elif v == "else":
                if not stack or stack[-1][0] != "if":
                    raise LuaError("line %d: 'else' outside if" % line)
                stack[-1] = ("ifelse", stack[-1][1])
            elif v == "end":
                if not stack or stack[-1][0] not in ("function", "do", "if", "ifelse"):
                    raise LuaError("line %d: 'end' does not close a block (top: %r)" % (line, stack[-1] if stack else None))
                stack.pop()
            elif v == "until":
                if not stack or stack[-1][0] != "repeat":
                    raise LuaError("line %d: 'until' without repeat" % line)
                stack.pop()
    if stack:
        raise LuaError("unclosed at end of file: %r" % (stack[-1],))
    return len(toks)


def main(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, x) for x in names if x.endswith(".lua")]
        else:
            files.append(p)
    bad = 0
    for f in sorted(files):
        src = open(f, encoding="latin-1").read()
        try:
            n = check(src)
            print("OK   %6d tokens  %s" % (n, f))
        except LuaError as e:
            bad += 1
            print("FAIL %s: %s" % (f, e))
    print("%d file(s), %d failure(s)" % (len(files), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
