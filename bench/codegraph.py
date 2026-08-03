#!/usr/bin/env python3
"""codegraph.py -- parse a C source tree into an in-memory graph and query it.

Every run re-reads and re-parses the tree, builds the whole graph in a
`:memory:` database, answers questions and exits. A file on disk could be read
when it no longer describes the code -- and a stale graph is worse than none.

Covers: files, C functions, structs, unions, enums, typedefs, macros, globals;
call edges, call sites (line-accurate), include edges, struct fields, function
parameters, per-function metrics and hazard hits.

No compiler frontend required. C is tokenised after comments and string/char
literals are blanked, functions are found by brace-matching, and a call edge is
any identifier in a body that resolves to a known function.

LIMITATION: calls through FUNCTION POINTERS are not resolved.
`symbols.n_fnptr_calls` counts indirect call sites so queries can see where
that blindness applies.

Usage:
  python3 bench/codegraph.py              # -> bench/codegraph.db
  python3 bench/codegraph.py --print      # rebuild, then report
  python3 bench/codegraph.py --db /tmp/x.db /repo
  python3 bench/codegraph.py --schema     # dump schema only
  python3 bench/codegraph.py --list       # list available queries
"""
from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import os
import re
import sqlite3
import sys
import time
from dataclasses import dataclass, field
from itertools import accumulate
from typing import Dict, List, Optional, Tuple, Set, Iterable, Any

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
C_EXT = (".c", ".h", ".inc")
SKIP_DIRS = {".git", ".obj", ".dev", "node_modules", "third_party", "test262",
             "__pycache__", ".claude", "build", "dist"}
LANG_BY_EXT = {
    ".c": "c", ".h": "c", ".inc": "c",
    ".js": "js", ".mjs": "js",
    ".py": "py", ".sh": "sh", ".md": "md", ".lua": "lua",
    ".json": "json", ".yml": "yaml", ".yaml": "yaml",
    ".txt": "text", ".s": "asm", ".S": "asm",
}

HAZARD_FUNCS: dict[str, str] = {
    "memcpy": "memory", "memmove": "memory", "memset": "memory",
    "memcmp": "memory", "memchr": "memory", "strcpy": "memory",
    "strncpy": "memory", "strcat": "memory", "strncat": "memory",
    "strlen": "memory", "sprintf": "memory", "snprintf": "memory",
    "vsprintf": "memory", "vsnprintf": "memory", "alloca": "memory",
    "gets": "memory", "scanf": "memory", "sscanf": "memory",
    "malloc": "alloc", "calloc": "alloc", "realloc": "alloc", "free": "alloc",
    "aligned_alloc": "alloc", "strdup": "alloc", "strndup": "alloc",
    "js_malloc": "alloc", "js_free": "alloc", "js_realloc": "alloc",
    "fwrite": "stdio", "fprintf": "stdio", "fputs": "stdio", "fputc": "stdio",
    "printf": "stdio", "puts": "stdio", "fflush": "stdio",
    "recv": "io", "send": "io", "read": "io", "write": "io", "pread": "io",
    "pwrite": "io", "recvfrom": "io", "sendto": "io", "accept": "io",
    "connect": "io", "open": "io", "openat": "io", "close": "io",
    "system": "exec", "popen": "exec", "execve": "exec", "execl": "exec",
    "execlp": "exec", "fork": "exec",
    "sqrt": "libm", "exp": "libm", "log": "libm", "log2": "libm",
    "log10": "libm", "pow": "libm", "sin": "libm", "cos": "libm",
    "tan": "libm", "atan": "libm", "atan2": "libm", "tgamma": "libm",
    "lgamma": "libm", "erf": "libm", "fmod": "libm", "cbrt": "libm",
    "pthread_mutex_lock": "concurrency", "pthread_mutex_unlock": "concurrency",
    "pthread_create": "concurrency", "pthread_join": "concurrency",
    "atomic_load": "concurrency", "atomic_store": "concurrency",
    "atomic_fetch_add": "concurrency", "atomic_compare_exchange_strong": "concurrency",
    "setjmp": "control", "longjmp": "control", "abort": "control",
    "exit": "control", "assert": "control",
}

HAZARD_RE: list[tuple[str, str, re.Pattern[str]]] = [
    ("ptr_cast", "integer", re.compile(r'\(\s*(?:const\s+)?[A-Za-z_]\w*\s*\*+\s*\)')),
    ("shift", "integer", re.compile(r'<<|>>')),
    ("mul_sizeof", "integer", re.compile(r'\*\s*sizeof|sizeof\s*\([^)]*\)\s*\*')),
    ("fixed_buffer", "memory", re.compile(r'\b(?:char|uint8_t|unsigned\s+char|int8_t)\s+\w+\s*\[\s*\d+\s*\]')),
    ("vla", "memory", re.compile(r'\b(?:char|int|uint8_t|double|float)\s+\w+\s*\[\s*[a-z_]\w*\s*\]')),
    ("signed_cmp", "integer", re.compile(r'\bint\s+\w+\s*=\s*\w+\s*-\s*\w+')),
]

CTRL_KW = re.compile(r'\b(if|for|while|case|catch)\b')
LOOP_KW = re.compile(r'\b(for|while|do)\b')
BRANCH_KW = re.compile(r'\b(if|switch|case)\b')
LOGIC = re.compile(r'&&|\|\||\?')
RETURN_KW = re.compile(r'\breturn\b')
GOTO_KW = re.compile(r'\bgoto\b')
IDENT = re.compile(r'[A-Za-z_]\w*')
OPERATOR = re.compile(r'[+\-*/%=<>!&|^~]+|\[|\]|\.|->')
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.M)
DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)(\([^)]*\))?[ \t]*(.*)$', re.M)
TYPEDEF_SIMPLE = re.compile(r'^[ \t]*typedef[ \t]+(.+?)\b(\w+)[ \t]*;', re.M)
TAG_BODY = re.compile(r'^[ \t]*(?:typedef[ \t]+)?(struct|enum|union)[ \t]*(\w*)[ \t]*\{', re.M)
GLOBAL_RE = re.compile(
    r'^[ \t]*(static[ \t]+|extern[ \t]+)?'
    r'((?:const[ \t]+|volatile[ \t]+|_Atomic[ \t]+|atomic_\w+[ \t]+|unsigned[ \t]+|signed[ \t]+|struct[ \t]+|enum[ \t]+)*[A-Za-z_]\w*)'
    r'[ \t]+(\*+)[ \t]*(\w+)[ \t]*(\[[^\]]*\])?[ \t]*(=[^;]*)?;',
    re.M)
KEYWORDS: set[str] = {
    "if", "for", "while", "switch", "return", "sizeof", "case", "do", "else",
    "goto", "typedef", "struct", "enum", "union", "static", "const", "void",
    "int", "char", "unsigned", "signed", "long", "short", "float", "double",
    "volatile", "register", "extern", "inline", "restrict", "_Static_assert",
    "defined", "break", "continue", "default", "_Atomic", "_Bool", "alignas",
    "alignof", "__attribute__", "__typeof__", "typeof", "asm", "__asm__",
}
TYPE_WORDS = {"const", "volatile", "static", "inline", "extern", "restrict",
              "unsigned", "signed", "struct", "enum", "union", "_Atomic"}

LOCAL_RE = re.compile(
    r'^[ \t]*((?:const[ \t]+|volatile[ \t]+|static[ \t]+|register[ \t]+|unsigned[ \t]+|signed[ \t]+|struct[ \t]+|enum[ \t]+|union[ \t]+)*)'
    r'([A-Za-z_]\w*)[ \t]+(\*+)[ \t]*(\w+)[ \t]*(\[[^\]]*\])?[ \t]*(=[^;]*)?;',
    re.M)
NUM_RE = re.compile(r'\b(0[xX][0-9a-fA-F]+|\d+[uUlL]*)\b')
MARKER_RE = re.compile(
    r'(?:^|/\*|//|\*)[ \t]*(TODO|FIXME|XXX|HACK|BUG|NOTE|WARNING)\b[ \t]*[:\-]'
    r'|\b(TODO|FIXME|XXX|HACK|BUG)[ \t]*:',
    re.M)
IFDEF_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif)[ \t]+(.+)$', re.M)
LABEL_RE = re.compile(r'^[ \t]*([A-Za-z_]\w*)[ \t]*:(?![:=])', re.M)
CASE_RE = re.compile(r'\bcase\b[ \t]+([^:]+):')
ATTR_RE = re.compile(r'__attribute__\s*\(\(([^)]*)\)\)')
INTRIN_RE = re.compile(r'\b(v[a-z0-9_]+q?_[a-z0-9_]+|_mm\d*_[a-z0-9_]+|sv[a-z0-9_]+_[a-z0-9_]+)\s*\(')
RETURN_SHAPE = re.compile(r'\breturn\b([^;]*);')
FUNC_SCAN = re.compile(r'[{}]|\b([A-Za-z_]\w*)\s*\(')

# ---------------------------------------------------------------------------
# Safe newline-index cache
# ---------------------------------------------------------------------------
class _TextRef:
    __slots__ = ("text", "offs")
    def __init__(self, text: str, offs: list[int]):
        self.text = text
        self.offs = offs

_NL_CACHE: dict[int, _TextRef] = {}
_NL_CACHE_MAX = 16

def _nl_index(text: str) -> list[int]:
    """Return list of newline byte offsets for `text`. Cached safely."""
    key = id(text)
    ent = _NL_CACHE.get(key)
    if ent is not None and ent.text is text:
        return ent.offs
    parts = text.split("\n")
    offs = list(accumulate(len(s) + 1 for s in parts[:-1]))
    if len(_NL_CACHE) >= _NL_CACHE_MAX:
        _NL_CACHE.clear()
    _NL_CACHE[key] = _TextRef(text, offs)
    return offs

def line_of(text: str, pos: int) -> int:
    """1-based line number for byte offset `pos` in `text`."""
    return bisect.bisect_right(_nl_index(text), pos) + 1

# ---------------------------------------------------------------------------
# File / module utilities
# ---------------------------------------------------------------------------
def module_of(rel: str) -> str:
    """Subsystem grouping: the thing you actually filter reports by."""
    p = rel.replace(os.sep, "/")
    if p.startswith("tests/"):
        return "tests"
    if p.startswith("tools/"):
        return "tools"
    # src/fuzz and src/compat must be named BEFORE the generic src/ branch, or
    # they become module "src/fuzz" and the kind rule below tags them engine --
    # which puts harness code into every engine ranking.
    if p.startswith("src/fuzz/"):
        return "fuzz"
    if p.startswith("src/compat/"):
        return "compat"
    if p.startswith("examples/"):
        return "examples"
    if p.startswith("docs/"):
        return "docs"
    if p.startswith("bench/"):
        return "bench"
    if p.startswith("src/"):
        parts = p.split("/")
        if len(parts) > 2:
            return "src/" + parts[1]
        base = os.path.basename(p)
        if base.startswith("dyna-"):
            return "native/" + base[5:].split(".")[0].split("-")[0]
        return "src/root"
    return p.split("/")[0] if "/" in p else "root"

def blank_c(src: str) -> str:
    """Comments and string/char literals -> spaces; newlines preserved so
    every byte offset still maps to its original line."""
    n = len(src)
    i = 0
    segments: list[str] = []
    prev = 0

    def emit_blank(start: int, end: int) -> None:
        segments.append(src[prev:start])
        segments.append(''.join('\n' if ch == '\n' else ' ' for ch in src[start:end]))

    while i < n:
        two = src[i:i + 2]
        if two == "/*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            emit_blank(i, j)
            i = j
            prev = i
            continue
        if two == "//":
            j = src.find('\n', i)
            # If no newline, blank to EOF
            j = n if j < 0 else j
            emit_blank(i, j)
            i = j
            prev = i
            continue
        c = src[i]
        if c in '"\'':
            q, j = c, i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == q:
                    j += 1
                    break
                if src[j] == '\n':
                    break
                j += 1
            emit_blank(i, j)
            i = j
            prev = i
            continue
        i += 1
    segments.append(src[prev:])
    return ''.join(segments)

# ---------------------------------------------------------------------------
# Signature / parameter parsing
# ---------------------------------------------------------------------------
def split_params(sig: str) -> list[tuple[int, str, str, int, int, int, int]]:
    """Parameter list out of a signature, depth-aware."""
    l = sig.find("(")
    if l < 0:
        return []
    d, i, n = 0, l, len(sig)
    while i < n:
        if sig[i] == '(':
            d += 1
        elif sig[i] == ')':
            d -= 1
            if d == 0:
                break
        i += 1
    inner = sig[l + 1:i]
    parts, depth, cur = [], 0, ""
    for ch in inner:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    out: list[tuple[int, str, str, int, int, int, int]] = []
    for pos, raw in enumerate(parts):
        raw = raw.strip()
        if not raw or raw == "void":
            continue
        stars = raw.count("*")
        m = re.search(r'(\w+)\s*(\[[^\]]*\])?\s*(?::\s*\d+)?$', raw)
        pname = m.group(1) if m else ""
        is_arr = 1 if (m and m.group(2)) else 0
        ptype = raw[:m.start(1)].strip() if m else raw
        if pname in TYPE_WORDS or (m is None):
            pname, ptype = "", raw
        out.append((pos, ptype[:120] or raw[:120], pname[:80], stars, is_arr,
                    1 if "const" in raw else 0, 1 if "..." in raw else 0))
    return out

def return_type_of(sig: str, name: str) -> str:
    i = sig.find(name + "(")
    if i < 0:
        i = sig.find(name)
    rt = sig[:i].strip() if i > 0 else ""
    return re.sub(r'\s+', ' ', rt)[:120]

# ---------------------------------------------------------------------------
# Function discovery
# ---------------------------------------------------------------------------
def find_functions(blank: str, raw: str) -> list[tuple[str, str, int, int, int, int, str, int]]:
    """Return (name, sig, line_start, line_end, is_static, is_inline, body, body_off)."""
    n, i, depth, res = len(blank), 0, 0, []
    while i < n:
        mm = FUNC_SCAN.search(blank, i)
        if not mm:
            break
        i = mm.start()
        ch = blank[i]
        if ch == "{":
            depth += 1
            i += 1
            continue
        if ch == "}":
            depth = max(0, depth - 1)
            i += 1
            continue
        if depth == 0:
            name = mm.group(1)
            if name not in KEYWORDS:
                pd, j = 0, mm.end() - 1
                while j < n:
                    if blank[j] == '(':
                        pd += 1
                    elif blank[j] == ')':
                        pd -= 1
                        if pd == 0:
                            break
                    j += 1
                k = j + 1
                while k < n and blank[k] in " \t\r\n":
                    k += 1
                if k < n and blank[k] == "{":
                    bd, e = 0, k
                    while e < n:
                        if blank[e] == "{":
                            bd += 1
                        elif blank[e] == "}":
                            bd -= 1
                            if bd == 0:
                                break
                        e += 1
                    ss = raw.rfind('\n', 0, i) + 1
                    sig = re.sub(r'\s+', ' ', raw[ss:k].strip())[:400]
                    res.append((name, sig, line_of(blank, i),
                                line_of(blank, e),
                                1 if re.search(r'\bstatic\b', sig) else 0,
                                1 if re.search(r'\binline\b', sig) else 0,
                                blank[k:e + 1], k))
                    i = e + 1
                    continue
        i = mm.end()
    return res

# ---------------------------------------------------------------------------
# Type size / struct layout (LP64)
# ---------------------------------------------------------------------------
TYPE_SIZE: dict[str, int] = {
    "char": 1, "signed char": 1, "unsigned char": 1, "uint8_t": 1, "int8_t": 1,
    "_Bool": 1, "bool": 1,
    "short": 2, "unsigned short": 2, "uint16_t": 2, "int16_t": 2,
    "int": 4, "unsigned": 4, "unsigned int": 4, "uint32_t": 4, "int32_t": 4,
    "float": 4, "JSAtom": 4, "JSClassID": 4,
    "long": 8, "unsigned long": 8, "uint64_t": 8, "int64_t": 8, "double": 8,
    "size_t": 8, "ssize_t": 8, "long long": 8, "unsigned long long": 8,
    "intptr_t": 8, "uintptr_t": 8, "off_t": 8, "time_t": 8, "pthread_t": 8,
    "JSValue": 16, "JSValueConst": 16,
}
TYPE_ALIGN: dict[str, int] = {"JSValue": 8, "JSValueConst": 8}
PTR_SIZE = 8

def type_size(ctype: str, ptr_depth: int, array_len: int) -> tuple[int, int, int]:
    """(size, align, exact). ptr_depth>0 -> pointer, always 8/8/1."""
    if ptr_depth > 0:
        base, align, exact = PTR_SIZE, PTR_SIZE, 1
    else:
        t = re.sub(r'\b(const|volatile|struct|union|enum|_Atomic|register)\b', ' ', ctype)
        t = re.sub(r'\s+', ' ', t).strip()
        if t in TYPE_SIZE:
            base = TYPE_SIZE[t]
            align = TYPE_ALIGN.get(t, min(base, 8))
            exact = 1
        else:
            base = align = 0
            exact = 0
    if array_len > 0 and base:
        return base * array_len, align, exact
    if array_len < 0:
        return 0, align, 0
    return base, align, exact

def layout_struct(flds: list[tuple[Any, ...]], is_union: bool) -> tuple[
    list[tuple[int, int, int, int, int]], int, int, int, int]:
    """Assign byte offsets to top-level fields. Returns (layout_rows, total_size,
    tail_pad, all_exact, max_align)."""
    off, maxalign, out, all_exact = 0, 1, [], 1
    for f in flds:
        ordinal, ftype, fname, ptr, alen, isfn, depth, inu, line = f
        if depth != 0 or inu:
            out.append((ordinal, -1, 0, 0, 0))
            continue
        sz, al, ex = type_size(ftype, ptr, alen)
        if not ex:
            all_exact = 0
        al = al or 1
        maxalign = max(maxalign, al)
        pad = (-off) % al if not is_union else 0
        this_off = 0 if is_union else off + pad
        out.append((ordinal, this_off, sz, pad, ex))
        if is_union:
            off = max(off, sz)
        else:
            off += pad + sz
    tail = (-off) % maxalign if maxalign else 0
    return out, off + tail, tail, all_exact, maxalign

# ---------------------------------------------------------------------------
# Per-function analysis
# ---------------------------------------------------------------------------
def expr_counts(body: str) -> dict[str, int]:
    """Expression-level texture metrics."""
    return dict(
        n_deref=len(re.findall(r'(?<![->])\*\s*[A-Za-z_(]', body)) + body.count("->"),
        n_arrow=body.count("->"),
        n_subscript=body.count("["),
        n_addrof=len(re.findall(r'(?<!\w)&[A-Za-z_]', body)),
        n_cast=len(re.findall(r'\(\s*(?:const\s+)?[A-Za-z_]\w*\s*\*+\s*\)', body)),
        n_sizeof=len(re.findall(r'\bsizeof\b', body)),
        n_ternary=body.count('?'),
        n_bitop=len(re.findall(r'[&|^~]', body)),
        n_shift=len(re.findall(r'<<|>>', body)),
        n_cmp=len(re.findall(r'==|!=|<=|>=|(?<![=<])<(?![<=])|(?<![=>])>(?![>=])', body)),
        n_assign=len(re.findall(r'(?<![+\-*/%&|^])=(?!=)', body)),
        n_compound_assign=len(re.findall(r'[+\-*/%&|^]=|<<=|>>=', body)),
        n_incdec=len(re.findall(r'\+\+|--', body)),
        n_logical=len(re.findall(r'&&|\|\|', body)),
        n_float_lit=len(re.findall(r'\b\d+\.\d+', body)),
        n_null_check=len(re.findall(r'(?:==|!=)\s*NULL|\bif\s*\(\s*!\s*\w+\s*\)', body)),
        n_intrinsic=len(INTRIN_RE.findall(body)),
        n_atomic=len(re.findall(r'\batomic_\w+|__atomic_\w+|_Atomic\b', body)),
        n_volatile=len(re.findall(r'\bvolatile\b', body)),
        n_restrict=len(re.findall(r'\brestrict\b|__restrict', body)),
        n_likely=len(re.findall(r'\b(likely|unlikely|__builtin_expect)\b', body)),
        n_builtin=len(re.findall(r'__builtin_\w+', body)),
        n_static_assert=len(re.findall(r'_Static_assert|static_assert', body)),
        n_labels=len(LABEL_RE.findall(body)),
        n_cases=len(CASE_RE.findall(body)),
    )

def return_shapes(body: str) -> dict[str, int]:
    """How a function reports failure."""
    r: dict[str, int] = dict(ret_null=0, ret_neg=0, ret_zero=0, ret_val=0, ret_void=0)
    for m in RETURN_SHAPE.finditer(body):
        v = m.group(1).strip()
        if not v:
            r["ret_void"] += 1
        elif v == "NULL":
            r["ret_null"] += 1
        elif re.match(r'^-\s*\d+$', v):
            r["ret_neg"] += 1
        elif v == "0":
            r["ret_zero"] += 1
        else:
            r["ret_val"] += 1
    return r

def opener_kind(body: str, brace_pos: int) -> str:
    """The keyword that opened the block at brace_pos."""
    j = brace_pos - 1
    while j >= 0 and body[j] in " \t\r\n":
        j -= 1
    if j < 0:
        return ""
    if body[j] == ")":
        d = 0
        while j >= 0:
            if body[j] == ")":
                d += 1
            elif body[j] == "(":
                d -= 1
                if d == 0:
                    break
            j -= 1
        j -= 1
        while j >= 0 and body[j] in " \t\r\n":
            j -= 1
    e = j
    while e >= 0 and (body[e].isalnum() or body[e] == "_"):
        e -= 1
    return body[e + 1:j + 1]

LOOP_ALLOC = ("malloc", "calloc", "realloc", "strdup", "js_malloc", "js_realloc")
LOOP_LIBM = ("sqrt", "exp", "log", "log2", "log10", "pow", "sin", "cos", "tan",
             "atan", "atan2", "tgamma", "lgamma", "erf", "fmod", "cbrt")

def loop_analysis(body: str) -> dict[str, int]:
    """Per-iteration hazards: what sits INSIDE a loop body."""
    r = dict(max_loop_depth=0, switch_in_loop=0, alloc_in_loop=0,
             call_in_loop=0, libm_in_loop=0, div_in_loop=0, strlen_in_loop=0)
    stack: list[bool] = []
    depth = 0
    n = len(body)
    i = 0
    while i < n:
        ch = body[i]
        if ch == "{":
            kw = opener_kind(body, i)
            is_loop = kw in ("for", "while", "do")
            stack.append(is_loop)
            if is_loop:
                depth += 1
                r["max_loop_depth"] = max(r["max_loop_depth"], depth)
            i += 1
            continue
        if ch == "}":
            if stack and stack.pop():
                depth = max(0, depth - 1)
            i += 1
            continue
        if depth > 0:
            if ch == "/" and i + 1 < n and body[i + 1] != "/":
                r["div_in_loop"] += 1
            elif ch.isalpha() or ch == "_":
                m = IDENT.match(body, i)
                if m:
                    tok = m.group(0)
                    e = m.end()
                    while e < n and body[e] in " \t\r\n":
                        e += 1
                    called = e < n and body[e] == "("
                    if tok == "switch":
                        r["switch_in_loop"] += 1
                    elif called and tok not in KEYWORDS:
                        r["call_in_loop"] += 1
                        if tok in LOOP_ALLOC:
                            r["alloc_in_loop"] += 1
                        if tok in LOOP_LIBM:
                            r["libm_in_loop"] += 1
                        if tok == "strlen":
                            r["strlen_in_loop"] += 1
                    i = m.end()
                    continue
        i += 1
    return r

def metrics(body: str) -> dict[str, int]:
    """cyclomatic, cognitive, max nesting, sloc, loops, branches, returns,
    gotos, tokens, operators."""
    cyclo = 1 + len(CTRL_KW.findall(body)) + len(LOGIC.findall(body))
    depth = mx = cog = 0
    for ch in body:
        if ch == "{":
            depth += 1
            mx = max(mx, depth)
        elif ch == "}":
            depth = max(0, depth - 1)
    d = 0
    for m in re.finditer(r'[{}]|\b(if|for|while|switch|catch)\b', body):
        t = m.group(0)
        if t == "{":
            d += 1
        elif t == "}":
            d = max(0, d - 1)
        else:
            cog += max(1, d)
    return dict(
        cyclomatic=cyclo, cognitive=cog, max_nesting=mx,
        sloc=sum(1 for l in body.splitlines() if l.strip()),
        n_loops=len(LOOP_KW.findall(body)),
        n_branches=len(BRANCH_KW.findall(body)),
        n_returns=len(RETURN_KW.findall(body)),
        n_gotos=len(GOTO_KW.findall(body)),
        n_tokens=len(IDENT.findall(body)) + len(OPERATOR.findall(body)),
        n_operators=len(OPERATOR.findall(body)),
    )

# ---------------------------------------------------------------------------
# Struct field extraction
# ---------------------------------------------------------------------------
def struct_fields(blank: str, raw: str, open_brace: int) -> tuple[list[tuple[Any, ...]], int]:
    """Members of the struct/union body whose `{` is at open_brace."""
    n, d, e = len(blank), 0, open_brace
    while e < n:
        if blank[e] == "{":
            d += 1
        elif blank[e] == "}":
            d -= 1
            if d == 0:
                break
        e += 1
    body, base = blank[open_brace + 1:e], open_brace + 1
    out: list[tuple[Any, ...]] = []
    ordinal = 0
    i = 0
    start = 0
    stack: list[str] = []

    def emit(stmt: str, off: int) -> None:
        nonlocal ordinal
        depth = len(stack)
        in_union = 1 if any(k == "union" for k in stack) else 0
        if "(" in stmt:  # function-pointer member
            m = re.search(r'\(\s*\*\s*(\w+)\s*\)', stmt)
            if m:
                out.append((ordinal, re.sub(r'\s+', ' ', stmt.strip(" ;\n"))[:160],
                            m.group(1), stmt.count("*"), 0, 1, depth, in_union,
                            line_of(raw, base + off)))
                ordinal += 1
            return
        for decl in stmt.rstrip(";").split(","):
            decl = decl.strip()
            if not decl:
                continue
            mm = re.search(r'(\w+)\s*(\[[^\]]*\])?\s*(?::\s*\d+)?$', decl)
            if not mm:
                continue
            fname = mm.group(1)
            if fname in TYPE_WORDS:
                continue
            ftype = decl[:mm.start(1)].strip() or decl
            arr, alen = mm.group(2) or "", 0
            if arr:
                a = re.sub(r'[\[\]]', '', arr).strip()
                alen = int(a) if a.isdigit() else -1
            out.append((ordinal, re.sub(r'\s+', ' ', ftype)[:160], fname[:80],
                        decl.count("*"), alen, 0, depth, in_union,
                        line_of(raw, base + off)))
            ordinal += 1

    while i < len(body):
        ch = body[i]
        if ch == "{":
            back = body[max(0, i - 60):i]
            stack.append("union" if re.search(r'\bunion\b[^;{}]*$', back) else "struct")
            start = i + 1
        elif ch == "}":
            if stack:
                stack.pop()
            start = i + 1
        elif ch == ";":
            emit(body[start:i + 1], start)
            start = i + 1
        i += 1
    return out, line_of(blank, e)

# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------
SCHEMA = r"""
PRAGMA journal_mode=OFF;
PRAGMA synchronous=OFF;
PRAGMA page_size=16384;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-65536;

CREATE TABLE modules(
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    kind TEXT NOT NULL,
    n_files INT NOT NULL DEFAULT 0,
    n_symbols INT NOT NULL DEFAULT 0,
    sloc INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE files(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    dir TEXT NOT NULL,
    basename TEXT NOT NULL,
    ext TEXT NOT NULL,
    lang TEXT NOT NULL,
    module_id INT REFERENCES modules(id),
    bytes INT NOT NULL,
    lines INT NOT NULL,
    sloc INT NOT NULL,
    blank_lines INT NOT NULL DEFAULT 0,
    comment_lines INT NOT NULL DEFAULT 0,
    max_line_len INT NOT NULL DEFAULT 0,
    sha1 TEXT NOT NULL,
    parsed INT NOT NULL DEFAULT 0,
    is_header INT NOT NULL DEFAULT 0,
    is_test INT NOT NULL DEFAULT 0,
    is_generated INT NOT NULL DEFAULT 0,
    n_functions INT NOT NULL DEFAULT 0,
    n_includes INT NOT NULL DEFAULT 0,
    n_globals INT NOT NULL DEFAULT 0,
    total_cyclo INT NOT NULL DEFAULT 0,
    max_cyclo INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE symbols(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    module_id INT REFERENCES modules(id),
    name TEXT NOT NULL,
    kind TEXT NOT NULL,
    line_start INT NOT NULL,
    line_end INT NOT NULL,
    n_lines INT NOT NULL DEFAULT 0,
    is_static INT NOT NULL DEFAULT 0,
    is_inline INT NOT NULL DEFAULT 0,
    return_type TEXT,
    signature TEXT,
    n_params INT NOT NULL DEFAULT 0,
    n_ptr_params INT NOT NULL DEFAULT 0,
    sloc INT NOT NULL DEFAULT 0,
    body_bytes INT NOT NULL DEFAULT 0,
    cyclomatic INT NOT NULL DEFAULT 0,
    cognitive INT NOT NULL DEFAULT 0,
    max_nesting INT NOT NULL DEFAULT 0,
    n_loops INT NOT NULL DEFAULT 0,
    n_branches INT NOT NULL DEFAULT 0,
    n_returns INT NOT NULL DEFAULT 0,
    n_gotos INT NOT NULL DEFAULT 0,
    n_tokens INT NOT NULL DEFAULT 0,
    n_operators INT NOT NULL DEFAULT 0,
    max_loop_depth INT NOT NULL DEFAULT 0,
    switch_in_loop INT NOT NULL DEFAULT 0,
    alloc_in_loop INT NOT NULL DEFAULT 0,
    call_in_loop INT NOT NULL DEFAULT 0,
    libm_in_loop INT NOT NULL DEFAULT 0,
    div_in_loop INT NOT NULL DEFAULT 0,
    strlen_in_loop INT NOT NULL DEFAULT 0,
    n_deref INT NOT NULL DEFAULT 0,
    n_arrow INT NOT NULL DEFAULT 0,
    n_subscript INT NOT NULL DEFAULT 0,
    n_addrof INT NOT NULL DEFAULT 0,
    n_cast INT NOT NULL DEFAULT 0,
    n_sizeof INT NOT NULL DEFAULT 0,
    n_ternary INT NOT NULL DEFAULT 0,
    n_bitop INT NOT NULL DEFAULT 0,
    n_shift INT NOT NULL DEFAULT 0,
    n_cmp INT NOT NULL DEFAULT 0,
    n_assign INT NOT NULL DEFAULT 0,
    n_compound_assign INT NOT NULL DEFAULT 0,
    n_incdec INT NOT NULL DEFAULT 0,
    n_logical INT NOT NULL DEFAULT 0,
    n_float_lit INT NOT NULL DEFAULT 0,
    n_null_check INT NOT NULL DEFAULT 0,
    n_intrinsic INT NOT NULL DEFAULT 0,
    n_atomic INT NOT NULL DEFAULT 0,
    n_volatile INT NOT NULL DEFAULT 0,
    n_restrict INT NOT NULL DEFAULT 0,
    n_likely INT NOT NULL DEFAULT 0,
    n_builtin INT NOT NULL DEFAULT 0,
    n_static_assert INT NOT NULL DEFAULT 0,
    n_labels INT NOT NULL DEFAULT 0,
    n_cases INT NOT NULL DEFAULT 0,
    n_locals INT NOT NULL DEFAULT 0,
    n_ptr_locals INT NOT NULL DEFAULT 0,
    n_magic INT NOT NULL DEFAULT 0,
    n_comment_lines INT NOT NULL DEFAULT 0,
    ret_null INT NOT NULL DEFAULT 0,
    ret_neg INT NOT NULL DEFAULT 0,
    ret_zero INT NOT NULL DEFAULT 0,
    ret_val INT NOT NULL DEFAULT 0,
    ret_void INT NOT NULL DEFAULT 0,
    fan_in INT NOT NULL DEFAULT 0,
    fan_out INT NOT NULL DEFAULT 0,
    n_callsites INT NOT NULL DEFAULT 0,
    is_recursive INT NOT NULL DEFAULT 0,
    is_leaf INT NOT NULL DEFAULT 0,
    is_root INT NOT NULL DEFAULT 0,
    n_fnptr_calls INT NOT NULL DEFAULT 0,
    n_hazards INT NOT NULL DEFAULT 0,
    n_mem INT NOT NULL DEFAULT 0,
    n_alloc INT NOT NULL DEFAULT 0,
    n_free INT NOT NULL DEFAULT 0,
    n_io INT NOT NULL DEFAULT 0,
    n_exec INT NOT NULL DEFAULT 0,
    n_libm INT NOT NULL DEFAULT 0,
    n_integer INT NOT NULL DEFAULT 0,
    n_concurrency INT NOT NULL DEFAULT 0,
    risk_score INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE params(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    pos INT NOT NULL,
    type TEXT NOT NULL,
    name TEXT,
    ptr_depth INT NOT NULL DEFAULT 0,
    is_array INT NOT NULL DEFAULT 0,
    is_const INT NOT NULL DEFAULT 0,
    is_varargs INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, pos)
) WITHOUT ROWID, STRICT;

CREATE TABLE fields(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    type TEXT NOT NULL,
    name TEXT NOT NULL,
    ptr_depth INT NOT NULL DEFAULT 0,
    array_len INT NOT NULL DEFAULT 0,
    is_fnptr INT NOT NULL DEFAULT 0,
    depth INT NOT NULL DEFAULT 0,
    in_union INT NOT NULL DEFAULT 0,
    line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

CREATE TABLE globals(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    module_id INT REFERENCES modules(id),
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    line INT NOT NULL,
    is_static INT NOT NULL DEFAULT 0,
    is_const INT NOT NULL DEFAULT 0,
    is_volatile INT NOT NULL DEFAULT 0,
    is_atomic INT NOT NULL DEFAULT 0,
    is_array INT NOT NULL DEFAULT 0,
    ptr_depth INT NOT NULL DEFAULT 0,
    has_init INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE edges(
    caller_id INT NOT NULL REFERENCES symbols(id),
    callee_id INT NOT NULL REFERENCES symbols(id),
    n_calls INT NOT NULL DEFAULT 1,
    same_file INT NOT NULL DEFAULT 0,
    same_module INT NOT NULL DEFAULT 0,
    is_self INT NOT NULL DEFAULT 0,
    PRIMARY KEY(caller_id, callee_id)
) WITHOUT ROWID, STRICT;

CREATE TABLE callsites(
    caller_id INT NOT NULL REFERENCES symbols(id),
    callee_id INT NOT NULL REFERENCES symbols(id),
    line INT NOT NULL,
    PRIMARY KEY(caller_id, callee_id, line)
) WITHOUT ROWID, STRICT;

CREATE TABLE includes(
    file_id INT NOT NULL REFERENCES files(id),
    target TEXT NOT NULL,
    target_id INT REFERENCES files(id),
    is_system INT NOT NULL DEFAULT 0,
    line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(file_id, target, line)
) WITHOUT ROWID, STRICT;

CREATE TABLE hazards(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    pattern TEXT NOT NULL,
    category TEXT NOT NULL,
    n INT NOT NULL DEFAULT 1,
    first_line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, pattern)
) WITHOUT ROWID, STRICT;

CREATE TABLE macros(
    symbol_id INT NOT NULL PRIMARY KEY REFERENCES symbols(id),
    is_functionlike INT NOT NULL DEFAULT 0,
    n_params INT NOT NULL DEFAULT 0,
    body TEXT,
    body_len INT NOT NULL DEFAULT 0,
    is_multiline INT NOT NULL DEFAULT 0
) WITHOUT ROWID, STRICT;

CREATE TABLE locals(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    type TEXT NOT NULL,
    name TEXT NOT NULL,
    ptr_depth INT NOT NULL DEFAULT 0,
    is_array INT NOT NULL DEFAULT 0,
    is_const INT NOT NULL DEFAULT 0,
    is_static INT NOT NULL DEFAULT 0,
    has_init INT NOT NULL DEFAULT 0,
    line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

CREATE TABLE layout(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    byte_off INT NOT NULL,
    byte_size INT NOT NULL,
    pad_before INT NOT NULL,
    exact INT NOT NULL,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

CREATE TABLE struct_size(
    symbol_id INT NOT NULL PRIMARY KEY REFERENCES symbols(id),
    total_size INT NOT NULL,
    tail_pad INT NOT NULL,
    total_pad INT NOT NULL,
    max_align INT NOT NULL,
    exact INT NOT NULL,
    n_lines_64 INT NOT NULL
) WITHOUT ROWID, STRICT;

CREATE TABLE literals(
    id INTEGER PRIMARY KEY,
    symbol_id INT REFERENCES symbols(id),
    file_id INT NOT NULL REFERENCES files(id),
    kind TEXT NOT NULL,
    value TEXT NOT NULL,
    line INT NOT NULL,
    is_magic INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE enum_members(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    name TEXT NOT NULL,
    value TEXT,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

CREATE TABLE markers(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    kind TEXT NOT NULL,
    line INT NOT NULL,
    text TEXT
) STRICT;

CREATE TABLE config_blocks(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    directive TEXT NOT NULL,
    expr TEXT NOT NULL,
    line INT NOT NULL,
    is_config INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE attributes(
    id INTEGER PRIMARY KEY,
    symbol_id INT REFERENCES symbols(id),
    file_id INT NOT NULL REFERENCES files(id),
    attr TEXT NOT NULL,
    line INT NOT NULL
) STRICT;

CREATE TABLE makefile_rules(
    id INTEGER PRIMARY KEY,
    rule TEXT NOT NULL,
    line INT NOT NULL,
    n_objs INT NOT NULL DEFAULT 0,
    n_srcs INT NOT NULL DEFAULT 0,
    uses_ar INT NOT NULL DEFAULT 0
) STRICT;

CREATE VIRTUAL TABLE sym_fts USING fts5(name, signature, content='');
"""

INDEXES = r"""
CREATE INDEX idx_sym_name ON symbols(name);
CREATE INDEX idx_sym_file_line ON symbols(file_id, line_start);
CREATE INDEX idx_sym_module_kind ON symbols(module_id, kind);

CREATE INDEX idx_fn_fanin ON symbols(fan_in DESC, name, file_id, cyclomatic, sloc, fan_out) WHERE kind='function';
CREATE INDEX idx_fn_cyclo ON symbols(cyclomatic DESC, name, file_id, sloc, max_nesting, cognitive) WHERE kind='function';
CREATE INDEX idx_fn_cog ON symbols(cognitive DESC, name, file_id, cyclomatic, max_nesting) WHERE kind='function';
CREATE INDEX idx_fn_nest ON symbols(max_nesting DESC, name, file_id, cyclomatic) WHERE kind='function';
CREATE INDEX idx_fn_risk ON symbols(risk_score DESC, name, file_id, cyclomatic, n_mem, n_io, is_recursive) WHERE kind='function';
CREATE INDEX idx_fn_rec ON symbols(cyclomatic DESC, name, file_id, max_nesting, n_io) WHERE is_recursive=1;
CREATE INDEX idx_fn_leaf ON symbols(fan_in DESC, name, file_id) WHERE is_leaf=1;
CREATE INDEX idx_fn_fnptr ON symbols(n_fnptr_calls DESC, name, file_id) WHERE n_fnptr_calls>0;
CREATE INDEX idx_fn_sloc ON symbols(sloc DESC, name, file_id, cyclomatic) WHERE kind='function';

CREATE INDEX idx_fn_mem_cover ON symbols(n_mem DESC, name, file_id, cyclomatic) WHERE n_mem>0;
CREATE INDEX idx_fn_io_cover ON symbols(n_io DESC, name, file_id) WHERE n_io>0;
CREATE INDEX idx_fn_libm ON symbols(n_libm DESC, name, file_id) WHERE n_libm>0;
CREATE INDEX idx_fn_leak ON symbols(n_alloc DESC, name, file_id) WHERE n_alloc>0 AND n_free=0;

CREATE INDEX idx_fn_swloop ON symbols(switch_in_loop DESC, name, file_id, max_loop_depth) WHERE switch_in_loop>0;
CREATE INDEX idx_fn_alloop ON symbols(alloc_in_loop DESC, name, file_id) WHERE alloc_in_loop>0;
CREATE INDEX idx_fn_nested ON symbols(max_loop_depth DESC, name, file_id, sloc) WHERE max_loop_depth>1;
CREATE INDEX idx_fn_libmlp ON symbols(libm_in_loop DESC, name, file_id) WHERE libm_in_loop>0;
CREATE INDEX idx_fn_divlp ON symbols(div_in_loop DESC, name, file_id) WHERE div_in_loop>0;
CREATE INDEX idx_fn_strlen ON symbols(strlen_in_loop DESC, name, file_id) WHERE strlen_in_loop>0;
CREATE INDEX idx_edge_callee ON edges(callee_id, caller_id);
CREATE INDEX idx_edge_xmod ON edges(caller_id) WHERE same_module=0;
CREATE INDEX idx_cs_callee ON callsites(callee_id, line);

CREATE INDEX idx_haz_cat ON hazards(category, n DESC);
CREATE INDEX idx_haz_pattern ON hazards(pattern, symbol_id);

CREATE INDEX idx_inc_target ON includes(target);
CREATE INDEX idx_inc_resolved ON includes(target_id) WHERE target_id IS NOT NULL;

CREATE INDEX idx_fields_sym ON fields(symbol_id, ordinal);
CREATE INDEX idx_layout_pad ON layout(symbol_id, pad_before DESC) WHERE pad_before>0;
CREATE INDEX idx_ss_pad ON struct_size(total_pad DESC, total_size);
CREATE INDEX idx_ss_lines ON struct_size(n_lines_64 DESC, total_size DESC);
CREATE INDEX idx_locals_sym ON locals(symbol_id, ordinal);
CREATE INDEX idx_locals_type ON locals(type);
CREATE INDEX idx_lit_val ON literals(value, file_id) WHERE is_magic=1;
CREATE INDEX idx_lit_sym ON literals(symbol_id);
CREATE INDEX idx_mark_kind ON markers(kind, file_id);
CREATE INDEX idx_cfg_expr ON config_blocks(expr) WHERE is_config=1;
CREATE INDEX idx_enum_sym ON enum_members(symbol_id, ordinal);
CREATE INDEX idx_attr_sym ON attributes(symbol_id);
CREATE INDEX idx_fn_intrin ON symbols(n_intrinsic DESC, name, file_id) WHERE n_intrinsic>0;
CREATE INDEX idx_fn_likely ON symbols(n_likely DESC, name, file_id) WHERE n_likely>0;
CREATE INDEX idx_fn_magic ON symbols(n_magic DESC, name, file_id) WHERE n_magic>0;
CREATE INDEX idx_fn_cast ON symbols(n_cast DESC, name, file_id) WHERE n_cast>0;
CREATE INDEX idx_fields_ptr ON fields(symbol_id) WHERE ptr_depth>0;
CREATE INDEX idx_fields_top ON fields(symbol_id, ptr_depth) WHERE in_union=0 AND depth=0;
CREATE INDEX idx_params_type ON params(type);

CREATE INDEX idx_glob_shared ON globals(module_id) WHERE is_static=1 AND is_const=0;
CREATE INDEX idx_glob_name ON globals(name);

CREATE INDEX idx_files_module ON files(module_id, sloc DESC);
CREATE INDEX idx_files_lang ON files(lang, sloc DESC);
"""

VIEWS = r"""
CREATE VIEW v_fn AS
SELECT s.id, s.name, f.path, m.name AS module, s.line_start, s.line_end,
    s.sloc, s.cyclomatic, s.cognitive, s.max_nesting, s.fan_in, s.fan_out,
    s.is_recursive, s.is_static, s.n_params, s.n_mem, s.n_alloc, s.n_free,
    s.n_io, s.n_exec, s.n_libm, s.n_integer, s.n_concurrency, s.n_fnptr_calls,
    s.risk_score
FROM symbols s JOIN files f ON f.id=s.file_id
LEFT JOIN modules m ON m.id=s.module_id
WHERE s.kind='function';

CREATE VIEW v_hotspots AS
SELECT *, (cyclomatic * 2 + cognitive + max_nesting * 5 + n_mem * 10
    + n_io * 8 + n_exec * 15 + n_integer + fan_in) AS heat
FROM v_fn ORDER BY heat DESC;

CREATE VIEW v_attack_surface AS
SELECT * FROM v_fn WHERE n_io>0 OR n_exec>0 OR (n_mem>0 AND fan_in=0);

CREATE VIEW v_struct_shape AS
SELECT s.name AS struct_name, f.path, COUNT(*) AS n_fields,
    SUM(fl.ptr_depth>0) AS n_pointers,
    SUM(fl.is_fnptr) AS n_fnptrs,
    SUM(fl.array_len>0) AS n_arrays
FROM symbols s JOIN fields fl ON fl.symbol_id=s.id
JOIN files f ON f.id=s.file_id
WHERE s.kind IN ('struct','union')
GROUP BY s.id;
"""

# ---------------------------------------------------------------------------
# Build phases
# ---------------------------------------------------------------------------
@dataclass
class _FileInfo:
    fid: int
    mid: int
    rel: str
    raw: str
    blank: str

@dataclass
class _FuncInfo:
    sid: int
    fid: int
    mid: int
    name: str
    body: str
    boff: int
    raw: str

@dataclass
class _ParseBuffers:
    locals: list[tuple[Any, ...]] = field(default_factory=list)
    literals: list[tuple[Any, ...]] = field(default_factory=list)
    attrs: list[tuple[Any, ...]] = field(default_factory=list)
    params: list[tuple[Any, ...]] = field(default_factory=list)
    hazards: list[tuple[Any, ...]] = field(default_factory=list)
    fnptr: list[tuple[int, int]] = field(default_factory=list)

    def clear(self) -> None:
        self.locals.clear()
        self.literals.clear()
        self.attrs.clear()
        self.params.clear()
        self.hazards.clear()
        self.fnptr.clear()


def _discover_files(root: str, db: sqlite3.Connection) -> list[_FileInfo]:
    """Phase 0: walk the tree, insert file rows, return C files for parsing."""
    mod_id: dict[str, int] = {}
    file_id: dict[str, int] = {}
    parsed: list[_FileInfo] = []

    def get_module(rel: str) -> int:
        name = module_of(rel)
        if name not in mod_id:
            kind = ("test" if name in ("tests", "fuzz") else
                    "tool" if name in ("tools", "bench") else
                    "doc" if name in ("docs", "examples") else
                    "native" if name.startswith("native/") else
                    "engine" if name.startswith("src/") else "other")
            cur = db.execute("INSERT INTO modules(name,kind) VALUES(?,?)", (name, kind))
            mod_id[name] = cur.lastrowid
        return mod_id[name]

    scan_root = os.path.join(root, "src")
    if not os.path.isdir(scan_root):
        scan_root = root

    for dirpath, dirnames, filenames in os.walk(scan_root):
        # corpus_* holds fuzz INPUTS, not source: 851 binary blobs that carry no
        # symbols but add 851 junk rows and overstate the file count 6x.
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and not d.startswith("corpus")]
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root)
            ext = os.path.splitext(fn)[1]
            try:
                data = open(full, "rb").read()
            except OSError:
                continue
            text = data.decode("utf-8", "replace")
            lines = text.splitlines()
            blank_n = sum(1 for l in lines if not l.strip())
            cmt = sum(1 for l in lines if l.strip().startswith(("//", "/*", "*", "#")))
            mid = get_module(rel)
            cur = db.execute(
                "INSERT INTO files(path,dir,basename,ext,lang,module_id,bytes,"
                "lines,sloc,blank_lines,comment_lines,max_line_len,sha1,parsed,"
                "is_header,is_test,is_generated) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (rel, os.path.dirname(rel) or ".", fn, ext,
                 LANG_BY_EXT.get(ext, "other"), mid, len(data), len(lines) + 1,
                 sum(1 for l in lines if l.strip()), blank_n, cmt,
                 max((len(l) for l in lines), default=0),
                 hashlib.sha1(data).hexdigest(),
                 1 if ext in C_EXT else 0, 1 if ext == ".h" else 0,
                 1 if rel.startswith(("tests/", "src/fuzz/")) else 0,
                 1 if ("unicode_gen" in fn or "-gen" in fn or
                       "GENERATED" in text[:400]) else 0))
            fid = cur.lastrowid
            file_id[rel] = fid
            if ext in C_EXT and text:
                parsed.append(_FileInfo(fid, mid, rel, text, blank_c(text)))

    # Store file_id map for include resolution in phase 1
    _discover_files.file_id = file_id  # type: ignore[attr-defined]
    return parsed


def _parse_file_symbols(
    finfo: _FileInfo,
    db: sqlite3.Connection,
    bufs: _ParseBuffers,
    fn_by_name: dict[str, list[tuple[int, int, int]]],
    fn_bodies: list[_FuncInfo],
) -> None:
    """Phase 1: extract symbols, params, fields, globals, macros, includes."""
    fid, mid, rel, raw, blank = finfo.fid, finfo.mid, finfo.rel, finfo.raw, finfo.blank
    file_id = getattr(_discover_files, 'file_id', {})

    # markers
    for mk in MARKER_RE.finditer(raw):
        ln = line_of(raw, mk.start())
        eol = raw.find('\n', mk.start())
        db.execute(
            "INSERT INTO markers(file_id,kind,line,text) VALUES(?,?,?,?)",
            (fid, (mk.group(1) or mk.group(2)).upper(), ln,
             raw[mk.start():eol if eol > 0 else None][:160]))

    # config blocks
    for cb in IFDEF_RE.finditer(raw):
        expr = cb.group(2).strip()[:160]
        db.execute(
            "INSERT INTO config_blocks(file_id,directive,expr,line,is_config) VALUES(?,?,?,?,?)",
            (fid, cb.group(1), expr, line_of(raw, cb.start()),
             1 if "CONFIG_" in expr else 0))

    # includes
    for m in INCLUDE.finditer(raw):
        tgt, sys_inc = m.group(2), 1 if m.group(1) == "<" else 0
        cand = os.path.normpath(os.path.join(os.path.dirname(rel), tgt))
        tid = file_id.get(cand) or file_id.get(tgt) or file_id.get(os.path.join("src", tgt))
        db.execute(
            "INSERT OR IGNORE INTO includes(file_id,target,target_id,is_system,line) VALUES(?,?,?,?,?)",
            (fid, tgt, tid, sys_inc, line_of(raw, m.start())))

    # macros
    for m in DEFINE.finditer(raw):
        name, args, body = m.group(1), m.group(2), (m.group(3) or "")
        ln = line_of(raw, m.start())
        cur = db.execute(
            "INSERT INTO symbols(file_id,module_id,name,kind,line_start,line_end,n_lines,signature) "
            "VALUES(?,?,?,'macro',?,?,1,?)",
            (fid, mid, name, ln, ln, m.group(0)[:200]))
        sid = cur.lastrowid
        db.execute(
            "INSERT INTO macros(symbol_id,is_functionlike,n_params,body,body_len,is_multiline) VALUES(?,?,?,?,?,?)",
            (sid, 1 if args else 0,
             len([a for a in (args or "()")[1:-1].split(",") if a.strip()]),
             body[:500], len(body), 1 if body.rstrip().endswith("\\") else 0))

    # typedefs
    for m in TYPEDEF_SIMPLE.finditer(raw):
        if "(" in m.group(1):
            continue
        ln = line_of(raw, m.start())
        db.execute(
            "INSERT INTO symbols(file_id,module_id,name,kind,line_start,line_end,n_lines,return_type) "
            "VALUES(?,?,?,'typedef',?,?,1,?)",
            (fid, mid, m.group(2), ln, ln,
             re.sub(r'\s+', ' ', m.group(1)).strip()[:120]))

    # structs / unions / enums
    for m in TAG_BODY.finditer(blank):
        kind, tag = m.group(1), m.group(2)
        ob = blank.index("{", m.start())
        flds, endline = struct_fields(blank, raw, ob)
        name = tag
        if not name:
            close = blank.find("}", ob)
            after = raw[close:close + 120]
            mm = re.search(r'}\s*(\w+)\s*;', after)
            name = mm.group(1) if mm else "(anon@%d)" % line_of(raw, m.start())
        ln = line_of(raw, m.start())
        cur = db.execute(
            "INSERT INTO symbols(file_id,module_id,name,kind,line_start,line_end,n_lines) "
            "VALUES(?,?,?,?,?,?,?)",
            (fid, mid, name, kind, ln, endline, endline - ln + 1))
        sid = cur.lastrowid
        if kind in ("struct", "union"):
            for f in flds:
                db.execute(
                    "INSERT OR IGNORE INTO fields(symbol_id,ordinal,type,name,ptr_depth,array_len,is_fnptr,depth,in_union,line) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?)",
                    (sid,) + f)
            lay, tot, tail, ex, mal = layout_struct(flds, kind == "union")
            for row in lay:
                db.execute(
                    "INSERT OR IGNORE INTO layout(symbol_id,ordinal,byte_off,byte_size,pad_before,exact) "
                    "VALUES(?,?,?,?,?,?)", (sid,) + row)
            tpad = sum(r[3] for r in lay) + tail
            db.execute(
                "INSERT OR IGNORE INTO struct_size(symbol_id,total_size,tail_pad,total_pad,max_align,exact,n_lines_64) "
                "VALUES(?,?,?,?,?,?,?)",
                (sid, tot, tail, tpad, mal, ex, (tot + 63) // 64 if tot else 0))
        elif kind == "enum":
            close = blank.find("}", ob)
            inner = raw[ob + 1:close] if close > ob else ""
            for i2, part in enumerate(inner.split(",")):
                em = re.match(r'\s*([A-Za-z_]\w*)\s*(?:=\s*([^,]+))?\s*', part)
                if em:
                    db.execute(
                        "INSERT OR IGNORE INTO enum_members(symbol_id,ordinal,name,value) VALUES(?,?,?,?)",
                        (sid, i2, em.group(1)[:80],
                         (em.group(2) or "").strip()[:60] or None))

    # functions (need spans first for globals exclusion)
    funcs = find_functions(blank, raw)
    fn_spans = sorted((ls, le) for _, _, ls, le, _, _, _, _ in funcs)
    span_starts = [s for s, _ in fn_spans]
    span_ends = [e for _, e in fn_spans]

    def in_function(ln: int) -> bool:
        idx = bisect.bisect_right(span_starts, ln) - 1
        return idx >= 0 and ln <= span_ends[idx]

    # globals
    for m in GLOBAL_RE.finditer(blank):
        ln = line_of(blank, m.start())
        if in_function(ln):
            continue
        storage, ctype, stars, gname, arr, init = m.groups()
        if gname in KEYWORDS or ctype.strip() in ("return", "typedef"):
            continue
        db.execute(
            "INSERT INTO globals(file_id,module_id,name,type,line,is_static,is_const,is_volatile,is_atomic,is_array,ptr_depth,has_init) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            (fid, mid, gname, re.sub(r'\s+', ' ', ctype).strip()[:120], ln,
             1 if (storage or "").strip() == "static" else 0,
             1 if "const" in ctype else 0,
             1 if "volatile" in ctype else 0,
             1 if ("_Atomic" in ctype or "atomic_" in ctype) else 0,
             1 if arr else 0, len(stars or ""), 1 if init else 0))

    # functions
    for name, sig, ls, le, st, inl, body, boff in funcs:
        mt = metrics(body)
        la = loop_analysis(body)
        ec = expr_counts(body)
        rs = return_shapes(body)
        ps = split_params(sig)
        raw_body = raw[boff:boff + len(body)]
        n_cmt = sum(1 for l in raw_body.splitlines()
                    if l.strip().startswith(("/*", "//", "*")))
        locs: list[tuple[Any, ...]] = []
        for lm in LOCAL_RE.finditer(body):
            stor, lty, lstars, lname = lm.group(1), lm.group(2), lm.group(3), lm.group(4)
            if lname in KEYWORDS or lty in ("return", "case", "goto"):
                continue
            locs.append((len(locs), (stor + lty).strip()[:120], lname[:80],
                         len(lstars or ""), 1 if lm.group(5) else 0,
                         1 if "const" in stor else 0,
                         1 if "static" in stor else 0,
                         1 if lm.group(6) else 0,
                         line_of(body, lm.start()) + ls - 1))
        n_loc = len(locs)
        n_ploc = sum(1 for l in locs if l[3] > 0)
        mags = [m for m in NUM_RE.finditer(body)
                if m.group(1) not in ("0", "1", "2", "8", "16", "32", "64")]
        n_magic = len(mags)

        cur = db.execute(
            "INSERT INTO symbols(file_id,module_id,name,kind,line_start,line_end,n_lines,"
            "is_static,is_inline,return_type,signature,n_params,n_ptr_params,sloc,body_bytes,"
            "cyclomatic,cognitive,max_nesting,n_loops,n_branches,n_returns,n_gotos,n_tokens,n_operators,"
            "max_loop_depth,switch_in_loop,alloc_in_loop,call_in_loop,libm_in_loop,div_in_loop,strlen_in_loop,"
            "n_deref,n_arrow,n_subscript,n_addrof,n_cast,n_sizeof,n_ternary,n_bitop,n_shift,n_cmp,n_assign,"
            "n_compound_assign,n_incdec,n_logical,n_float_lit,n_null_check,n_intrinsic,n_atomic,n_volatile,"
            "n_restrict,n_likely,n_builtin,n_static_assert,n_labels,n_cases,n_locals,n_ptr_locals,n_magic,"
            "n_comment_lines,ret_null,ret_neg,ret_zero,ret_val,ret_void) "
            "VALUES(?,?,?,'function'," + ",".join(['?'] * 61) + ")",
            (fid, mid, name, ls, le, le - ls + 1, st, inl,
             return_type_of(sig, name), sig, len(ps),
             sum(1 for p in ps if p[3] > 0), mt["sloc"], len(body),
             mt["cyclomatic"], mt["cognitive"], mt["max_nesting"],
             mt["n_loops"], mt["n_branches"], mt["n_returns"],
             mt["n_gotos"], mt["n_tokens"], mt["n_operators"],
             la["max_loop_depth"], la["switch_in_loop"],
             la["alloc_in_loop"], la["call_in_loop"], la["libm_in_loop"],
             la["div_in_loop"], la["strlen_in_loop"],
             ec["n_deref"], ec["n_arrow"], ec["n_subscript"], ec["n_addrof"],
             ec["n_cast"], ec["n_sizeof"], ec["n_ternary"], ec["n_bitop"], ec["n_shift"],
             ec["n_cmp"], ec["n_assign"], ec["n_compound_assign"], ec["n_incdec"],
             ec["n_logical"], ec["n_float_lit"], ec["n_null_check"], ec["n_intrinsic"],
             ec["n_atomic"], ec["n_volatile"], ec["n_restrict"], ec["n_likely"],
             ec["n_builtin"], ec["n_static_assert"], ec["n_labels"], ec["n_cases"],
             n_loc, n_ploc, n_magic, n_cmt,
             rs["ret_null"], rs["ret_neg"], rs["ret_zero"], rs["ret_val"], rs["ret_void"]))
        sid = cur.lastrowid

        for l in locs:
            bufs.locals.append((sid,) + l)
        for m2 in mags[:200]:
            v = m2.group(1)
            bufs.literals.append((sid, fid,
                "hex" if v[:2].lower() == "0x" else "int",
                v[:40], line_of(body, m2.start()) + ls - 1))
        for am in ATTR_RE.finditer(sig):
            bufs.attrs.append((sid, fid, am.group(1)[:80], ls))
        for p in ps:
            bufs.params.append((sid,) + p)

        fn_by_name.setdefault(name, []).append((sid, fid, mid))
        fn_bodies.append(_FuncInfo(sid, fid, mid, name, body, boff, raw))


def _build_call_graph(
    db: sqlite3.Connection,
    fn_by_name: dict[str, list[tuple[int, int, int]]],
    fn_bodies: list[_FuncInfo],
    bufs: _ParseBuffers,
) -> None:
    """Phase 2: resolve call edges, callsites, hazards, fnptr counts."""

    def resolve(nm: str, cfid: int) -> Optional[tuple[int, int, int]]:
        c = fn_by_name.get(nm)
        if not c:
            return None
        if len(c) == 1:
            return c[0]
        for t in c:
            if t[1] == cfid:
                return t
        return c[0]

    edge: dict[tuple[int, int], list[int]] = {}
    sites: set[tuple[int, int, int]] = set()

    for fi in fn_bodies:
        sid, fid, mid, name, body, boff, raw = fi.sid, fi.fid, fi.mid, fi.name, fi.body, fi.boff, fi.raw
        hz: dict[str, list[Any]] = {}
        fnptr = 0
        for m in IDENT.finditer(body):
            tok = m.group(0)
            e = m.end()
            while e < len(body) and body[e] in " \t\r\n":
                e += 1
            if e >= len(body) or body[e] != "(":
                continue
            if tok in KEYWORDS:
                continue
            line = line_of(raw, boff + m.start())
            t = resolve(tok, fid)
            if t:
                key = (sid, t[0])
                if key in edge:
                    edge[key][0] += 1
                else:
                    edge[key] = [1, 1 if t[1] == fid else 0,
                                 1 if t[2] == mid else 0, 1 if t[0] == sid else 0]
                sites.add((sid, t[0], line))
            if tok in HAZARD_FUNCS:
                cat = HAZARD_FUNCS[tok]
                if tok in hz:
                    hz[tok][1] += 1
                else:
                    hz[tok] = [cat, 1, line]
        fnptr = (len(re.findall(r'\(\s*\*\s*\w+\s*\)\s*\(', body)) +
                 len(re.findall(r'(?:->|\.)\w+\s*\(', body)))
        for pat, cat, rx in HAZARD_RE:
            c = len(rx.findall(body))
            if c:
                hz[pat] = [cat, c, 0]
        for pat, (cat, c, ln) in hz.items():
            bufs.hazards.append((sid, pat, cat, c, ln))
        bufs.fnptr.append((fnptr, sid))

    db.executemany(
        "INSERT OR IGNORE INTO locals(symbol_id,ordinal,type,name,ptr_depth,is_array,is_const,is_static,has_init,line) "
        "VALUES(?,?,?,?,?,?,?,?,?,?)", bufs.locals)
    db.executemany(
        "INSERT INTO literals(symbol_id,file_id,kind,value,line,is_magic) VALUES(?,?,?,?,?,1)",
        bufs.literals)
    db.executemany(
        "INSERT INTO attributes(symbol_id,file_id,attr,line) VALUES(?,?,?,?)",
        bufs.attrs)
    db.executemany(
        "INSERT OR IGNORE INTO params(symbol_id,pos,type,name,ptr_depth,is_array,is_const,is_varargs) "
        "VALUES(?,?,?,?,?,?,?,?)", bufs.params)
    db.executemany(
        "INSERT OR IGNORE INTO hazards(symbol_id,pattern,category,n,first_line) VALUES(?,?,?,?,?)",
        bufs.hazards)
    db.executemany(
        "UPDATE symbols SET n_fnptr_calls=? WHERE id=?", bufs.fnptr)
    db.executemany(
        "INSERT INTO edges(caller_id,callee_id,n_calls,same_file,same_module,is_self) VALUES(?,?,?,?,?,?)",
        [(a, b, v[0], v[1], v[2], v[3]) for (a, b), v in edge.items()])
    db.executemany(
        "INSERT OR IGNORE INTO callsites(caller_id,callee_id,line) VALUES(?,?,?)",
        list(sites))


def _materialize(db: sqlite3.Connection) -> None:
    """Phase 3: compute and store aggregates."""
    db.executescript(r"""
    UPDATE symbols SET
        fan_out = (SELECT COUNT(*) FROM edges e WHERE e.caller_id=symbols.id AND e.is_self=0),
        fan_in = (SELECT COUNT(*) FROM edges e WHERE e.callee_id=symbols.id AND e.is_self=0),
        n_callsites = (SELECT COUNT(*) FROM callsites c WHERE c.callee_id=symbols.id),
        is_recursive = (SELECT COUNT(*) FROM edges e WHERE e.caller_id=symbols.id AND e.is_self=1)>0
    WHERE kind='function';

    UPDATE symbols SET is_leaf = (fan_out=0), is_root = (fan_in=0)
    WHERE kind='function';

    UPDATE symbols SET
        n_hazards = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id),
        n_mem = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='memory'),
        n_alloc = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='alloc' AND h.pattern<>'free'),
        n_free = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.pattern='free'),
        n_io = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='io'),
        n_exec = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='exec'),
        n_libm = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='libm'),
        n_integer = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='integer'),
        n_concurrency = (SELECT COALESCE(SUM(n),0) FROM hazards h WHERE h.symbol_id=symbols.id AND h.category='concurrency')
    WHERE kind='function';

    UPDATE symbols SET risk_score =
        cyclomatic*2 + cognitive + max_nesting*5 + n_mem*10 + n_io*8
        + n_exec*15 + n_integer + (CASE WHEN is_recursive THEN 25 ELSE 0 END)
        + (CASE WHEN n_alloc>0 AND n_free=0 THEN 10 ELSE 0 END)
    WHERE kind='function';

    UPDATE files SET
        n_functions = (SELECT COUNT(*) FROM symbols s WHERE s.file_id=files.id AND s.kind='function'),
        n_includes = (SELECT COUNT(*) FROM includes i WHERE i.file_id=files.id),
        n_globals = (SELECT COUNT(*) FROM globals g WHERE g.file_id=files.id),
        total_cyclo = (SELECT COALESCE(SUM(cyclomatic),0) FROM symbols s WHERE s.file_id=files.id),
        max_cyclo = (SELECT COALESCE(MAX(cyclomatic),0) FROM symbols s WHERE s.file_id=files.id);

    UPDATE modules SET
        n_files = (SELECT COUNT(*) FROM files f WHERE f.module_id=modules.id),
        n_symbols = (SELECT COUNT(*) FROM symbols s WHERE s.module_id=modules.id),
        sloc = (SELECT COALESCE(SUM(sloc),0) FROM files f WHERE f.module_id=modules.id);
    """)


def _parse_makefile(root: str, db: sqlite3.Connection) -> None:
    """Parse Makefile link lines."""
    for mk_name in ("Makefile", "makefile", "GNUmakefile"):
        mk = os.path.join(root, mk_name)
        if os.path.isfile(mk):
            break
    else:
        return

    rows: list[tuple[str, int, int, int, int]] = []
    cont, rule, start = "", None, 0
    for ln_no, ln in enumerate(open(mk, errors="replace"), 1):
        ln = ln.rstrip('\n')
        if cont:
            ln, cont = cont + " " + ln.lstrip(), ""
        if ln.endswith("\\"):
            cont, start = ln[:-1], start or ln_no
            continue
        m = re.match(r'^([A-Za-z0-9_./$()-]+)\s*:[^=]', ln)
        if not m:
            start = 0
            continue
        rule, pre = m.group(1), ln.split(":", 1)[1]
        rows.append((rule, start or ln_no,
                     len(re.findall(r'[\w/$()-]+\.o\b', pre)),
                     len(re.findall(r'[\w/-]+\.c\b', pre)),
                     1 if ".a" in pre else 0))
        start = 0
    db.executemany(
        "INSERT INTO makefile_rules(rule,line,n_objs,n_srcs,uses_ar) VALUES(?,?,?,?,?)",
        rows)


def build(root: str, db: sqlite3.Connection, quiet: bool = False) -> int:
    """Parse `root` into the already-open connection `db`."""
    db.executescript(SCHEMA)

    t0 = time.time()
    parsed = _discover_files(root, db)
    if not quiet:
        print(f"  {len(parsed)} C files discovered in {time.time()-t0:.1f}s")

    bufs = _ParseBuffers()
    fn_by_name: dict[str, list[tuple[int, int, int]]] = {}
    fn_bodies: list[_FuncInfo] = []

    t1 = time.time()
    for i, finfo in enumerate(parsed):
        _parse_file_symbols(finfo, db, bufs, fn_by_name, fn_bodies)
        if not quiet and (i + 1) % 50 == 0:
            print(f"  ... {i+1}/{len(parsed)} files parsed")
    if not quiet:
        print(f"  symbols parsed in {time.time()-t1:.1f}s")

    t2 = time.time()
    _build_call_graph(db, fn_by_name, fn_bodies, bufs)
    if not quiet:
        print(f"  call graph built in {time.time()-t2:.1f}s")

    _parse_makefile(root, db)

    t3 = time.time()
    _materialize(db)
    if not quiet:
        print(f"  aggregates materialized in {time.time()-t3:.1f}s")

    db.execute("INSERT INTO sym_fts(rowid,name,signature) "
               "SELECT id,name,COALESCE(signature,'') FROM symbols")
    db.executescript(INDEXES)
    db.executescript(VIEWS)
    db.commit()
    db.execute("ANALYZE")
    return len(parsed)

# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
MAX_CELL = 78

def _cell(v: Any) -> str:
    t = str(v)
    return t if len(t) <= MAX_CELL else t[:MAX_CELL - 3] + "..."

def render(rows: list[tuple[Any, ...]], cols: list[str], out: Any = sys.stdout) -> None:
    if not rows:
        print(" (no rows)", file=out)
        return
    rows = [[_cell(v) for v in r] for r in rows]
    w = [max(len(str(cols[i])), max(len(str(r[i])) for r in rows))
         for i in range(len(cols))]
    print(" " + " ".join(str(cols[i]).ljust(w[i]) for i in range(len(cols))), file=out)
    print(" " + " ".join("-" * w[i] for i in range(len(cols))), file=out)
    for r in rows:
        print(" " + " ".join(str(r[i]).ljust(w[i]) for i in range(len(r))), file=out)


def report(db: sqlite3.Connection) -> None:
    q = lambda s, *a: db.execute(s, a).fetchall()

    def table(title: str, rows: list[tuple[Any, ...]], cols: tuple[str, ...], note: Optional[str] = None) -> None:
        print("\n=== " + title + " ===")
        if note:
            print(" (" + note + ")")
        if not rows:
            print(" (none)")
            return
        w = [max(len(cols[i]), max(len(str(r[i])) for r in rows))
             for i in range(len(cols))]
        print(" " + " ".join(cols[i].ljust(w[i]) for i in range(len(cols))))
        for r in rows:
            print(" " + " ".join(str(r[i]).ljust(w[i]) for i in range(len(r))))

    print("codegraph: %d files / %d modules / %d symbols (%d fn) / %d edges / "
          "%d callsites / %d fields / %d globals" % (
              q("SELECT COUNT(*) FROM files")[0][0],
              q("SELECT COUNT(*) FROM modules")[0][0],
              q("SELECT COUNT(*) FROM symbols")[0][0],
              q("SELECT COUNT(*) FROM symbols WHERE kind='function'")[0][0],
              q("SELECT COUNT(*) FROM edges")[0][0],
              q("SELECT COUNT(*) FROM callsites")[0][0],
              q("SELECT COUNT(*) FROM fields")[0][0],
              q("SELECT COUNT(*) FROM globals")[0][0]))

    table("modules by weight",
          q("SELECT name, kind, n_files, sloc, n_symbols FROM modules ORDER BY sloc DESC LIMIT 20"),
          ("module", "kind", "files", "sloc", "symbols"))

    table("highest fan-in (a win here multiplies across callers)",
          q("""SELECT name, fan_in, n_callsites, cyclomatic, sloc,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE kind='function' ORDER BY fan_in DESC LIMIT 20"""),
          ("function", "fan_in", "sites", "cyclo", "sloc", "file"))

    table("risk_score: complexity x hazard x reach, precomputed",
          q("""SELECT name, risk_score, cyclomatic, cognitive, max_nesting,
              n_mem, n_io, is_recursive,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE kind='function' ORDER BY risk_score DESC LIMIT 20"""),
          ("function", "risk", "cyclo", "cog", "nest", "mem", "io", "rec", "file"))

    table("libm inside a body (categorical vectorisation blocker)",
          q("""SELECT name, n_libm, n_loops, sloc,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE n_libm>0 AND n_loops>0 ORDER BY n_libm DESC LIMIT 20"""),
          ("function", "libm", "loops", "sloc", "file"),
          "a libm call in a loop body makes that loop unvectorizable")

    table("pointer-dense structs",
          q("""SELECT s.name, COUNT(*) n_fields, SUM(f.ptr_depth>0) n_ptr,
              SUM(f.is_fnptr) n_fnptr, ROUND(100.0*SUM(f.ptr_depth>0)/COUNT(*)) pct,
              (SELECT path FROM files WHERE id=s.file_id)
              FROM symbols s JOIN fields f ON f.symbol_id=s.id
              WHERE s.kind IN ('struct','union') AND f.in_union=0 AND f.depth=0
              GROUP BY s.id HAVING n_fields>=6 AND pct>=50 ORDER BY n_ptr DESC LIMIT 20"""),
          ("struct", "fields", "ptrs", "fnptr", "%ptr", "file"),
          "TOP-LEVEL fields only: union arms are alternatives")

    table("widest structs by field count",
          q("""SELECT s.name, COUNT(*) n_fields, SUM(f.array_len>0) arrays,
              (SELECT path FROM files WHERE id=s.file_id)
              FROM symbols s JOIN fields f ON f.symbol_id=s.id
              WHERE s.kind IN ('struct','union') GROUP BY s.id ORDER BY n_fields DESC LIMIT 20"""),
          ("struct", "fields", "arrays", "file"))

    table("cross-module call coupling",
          q("""SELECT mc.name caller_mod, mt.name callee_mod, COUNT(*) edges
              FROM edges e JOIN symbols sc ON sc.id=e.caller_id
              JOIN symbols st ON st.id=e.callee_id
              JOIN modules mc ON mc.id=sc.module_id
              JOIN modules mt ON mt.id=st.module_id
              WHERE e.same_module=0 GROUP BY mc.id, mt.id ORDER BY edges DESC LIMIT 20"""),
          ("from_module", "to_module", "edges"))

    table("mutable file-scope state",
          q("""SELECT g.name, g.type, g.is_atomic, g.is_volatile, g.line,
              (SELECT path FROM files WHERE id=g.file_id)
              FROM globals g WHERE g.is_const=0 AND g.is_static=1 AND g.is_atomic=0
              AND (SELECT path FROM files WHERE id=g.file_id) LIKE 'src/%'
              AND g.type NOT LIKE '%JSClassDef%' ORDER BY g.ptr_depth DESC, g.name LIMIT 20"""),
          ("global", "type", "atomic", "vol", "line", "file"),
          "a threaded module mutating any of these without a lock is a race")

    table("untrusted-input frontier",
          q("""SELECT name, n_io, n_mem, n_integer, cyclomatic,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE n_io>0 ORDER BY (n_mem+n_integer) DESC, n_io DESC LIMIT 20"""),
          ("function", "io", "mem", "int", "cyclo", "file"),
          "parses attacker bytes AND does pointer/size math = review first")

    table("recursive on untrusted input",
          q("""SELECT name, cyclomatic, max_nesting, n_io,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE is_recursive=1
              AND (SELECT path FROM files WHERE id=file_id) LIKE 'src/%'
              ORDER BY cyclomatic DESC LIMIT 20"""),
          ("function", "cyclo", "nest", "io", "file"))

    table("alloc without free in the same function",
          q("""SELECT name, n_alloc, n_free, fan_in,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE n_alloc>0 AND n_free=0
              AND (SELECT path FROM files WHERE id=file_id) LIKE 'src/%'
              ORDER BY n_alloc DESC LIMIT 20"""),
          ("function", "alloc", "free", "fan_in", "file"),
          "not a leak per se: ownership often transfers out")

    table("integer-hazard density",
          q("""SELECT name, n_integer, n_mem, n_io, cyclomatic,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE n_integer>0 AND (n_io>0 OR n_mem>0)
              AND (SELECT path FROM files WHERE id=file_id) LIKE 'src/%'
              ORDER BY n_integer DESC LIMIT 20"""),
          ("function", "int", "mem", "io", "cyclo", "file"),
          "shift/ptr-cast/size-mul next to I/O is exactly the CWE-190 shape")

    table("where the call graph is BLIND",
          q("""SELECT name, n_fnptr_calls, fan_out, n_io,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE n_fnptr_calls>0 ORDER BY n_fnptr_calls DESC LIMIT 20"""),
          ("function", "fnptr", "fan_out", "io", "file"),
          "reachability UNDERCOUNTS through these: callbacks are unresolved")

    table("concurrency primitives by function",
          q("""SELECT name, n_concurrency, fan_in,
              (SELECT path FROM files WHERE id=file_id)
              FROM symbols WHERE n_concurrency>0 ORDER BY n_concurrency DESC LIMIT 20"""),
          ("function", "conc", "fan_in", "file"))


# ---------------------------------------------------------------------------
# Queries
# ---------------------------------------------------------------------------
_Q_EXTRA = [
(
    "allocator-split",
    "libc malloc vs the engine allocator: which files mix them",
    "ANSWERS whether a module's buffers are visible to memoryUsage().\n"
    "ACT pick ONE per file. A file using both is usually an accident.\n"
    "MISLEADS a file may legitimately use js_malloc for JS values and libc for private scratch.",
    """SELECT f.path,
        SUM(CASE WHEN h.pattern IN ('malloc','calloc','realloc','strdup','strndup') THEN h.n ELSE 0 END) AS libc,
        SUM(CASE WHEN h.pattern IN ('js_malloc','js_realloc') THEN h.n ELSE 0 END) AS engine,
        COUNT(DISTINCT s.id) AS fns
    FROM hazards h JOIN symbols s ON s.id=h.symbol_id JOIN files f ON f.id=s.file_id
    WHERE h.category='alloc' AND h.pattern<>'free' AND h.pattern<>'js_free'
    GROUP BY f.path HAVING libc>0 AND engine>0
    ORDER BY (libc+engine) DESC LIMIT :lim"""),
(
    "alloc-cost",
    "Allocations per call TRANSITIVELY",
    "ANSWERS what one call to this function really costs the allocator.\n"
    "ACT direct counts literal malloc; xalloc walks the call graph 3 deep.\n"
    "MISLEADS multiplier is STATIC call sites, not trip count.",
    """WITH RECURSIVE
    edge(caller, callee, mult) AS (
        SELECT caller_id, callee_id, COUNT(*) FROM callsites GROUP BY caller_id, callee_id),
    direct(sym, n) AS (
        SELECT symbol_id, SUM(n) FROM hazards
        WHERE category='alloc' AND pattern NOT IN ('free','js_free') GROUP BY symbol_id),
    walk(root, sym, depth, mult) AS (
        SELECT s.id, s.id, 0, 1 FROM symbols s WHERE s.kind='function'
        UNION ALL
        SELECT w.root, e.callee, w.depth+1, w.mult*e.mult
        FROM walk w JOIN edge e ON e.caller=w.sym
        WHERE w.depth < 3 AND e.callee <> w.root AND w.mult < 4096)
    SELECT s.name, SUM(w.mult*d.n) AS xalloc,
        COALESCE((SELECT n FROM direct WHERE sym=s.id),0) AS direct,
        s.n_loops AS loops, m.name AS module,
        f.path || ':' || s.line_start AS at
    FROM walk w JOIN direct d ON d.sym=w.sym
    JOIN symbols s ON s.id=w.root JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND f.path LIKE :mod
    GROUP BY w.root HAVING xalloc >= 8
    ORDER BY xalloc DESC LIMIT :lim"""),
(
    "stdio-not-write",
    "Buffered stdio where a single write(2) is the atomic form",
    "ANSWERS where output framing can interleave between concurrent writers.\n"
    "ACT fwrite/fprintf give NO guarantee of one syscall per call.\n"
    "MISLEADS a diagnostic nobody parses does not need atomicity.",
    """SELECT s.name, SUM(h.n) AS stdio_calls,
        GROUP_CONCAT(DISTINCT h.pattern) AS via,
        f.path || ':' || s.line_start AS at
    FROM hazards h JOIN symbols s ON s.id=h.symbol_id JOIN files f ON f.id=s.file_id
    WHERE h.category='stdio'
    GROUP BY s.id ORDER BY stdio_calls DESC LIMIT :lim"""),
(
    "loop-multiplied",
    "A loop inside a loop ACROSS a call",
    "ANSWERS where an inner scan is paid once per element of an outer one.\n"
    "ACT this is the O(n*m) the profile blames on the caller.\n"
    "MISLEADS neither trip count is known statically.",
    """WITH RECURSIVE
    edge(caller, callee) AS (SELECT DISTINCT caller_id, callee_id FROM callsites),
    reach(root, sym, depth) AS (
        SELECT s.id, s.id, 0 FROM symbols s WHERE s.kind='function' AND s.n_loops > 0
        UNION ALL
        SELECT r.root, e.callee, r.depth+1 FROM reach r JOIN edge e ON e.caller=r.sym
        WHERE r.depth < 2 AND e.callee <> r.root)
    SELECT outer_s.name AS outer_fn, outer_s.n_loops AS outer_loops,
        inner_s.name AS inner_fn, inner_s.n_loops AS inner_loops,
        inner_s.sloc AS inner_sloc, r.depth,
        f.path || ':' || inner_s.line_start AS inner_at
    FROM reach r
    JOIN symbols outer_s ON outer_s.id=r.root
    JOIN symbols inner_s ON inner_s.id=r.sym
    JOIN files f ON f.id=inner_s.file_id
    WHERE r.depth > 0 AND inner_s.n_loops > 0 AND inner_s.kind='function'
    AND f.path LIKE :mod
    ORDER BY (outer_s.n_loops * inner_s.n_loops) DESC, inner_s.sloc ASC LIMIT :lim"""),
]

QUERIES = [
(
    "hot-multipliers",
    "Where one fix multiplies: highest fan-in, ranked with complexity",
    "ANSWERS which functions the rest of the tree leans on hardest.\n"
    "ACT a win in a high-fan-in leaf pays back once per caller.\n"
    "MISLEADS fan_in counts STATIC call sites, not dynamic frequency.",
    """SELECT s.name, s.fan_in, s.n_callsites AS sites, s.cyclomatic AS cyclo,
        s.sloc, s.is_static AS stat, m.name AS module,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.fan_in DESC, s.cyclomatic DESC LIMIT :lim"""),
(
    "risk-ranked",
    "Security review order: complexity x hazard x recursion",
    "ANSWERS if you can only review N functions this week, which N.\n"
    "ACT risk_score = 2*cyclo + cognitive + 5*nesting + 10*mem + 8*io + 15*exec + integer + 25 if recursive + 10 if alloc-without-free.\n"
    "MISLEADS it is a heuristic, not a finding.",
    """SELECT s.name, s.risk_score AS risk, s.cyclomatic AS cyclo, s.cognitive AS cog,
        s.max_nesting AS nest, s.n_mem AS mem, s.n_io AS io,
        s.n_integer AS int_, s.is_recursive AS rec,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.risk_score DESC LIMIT :lim"""),
(
    "vectorisation-blocked",
    "Loops that CANNOT vectorise: a libm call in the body",
    "ANSWERS which hot loops are categorically unvectorizable as written.\n"
    "ACT a libm call in a loop body is a hard stop for the vectoriser.\n"
    "MISLEADS it does not know if the loop is hot.",
    """SELECT s.name, s.n_libm AS libm, s.n_loops AS loops, s.max_nesting AS nest,
        s.sloc, s.fan_in, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_libm>0 AND s.n_loops>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_libm DESC, s.n_loops DESC LIMIT :lim"""),
(
    "cache-hostile-layout",
    "Pointer-dense structs: each pointer field defeats the prefetcher",
    "ANSWERS which structures drag a whole cache line to read one field.\n"
    "ACT candidates for splitting hot fields into their own array (SoA).\n"
    "MISLEADS counts TOP-LEVEL fields only.",
    """SELECT s.name AS struct_, COUNT(*) AS fields,
        SUM(fl.ptr_depth>0) AS ptrs, SUM(fl.is_fnptr) AS fnptr,
        CAST(ROUND(100.0*SUM(fl.ptr_depth>0)/COUNT(*)) AS INT) AS pct_ptr,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN fields fl ON fl.symbol_id=s.id
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind IN ('struct','union') AND fl.in_union=0 AND fl.depth=0
    AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id HAVING fields>=6 AND pct_ptr>=50
    ORDER BY ptrs DESC LIMIT :lim"""),
(
    "untrusted-frontier",
    "Parses attacker bytes AND does pointer/size arithmetic",
    "ANSWERS the functions where a memory-safety bug is actually reachable.\n"
    "ACT review these against the CWE-190 shape.\n"
    "MISLEADS raw recv/read only.",
    """SELECT s.name, s.n_io AS io, s.n_mem AS mem, s.n_integer AS int_,
        s.n_alloc AS alloc, s.cyclomatic AS cyclo,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_io>0 AND (s.n_mem>0 OR s.n_integer>0)
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_mem + s.n_integer) DESC, s.n_io DESC LIMIT :lim"""),
(
    "stack-exhaustion",
    "Self-recursive functions: unbounded input depth = stack DoS",
    "ANSWERS where a deeply nested input can exhaust the C stack.\n"
    "ACT every one of these needs a depth cap that is TESTED.\n"
    "MISLEADS only DIRECT self-recursion.",
    """SELECT s.name, s.cyclomatic AS cyclo, s.max_nesting AS nest, s.n_io AS io,
        s.fan_in, s.sloc, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.is_recursive=1 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.cyclomatic DESC LIMIT :lim"""),
(
    "ownership-review",
    "Allocates but never frees in the same function",
    "ANSWERS where allocation ownership crosses a function boundary.\n"
    "ACT each row must have a named owner that frees it on EVERY path.\n"
    "MISLEADS this is a REVIEW list, NOT a leak list.",
    """SELECT s.name, s.n_alloc AS allocs, s.n_free AS frees, s.fan_in,
        s.n_returns AS returns_, s.n_gotos AS gotos,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_alloc>0 AND s.n_free=0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_alloc DESC, s.fan_in DESC LIMIT :lim"""),
(
    "race-surface",
    "Mutable, non-atomic, file-scope state",
    "ANSWERS what two threads could be writing at once.\n"
    "ACT every one of these needs a lock, an atomic, or a proof.\n"
    "MISLEADS it does not know which modules are threaded.",
    """SELECT g.name, g.type, g.ptr_depth AS ptr, g.is_array AS arr,
        g.has_init AS init, m.name AS module,
        f.path || ':' || g.line AS at,
        (SELECT COUNT(*) FROM symbols s2 WHERE s2.module_id=g.module_id AND s2.n_concurrency>0) AS mod_conc_fns
    FROM globals g JOIN files f ON f.id=g.file_id
    LEFT JOIN modules m ON m.id=g.module_id
    WHERE g.is_const=0 AND g.is_static=1 AND g.is_atomic=0 AND g.is_volatile=0
    AND f.path LIKE 'src/%' AND COALESCE(m.name,'') LIKE :mod
    ORDER BY mod_conc_fns DESC, g.ptr_depth DESC LIMIT :lim"""),
(
    "module-coupling",
    "Cross-module call edges: where a seam would actually cut",
    "ANSWERS how entangled the subsystems are.\n"
    "ACT a heavy one-way edge is a real seam; a heavy pair is a cycle.\n"
    "MISLEADS counts DISTINCT pairs, not frequency, and misses callbacks.",
    """SELECT mc.name AS from_module, mt.name AS to_module, COUNT(*) AS edges,
        SUM(e.n_calls) AS calls,
        COUNT(DISTINCT e.callee_id) AS distinct_targets
    FROM edges e
    JOIN symbols sc ON sc.id=e.caller_id
    JOIN symbols st ON st.id=e.callee_id
    JOIN modules mc ON mc.id=sc.module_id
    JOIN modules mt ON mt.id=st.module_id
    WHERE e.same_module=0 AND mc.name LIKE :mod
    GROUP BY mc.id, mt.id ORDER BY edges DESC LIMIT :lim"""),
(
    "graph-blindspots",
    "Where reachability is WRONG: indirect dispatch density",
    "ANSWERS which functions call through pointers.\n"
    "ACT read these before trusting any 'X is unreachable' claim.\n"
    "MISLEADS includes ordinary struct-member calls.",
    """SELECT s.name, s.n_fnptr_calls AS fnptr, s.fan_out, s.fan_in, s.n_io AS io,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_fnptr_calls>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_fnptr_calls DESC, s.fan_out DESC LIMIT :lim"""),
(
    "per-element-dispatch",
    "A switch INSIDE a loop: type dispatch paid once per element",
    "ANSWERS where a loop re-decides the same thing every iteration.\n"
    "ACT hoist the switch out of the loop.\n"
    "MISLEADS a bytecode interpreter's dispatch loop is correct by design.",
    """SELECT s.name, s.switch_in_loop AS sw, s.max_loop_depth AS depth,
        s.call_in_loop AS calls, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.switch_in_loop>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.switch_in_loop DESC, s.call_in_loop DESC LIMIT :lim"""),
(
    "loop-invariant-strlen",
    "strlen() inside a loop: accidental O(n^2)",
    "ANSWERS where a length is recomputed that cannot have changed.\n"
    "ACT hoist it, or store the length beside the string.\n"
    "MISLEADS a strlen over a SHORT string is a few cycles.",
    """SELECT s.name, s.strlen_in_loop AS strlens, s.max_loop_depth AS depth,
        s.fan_in, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.strlen_in_loop>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.max_loop_depth DESC, s.strlen_in_loop DESC LIMIT :lim"""),
(
    "alloc-per-iteration",
    "malloc/realloc inside a loop body",
    "ANSWERS which loops allocate once per item.\n"
    "ACT hoist the allocation, reserve capacity up front.\n"
    "MISLEADS a realloc-grow loop is amortised O(1).",
    """SELECT s.name, s.alloc_in_loop AS allocs, s.max_loop_depth AS depth,
        s.n_free AS frees, s.fan_in, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.alloc_in_loop>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.alloc_in_loop DESC LIMIT :lim"""),
(
    "nested-loops",
    "Loop depth >= 2: the O(n^k) candidates",
    "ANSWERS where cost grows super-linearly in the input.\n"
    "ACT check what bounds the INNER trip count.\n"
    "MISLEADS depth counts LEXICAL nesting, not asymptotics.",
    """SELECT s.name, s.max_loop_depth AS depth, s.n_loops AS loops,
        s.call_in_loop AS calls, s.div_in_loop AS divs, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.max_loop_depth>1 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.max_loop_depth DESC, s.call_in_loop DESC LIMIT :lim"""),
(
    "inner-loop-cost",
    "Division and call overhead inside loops",
    "ANSWERS the two per-iteration costs people forget.\n"
    "ACT replace loop-invariant divide with reciprocal multiply.\n"
    "MISLEADS a '/' count includes division by CONSTANT.",
    """SELECT s.name, s.div_in_loop AS divs, s.call_in_loop AS calls,
        s.max_loop_depth AS depth, s.libm_in_loop AS libm, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE (s.div_in_loop>0 OR s.call_in_loop>20) AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.div_in_loop DESC, s.call_in_loop DESC LIMIT :lim"""),
(
    "struct-padding",
    "Byte-accurate layout: bytes lost to alignment holes",
    "ANSWERS which structs waste memory to padding.\n"
    "ACT reorder fields largest-alignment-first.\n"
    "MISLEADS LP64 model, exact=1 rows only.",
    """SELECT s.name AS struct_, ss.total_size AS sz, ss.total_pad AS pad,
        ss.n_lines_64 AS lines, ss.max_align AS algn,
        f.path || ':' || s.line_start AS at
    FROM struct_size ss JOIN symbols s ON s.id=ss.symbol_id
    JOIN files f ON f.id=s.file_id LEFT JOIN modules m ON m.id=s.module_id
    WHERE ss.exact=1 AND ss.total_pad>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY ss.total_pad DESC LIMIT :lim"""),
(
    "cache-line-crossers",
    "Structs just over a 64-byte line",
    "ANSWERS which hot objects need two lines where one would do.\n"
    "ACT 65-96 bytes is the painful band.\n"
    "MISLEADS a struct that is never hot does not care.",
    """SELECT s.name AS struct_, ss.total_size AS sz, ss.total_pad AS pad,
        ss.n_lines_64 AS lines, f.path || ':' || s.line_start AS at
    FROM struct_size ss JOIN symbols s ON s.id=ss.symbol_id
    JOIN files f ON f.id=s.file_id LEFT JOIN modules m ON m.id=s.module_id
    WHERE ss.exact=1 AND ss.total_size BETWEEN 65 AND 128
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY ss.total_size LIMIT :lim"""),
(
    "stack-pressure",
    "Functions with the most locals / pointer locals",
    "ANSWERS which frames are large enough to matter.\n"
    "ACT a big frame in a RECURSIVE function multiplies by depth.\n"
    "MISLEADS counts DECLARATIONS, not simultaneous liveness.",
    """SELECT s.name, s.n_locals AS locals, s.n_ptr_locals AS ptrs,
        s.is_recursive AS rec, s.max_loop_depth AS depth, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_locals DESC LIMIT :lim"""),
(
    "magic-numbers",
    "Unnamed numeric literals, densest first",
    "ANSWERS where a tuning constant is written in digits.\n"
    "ACT name them.\n"
    "MISLEADS 0/1/2/8/16/32/64 are already excluded.",
    """SELECT s.name, s.n_magic AS magic, s.sloc,
        CAST(ROUND(100.0*s.n_magic/MAX(s.sloc,1)) AS INT) AS per100,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_magic>0 AND s.sloc>10 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_magic DESC LIMIT :lim"""),
(
    "error-shape-mix",
    "Functions returning failure in more than one shape",
    "ANSWERS where a caller can plausibly check the wrong thing.\n"
    "ACT a function returning both NULL and -1 is one a caller gets wrong.\n"
    "MISLEADS returning 0 for success AND 0 for a legitimate value is invisible.",
    """SELECT s.name, s.ret_null AS r_null, s.ret_neg AS r_neg, s.ret_zero AS r_0,
        s.ret_val AS r_val, s.fan_in, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    AND ((s.ret_null>0) + (s.ret_neg>0) + (s.ret_zero>0)) >= 2
    ORDER BY s.fan_in DESC LIMIT :lim"""),
(
    "cast-density",
    "Pointer casts: where the type system was overruled",
    "ANSWERS the places a wrong assumption becomes a memory bug.\n"
    "ACT each cast is a claim the compiler cannot check.\n"
    "MISLEADS the regex counts some compound literals as casts.",
    """SELECT s.name, s.n_cast AS casts, s.n_deref AS derefs, s.n_shift AS shifts,
        s.n_mem AS mem, s.n_io AS io, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_cast>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_cast DESC LIMIT :lim"""),
(
    "explicit-simd",
    "Hand-written intrinsics and branch hints",
    "ANSWERS where the code already commits to an ISA.\n"
    "ACT every intrinsic site needs a scalar fallback BUILT in CI.\n"
    "MISLEADS a high count is not a fast function.",
    """SELECT s.name, s.n_intrinsic AS intrin, s.n_likely AS hints,
        s.n_restrict AS restrict_, s.n_builtin AS builtins,
        s.max_loop_depth AS depth, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE (s.n_intrinsic>0 OR s.n_likely>0) AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_intrinsic DESC, s.n_likely DESC LIMIT :lim"""),
(
    "config-gated",
    "Code behind a CONFIG_* flag",
    "ANSWERS which regions a plain build silently omits.\n"
    "ACT build each flag in CI or the code is unrun.\n"
    "MISLEADS lists DIRECTIVES, not region sizes.",
    """SELECT cb.expr, COUNT(*) AS sites, COUNT(DISTINCT cb.file_id) AS files,
        GROUP_CONCAT(DISTINCT f.basename) AS in_files
    FROM config_blocks cb JOIN files f ON f.id=cb.file_id
    WHERE cb.is_config=1
    GROUP BY cb.expr ORDER BY sites DESC LIMIT :lim"""),
(
    "markers",
    "TODO / FIXME / XXX / HACK left in the tree",
    "ANSWERS the debt the authors already flagged.\n"
    "ACT a FIXME next to a hazard is the highest-value review row.\n"
    "MISLEADS a NOTE is usually documentation.",
    """SELECT mk.kind, COUNT(*) AS n,
        GROUP_CONCAT(DISTINCT f.basename) AS files
    FROM markers mk JOIN files f ON f.id=mk.file_id
    GROUP BY mk.kind ORDER BY n DESC LIMIT :lim"""),
(
    "undocumented-complexity",
    "Complex functions with almost no comments",
    "ANSWERS where the next reader has no help and the cost is highest.\n"
    "ACT a comment carries a CONSTRAINT or a non-obvious fact.\n"
    "MISLEADS comment COUNT is not comment quality.",
    """SELECT s.name, s.cyclomatic AS cyclo, s.sloc, s.n_comment_lines AS cmts,
        s.max_nesting AS nest, s.fan_in, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.cyclomatic>=20
    AND s.n_comment_lines*20 < s.sloc AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.cyclomatic DESC LIMIT :lim"""),
(
    "backend-parity",
    "One name, two definitions: which #if-selected backend is the STUB",
    "ANSWERS where a compile-time alternative silently drops a feature.\n"
    "ACT a body a fraction of its sibling's is a stub.\n"
    "MISLEADS a small body can be complete.",
    """SELECT a.name,
        MIN(a.sloc) AS small, MAX(a.sloc) AS large,
        COUNT(*) AS defs,
        GROUP_CONCAT(DISTINCT f2.basename) AS files
    FROM symbols a
    JOIN files f2 ON f2.id=a.file_id
    JOIN modules m ON m.id=a.module_id
    WHERE a.kind='function' AND COALESCE(m.name,'') LIKE :mod
    AND m.kind NOT IN ('test','tool')
    AND a.name NOT IN ('main','LLVMFuzzerTestOneInput','usage')
    GROUP BY a.name
    HAVING COUNT(*)>1 AND MAX(a.sloc) >= 3*MIN(a.sloc)+3
    AND (COUNT(DISTINCT a.file_id)=1 OR MIN(f2.basename) <> MAX(f2.basename))
    ORDER BY (MAX(a.sloc)-MIN(a.sloc)) DESC LIMIT :lim"""),
(
    "untrusted-unfuzzed",
    "Parses attacker bytes and NO fuzz target can reach it",
    "ANSWERS the gap between the frontier and the fuzz corpus.\n"
    "ACT these are where to add a target.\n"
    "MISLEADS reachability is static and blind through function pointers.",
    """WITH RECURSIVE seed(id) AS (
        SELECT s.id FROM symbols s JOIN files f ON f.id=s.file_id
        WHERE f.dir LIKE '%fuzz%' AND s.kind='function'
    ), reach(id) AS (
        SELECT id FROM seed
        UNION
        SELECT e.callee_id FROM edges e JOIN reach r ON r.id=e.caller_id
    )
    SELECT s.name, s.n_io AS io, s.n_mem AS mem, s.n_integer AS int_,
        s.cyclomatic AS cyclo, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.id NOT IN (SELECT id FROM reach)
    AND COALESCE(m.kind,'') NOT IN ('test','tool')
    AND s.n_mem>0 AND (s.n_io>0 OR s.n_integer>=3)
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_io*8 + s.n_mem*4 + s.n_integer) DESC LIMIT :lim"""),
(
    "profiler-invisible",
    "static inline with real fan-in: zero self-time is not zero cost",
    "ANSWERS which functions a sampling profiler CANNOT attribute cost to.\n"
    "ACT never conclude one of these is cold from a flat profile.\n"
    "MISLEADS `static inline` is a request, not a guarantee.",
    """SELECT s.name, s.fan_in, s.sloc, s.cyclomatic AS cyclo,
        s.fan_in*s.sloc AS hidden, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.is_inline=1 AND s.is_static=1
    AND s.fan_in>=3 AND s.sloc>=4 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY hidden DESC LIMIT :lim"""),
(
    "bypass-tax",
    "Allocates BEFORE it knows the fast path applies",
    "ANSWERS candidate BOSCC bypasses that pay setup on refused inputs.\n"
    "ACT probe FIRST, allocate second.\n"
    "MISLEADS an allocation before a loop is usually just an output buffer.",
    """SELECT s.name, s.n_alloc AS allocs, s.n_free AS frees, s.n_loops AS loops,
        s.n_branches AS brs, s.sloc, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.n_alloc>0 AND s.n_free>0
    AND s.n_loops>0 AND s.sloc<=120 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_loops*3 + s.n_branches + s.n_alloc*2) DESC, s.sloc ASC LIMIT :lim"""),
(
    "silent-invariant",
    "Layout the code depends on, with no compile-time assertion",
    "ANSWERS structs whose field ORDER or size is load-bearing.\n"
    "ACT assert anything that fails QUIETLY.\n"
    "MISLEADS most structs are ordinary and need nothing.",
    """SELECT sy.name, ss.total_size AS bytes, ss.n_lines_64 AS lines64,
        (SELECT COUNT(*) FROM fields WHERE symbol_id=sy.id) AS nfields,
        (SELECT COALESCE(SUM(n_static_assert),0) FROM symbols WHERE file_id=sy.file_id) AS asserts,
        ss.total_pad AS pad, ss.tail_pad AS tailpad,
        f.path || ':' || sy.line_start AS at
    FROM symbols sy JOIN struct_size ss ON ss.symbol_id=sy.id
    JOIN files f ON f.id=sy.file_id
    LEFT JOIN modules m ON m.id=sy.module_id
    WHERE sy.kind='struct' AND ss.exact=1 AND ss.total_pad>0
    AND COALESCE(m.kind,'') NOT IN ('test','tool')
    AND COALESCE(m.name,'') LIKE :mod
    GROUP BY sy.id
    HAVING asserts=0 AND nfields>=4
    ORDER BY ss.total_pad DESC, bytes DESC LIMIT :lim"""),
(
    "hand-linked-objects",
    "Build rules that enumerate their objects by hand",
    "ANSWERS which link lines name objects individually.\n"
    "ACT these are the rules a NEW CALL from a shared source silently breaks.\n"
    "MISLEADS a hand-written list is correct until a dependency changes.",
    """SELECT rule, n_objs AS objs, n_srcs AS srcs, line AS at_line, 'Makefile' AS at
    FROM makefile_rules
    WHERE n_objs+n_srcs >= 2
    ORDER BY n_objs+n_srcs DESC LIMIT :lim"""),
]

QUERIES = QUERIES + _Q_EXTRA


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(
        description="Parse the tree into an in-memory graph and query it in one shot.")
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("which", nargs="*", type=int, help="1-based query numbers")
    ap.add_argument("--module", default="%", help="module-name LIKE filter")
    ap.add_argument("--limit", type=int, default=-1,
                    help="rows per query; -1 (default) is every row")
    ap.add_argument("--list", action="store_true", help="list the queries")
    ap.add_argument("--schema", action="store_true")
    ap.add_argument("--report", action="store_true", help="the narrative report")
    ap.add_argument("--sql", help="ad-hoc query, same formatting")
    ap.add_argument("--csv", type=int, metavar="N", help="emit query N as CSV")
    ap.add_argument("--save", metavar="PATH", help="also write the graph to a file")
    ap.add_argument("--quiet", action="store_true", help="suppress progress output")
    a = ap.parse_args()

    if a.schema:
        print(SCHEMA + INDEXES + VIEWS)
        return
    if a.list:
        for i, (name, title, _, _) in enumerate(QUERIES, 1):
            print("%2d. %-22s %s" % (i, name, title))
        return

    t0 = time.time()
    db = sqlite3.connect(":memory:")
    n = build(os.path.abspath(a.root), db, quiet=a.quiet)
    took = time.time() - t0
    p = {"mod": a.module, "lim": a.limit}

    if a.sql:
        cur = db.execute(a.sql)
        render(cur.fetchall(), [d[0] for d in cur.description])
        db.close()
        return
    if a.csv:
        _, _, _, sql = QUERIES[a.csv - 1]
        cur = db.execute(sql, p)
        w = csv.writer(sys.stdout)
        w.writerow([d[0] for d in cur.description])
        w.writerows(cur.fetchall())
        db.close()
        return

    print("codegraph: %d C files parsed into memory in %.1fs module=%s limit=%d"
          % (n, took, a.module, a.limit))
    if a.report:
        report(db)
    sel = a.which or range(1, len(QUERIES) + 1)
    for k in sel:
        if not (1 <= k <= len(QUERIES)):
            continue
        name, title, notes, sql = QUERIES[k - 1]
        print("\n" + "=" * 78)
        print("Q%d. %s -- %s" % (k, name, title))
        print("-" * 78)
        for line in notes.splitlines():
            print(" " + line)
        print()
        cur = db.execute(sql, p)
        render(cur.fetchall(), [d[0] for d in cur.description])

    if a.save:
        dest = sqlite3.connect(a.save)
        db.backup(dest)
        dest.close()
        print("\n(graph also written to %s)" % a.save)
    db.close()


if __name__ == "__main__":
    main()
