#!/usr/bin/env python3
"""The line-level C defect scanner the gate runs (D01..D45).

Lifted VERBATIM out of bench/codegraph.py when the code graph moved to the Go
tool (bench/codegraph/): the scanner never used the graph -- it reads source
text only -- so it stands alone. Nothing below the helpers was changed; the
finding list on the tree is byte-identical to the one codegraph.py printed.

Usage: tools/defects.py [root] [--defects-all] [--defects-baseline PATH]
       [--update-baseline PATH] [--jobs N]     exit 1 = an ERROR finding, or
       a finding absent from the baseline.
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import sys
from itertools import accumulate
from typing import Any, Callable, Optional


FUNC_SCAN = re.compile(r'[{}]|\b([A-Za-z_]\w*)\s*\(')


def split_top_args(text: str) -> list[str]:
    """Split an argument list on commas at paren/bracket depth 0.

    `text.split(",")` is wrong as soon as an argument contains a call, and in
    real code it constantly does: `epoll_wait(set->epoll_fd, set->events,
    Min(nevents, set->nevents_space), timeout)` splits INSIDE `Min(...)`, so
    arg[2] reads `Min(nevents` and the timeout -- which is the LAST argument
    and the one every question here actually asks about -- is never reached.
    """
    out: list[str] = []
    cur: list[str] = []
    depth = 0
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    out.append("".join(cur).strip())
    return out


KEYWORDS: set[str] = {
    "if", "for", "while", "switch", "return", "sizeof", "case", "do", "else",
    "goto", "typedef", "struct", "enum", "union", "static", "const", "void",
    "int", "char", "unsigned", "signed", "long", "short", "float", "double",
    "volatile", "register", "extern", "inline", "restrict", "_Static_assert",
    "defined", "break", "continue", "default", "_Atomic", "_Bool", "alignas",
    "alignof", "__attribute__", "__typeof__", "typeof", "asm", "__asm__",
    "_Generic", "__inline", "__inline__", "__restrict", "__restrict__",
    "__volatile__", "__extension__", "and", "or", "not",
}


class _TextRef:
    __slots__ = ("text", "offs")

    def __init__(self, text: str, offs: list[int]) -> None:
        self.text = text
        self.offs = offs


_NL_CACHE: dict[int, _TextRef] = {}


_NL_CACHE_MAX = 24


def _nl_index(text: str) -> list[int]:
    """Newline offsets for `text`, cached with an identity check.

    Keyed on `id()`, which CPython reuses after a string is collected -- hence
    the `is` test before trusting a hit.
    """
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


_BLANK_TOKEN = re.compile(r'/\*|//|"|\'')


def blank_c(src: str) -> str:
    """Comments and string/char literals -> spaces.

    Newlines are preserved so every byte offset in the result still maps to
    the same line in the original. That invariant is what lets the whole
    rest of this file scan the blanked text and still report exact line
    numbers.
    """
    n = len(src)
    out: list[str] = []
    prev = 0
    pos = 0
    for m in _BLANK_TOKEN.finditer(src):
        if m.start() < pos:
            continue
        start = m.start()
        tok = m.group(0)
        if tok == "/*":
            j = src.find("*/", start + 2)
            end = n if j < 0 else j + 2
        elif tok == "//":
            j = src.find('\n', start)
            end = n if j < 0 else j
        else:
            q = tok
            j = start + 1
            while j < n:
                c = src[j]
                if c == '\\':
                    j += 2
                    continue
                if c == q:
                    j += 1
                    break
                if c == '\n':
                    break
                j += 1
            end = j
        out.append(src[prev:start])
        out.append(''.join('\n' if ch == '\n' else ' '
                           for ch in src[start:end]))
        prev = end
        pos = end
    out.append(src[prev:])
    return ''.join(out)


def find_functions(blank: str, raw: str) -> list[tuple[str, str, int, int, int, int, str, int]]:
    """(name, sig, line_start, line_end, is_static, is_inline, body, body_off).

    A definition is an identifier at brace depth zero, followed by a balanced
    parenthesis group, followed by `{`. That rejects prototypes (they end in
    `;`), calls inside bodies (depth > 0) and most macro invocations, without
    knowing a single thing about types.
    """
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
                    res.append((name, sig, line_of(blank, i), line_of(blank, e),
                                1 if re.search(r'\bstatic\b', sig) else 0,
                                1 if re.search(r'\binline\b', sig) else 0,
                                blank[k:e + 1], k))
                    i = e + 1
                    continue
        i = mm.end()
    return res


DEFECT_SKIP_DIRS = {".git", ".obj", "node_modules", "third_party", "test262",
                    ".swarm", ".prepush", ".agent-work", "pgo-data", "__pycache__",
                    "scratch", "tmp_audit", ".commandcode", ".zcode", "docker", ".ws"}

_ERRCONV_CALL = re.compile(r"\b(JS_To[A-Z]\w*|dyn_path_borrow)\s*\(")


class Defect(object):
    __slots__ = ("check", "sev", "file", "line", "code", "msg")

    def __init__(self, check, sev, file, line, code, msg):
        self.check, self.sev, self.file = check, sev, file
        self.line, self.code, self.msg = line, code, msg

    def key(self):
        h = hashlib.md5(self.code.strip().encode("utf-8",
                                                 "replace")).hexdigest()[:10]
        return "%s|%s|%s" % (self.check, self.file, h)

    def fmt(self, root):
        p = os.path.relpath(self.file, root) if self.file.startswith(root) \
            else self.file
        return "%-5s %-38s %s:%d  %s" % (self.sev, self.check, p,
                                         self.line, self.msg)


DEFECT_META = []


def _check(cid, sev, title):
    def deco(fn):
        DEFECT_META.append((cid, sev, title, fn))
        return fn
    return deco


def _fn_params(sig):
    """Parameter names of a signature, best effort."""
    i = sig.find("(")
    if i < 0:
        return set()
    body = sig[i + 1:sig.rfind(")") if ")" in sig[i:] else len(sig)]
    names = set()
    for tok in body.split(","):
        tok = tok.strip()
        if not tok or tok in ("void", "..."):
            continue
        if "=" in tok:
            tok = tok.split("=")[0].strip()
        parts = tok.replace("*", " * ").split()
        if parts:
            names.add(parts[-1].strip("[]0123456789 "))
    return names


def _scan_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in DEFECT_SKIP_DIRS]
        for fn in filenames:
            if fn.endswith((".c", ".h", ".inc")):
                yield os.path.join(dirpath, fn)


def _gather(root):
    """(path, raw, blank, functions) for every C file, memoized per run."""
    for path in _scan_files(root):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                raw = fh.read()
        except OSError:
            continue
        if len(raw) > 8 * 1024 * 1024:
            continue
        blank = blank_c(raw)
        yield path, raw, blank, find_functions(blank, raw)


_INCLUDE_RE = re.compile(r'#include\s+"([^"]+)"')

_DEFECTS_CACHE_VERSION = 1


def _file_defects(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
    except OSError:
        return None
    if len(raw) > 8 * 1024 * 1024:
        return None
    _LOCALS_CACHE.clear()
    blank = blank_c(raw)
    fns = find_functions(blank, raw)
    out = []
    for (cid, sev, title, fn) in DEFECT_META:
        if cid in ("D18", "D19"):
            continue
        try:
            fn(path, raw, blank, fns, out)
        except Exception as ex:
            out.append(Defect(cid, "error", path, 0,
                              "<scanner>", "check crashed: %r" % ex))
    return _INCLUDE_RE.findall(raw), out


def _map_defects(files, jobs):
    if not files:
        return []
    n = jobs if jobs and jobs > 0 else (os.cpu_count() or 4)
    if n <= 1 or len(files) < 64 or not _MAIN_SCRIPT:
        return [_file_defects(p) for p in files]
    try:
        import multiprocessing
        ctx = multiprocessing.get_context()
        with ctx.Pool(min(n, len(files))) as pool:
            return pool.map(_file_defects, files, chunksize=1)
    except Exception:
        return [_file_defects(p) for p in files]


def _defects_cache_file(root):
    return os.path.join(root, ".obj", "codegraph-defects-cache.json")


def _defects_cache_sig():
    try:
        st = os.stat(__file__)
        return [_DEFECTS_CACHE_VERSION, st.st_size, st.st_mtime_ns,
                sys.version.split()[0]]
    except OSError:
        return [_DEFECTS_CACHE_VERSION, sys.version.split()[0]]


def _load_defects_cache(root):
    if os.environ.get("CODEGRAPH_NO_CACHE"):
        return {}
    try:
        with open(_defects_cache_file(root), "r", encoding="utf-8") as fh:
            blob = json.load(fh)
    except (OSError, ValueError):
        return {}
    if not isinstance(blob, dict) or blob.get("sig") != _defects_cache_sig():
        return {}
    files = blob.get("files")
    return files if isinstance(files, dict) else {}


def _save_defects_cache(root, files):
    if os.environ.get("CODEGRAPH_NO_CACHE"):
        return
    path = _defects_cache_file(root)
    tmp = "%s.tmp.%d" % (path, os.getpid())
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump({"sig": _defects_cache_sig(), "files": files}, fh)
        os.replace(tmp, path)
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def _body_raw(raw, off, body):
    """The UNblanked body text (same offsets: blank_c preserves positions)."""
    return raw[off:off + len(body)]



@_check("D01-c23-octal", "error",
        "C23 0o octal literal (breaks gcc/clang -std=gnu17; audit C16)")
def c23_octal(path, raw, blank, fns, out):
    for m in re.finditer(r"\b0[oO][0-7]+\w*", blank):
        ln = line_of(blank, m.start())
        code = raw[raw.rfind("\n", 0, m.start()) + 1:
                   raw.find("\n", m.end())]
        out.append(Defect("D01", "error", path, ln, code,
                          "0o literal: Apple clang accepts, gcc/Debian-clang "
                          "reject in gnu17; use 0-prefixed octal"))


@_check("D02-banned-fn", "error",
        "unbounded string/temp functions (strcpy, sprintf, gets, tmpnam...)")
def banned_fn(path, raw, blank, fns, out):
    sev = "info" if re.search(r"/tests?/", path) else "error"
    for m in re.finditer(r"\b(strcpy|strcat|sprintf|vsprintf|gets|tmpnam|"
                         r"tempnam)\s*\(", blank):
        ln = line_of(blank, m.start())
        out.append(Defect("D02", sev, path, ln,
                          raw[raw.rfind("\n", 0, m.start()) + 1:
                              raw.find("\n", m.end())],
                          m.group(1) + "(): unbounded; use the bounded form"))


@_check("D03-realloc-self", "warn",
        "p = realloc(p,...) clobbers the only pointer on failure")
def realloc_self(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b(\w+)\s*=\s*realloc\s*\(\s*\1\b", body):
            out.append(Defect(
                "D03", "warn", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(), body[m.start():m.end()]),
                "realloc into the same variable: on failure the block leaks "
                "(allocator shims where the caller keeps its own pointer "
                "are the accepted exception)"))


@_check("D04-ignored-errconv", "warn",
        "error-convention call as a bare statement (JS_To* returns -1; "
        "audit S11)")
def ignored_errconv(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in _ERRCONV_CALL.finditer(body):
            seg = body[m.start():m.start() + 160]
            head = re.match(r"[\w\s(*&,]*\([^;{)]*\)\s*;", seg)
            before = body[max(0, m.start() - 40):m.start()]
            if head and not re.search(r"(=|return|if|while|for)\s*$",
                                      before.strip()):
                out.append(Defect(
                    "D04", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():m.start() + 60]),
                    "return value ignored: -1 leaves a stale out-param under "
                    "a pending exception"))


@_check("D05-snprintf-accum", "warn",
        "x += snprintf(...): accumulates the UNclamped would-write count "
        "(audit: mathx/schema/url)")
def snprintf_accum(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b\w+\s*\+=\s*snprintf\s*\(", body):
            out.append(Defect(
                "D05", "warn", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[m.start():m.start() + 70]),
                "snprintf returns needed-length, not written-length; a "
                "truncating call poisons the accumulator"))


@_check("D06-assign-in-cond", "warn", "assignment inside if(...)")
def assign_in_cond(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\bif\s*\(", body):
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            cond = body[m.end():j - 1]
            for a in re.finditer(r"(?<![=!<>+\-*/%&|^])=(?!=)", cond):
                ctx = cond[max(0, a.start() - 30):a.start() + 30]
                if re.search(r"\b(for|sizeof)\b", ctx):
                    continue
                pre = cond[:a.start()]
                if re.search(r"\(\(\s*[\w.\->\[\]]+\s*$", pre):
                    continue
                out.append(Defect(
                    "D06", "warn", path, line_of(blank, off + m.start()),
                    "if (...) " + ctx.strip(),
                    "single = inside a condition (== typo class)"))


@_check("D07-float-eq", "info",
        "float literal in an ==/!= comparison")
def float_eq(path, raw, blank, fns, out):
    lit = r"(?:\d+\.\d(?:[eE][+-]?\d+)?f?|\d+[eE][+-]?\d+f?|\d+f)"
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"(%s)\s*(==|!=)\s*|%s\s*(==|!=)"
                             % (lit, lit), body):
            out.append(Defect(
                "D07", "info", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[max(0, m.start() - 20):m.start() + 30]),
                "float equality (tolerance compare unless value is exact "
                "by construction)"))


@_check("D08-strlen-repeat", "warn",
        "strlen(s) computed 2+ times in one function (repo-fixed pattern)")
def strlen_repeat(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        seen = {}
        for m in re.finditer(r"strlen\s*\(\s*(\w+)\s*\)", body):
            seen.setdefault(m.group(1), []).append(m.start())
        for v, poss in seen.items():
            if len(poss) >= 2:
                out.append(Defect(
                    "D08", "warn", path, line_of(blank, off + poss[0]),
                    "strlen(%s) x%d in %s" % (v, len(poss), name),
                    "hoist the length: every repeat rescans the string"))


@_check("D09-memset-dce", "warn",
        "memset() on a buffer with no later read (DCE risk; audit S10)")
def memset_dce(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"memset\s*\(\s*&?\s*(\w+)\b", body):
            v = m.group(1)
            ce, dd = m.end(), 0
            while ce < len(body):
                if body[ce] == "(":
                    dd += 1
                elif body[ce] == ")":
                    if dd == 0:
                        break
                    dd -= 1
                ce += 1
            rest = body[ce + 1:]
            uses = [u.start() for u in re.finditer(r"\b%s\b" % v, rest)]
            later_read = [u for u in uses
                          if not re.match(r"\s*\)?\s*;", rest[u:])
                          and not rest[max(0, u - 7):u + len(v) + 8]
                          .startswith("free")]
            only_free = uses and all(
                re.match(r"\s*\)", rest[u:]) or
                re.search(r"free\s*\(\s*%s" % v, rest[max(0, u - 6):u + 20])
                for u in uses)
            if not uses or only_free:
                out.append(Defect(
                    "D09", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():m.start() + 50]),
                    "memset with no later read: the optimizer may delete it "
                    "(key material must use explicit_bzero/memset_s)"))


@_check("D10-sizeof-param", "warn",
        "sizeof(pointer-parameter) as a size argument")
def sizeof_param(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        params = _fn_params(sig)
        if not params:
            continue
        for m in re.finditer(r"\b(memcpy|memmove|memset|strncpy|snprintf)"
                             r"\s*\(", body):
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            call = body[m.start():j]
            for s in re.finditer(r"sizeof\s*\(\s*(\w+)\s*\)", call):
                if s.group(1) in params:
                    out.append(Defect(
                        "D10", "warn", path, line_of(blank, off + m.start()),
                        call[:90],
                        "sizeof(%s) where %s is a parameter: sizeof(ptr)==8, "
                        "not the buffer size" % (s.group(1), s.group(1))))


@_check("D11-lock-balance", "warn",
        "pthread lock/unlock counts differ in one function")
def lock_balance(path, raw, blank, fns, out):
    pairs = [("pthread_mutex_lock", "pthread_mutex_unlock"),
             ("pthread_rwlock_rdlock", "pthread_rwlock_unlock"),
             ("pthread_rwlock_wrlock", "pthread_rwlock_unlock")]
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for lk, ul in pairs:
            nl = len(re.findall(r"\b%s\s*\(" % lk, body))
            nu = len(re.findall(r"\b%s\s*\(" % ul, body))
            if nl != nu:
                out.append(Defect(
                    "D11", "warn", path, l0,
                    "%s: %d %s vs %d %s" % (name, nl, lk, nu, ul),
                    "lock/unlock imbalance in one function (early returns "
                    "need cleanup labels)"))


@_check("D12-js-reentry", "warn",
        "dyn_res_get/dyn_plain_get BEFORE a JS-invoking call (violates the "
        "coerce-then-resolve doctrine)")
def js_reentry(path, raw, blank, fns, out):
    jsrun = re.compile(r"\b(JS_GetPropertyStr|JS_GetPropertyUint32|"
                       r"JS_ToCString|JS_Call|JS_GetProperty)\s*\(")
    get = re.compile(r"\b(dyn_res_get|dyn_plain_get|dyn_res_native)\s*\(")
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        g = get.search(body)
        if not g:
            continue
        j = jsrun.search(body, g.end())
        if j:
            out.append(Defect(
                "D12", "warn", path, line_of(blank, off + j.start()),
                _body_raw(raw, off + j.start(),
                          body[j.start():j.start() + 60]),
                "%s resolves the native handle then runs user JS; user JS "
                "can close()/dispose the resource mid-call -- coerce args "
                "FIRST (dyna-nat.h contract)" % name))


@_check("D13-blocking-io", "warn",
        "blocking filesystem call inside the reactor core (aio/evloop)")
def blocking_io(path, raw, blank, fns, out):
    if not re.search(r"dyna-(aio|evloop)", path):
        return
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b(fopen|fsync|fdatasync|opendir|unlink|"
                             r"rename|mkdir|remove)\s*\(", body):
            out.append(Defect(
                "D13", "warn", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[m.start():m.start() + 50]),
                "%s: blocking fs call in the reactor core stalls every "
                "connection -- offload to the disk pool" % m.group(1)))


@_check("D14-loop-counter-write", "info",
        "for-loop counter assigned inside the body")
def loop_counter_write(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\bfor\s*\(\s*(?:\w+\s+)?(\w+)\s*=", body):
            v = m.group(1)
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            while j < len(body) and body[j] in " \t\r\n":
                j += 1
            if j >= len(body) or body[j] != "{":
                continue
            e, bd = j, 0
            while e < len(body):
                if body[e] == "{":
                    bd += 1
                elif body[e] == "}":
                    bd -= 1
                    if bd == 0:
                        break
                e += 1
            inner = body[j:e]
            for w in re.finditer(r"\b%s\s*(?:\+\+|--|=[^=])" % v, inner):
                out.append(Defect(
                    "D14", "info", path, line_of(blank, off + w.start()),
                    _body_raw(raw, off + w.start(),
                              inner[w.start():w.start() + 40]),
                    "loop counter '%s' modified in body" % v))
                break


@_check("D15-js-value-balance", "info",
        "JS get/free counts far apart in one function (leak smell)")
def js_value_balance(path, raw, blank, fns, out):
    get = (len(re.findall(r"\bJS_GetProperty(?:Str|Uint32)?\s*\(", body))
           + len(re.findall(r"\bJS_ToCString(?:Len)?\s*\(", body))
           for (name, sig, l0, l1, st, inl, body, off) in fns)
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        g = (len(re.findall(r"\bJS_GetProperty(?:Str|Uint32)?\s*\(", body))
             + len(re.findall(r"\bJS_ToCString(?:Len)?\s*\(", body)))
        f = (len(re.findall(r"\bJS_FreeValue(?:RT)?\s*\(", body))
             + len(re.findall(r"\bJS_FreeCString\s*\(", body)))
        if g - f >= 4:
            out.append(Defect(
                "D15", "info", path, l0,
                "%s: %d gets vs %d frees" % (name, g, f),
                "owned-JSValue surplus in one function -- early-return paths "
                "are the usual leak; verify each goto/err label frees"))


@_check("D16-narrow-cmp", "info",
        "32-bit cast on one side of a size comparison (64->32 boundary)")
def narrow_cmp(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\((?:int|int32_t|uint32_t|unsigned)\)"
                             r"\s*\w+\s*(?:<=|>=|<|>)", body):
            out.append(Defect(
                "D16", "info", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[m.start():m.start() + 50]),
                "narrowed operand in a size comparison: values past 2^31 "
                "compare wrong (timestamps, offsets)"))


@_check("D17-comment-marker", "warn",
        "line comment containing block-comment markers")
def comment_marker(path, raw, blank, fns, out):
    for i, ln in enumerate(raw.split("\n")):
        if "//" in ln and ("*/" in ln.split("//", 1)[1]
                           or "/*" in ln.split("//", 1)[1]):
            out.append(Defect(
                "D17", "warn", path, i + 1, ln.strip()[:90],
                "block-comment marker inside a // comment -- often a "
                "half-converted docstring"))



@_check("D18-inc-include", "error",
        "*.inc.c must be #included exactly once project-wide")
def inc_include(path, raw, blank, fns, out, incmap=None):
    return


@_check("D19-build-coverage", "error",
        "src/*.c not referenced by the build system, build.sh (agent added a "
        "file, forgot the build)")
def makefile_coverage(path, raw, blank, fns, out, mk_text="", src_files=()):
    for sf in src_files:
        stem = os.path.basename(sf)[:-2]
        if not re.search(r"\b%s\b" % re.escape(stem), mk_text):
            out.append(Defect(
                "D19", "error", sf, 1, "",
                "source file absent from build.sh -- it will silently "
                "not be built"))


@_check("D20-error-grammar", "info",
        "JS_Throw literal without the '<module>.<ctx>:' prefix grammar")
def error_grammar(path, raw, blank, fns, out):
    if not re.search(r"dyna-[a-z0-9-]+\.(c|inc\.c)$", path.replace(os.sep, "/")):
        return
    for m in re.finditer(r"JS_Throw\w+\s*\(\s*\w+\s*,\s*\"([^\"]{12,})\"",
                         raw):
        s = m.group(1)
        if "%" in s:
            continue
        if not re.match(r"^[A-Za-z0-9_ ./<>-]{2,32}:", s):
            ln = line_of(blank, m.start())
            out.append(Defect(
                "D20", "info", path, ln, s[:80],
                "literal throw without '<module>.<context>:' prefix "
                "(docs/ERRORS.md grammar)"))


def _project_checks(root, out, includes_of, incs):
    by_base = {}
    for path, incs_list in includes_of.items():
        for inc in incs_list:
            by_base.setdefault(os.path.basename(inc), []).append(path)
    for base, ipath in incs.items():
        users = by_base.get(base, [])
        if len(users) == 0:
            out.append(Defect("D18", "error", ipath, 1, "",
                              ".inc.c fragment included by NOTHING (dead "
                              "code or a forgotten #include)"))
        elif len(users) > 1:
            try:
                with open(ipath, "r", errors="replace") as fh:
                    frag = fh.read()
                fns = find_functions(blank_c(frag), frag)
                nonstatic = [n for (n, sig, _l0, _l1, st, _inl, _b, _o)
                             in fns if not st]
            except OSError:
                nonstatic = ["?"]
            sev = "error" if nonstatic else "info"
            out.append(Defect("D18", sev, ipath, 1, "",
                              ".inc.c fragment included by %d files %s "
                              "(%s)" % (len(users), users,
                                        "non-static symbols: duplicate at "
                                        "link" if nonstatic else
                                        "all-static: legal duplication, "
                                        "two copies to keep in sync")))
    # the build manifest: build.sh (the Makefile was eliminated) is the system
    # that names every source the build compiles; a Makefile, if one reappears,
    # is honored too
    for mk_name in ("build.sh", "Makefile"):
        mk = os.path.join(root, mk_name)
        if os.path.exists(mk):
            break
    else:
        mk = None
    if mk is not None:
        with open(mk, "r", encoding="utf-8", errors="replace") as fh:
            mk_text = fh.read()
        src_files = [p for p in _scan_files(root)
                     if ("/src/" in p.replace(os.sep, "/"))
                     and p.endswith(".c")
                     and not p.endswith(".inc.c")]
        makefile_coverage(root, "", "", [], out, mk_text=mk_text,
                          src_files=src_files)


def run_defects(root, args):
    root = os.path.abspath(root)
    if not os.path.isdir(root):
        print("defect scanner: not a directory: %s" % root,
              file=sys.stderr)
        return 2
    jobs = getattr(args, "jobs", 0) or 0
    files = list(_scan_files(root))
    cached = _load_defects_cache(root)
    live = {}
    todo = []
    prestats = {}
    for p in files:
        try:
            st = os.stat(p)
        except OSError:
            continue
        ent = cached.get(p)
        hit = False
        if ent is not None:
            try:
                if ent[0] == st.st_size and ent[1] == st.st_mtime_ns \
                        and os.access(p, os.R_OK):
                    live[p] = (ent[2], [Defect(*d) for d in ent[3]])
                    hit = True
            except Exception:
                hit = False
        if not hit:
            todo.append(p)
            prestats[p] = (st.st_size, st.st_mtime_ns)
    for p, res in zip(todo, _map_defects(todo, jobs)):
        if res is not None:
            live[p] = res

    all_defects = []
    includes_of = {}
    incs = {}
    new_entries = {}
    for p in files:
        res = live.get(p)
        if res is None:
            continue
        inc, out = res
        includes_of[p] = inc
        if p.endswith(".inc.c"):
            incs[os.path.basename(p)] = p
        all_defects.extend(out)
        if p in prestats:
            new_entries[p] = [prestats[p][0], prestats[p][1], inc,
                              [[d.check, d.sev, d.file, d.line,
                                d.code, d.msg] for d in out]]
    merged = dict(cached)
    merged.update(new_entries)
    _save_defects_cache(root, merged)
    _project_checks(root, all_defects, includes_of, incs)

    stats = {}
    order = {"error": 0, "warn": 1, "info": 2}
    all_defects.sort(key=lambda d: (order[d.sev], d.check, d.file, d.line))
    for d in all_defects:
        stats[d.check] = stats.get(d.check, 0) + 1

    print("defect scanner: %d findings (%d error, %d warn, %d info)"
          % (len(all_defects),
             sum(1 for d in all_defects if d.sev == "error"),
             sum(1 for d in all_defects if d.sev == "warn"),
             sum(1 for d in all_defects if d.sev == "info")))
    for (cid, sev, title, fn) in DEFECT_META:
        n = stats.get(cid.split("-")[0], 0)
        if n:
            print("  %-34s %-5s %4d  %s" % (cid, sev, n, title))
    show = (os.environ.get("DEFECT_VERBOSE", "")
            or ("1" if getattr(args, "defects_all", False) else ""))
    for d in all_defects:
        if show in ("1", "all") or d.sev == "error":
            print(d.fmt(root))

    if getattr(args, "update_baseline", None):
        keys = sorted(set(d.key() for d in all_defects))
        with open(args.update_baseline, "w") as fh:
            json.dump({"keys": keys,
                       "counts": {k: stats.get(k, 0) for k in stats}},
                      fh, indent=1, sort_keys=True)
        print("baseline written: %d keys -> %s" % (len(keys),
                                                   args.update_baseline))
        return 0
    bl = getattr(args, "defects_baseline", None)
    if bl and os.path.exists(bl):
        try:
            with open(bl, "r") as fh:
                base = set(json.load(fh).get("keys", []))
        except (ValueError, OSError) as ex:
            print("defect scanner: unreadable baseline %s: %r"
                  % (bl, ex), file=sys.stderr)
            return 2
        new = [d for d in all_defects if d.key() not in base]
        for d in new:
            print("NEW " + d.fmt(root))
        print("baseline: %d known, %d new" % (len(base), len(new)))
        if new:
            return 1
    n_err = sum(1 for d in all_defects if d.sev == "error")
    return 1 if n_err else 0






_ALLOC_FNS = (r"js_mallocz?|malloc|calloc")
_FREE_OF = {"free": "free", "close": "close", "fclose": "fclose",
            "closedir": "closedir"}

_LOCALS_CACHE: dict[int, tuple[str, dict]] = {}
_LOCALS_CACHE_MAX = 4096


def _locals(body):
    """name -> {'array': N} for locals with a constant array size.

    Powers the free-of-stack / simd-store / stack-escape checks. Only
    CONSTANT sizes are captured -- a VLA or a runtime size is invisible to
    this scanner, which is why every consumer treats absence as 'unknown'
    rather than 'not an array'.
    """
    key = id(body)
    ent = _LOCALS_CACHE.get(key)
    if ent is not None and ent[0] is body:
        return ent[1]
    out = {}
    _ELEM = {"char": 1, "int8": 1, "uint8": 1, "int16": 2, "uint16": 2,
             "short": 2, "int": 4, "uint32": 4, "int32": 4, "float": 4,
             "long": 8, "int64": 8, "uint64": 8, "double": 8}
    for m in re.finditer(
            r"\b(?:static\s+|const\s+|unsigned\s+|struct\s+\w+\s+)*"
            r"(\w[\w]*)\s+(\w+)\s*\[\s*(\d+)\s*\]", body):
        t, v, n = m.group(1), m.group(2), int(m.group(3))
        if t in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        es = 0
        for k, b in _ELEM.items():
            if k in t:
                es = b
                break
        out.setdefault(v, {"array": n, "elem": es})
    for m in re.finditer(
            r"\b(?:static\s+|const\s+|unsigned\s+|register\s+)*"
            r"(?:void|int|char|long|short|float|double|unsigned|signed|"
            r"size_t|ssize_t|uint\d+_t|int\d+_t|JSValue|JSValueConst|"
            r"\w+_t)\s+\*?\s*(\w+)\s*(?:=[^=]|;)", body):
        v = m.group(1)
        if v not in ("if", "for", "while", "switch", "return", "sizeof"):
            out.setdefault(v, {"scalar": True})
    if len(_LOCALS_CACHE) >= _LOCALS_CACHE_MAX:
        _LOCALS_CACHE.clear()
    _LOCALS_CACHE[key] = (body, out)
    return out


def _bare_statement(body, fname_re):
    """Calls whose result is dropped: `fn(...);` with no `x =` / `if` wrap."""
    hits = []
    for m in re.finditer(fname_re, body):
        seg = body[m.start():m.start() + 200]
        before = body[max(0, m.start() - 40):m.start()].strip()
        if re.search(r"(=|return|if|while|for|&&|\|\||,)\s*$|=\s*$", before):
            continue
        d, j = 0, m.end() - 1
        if j >= 0 and body[j] == "(":
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            k = j
            while k < len(body) and body[k] in " \t\r\n":
                k += 1
            if k < len(body) and body[k] == ";":
                hits.append(m)
    return hits


@_check("D21-alloc-arith-overflow", "warn",
        "malloc(a * b): allocation size is unchecked arithmetic (CWE-190)")
def alloc_arith(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b(%s)\s*\(" % _ALLOC_FNS, body):
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            arg = body[m.end():j - 1]
            a1 = arg.split(",")[0].strip()
            for expr in (a1, arg if "," in arg and "calloc" in m.group(0)
                         else a1):
                if (re.search(r"(?<!sizeof\s\()\b\w+\s*\*\s*\w+",
                              expr)
                        and "sizeof" not in expr
                        and "SIZE_MAX" not in arg
                        and "size_max" not in arg
                        and not re.match(r"sizeof", expr.strip())
                        and not re.fullmatch(r"[\w\s.\->]+\*", expr.strip())):
                    out.append(Defect(
                        "D21", "warn", path, line_of(blank, off + m.start()),
                        _body_raw(raw, off + m.start(),
                                  body[m.start():m.start() + 70]),
                        "allocation size is a product of two values with no "
                        "overflow guard on the statement"))


@_check("D22-sizeof-mismatch-copy", "warn",
        "memcpy/memmove size uses sizeof(X) where X is not either operand")
def sizeof_mismatch(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b(memcpy|memmove|strcpy)\s*\(", body):
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            args = split_top_args(body[m.end():j - 1])
            if len(args) < 3:
                continue
            roots = set()
            for a in args[:2]:
                mm = re.search(r"(\w+)\s*(?:\[[^\]]*\])?\s*$",
                               a.replace(")", "").strip())
                if mm:
                    roots.add(mm.group(1))
            for s in re.finditer(r"sizeof\s*\(\s*(\w+)\s*\)", args[2]):
                if s.group(1) not in roots and s.group(1) != "struct":
                    out.append(Defect(
                        "D22", "warn", path,
                        line_of(blank, off + m.start()),
                        _body_raw(raw, off + m.start(),
                                  body[m.start():m.start() + 70]),
                        "size arg is sizeof(%s) but neither operand is %s "
                        "-- copying the wrong object's size"
                        % (s.group(1), s.group(1))))


@_check("D23-use-after-realloc", "warn",
        "old pointer used after q = realloc(p, ...)")
def use_after_realloc(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b(\w+)\s*=\s*realloc\s*\(\s*(\w+)\b",
                             body):
            newv, oldv = m.group(1), m.group(2)
            if newv == oldv:
                continue
            for u in re.finditer(r"\b%s\b" % oldv, body[m.end():]):
                ctx = body[m.end() + u.start():m.end() + u.start() + 3]
                if ctx.startswith(")") or ctx.startswith(";"):
                    continue
                out.append(Defect(
                    "D23", "warn", path,
                    line_of(blank, off + m.end() + u.start()),
                    oldv + " used after realloc moved it to " + newv,
                    "the old block may be freed; use the new pointer"))
                break


@_check("D24-double-dispose", "warn",
        "free/close/fclose of the same resource twice with no reassignment")
def double_dispose(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for fn in ("free", "close", "fclose"):
            poss = [m for m in re.finditer(
                r"\b%s\s*\(\s*(\w+)\s*\)" % fn, body)]
            seen = {}
            for m in poss:
                v = m.group(1)
                if v in seen:
                    between = body[seen[v].end():m.start()]
                    if not re.search(r"[}]|\belse\b|\bcase\b|\bgoto\b|"
                                     r"\breturn\b|^\s*\w+:\s*$|"
                                     r"\b%s\s*=" % v, between, re.M):
                        out.append(Defect(
                            "D24", "warn", path,
                            line_of(blank, off + m.start()),
                            "%s(%s) again; no reassignment between"
                            % (fn, v),
                            "double %s" % fn))
                seen[v] = m


@_check("D25-free-of-stack", "error",
        "free()/close() of a local array or &local (stack memory)")
def free_of_stack(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        locs = _locals(body)
        for m in re.finditer(r"\bfree\s*\(\s*(?:\(\s*\w[\w\s]*\*\s*\)\s*)?"
                             r"(\w+)\s*\)", body):
            v = m.group(1)
            if v in locs and "array" in locs[v]:
                if re.search(r"\b%s\s*=[^=]" % v, body):
                    continue
                out.append(Defect(
                    "D25", "error", path, line_of(blank, off + m.start()),
                    "free(%s)" % v,
                    "%s is a local array -- freeing stack memory" % v))
        for m in re.finditer(r"\b(?:free|close)\s*\(\s*&\s*(\w+)", body):
            out.append(Defect(
                "D25", "error", path, line_of(blank, off + m.start()),
                "free/&%s" % m.group(1),
                "free() of the ADDRESS of a local -- stack memory"))


@_check("D26-stack-addr-returned", "error",
        "return &local (dangling pointer)")
def stack_addr_returned(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        locs = _locals(body)
        for m in re.finditer(r"\breturn\s*&\s*(\w+)\s*;", body):
            if m.group(1) in locs:
                out.append(Defect(
                    "D26", "error", path, line_of(blank, off + m.start()),
                    "return &" + m.group(1),
                    "address of a local outlives the frame"))


@_check("D27-stack-addr-stored", "warn",
        "address of a local stored into a pointer/global/struct field")
def stack_addr_stored(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        locs = _locals(body)
        for m in re.finditer(r"(\w+(?:\s*->\s*\w+)*|\*\w+)\s*=\s*&"
                             r"\s*(\w+)\s*;", body):
            if m.group(2) in locs and m.group(1) not in locs \
                    and locs[m.group(2)].get("array", 1):
                out.append(Defect(
                    "D27", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():m.start() + 50]),
                    "&%s (a local) escapes into %s" % (m.group(2),
                                                       m.group(1))))


@_check("D28-deref-before-nullcheck", "warn",
        "allocation result dereferenced before any NULL test")
def deref_before_nullcheck(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\b(\w+)\s*=\s*(?:%s)\s*\(" % _ALLOC_FNS,
                             body):
            v = m.group(1)
            rest = body[m.end():m.end() + 300]
            deref = re.search(r"\b%s\s*(?:\[|->|\.)" % v, rest)
            if not deref:
                continue
            pre = rest[:deref.start()]
            if re.search(r"\bif\s*\([^)]*\b%s\b" % v, pre) or \
               re.search(r"!\s*%s\b" % v, pre) or \
               re.search(r"\b%s\s*==" % v, pre):
                continue
            out.append(Defect(
                "D28", "warn", path,
                line_of(blank, off + m.end() + deref.start()),
                v + re.search(r"\b%s\s*(?:\[|->|\.)" % v, rest).group(0),
                "%s is dereferenced before any NULL check" % v))


_RESOURCE_LEAK = re.compile(
    r"\b(\w+)\s*=\s*(malloc|calloc|realloc|js_malloc|js_mallocz|"
    r"socket|accept4?|fopen|opendir|open)\s*\(")


@_check("D29-resource-no-dispose", "info",
        "acquired resource never released/returned in its function "
        "(leak smell; cleanup labels make this noisy -- review only)")
def resource_no_dispose(path, raw, blank, fns, out):
    release = {"malloc": "free", "calloc": "free", "realloc": "free",
               "js_malloc": "js_free", "js_mallocz": "js_free",
               "socket": "close", "accept": "close", "accept4": "close",
               "fopen": "fclose", "opendir": "closedir", "open": "close"}
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in _RESOURCE_LEAK.finditer(body):
            v, fn = m.group(1), m.group(2)
            rel = release[fn]
            if re.search(r"\b%s\s*\(\s*%s\b" % (rel, v), body):
                continue
            if re.search(r"\breturn\s[^;]*\b%s\b" % v, body):
                continue
            if re.search(r"->\s*\w+\s*=\s*%s\b" % v, body):
                continue
            out.append(Defect(
                "D29", "info", path, line_of(blank, off + m.start()),
                "%s = %s(...)" % (v, fn),
                "no %s(%s) and no ownership transfer visible in %s"
                % (rel, v, name)))


@_check("D30-loop-le-index", "warn",
        "for (i = 0; i <= bound; ...) with i used as an index "
        "(off-by-one; the '<' form is the norm)")
def loop_le_index(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\bfor\s*\(\s*\w+\s*=\s*0\s*;\s*(\w+)\s*<="
                             r"\s*([\w.\->]+)", body):
            v, bound = m.group(1), m.group(2)
            inner = body[m.end():m.end() + 600]
            if re.search(r"\[\s*%s\s*\]" % v, inner) or \
               re.search(r"\[\s*%s\s*\]" % bound.replace(".", r"\."),
                         inner):
                out.append(Defect(
                    "D30", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():m.start() + 50]),
                    "'<=' over a length-like bound with %s as index -- "
                    "one past the end" % v))


@_check("D31-memcpy-self-overlap", "warn",
        "memcpy between two expressions of the same buffer (needs memmove)")
def memcpy_self_overlap(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\bmemcpy\s*\(", body):
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            args = split_top_args(body[m.end():j - 1])
            if len(args) < 2:
                continue
            roots = []
            for a in args[:2]:
                mm = re.search(r"\b(\w+)\b", a.strip())
                if mm:
                    roots.append(mm.group(1))
            if len(roots) == 2 and roots[0] == roots[1]:
                out.append(Defect(
                    "D31", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():m.start() + 60]),
                    "memcpy with both operands rooted at '%s': overlapping "
                    "copy must be memmove" % roots[0]))


@_check("D32-strlen-in-loop-cond", "warn",
        "strlen() in a loop condition: O(n^2) rescans")
def strlen_in_loop_cond(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\bfor\s*\(", body):
            d, j = 1, m.end()
            while j < len(body) and d:
                if body[j] == "(":
                    d += 1
                elif body[j] == ")":
                    d -= 1
                j += 1
            if re.search(r"strlen\s*\(", body[m.start():j]):
                out.append(Defect(
                    "D32", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():j][:70]),
                    "strlen re-evaluated every iteration -- hoist it"))


_IO_WARN = re.compile(r"\b(recv|recvfrom|send|sendto|read|write|fread|"
                      r"fwrite|fgets)\s*\(")
_IO_INFO = re.compile(r"\b(connect|bind|listen|setsockopt|pthread_create)\s*\(")


@_check("D33-partial-io-ignored", "warn",
        "IO call as a bare statement: partial read/write/recv goes undetected")
def partial_io_ignored(path, raw, blank, fns, out):
    sev = "info" if re.search(r"/tests?/", path) else "warn"
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in _bare_statement(body, _IO_WARN):
            out.append(Defect(
                "D33", sev, path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[m.start():m.start() + 50]),
                "return value dropped: short reads/writes and EOF are "
                "silent"))
        for m in _bare_statement(body, _IO_INFO):
            out.append(Defect(
                "D33", "info", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[m.start():m.start() + 50]),
                "failure of this call is ignored (bare statement)"))


@_check("D35-port-no-htons", "warn",
        "sin_port assigned without htons/htobe16 (endianness trap)")
def port_no_htons(path, raw, blank, fns, out):
    if re.search(r"/tests?/", path):
        return
    for m in re.finditer(r"\bsin_port\s*=\s*([^;]{1,60});", blank):
        rhs = m.group(1)
        if "htons" in rhs or "htobe" in rhs or "constant" in rhs:
            continue
        if re.fullmatch(r"0|0x[0-9a-fA-F]+|\d+", rhs.strip()):
            out.append(Defect(
                "D35", "warn", path, line_of(blank, m.start()),
                "sin_port = " + rhs.strip(),
                "no htons(): network byte order required (a literal 0 is "
                "the only safe unconverted value)"))


@_check("D37-signal-handler-unsafe", "warn",
        "signal handler calling non-async-signal-safe functions")
def signal_handler_unsafe(path, raw, blank, fns, out):
    unsafe = re.compile(r"\b(malloc|calloc|free|printf|fprintf|fopen|"
                        r"pthread_mutex_\w+)\s*\(")
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        if not (re.search(r"sig", name) and re.search(r"handler", name)):
            continue
        for m in unsafe.finditer(body):
            out.append(Defect(
                "D37", "warn", path, line_of(blank, off + m.start()),
                "%s() inside %s" % (m.group(1), name),
                "not async-signal-safe (man 7 signal-safety)"))


_SIMD_CAST = re.compile(
    r"\(\s*(__m128[a-z]*|__m256[a-z]*|__m512[a-z]*|int8x16_t|int16x8_t|"
    r"int32x4_t|int64x2_t|float32x4_t)\s*\*\s*\)")


@_check("D38-simd-cast-align", "warn",
        "SIMD pointer cast + deref: alignment UB unless loadu/storeu/memcpy")
def simd_cast_align(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in _SIMD_CAST.finditer(body):
            tail = body[m.end():m.end() + 60]
            if re.match(r"\s*\w+\s*=", tail):
                continue
            window = body[max(0, m.start() - 48):m.end() + 48]
            if re.search(r"_mm\d*_\w*(loadu|storeu)\w*"
                         r"|vld1q|memcpy|\bloadu\b|\bstoreu\b", window):
                continue
            used = re.match(r"\s*\*\s*\w+", tail) or \
                re.match(r"\s*\w+\s*(?:\[|\)|,|;)", tail)
            if used:
                out.append(Defect(
                    "D38", "warn", path, line_of(blank, off + m.start()),
                    _body_raw(raw, off + m.start(),
                              body[m.start():m.end() + 40]),
                    "deref through a SIMD pointer cast: unless the base is "
                    "declared aligned this is UB -- use the unaligned "
                    "load/store intrinsics"))


@_check("D39-simd-store-oob", "error",
        "SIMD store into a local array smaller than the vector")
def simd_store_oob(path, raw, blank, fns, out):
    lane = {"128": 16, "256": 32, "512": 64}
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        locs = _locals(body)
        for m in re.finditer(r"_mm(\d*)_store[u]?\w*\s*\(\s*&?\s*(\w+)",
                             body):
            w = lane.get(m.group(1) or "128", 16)
            v = m.group(2)
            info = locs.get(v)
            nb = info["array"] * info.get("elem", 0) if info else 0
            if info and info.get("elem") and nb < w:
                out.append(Defect(
                    "D39", "error", path,
                    line_of(blank, off + m.start()),
                    "_mm%s_store...(%s[%d])" % (m.group(1) or "128", v,
                                                info["array"]),
                    "%d-byte vector stored into %s[%d] (%d bytes)"
                    % (w, v, info["array"], nb)))


@_check("D40-strncpy-unterminated", "warn",
        "strncpy with no visible NUL termination afterwards")
def strncpy_unterminated(path, raw, blank, fns, out):
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\bstrncpy\s*\(\s*(\w+)", body):
            v = m.group(1)
            after = body[m.end():]
            if re.search(r"\b%s\s*\[[^\]]*\]\s*=\s*0" % v, after) or \
               re.search(r"memset\s*\(\s*%s\b[^)]*,\s*0" % v, body) or \
               re.search(r"sizeof\s*\(\s*%s\s*\)\s*-\s*1" % v, after):
                continue
            out.append(Defect(
                "D40", "warn", path, line_of(blank, off + m.start()),
                "strncpy(" + v + ", ...)",
                "no visible termination of %s after strncpy -- a source "
                "longer than the buffer leaves it unterminated" % v))


@_check("D41-sscanf-unbounded-s", "warn",
        "sscanf %s without a field width (unbounded write)")
def sscanf_unbounded(path, raw, blank, fns, out):
    for m in re.finditer(r"\bsscanf\s*\([^;]*?\"([^\"]*)\"", raw):
        fmt = m.group(1)
        if re.search(r"%\d+s", fmt):
            continue
        if "%s" in fmt:
            ln = line_of(blank, m.start())
            out.append(Defect(
                "D41", "warn", path, ln,
                fmt[:60],
                "sscanf %%s without a width: unbounded copy into the "
                "target buffer (use %63s-style bounds)"))


@_check("D42-atoi-untrusted", "info",
        "atoi/atol family: no error detection (use strtol)")
def atoi_family(path, raw, blank, fns, out):
    if not re.search(r"dyna-", os.path.basename(path)):
        return
    for m in re.finditer(r"\b(atoi|atol|atoll|atof)\s*\(", blank):
        out.append(Defect(
            "D42", "info", path, line_of(blank, m.start()),
            _body_raw(raw, m.start(), blank[m.start():m.start() + 30]),
            m.group(1) + "(): silent garbage on overflow/non-numeric; "
            "strtol reports both"))


@_check("D43-strtok-not-r", "warn",
        "strtok() is not reentrant (use strtok_r)")
def strtok_not_r(path, raw, blank, fns, out):
    for m in re.finditer(r"\bstrtok\s*\(", blank):
        out.append(Defect(
            "D43", "warn", path, line_of(blank, m.start()),
            "strtok(...)",
            "hidden global state; strtok_r is the reentrant form"))


@_check("D45-int-cast-of-double", "warn",
        "(int) cast of a raw floating computation (UB past INT_MAX)")
def int_cast_of_double(path, raw, blank, fns, out):
    dbl = r"(?:sqrt|pow|log|log2|log10|exp|cbrt|hypot|atof)"
    for (name, sig, l0, l1, st, inl, body, off) in fns:
        for m in re.finditer(r"\(\s*(?:int|int32_t|uint32_t)\s*\)\s*"
                             r"\(?\s*(?:%s)\s*\(" % dbl, body):
            out.append(Defect(
                "D45", "warn", path, line_of(blank, off + m.start()),
                _body_raw(raw, off + m.start(),
                          body[m.start():m.start() + 50]),
                "(int) of an unclamped double: out-of-range conversion is "
                "UB -- wrap in floor()/lround() with a range test"))


_mf = getattr(sys.modules.get("__main__"), "__file__", None)
_MAIN_SCRIPT = bool(_mf) and os.path.abspath(_mf) == os.path.abspath(__file__)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="defects.py", description=__doc__.split("\n")[0])
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("--defects", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--defects-baseline", metavar="PATH")
    ap.add_argument("--update-baseline", metavar="PATH")
    ap.add_argument("--defects-all", action="store_true")
    ap.add_argument("--jobs", type=int, default=0, metavar="N")
    a = ap.parse_args(argv)
    return run_defects(a.root, a)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
