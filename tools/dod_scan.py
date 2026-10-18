#!/usr/bin/env python3
"""Data-oriented-design scan of a C code base: every loop, struct, allocation and hot-path idiom, joined with
what the compiler said and (optionally) where a sampling profile says the time goes.

Generic by default: works on any C tree. Project knowledge is DATA, in dod_scan.json (or tools/dod_scan.json)
at the root -- extra allocator/free/cold/push names (exact names, or "re:<regex>"), managed-allocation prefixes,
API prefixes, type sizes, source roots, a compile-command hook, hardening flags (a fixed list, a cache glob,
or a probe recipe), profile workloads and the binary to run them with. Without it the built-in generic
patterns apply and compile commands come from compile_commands.json, else from a plain `cc -O2`.

Compiler sources (the oracle for these columns; nothing here guesses them): loop-vectorize, licm, gvn,
regalloc, slp-vectorizer and inline optimization records, -Wpadded, and the complete record layouts
(exact struct sizes and field offsets). Profile source: macOS `sample` + llvm-symbolizer (inlined frames
expanded), giving self-time per source line, so each finding carries the share of time spent inside it.

The lexical checks follow sections of the "Extreme Performance Engineering Reference" (gist
corporatepiyush/45b22003498f43b7a1901a1cc861d7a7) plus pointer-idiom checks; CHECKS lists each with its section.

usage:
  dod_scan.py remarks   [--root DIR] [--project FILE] [--jobs N]   # compiler records + layouts
  dod_scan.py profile   [--binary BIN] [--seconds S]               # sample the configured workloads
  dod_scan.py aggregate                                            # re-symbolize saved samples
  dod_scan.py report    [--limit N] [--check NAME]                 # join + ranked tables
  dod_scan.py all | selftest                                       # selftest: every check fires on its fixture
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import tempfile

import json
import math


def find_root(start):
    for markers in ((".git", "compile_commands.json", "tools/dod_scan.json"), ("dod_scan.json",)):
        d = os.path.abspath(start)
        while d != "/":
            if any(os.path.exists(os.path.join(d, m)) for m in markers):
                return d
            d = os.path.dirname(d)
    return os.path.abspath(start)


ROOT = find_root(os.path.dirname(os.path.abspath(__file__)))
PROJECT = {}
DEFAULT_OUT = os.path.join(ROOT, ".dod-scan")
REMARK_PASSES = "loop-vectorize|licm|gvn|regalloc|slp-vectorizer|inline"

CHECKS = [
    ("alloc-churn", "5.1/5.2/5.3", "loop allocates AND frees in its body: per-iteration churn; reuse scratch or an arena"),
    ("alloc-in-loop", "4.3/5.1", "allocation inside an innermost loop: pre-size once (reserve) or pool"),
    ("push-per-element", "3.2/5.1", "per-element push/append inside an innermost loop: reserve the total once"),
    ("smallcpy-blocks-vec", "4.4.5/4.8.8", "fixed-size memcpy/memset stays a CALL in a loop and blocks vectorization"),
    ("libm-in-loop", "4.4.5", "libm call in a non-vectorized innermost loop: hoist, or use a vectorizable form"),
    ("indirect-call-in-loop", "4.8.10/10.3", "indirect call in a small innermost loop: no inlining, branch-target prediction"),
    ("bound-reload", "4.8.1", "loop bound read through a pointer while the body stores or calls: reloaded every iteration"),
    ("call-in-bound", "4.8.1", "function call in the loop condition (strlen and friends): evaluated every iteration"),
    ("invariant-branch", "4.8.2/4.4.4", "if() on values the loop never changes: unswitch (two clean loops)"),
    ("cold-in-loop", "4.8.3", "error/throw/log call inside a small innermost loop without unlikely(): cold code in the hot block"),
    ("modulo-in-loop", "4.8.5", "% by a variable or a non-power-of-two constant in a loop: a division per iteration"),
    ("div-in-loop", "4.8.5", "division by a variable in an innermost loop: hoist a reciprocal or shift where exact"),
    ("non-unit-stride", "4.8.8", "array indexed with the induction variable times a stride: packed loads impossible"),
    ("accum-through-pointer", "4.8.6", "compound update of a heap field inside a loop: shadow into a local, write back once"),
    ("deep-chain", "4.8.7", "store through a 2+ hop pointer chain inside a loop: pull the leaf into a local"),
    ("transform-and-reduce", "4.8.8", "loop both stores an array and reduces into a scalar: split into two passes"),
    ("loop-fission", "4.8.8/4.8.9", "innermost loop whose statements split (by dependence SCCs) into a clean vectorizable piece and a piece that blocks it: two loops; assumes distinct arrays do not alias and callees touch only their arguments"),
    ("loop-fusion", "4.8.9/2.4", "adjacent loops with the same header touching the same arrays: two memory passes where one would do; fuse"),
    ("loop-interchange", "1.3/4.8.8", "perfect nest whose inner loop walks the slow dimension (a[j][i] / a[j*n+i] with j inner) and nothing walks the fast one: swap the loops"),
    ("cache-tiling", "1.3/4.8.8", "nest where one access is unit-stride and another strided in the inner loop (transpose, matmul shapes): no order fixes both; block the loops"),
    ("kind-dispatch", "4.4.4/2.4", "per-element switch / if-chain on a type or kind field: mispredicts and cold code per element; bucket or sort by kind, one tight loop per kind"),
    ("flag-filter", "2.4/3.1", "loop skips or guards each element on a live/active flag or an empty slot: existence-based processing; keep a dense list of the live entries"),
    ("random-gather", "3.1.2/12.1", "a[idx[i]] gather or scatter into a table too large to stay in cache: sort or bucket the indices, or prefetch a[idx[i + d]]"),
    ("xtu-call", "4.2/10.3", "per-element call to a function defined (non-static) in another translation unit: cannot inline without LTO; a batch API over the array amortises the call"),
    ("ptr-to-index", "2.2/3.1.2", "[compiler] node struct with 2+ self-links: 32-bit pool indices instead of 8-byte pointers shrink every node (bytes measured after the best field order)"),
    ("wide-enum-field", "2.1", "[compiler] enum-typed field (4 B) whose enumerators fit in a byte: uint8_t storage shrinks the struct (bytes measured after the best field order)"),
    ("line-straddle", "2.3", "[compiler] struct iterated as an array whose size splits cache lines: that share of elements touches one more line than needed; pad or shrink to a divisor of 64"),
    ("many-streams", "4.8.9", "6+ distinct arrays indexed by the induction variable: register pressure, fission candidate"),
    ("loop-carried", "4.8.8", "x[i] computed from x[i-1]: serial dependency (prefix scan): cannot vectorize as written"),
    ("syscall-in-loop", "4.1/6.6", "system call inside a loop: batch it (writev/sendmmsg), cache it (time, pid), or hoist it"),
    ("lock-in-loop", "7.5", "lock or atomic read-modify-write inside a loop: accumulate locally, publish once"),
    ("format-in-loop", "9.1", "printf-family formatting inside a loop: per-element string formatting"),
    ("pointer-chase", "3.1.2/12.1", "p = p->next traversal: serialised cache misses; flat array, or prefetch ahead"),
    ("macro-list-walk", "3.1.2/12.1", "list_for_each-style macro loop: pointer chase the lexical loop finder cannot see"),
    ("bytewise-scan", "6.8.2", "byte-at-a-time delimiter scan: memchr / SIMD class scan"),
    ("regex-compile-in-loop", "19 (regex)", "regular expression compiled inside a loop: compile once"),
    ("aos-partial-touch", "2.4", "loop reads 1-2 fields of an array of structs with 4+ fields: SoA candidate"),
    ("binary-search", "12.8", "binary search loop: Eytzinger layout or branchless search if the table is large and static"),
    ("sort-call", "12.9", "qsort-family call: indirect comparator per compare; radix / inline sort for integer keys"),
    ("big-memset", "5.1 + hardening", "memset/calloc of 1 KiB+ in a function body: clearing may cost more than the work"),
    ("linear-realloc", "5.1/3.2", "realloc growing by a constant: O(n^2) copying; grow geometrically"),
    ("write-sequence", "6.3", "two or more write/send calls on the same fd in one function: writev/sendmsg candidate"),
    ("mmap-no-madvise", "5.8/6.5", "file maps memory but never advises the kernel of the access pattern"),
    ("seq-cst-atomic", "7.4", "atomic operation with the default (seq_cst) ordering: use the weakest correct ordering"),
    ("bool-fields", "2.1", "struct with 3+ bool/BOOL fields (BOOL is 4 bytes here): pack into a flags word"),
    ("false-sharing-review", "2.3/3.3.1", "struct with 2+ atomics (or atomics beside a lock) and no 64-byte alignment"),
    ("chained-hash", "3.1.1/3.1.2", "hash table with linked-list buckets (self-linked node + bucket pointer array)"),
    ("goto-loop", "4.4/4.8", "loop built from a backward goto (invisible to the for/while finder and to the vectorizer)"),
    ("local-heap-temp", "4.3/5.2", "constant-size heap allocation freed in the same function: stack, scratch or arena candidate"),
    ("alloc-heavy-fn", "5.1-5.3", "function with 4+ allocation calls (inventory; rank by heat)"),
    ("cold-path-in-hot-fn", "4.8.3", "hot function (profile) carrying error/throw/log calls not marked unlikely: cold code in the hot stream"),
    ("inline-too-costly", "4.2", "[compiler] call in a hot function the inliner refused (cost over threshold or noinline)"),
    ("slp-missed-hot", "4.8.8", "[compiler] hot function where the SLP vectorizer tried and gave up"),
    ("interleave-only", "4.4.5", "[compiler] loop 'vectorized' with width 1: only interleaved, no SIMD"),
    ("reorder-savings", "2.3", "[compiler] struct whose fields sorted by alignment would be smaller (bytes saved, cache lines)"),
    ("hot-fields-spread", "1.3/2.3", "[compiler+profile] large struct whose hot-path fields span more cache lines than they need"),
    ("fsync-call", "6.4", "fsync/fdatasync/F_FULLFSYNC call (durability vs latency: fdatasync where metadata is not needed)"),
    ("tcp-nodelay-missing", "6.6/8.5", "file creates TCP sockets but never sets TCP_NODELAY"),
    ("ptr-repeated-subscript", "4.8.1/ptr", "the same a[expr] is evaluated 3+ times in one loop body: take T* e = &a[expr] once"),
    ("ptr-repeated-chain", "4.8.7/ptr", "a 2+ hop member chain (a->b->c) repeats 3+ times in a loop: cache the intermediate pointer"),
    ("ptr-member-base-index", "4.8.6/ptr", "x->buf[i] indexed in a loop that also stores through pointers: the base is reloaded every iteration; hoist T* buf = x->buf"),
    ("ptr-row-pointer", "4.8.5/ptr", "a[i * n + j] in a loop over j: hoist the row pointer T* row = a + i * n"),
    ("ptr-struct-by-value", "4.8.4/ptr", "[compiler] struct of 32+ B passed or returned by value: pass const T*, return through T* out"),
    ("ptr-struct-copy-in-loop", "4.8.4/ptr", "struct element copied by value in a loop (T v = arr[i]) and only read: const T* v = &arr[i]"),
    ("ptr-prev-unlink", "3.x/ptr", "list unlink with a prev variable and a special-cased head: a T** pp cursor removes both branches"),
    ("ptr-array-of-pointers", "3.1.2/12.1", "items[i]->field in a loop: one dependent load per element through an array of pointers; store the records contiguously"),
    ("ptr-restrict-candidate", "4.8.6/ptr", "[compiler] loop refused for possible aliasing while the function takes 2+ same-type pointers: restrict-qualify them"),
    ("ptr-strlen-repeat", "4.8.1/ptr", "strlen() of the same unmodified string 2+ times in one function: keep the length (or an end pointer)"),
    ("stack-array", "hardening", "local array of 256 B+ that -ftrivial-auto-var-init=zero clears on every call"),
    ("single-vector-accumulator", "4.8.5/12.1", "SIMD loop whose reduction is ONE intrinsic chain (acc = vaddq(acc, ..)): latency-bound at one op per add latency; float chains want several accumulators"),
    ("branchy-merge", "4.4.4/12.9", "two-cursor merge whose step is an if on x[p] < y[q]: one mispredict per element on random data; branchless cursor advance"),
    ("fp-serial-reduction", "4.8.5/12.1", "one float/double accumulator in an innermost loop: a serial add chain (strict FP; an arm64 'vectorized' verdict is an ORDERED reduction, still serial): use several accumulators"),
    ("licm-blocked", "4.8.1/4.8.6", "[compiler] loads the loop may invalidate: not hoisted (aliasing store or call)"),
    ("gvn-clobbered", "4.8.1/4.8.6", "[compiler] loads not eliminated because a call/store clobbers them"),
    ("loop-spills", "4.8.9", "[compiler] register spills/reloads generated inside the loop"),
    ("padding", "2.3", "[compiler] struct padding holes reported by -Wpadded"),
]
CHECK_IDS = [c[0] for c in CHECKS]

GENERIC = {
    "alloc": r"\w*(?:malloc|calloc|realloc|strn?dup|aligned_alloc|posix_memalign|_alloc|alloc_)\w*",
    "free": r"free|\w*_free\w*|free_\w+|\w*(?:dealloc|release|destroy|dispose|unref)\w*",
    "push": r"\w*_(?:push|append|put)\w*",
    "cold": r"abort|perror|fprintf|err|errx|warn|warnx|\w*(?:[Tt]hrow|_die|[Ff]atal|[Pp]anic|_abort|assert_fail)\w*",
    "regex_compile": r"regcomp|pcre2?_compile\w*|\w*re_compile\w*|\w*compile_regexp\w*|\w*[Rr]eg[Ee]xp?_?[Cc]ompile\w*",
    "sort": r"qsort|qsort_r|mergesort|heapsort|\w*_qsort|rqsort",
}


def _alt(generic, extra):
    parts = [generic] + [re.escape(x) if not x.startswith("re:") else x[3:] for x in extra]
    return "|".join("(?:" + p + ")" for p in parts if p)


def configure(root=None, config_file=None):
    global ROOT, PROJECT, DEFAULT_OUT, ALLOC_RE, FREE_RE, PUSH_RE, COLD_RE, JSAPI_RE, REGEX_RE, SORT_RE, TYPE_SIZE
    if root:
        ROOT = os.path.abspath(root)
    PROJECT = {}
    for cand in ([config_file] if config_file else [os.path.join(ROOT, "dod_scan.json"), os.path.join(ROOT, "tools", "dod_scan.json")]):
        if cand and os.path.exists(cand):
            PROJECT = json.load(open(cand))
            PROJECT["_file"] = cand
            break
    DEFAULT_OUT = os.path.join(ROOT, PROJECT.get("out", ".dod-scan"))
    ALLOC_RE = re.compile(r"\b(" + _alt(GENERIC["alloc"], PROJECT.get("alloc", [])) + r")\s*\(")
    FREE_RE = re.compile(r"\b(" + _alt(GENERIC["free"], PROJECT.get("free", [])) + r")\s*\(")
    PUSH_RE = re.compile(r"\b(" + _alt(GENERIC["push"], PROJECT.get("push", [])) + r")\s*\(")
    COLD_RE = re.compile(r"(?<![\w.>])(" + _alt(GENERIC["cold"], PROJECT.get("cold", [])) + r")\s*\(")
    REGEX_RE = re.compile(r"\b(" + _alt(GENERIC["regex_compile"], PROJECT.get("regex_compile", [])) + r")\s*\(")
    SORT_RE = re.compile(r"\b(" + _alt(GENERIC["sort"], PROJECT.get("sort", [])) + r")\s*\(")
    api = PROJECT.get("api_prefixes", [])
    JSAPI_RE = re.compile(r"\b(?:" + "|".join(re.escape(x) for x in api) + r")\w*\s*\(") if api else re.compile(r"(?!x)x")
    TYPE_SIZE = dict(BASE_TYPE_SIZE)
    TYPE_SIZE.update(PROJECT.get("type_sizes", {}))


SMALLCPY_RE = re.compile(
    r"\b(memcpy|memmove|memset)\s*\(([^;]*?),\s*(1|2|3|4|8|16|sizeof\s*\(\s*(u?int(8|16|32|64)_t|float|double|"
    r"int|unsigned|short|char|size_t|uint32_t|uint64_t)\s*\))\s*\)")
LIBM_RE = re.compile(
    r"\b(sqrtf?|cbrtf?|sinf?|cosf?|tanf?|expf?|exp2f?|logf?|log2f?|log10f?|powf?|fmodf?|floorf?|ceilf?|roundf?|"
    r"lroundf?|truncf?|hypotf?|atan2f?|fabsf?|fminf?|fmaxf?|isnan|isfinite|isinf)\s*\(")
FNPTR_RE = re.compile(r"(->\s*\w+\s*\(|\(\s*\*\s*\w+\s*\)\s*\(|\b\w+\s*\[\s*[^]]+\]\s*\()")
DIV_RE = re.compile(r"[^/*]/\s*\(?\s*[A-Za-z_][\w.>-]*")
LOOP_RE = re.compile(r"\b(for|while|do)\b")
LOCALARR_RE = re.compile(
    r"(?:(?<=[{;}\n])|^)[ \t]*(?:const\s+|static\s+)?([A-Za-z_][\w]*(?:\s+[A-Za-z_]\w*)?(?:\s*\*+)?)\s+([A-Za-z_]\w*)\s*"
    r"((?:\[\s*[^]\[]+\s*\])+)\s*(?:DYN_IO_SCRATCH\s*)?[;=,]")
BASE_TYPE_SIZE = {"char": 1, "int8_t": 1, "uint8_t": 1, "unsigned char": 1, "signed char": 1, "bool": 1, "BOOL": 4,
             "short": 2, "int16_t": 2, "uint16_t": 2, "int": 4, "unsigned": 4, "int32_t": 4, "uint32_t": 4,
             "float": 4, "long": 8, "int64_t": 8, "uint64_t": 8, "double": 8, "size_t": 8, "uintptr_t": 8}
KEYWORDS = set("if else for while do return break continue switch case default goto sizeof struct union enum "
               "typedef static const volatile unsigned signed int char short long float double void inline "
               "likely unlikely NULL true false TRUE FALSE _Alignof defined".split())
MOD_RE = re.compile(r"(?<![%])%\s*=?\s*(\(?\s*[A-Za-z_][\w.>\-]*|\d+)")
SYSCALL_RE = re.compile(
    r"(?<![\w.>])(read|write|pread|pwrite|readv|writev|send|recv|sendto|recvfrom|sendmsg|recvmsg|sendmmsg|"
    r"clock_gettime|gettimeofday|time|getpid|gettid|stat|fstat|lstat|lseek|fsync|fdatasync|fcntl|ioctl|"
    r"epoll_ctl|kevent|poll|select|open|openat|close|access|mmap|munmap|mprotect|getrusage|sched_yield|"
    r"nanosleep|usleep|getenv|sysconf|mach_absolute_time)\s*\(")
SYSCALL_KIND = {"clock_gettime": "time", "gettimeofday": "time", "time": "time", "mach_absolute_time": "time",
                "getpid": "meta", "gettid": "meta", "getenv": "meta", "sysconf": "meta", "getrusage": "meta",
                "stat": "meta", "fstat": "meta", "lstat": "meta", "access": "meta",
                "fcntl": "fdctl", "ioctl": "fdctl", "epoll_ctl": "fdctl", "kevent": "fdctl"}
LOCK_RE = re.compile(r"\b(\w*mutex_lock|\w*rwlock_\w*lock|\w*spin_lock|\w*unfair_lock_lock|\w*Lock\w*Mutex|\w+_lock|atomic_fetch_\w+|atomic_exchange\w*|"
                     r"atomic_compare_exchange\w*|__atomic_fetch_\w+|__atomic_\w*exchange\w*|__sync_\w+)\s*\(")
FMT_RE = re.compile(r"\b(\w*printf)\s*\(")
CHASE_RE = re.compile(r"(?<![\w.>])([A-Za-z_]\w*)\s*=\s*\1\s*->\s*([A-Za-z_]\w*)\s*[;),]")
LISTWALK_RE = re.compile(r"\b(list_for_each\w*|LIST_FOREACH\w*|TAILQ_FOREACH\w*|SLIST_FOREACH\w*|hlist_for_each\w*)\s*\(")
BYTECMP_RE = re.compile(r"(\*\s*\(?\s*[A-Za-z_][\w.>\-]*\s*\)?|[A-Za-z_][\w.>\-]*\s*\[[^\]]+\])\s*(==|!=)\s*'[^'\n]{1,4}'"
                        r"|'[^'\n]{1,4}'\s*(==|!=)\s*(\*\s*[A-Za-z_]\w*|[A-Za-z_][\w.>\-]*\s*\[[^\]]+\])")
BSEARCH_RE = re.compile(r"\(\s*([A-Za-z_]\w*)\s*\+\s*([A-Za-z_]\w*)\s*\)\s*(?:/\s*2\b|>>\s*1\b)|"
                        r"\b([A-Za-z_]\w*)\s*\+\s*\(\s*\(?\s*([A-Za-z_]\w*)\s*-\s*\3\s*\)?\s*(?:/\s*2\b|>>\s*1\b)")
MEMSET_RE = re.compile(r"\bmemset\s*\(\s*([^,]+?)\s*,\s*0\s*,\s*([^;]+?)\)\s*;")
CALLOC_RE = re.compile(r"\b(\w*calloc\w*|\w*mallocz\w*)\s*\(([^;]*)\)")
REALLOC_RE = re.compile(r"\b(\w*realloc\w*)\s*\(([^;]*)\)\s*;")
WRITE_RE = re.compile(r"(?<![\w.>])(write|send|sendto)\s*\(\s*([A-Za-z_][\w.>\-]*)\s*,")
ATOMIC_DEFAULT_RE = re.compile(r"\b(atomic_(?:load|store|fetch_add|fetch_sub|fetch_or|fetch_and|fetch_xor|exchange|"
                               r"compare_exchange_strong|compare_exchange_weak))\s*\(|__ATOMIC_SEQ_CST|memory_order_seq_cst")
VACC_RE = re.compile(r"(?<![\w.>])([A-Za-z_]\w*)\s*=\s*((?:v(?:add|fma|mla|pada|padal|dot|max|min|qadd|abal|aba)\w*|"
                     r"_mm\d*_(?:add|fmadd|fmsub|fnmadd|max|min|dpbusd|madd|sad)\w*|"
                     r"sv(?:add|mla|mad|max|min|dot)\w*))\s*\(\s*(?:[A-Za-z_]\w*\s*,\s*)?([^,()]*\([^()]*\)[^,()]*|[^,()]+)?")
STRUCT_RE = re.compile(r"(?:typedef\s+)?struct\s+([A-Za-z_]\w*)?\s*\{")


def blank_comments_and_strings(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            fill = "x" if c == "'" else " "
            out.append(c + re.sub(r"[^\n]", fill, text[i + 1:j - 1]) + (c if j - 1 > i else ""))
            i = j
        elif c == "#" and (i == 0 or text[i - 1] == "\n"):
            j = i
            while True:
                k = text.find("\n", j)
                if k < 0:
                    k = n
                    break
                if text[k - 1] == "\\":
                    j = k + 1
                    continue
                break
            out.append(re.sub(r"[^\n]", " ", text[i:k]))
            i = k
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_paren(s, i, open_c, close_c):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == open_c:
            depth += 1
        elif s[j] == close_c:
            depth -= 1
            if depth == 0:
                return j
    return -1


def statement_end(s, i):
    j = i
    while j < len(s) and s[j] in " \t\n":
        j += 1
    if j < len(s) and s[j] == "{":
        return match_paren(s, j, "{", "}")
    m = LOOP_RE.match(s, j)
    if m and m.group(1) in ("for", "while"):
        p = s.find("(", m.end())
        q = match_paren(s, p, "(", ")") if p >= 0 else -1
        return statement_end(s, q + 1) if q >= 0 else -1
    if s.startswith("if", j) and re.match(r"if\b", s[j:]):
        p = s.find("(", j)
        q = match_paren(s, p, "(", ")")
        e = statement_end(s, q + 1)
        k = e + 1
        while k < len(s) and s[k] in " \t\n":
            k += 1
        if s.startswith("else", k) and re.match(r"else\b", s[k:]):
            return statement_end(s, k + 4)
        return e
    depth = 0
    for k in range(j, len(s)):
        if s[k] in "([{":
            depth += 1
        elif s[k] in ")]}":
            depth -= 1
        elif s[k] == ";" and depth <= 0:
            return k
    return -1


def defines(paths):
    d = {}
    for p in paths:
        try:
            for line in open(p, errors="replace"):
                m = re.match(r"\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+\(?\s*(\d+)\s*(?:u|U|ull|ULL)?\s*\)?\s*$", line)
                if m:
                    d[m.group(1)] = int(m.group(2))
                m = re.match(r"\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+\(\s*1u?\s*<<\s*(\d+)\s*\)", line)
                if m:
                    d[m.group(1)] = 1 << int(m.group(2))
        except OSError:
            pass
    return d


def eval_dim(expr, defs):
    expr = expr.strip()
    expr = re.sub(r"\b([A-Z_][A-Z0-9_]*)\b", lambda m: str(defs.get(m.group(1), "X")), expr)
    if "X" in expr or not re.fullmatch(r"[\d\s+\-*/()<]+", expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}))
    except Exception:
        return None


def assigned_names(body):
    out = set()
    for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:[-+*/%&|^]|<<|>>)?=(?!=)", body):
        out.add(m.group(1))
    for m in re.finditer(r"(?<![\w.>])(?:\+\+|--)\s*([A-Za-z_]\w*)|(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--)", body):
        out.add(m.group(1) or m.group(2))
    for m in re.finditer(r"&\s*([A-Za-z_]\w*)", body):
        out.add(m.group(1))
    return out


def loop_var(kw, header):
    if kw != "for":
        return None
    parts = header.split(";")
    if len(parts) != 3:
        return None
    m = re.search(r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|[-+]=)", parts[2]) or \
        re.search(r"(?:\+\+|--)\s*([A-Za-z_]\w*)", parts[2])
    return m.group(1) if m else None


def free_idents(expr):
    out = []
    for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)", expr):
        name = m.group(1)
        if name in KEYWORDS or re.fullmatch(r"[A-Z][A-Z0-9_]+", name):
            continue
        out.append(name)
    return out


def is_pow2(n):
    return n > 0 and n & (n - 1) == 0


def gist_loop_checks(L, body, header, defs):
    f = {}
    lv = loop_var(L["kw"], header)
    L["lv"] = lv or ""
    assigned = assigned_names(body)
    calls_or_stores = bool(re.search(r"(?<![\w.>])[A-Za-z_]\w*\s*\(", body.split(")", 1)[-1])) or \
        bool(re.search(r"(->\s*\w+|\]|\*\s*\w+)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)", body))
    cond = header.split(";")[1] if L["kw"] == "for" and header.count(";") == 2 else header
    m = re.search(r"(<=?|!=|>=?)\s*([A-Za-z_]\w*(?:\s*->\s*\w+|\s*\.\s*\w+)+)", cond)
    if m and calls_or_stores and m.group(2).split("-")[0].split(".")[0].strip() not in assigned:
        f["bound-reload"] = [" ".join(m.group(2).split())]
    cm = [x for x in re.findall(r"(?<![\w.>])([A-Za-z_]\w*)\s*\(", cond) if x not in KEYWORDS and
          x not in ("likely", "unlikely") and not re.fullmatch(r"[A-Z][A-Z0-9_]+", x)]
    if cm:
        f["call-in-bound"] = sorted(set(cm))
    if L["inner"]:
        inv = []
        for im in re.finditer(r"(?<![\w])if\s*\(", body):
            q = match_paren(body, im.end() - 1, "(", ")")
            if q < 0:
                continue
            c = body[im.end():q]
            c = re.sub(r"\b(?:likely|unlikely)\s*\(", "(", c)
            if "[" in c or re.search(r"(?<![\w.>])[A-Za-z_]\w*\s*\(", c) or re.search(r"(?<![\w)\]])\*\s*\w", c):
                continue
            ids = free_idents(c)
            if ids and lv not in ids and not (set(ids) & assigned):
                inv.append(" ".join(c.split())[:50])
        if inv:
            f["invariant-branch"] = inv
        cold = [x.group(1) for x in COLD_RE.finditer(body)
                if "unlikely" not in body[max(0, x.start() - 160):x.start()]]
        if cold and L["lines"] <= 12:
            f["cold-in-loop"] = sorted(set(cold))
    mods = []
    for mm in MOD_RE.finditer(body):
        rhs = mm.group(1).strip("( ")
        if rhs.isdigit():
            if not is_pow2(int(rhs)):
                mods.append(rhs)
        elif rhs in defs:
            if not is_pow2(defs[rhs]):
                mods.append(rhs)
        else:
            mods.append(rhs)
    if mods:
        f["modulo-in-loop"] = sorted(set(mods))
    if L["inner"] and L["div"]:
        f["div-in-loop"] = [str(L["div"])]
    if lv:
        st = []
        for sm in re.finditer(r"\[\s*([^\]\[]*\b" + re.escape(lv) + r"\b[^\]\[]*)\]", body):
            e = sm.group(1)
            mm = re.search(r"\b" + re.escape(lv) + r"\s*\*\s*([A-Za-z_]\w*|\d+)|([A-Za-z_]\w*|\d+)\s*\*\s*\b" + re.escape(lv) + r"\b", e)
            if mm:
                k = mm.group(1) or mm.group(2)
                if k != "1":
                    st.append(k)
        if st:
            f["non-unit-stride"] = sorted(set(st))
        bases = set(re.findall(r"([A-Za-z_][\w]*(?:\s*(?:->|\.)\s*\w+)*)\s*\[[^\]\[]*\b" + re.escape(lv) + r"\b", body))
        if len(bases) >= 6:
            f["many-streams"] = [str(len(bases))]
        stores = set(re.findall(r"([A-Za-z_]\w*)\s*\[\s*" + re.escape(lv) + r"\s*\]\s*=(?!=)", body))
        carried = [b for b in stores if re.search(re.escape(b) + r"\s*\[\s*" + re.escape(lv) + r"\s*-\s*1\s*\]", body)]
        if carried:
            f["loop-carried"] = carried
        if L["inner"] and re.search(r"[A-Za-z_][\w.>\-]*\s*\[[^\]]*\b" + re.escape(lv) + r"\b[^\]]*\]\s*=(?!=)", body):
            red = [x for x in re.findall(r"(?<![\w.>\]\)])([a-z_]\w*)\s*(?:\+=|-=|\*=|\|=|\^=)", body)
                   if x != lv and not re.search(r"\b" + re.escape(x) + r"\s*\[", body)]
            if red:
                f["transform-and-reduce"] = sorted(set(red))
        if L["inner"]:
            aos = collections.defaultdict(set)
            for am in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*\[[^\]\[]*\b" + re.escape(lv) + r"\b[^\]\[]*\]\s*\.\s*([A-Za-z_]\w*)", body):
                aos[am.group(1)].add(am.group(2))
            for am in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*\[[^\]\[]*\b" + re.escape(lv) + r"\b[^\]\[]*\]\s*->\s*([A-Za-z_]\w*)", body):
                aos[am.group(1)].add(am.group(2))
            if aos:
                L["_aos"] = {k: sorted(v) for k, v in aos.items() if len(v) <= 2}
                L["_aos_all"] = {k: sorted(v) for k, v in aos.items()}
    acc, deep = [], []
    for am in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)((?:\s*(?:->|\.)\s*[A-Za-z_]\w*)+)\s*(\+=|-=|\*=|\|=|&=|\^=|\+\+|--)", body):
        base, chain = am.group(1), am.group(2)
        if "->" not in chain or base in assigned or base == lv:
            continue
        acc.append(base + "".join(chain.split()))
    for am in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)((?:\s*(?:->|\.)\s*[A-Za-z_]\w*){2,})\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)", body):
        base, chain = am.group(1), am.group(2)
        if chain.count("->") >= 1 and chain.count("->") + chain.count(".") >= 2 and base not in assigned and base != lv:
            deep.append(base + "".join(chain.split()))
    if acc:
        f["accum-through-pointer"] = sorted(set(acc))[:4]
    if deep:
        f["deep-chain"] = sorted(set(deep))[:4]
    sc = [x.group(1) for x in SYSCALL_RE.finditer(body)]
    if sc and not ("EINTR" in body and L["lines"] <= 6 and set(sc) <= {"read", "write", "recv", "send", "close", "open"}):
        f["syscall-in-loop"] = sorted(set(f"{x}({SYSCALL_KIND.get(x, 'io')})" for x in sc))
    lk = [x.group(1) for x in LOCK_RE.finditer(body)]
    if lk:
        f["lock-in-loop"] = sorted(set(lk))
    fm = [x.group(1) for x in FMT_RE.finditer(body)]
    if fm:
        f["format-in-loop"] = sorted(set(fm))
    ch = [f"{x.group(1)}->{x.group(2)}" for x in CHASE_RE.finditer(header + ";" + body)]
    if ch:
        f["pointer-chase"] = sorted(set(ch)) + (["prefetched"] if "__builtin_prefetch" in body else [])
    if L["inner"] and L["lines"] <= 5 and (BYTECMP_RE.search(header) or BYTECMP_RE.search(body)) and \
            re.search(r"\+\+|\+=\s*1\b", header + body):
        n = len(BYTECMP_RE.findall(header + body))
        f["bytewise-scan"] = [f"{n} byte compare(s)"]
    if L["inner"]:
        accs = {}
        for vm in VACC_RE.finditer(body):
            var, fn = vm.group(1), vm.group(2)
            call = body[vm.end(2):]
            q = match_paren(call, call.find("("), "(", ")")
            args = call[call.find("(") + 1:q] if q > 0 else ""
            if re.search(r"(?<![\w.>])" + re.escape(var) + r"\b", args) and \
                    not re.search(r"\b[A-Za-z_]\w*\s+" + re.escape(var) + r"\s*=", body):
                accs[var] = fn
        if len(accs) == 1:
            var, fn = next(iter(accs.items()))
            kind = "float" if re.search(r"_f(16|32|64)\b|_p[sd]\b|_sd\b|_ss\b|f32|f64", fn) else "int"
            f["single-vector-accumulator"] = [f"{var}={fn} ({kind})"]
    if L["inner"] and re.search(r"\b(\w+)\s*<\s*[\w>.\-]+\s*(?:&&|\|\|)\s*\(?\s*(\w+)\s*<", header):
        for mm in re.finditer(r"(?<![\w])if\s*\(([^;{]*?)\b([A-Za-z_]\w*)\s*\[\s*(\w+)\s*\]\s*(<|>|<=|>=)\s*([A-Za-z_]\w*)\s*\[\s*(\w+)\s*\]", body):
            if mm.group(3) != mm.group(6) and mm.group(2) != mm.group(5):
                f["branchy-merge"] = [f"{mm.group(2)}[{mm.group(3)}] {mm.group(4)} {mm.group(5)}[{mm.group(6)}]"]
                break
    subs = collections.Counter(" ".join(x.group(0).split()) for x in re.finditer(
        r"(?<![\w.>])[A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*\s*\[[^\[\]]*[A-Za-z_][^\[\]]*\]", body))
    rep_subs = [k for k, n in subs.items() if n >= 3 and not re.search(r"\[\s*\d+\s*\]$", k)]
    rep_subs = [k for k in rep_subs if k.split("[")[0].split("->")[0].split(".")[0].strip() not in assigned]
    if rep_subs:
        f["ptr-repeated-subscript"] = [f"{k} x{subs[k]}" for k in sorted(rep_subs, key=lambda k: -subs[k])[:3]]
    chains = collections.Counter()
    for x in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*){2,})", body):
        parts = re.split(r"\s*(->|\.)\s*", " ".join(x.group(1).split()))
        for k in range(5, len(parts), 2):
            chains["".join(parts[:k])] += 1
    rep_chains = [k for k, n in chains.items() if n >= 3 and "->" in k and k.split("->")[0].split(".")[0].strip() not in assigned]
    if rep_chains:
        f["ptr-repeated-chain"] = [f"{k} x{chains[k]}" for k in sorted(rep_chains, key=lambda k: -chains[k])[:3]]
    if lv:
        mb = sorted(set(" ".join(x.group(1).split()) for x in re.finditer(
            r"(?<![\w.>])([A-Za-z_]\w*\s*->\s*\w+(?:\s*(?:->|\.)\s*\w+)*)\s*\[[^\]]*\b" + re.escape(lv) + r"\b", body)))
        mb = [m for m in mb if m.split("->")[0].strip() not in assigned]
        if mb and calls_or_stores:
            f["ptr-member-base-index"] = mb[:3]
        rows = sorted(set(x.group(0) for x in re.finditer(
            r"(?<![\w.>])[A-Za-z_]\w*\s*\[\s*([A-Za-z_]\w*)\s*\*\s*[A-Za-z_][\w>.\-]*\s*\+\s*" + re.escape(lv) + r"\s*\]", body)
            if x.group(1) != lv))
        if rows:
            f["ptr-row-pointer"] = [" ".join(r.split()) for r in rows[:3]]
        apc = sorted(set(x.group(1) for x in re.finditer(
            r"(?<![\w.>])([A-Za-z_][\w.>\-]*\s*\[\s*" + re.escape(lv) + r"\s*\])\s*->\s*\w+", body)))
        if apc:
            f["ptr-array-of-pointers"] = [" ".join(a.split()) for a in apc[:3]]
        cp = [x.group(1) for x in re.finditer(
            r"(?:^|(?<=[;{}\n]))\s*(?:const\s+)?(?:struct\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*=\s*[A-Za-z_][\w.>\-]*\s*\[\s*" + re.escape(lv) + r"\s*\]\s*;", body)
              if x.group(1) not in KEYWORDS and x.group(1) not in TYPE_SIZE and not re.fullmatch(r"u?int\d+_t|size_t|char|double|float|long|short|unsigned|JSValue|JSAtom", x.group(1))]
        if cp:
            L["_copytypes"] = sorted(set(cp))
    if re.search(r"\bprev\s*->\s*next\s*=", body) and re.search(r"\belse\b[^;]*\b\w+\s*=\s*\w+\s*->\s*next\s*;|\bif\s*\(\s*!?\s*prev\s*\)", body):
        f["ptr-prev-unlink"] = ["prev-tracked unlink"]
    rg = [x.group(1) for x in REGEX_RE.finditer(body)]
    if rg:
        f["regex-compile-in-loop"] = sorted(set(rg))
    if BSEARCH_RE.search(body) or BSEARCH_RE.search(header):
        f["binary-search"] = ["midpoint"]
    rl = []
    for rm in REALLOC_RE.finditer(body):
        a = rm.group(2)
        if re.search(r"\+\s*(\d+|[A-Za-z_]\w*)\s*\)?\s*\*?", a) and not re.search(r"\*\s*2|<<|\* *3 */ *2|\bcap\s*\*|grow|next_pow|\* *1\.5", a):
            rl.append(" ".join(a.split())[:50])
    if rl:
        f["linear-realloc"] = rl
    return f


def parse_structs(s, line_of, relpath):
    out = []
    for m in STRUCT_RE.finditer(s):
        ob = m.end() - 1
        cb = match_paren(s, ob, "{", "}")
        if cb < 0:
            continue
        tail = re.match(r"\s*([A-Za-z_]\w*)?\s*;", s[cb + 1:cb + 80])
        name = m.group(1) or (tail.group(1) if tail and tail.group(1) else None)
        alias = tail.group(1) if tail and tail.group(1) else None
        if not name:
            continue
        inner = s[ob + 1:cb]
        depth, cur, fields = 0, [], []
        for ch in inner:
            if ch in "{(":
                depth += 1
            elif ch in "})":
                depth -= 1
            if ch == ";" and depth == 0:
                fields.append(" ".join("".join(cur).split()))
                cur = []
            else:
                cur.append(ch)
        fields = [x for x in fields if x]
        bools = [x for x in fields if re.match(r"(?:const\s+)?(?:bool|BOOL|_Bool)\s+\w+\s*$", x)]
        atomics = [x for x in fields if re.search(r"\b_Atomic\b|\batomic_\w+\b|\bstd::atomic\b", x)]
        locks = [x for x in fields if re.search(r"\bpthread_mutex_t\b|\bpthread_spinlock_t\b|\bos_unfair_lock\b|\bSDL_Mutex\b", x)]
        aligned = bool(re.search(r"_Alignas|alignas|aligned\s*\(\s*\(?\s*(64|128)", inner))
        selfnext = [x for x in fields if re.search(r"\bstruct\s+" + re.escape(name) + r"\s*\*\s*(\w*next\w*|\w*link\w*|chain\w*)\b", x)
                    or (alias and re.search(r"\b" + re.escape(alias) + r"\s*\*\s*(\w*next\w*|\w*link\w*|chain\w*)\b", x))]
        dptr = [x for x in fields if re.search(r"\*\s*\*\s*(\w*(hash|bucket|table|tab|slot|chain)\w*)\s*$", x, re.I)]
        out.append({"file": relpath, "line": line_of[m.start()], "name": name, "alias": alias or "",
                    "nfields": len(fields), "bools": len(bools), "atomics": len(atomics), "locks": len(locks),
                    "aligned": aligned, "selfnext": bool(selfnext), "dptr": dptr})
    return out


GOTO_LABEL_RE = re.compile(r"(?:^|(?<=[;{}\n]))[ \t]*([A-Za-z_]\w*)[ \t]*:(?![:=])")
ALLOC_ASSIGN_RE = re.compile(r"(?<![\w.>])([A-Za-z_][\w.>\-]*)\s*=\s*(?:\([^()]*\)\s*)?(\w*(?:malloc|calloc|mallocz|_alloc)\w*)\s*\(")
FSYNC_RE = re.compile(r"(?<![\w.>])(fsync|fdatasync)\s*\(|F_FULLFSYNC")


FIS_TOK = re.compile(
    r"\s*(?:(?P<id>[A-Za-z_]\w*)"
    r"|(?P<num>(?:0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)[uUlLfF]*)"
    r"|(?P<op>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|[-+*/%&|^~!<>=?:,.;()\[\]{}])"
    r"|(?P<lit>'[^'\n]*'|\"[^\"\n]*\"))")
FIS_ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
FIS_TYPE_WORDS = set("const volatile static register unsigned signed long short struct union enum restrict".split())
FIS_BASE_TYPES = set("void char short int long float double _Bool bool size_t ssize_t ptrdiff_t intptr_t uintptr_t "
                     "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t".split())
FIS_CTRL = set("if else for while do switch case default return break continue goto sizeof".split())
FIS_EXIT_RE = re.compile(r"(?<![\w.>])(?:break|continue|return|goto|longjmp|siglongjmp|exit|_exit|abort)\b")
FIS_PURE_VEC = set("sqrt sqrtf fabs fabsf floor floorf ceil ceilf trunc truncf round roundf rint rintf nearbyint "
                   "fmin fminf fmax fmaxf copysign copysignf abs labs llabs fma fmaf".split())
FIS_INTRIN_PURE = re.compile(r"_mm\d*_(?!store|stream|maskstore|mask_store|mask_compressstore|i\d+scatter|mask_i\d+scatter)\w+"
                             r"|v(?!st\d)[a-z]+[0-9]*q?_(?:lane_|laneq_|n_)?[a-z]+\d+(?:_[a-z]+\d+)?"
                             r"|sv(?!st\d|stnt|scatter|prf)\w+")
FIS_PURE_SCALAR = set("exp expf exp2 exp2f expm1 expm1f log logf log2 log2f log10 log10f log1p log1pf pow powf sin "
                      "sinf cos cosf tan tanf asin asinf acos acosf atan atanf atan2 atan2f sinh sinhf cosh coshf "
                      "tanh tanhf cbrt cbrtf hypot hypotf erf erff erfc erfcf lgamma lgammaf tgamma tgammaf fmod "
                      "fmodf remainder lround lroundf llround isnan isinf isfinite signbit".split())


def fis_tokens(text):
    out, i, n = [], 0, len(text)
    while i < n:
        m = FIS_TOK.match(text, i)
        if not m or m.end() == i:
            if text[i:].strip() == "":
                break
            return None
        k = m.lastgroup
        out.append((k, m.group(k)))
        i = m.end()
    return out


def fis_match(toks, i, o, c):
    d = 0
    for j in range(i, len(toks)):
        v = toks[j][1]
        if v == o:
            d += 1
        elif v == c:
            d -= 1
            if d == 0:
                return j
    return -1


def fis_statement_end(toks, i):
    if i >= len(toks):
        return -1
    v = toks[i][1]
    if v == "{":
        return fis_match(toks, i, "{", "}")
    if v in ("if", "switch", "for", "while") and i + 1 < len(toks) and toks[i + 1][1] == "(":
        q = fis_match(toks, i + 1, "(", ")")
        if q < 0:
            return -1
        e = fis_statement_end(toks, q + 1)
        if v == "if" and e >= 0 and e + 1 < len(toks) and toks[e + 1][1] == "else":
            return fis_statement_end(toks, e + 2)
        return e
    if v == "do":
        e = fis_statement_end(toks, i + 1)
        if e < 0 or e + 2 >= len(toks) or toks[e + 1][1] != "while":
            return -1
        q = fis_match(toks, e + 2, "(", ")")
        return q + 1 if q >= 0 and q + 1 < len(toks) and toks[q + 1][1] == ";" else -1
    d = 0
    for j in range(i, len(toks)):
        t = toks[j][1]
        if t in ("(", "[", "{"):
            d += 1
        elif t in (")", "]", "}"):
            d -= 1
            if d < 0:
                return -1
        elif t == ";" and d == 0:
            return j
    return -1


def fis_split(toks):
    out, i = [], 0
    while i < len(toks):
        if toks[i][1] == ";":
            i += 1
            continue
        e = fis_statement_end(toks, i)
        if e < 0:
            return None
        out.append(toks[i:e + 1])
        i = e + 1
    return out


def fis_is_decl(st):
    k, tw = 0, 0
    while k < len(st) and st[k][1] in FIS_TYPE_WORDS:
        k += 1
        tw += 1
    if k >= len(st) or st[k][0] != "id" or st[k][1] in FIS_CTRL:
        return None
    tagged = tw and st[k - 1][1] in ("struct", "union", "enum")
    if tw and not tagged and k + 1 < len(st) and st[k + 1][1] in ("=", ";", ",", "["):
        return k
    k += 1
    while k < len(st) and st[k][1] in ("*", "const", "restrict", "volatile"):
        k += 1
    if k >= len(st) or st[k][0] != "id" or st[k][1] in FIS_CTRL or st[k][1] in FIS_TYPE_WORDS:
        return None
    if k + 1 < len(st) and st[k + 1][1] not in ("=", ";", ",", "["):
        return None
    return k


class FisStmt:
    def __init__(self, idx, toks):
        self.idx = idx
        self.toks = toks
        self.text = " ".join(v for _, v in toks)
        self.defs = {}
        self.uses = {}
        self.locals = set()
        self.calls = set()
        self.call_roots = set()
        self.roots = set()
        self.read_roots = set()
        self.decl_pos = set()
        self.block = []
        self.work = []
        self.counter_only = True
        self.cond = toks[0][1] in ("if", "switch") if toks else False
        self.kind = toks[0][1] if toks and toks[0][1] in ("if", "switch", "for", "while", "do") else "expr"

    def add(self, table, path, ix):
        table.setdefault(path, set()).add(ix)


def fis_chain(toks, i, end):
    if toks[i][0] != "id":
        return None
    path = [toks[i][1]]
    idxs = []
    j = i + 1
    while j < end:
        v = toks[j][1]
        if v in ("->", ".") and j + 1 < end and toks[j + 1][0] == "id":
            path.append(v + toks[j + 1][1])
            j += 2
        elif v == "[":
            q = fis_match(toks, j, "[", "]")
            if q < 0 or q >= end:
                return None
            idxs.append((j + 1, q))
            path.append("[]")
            j = q + 1
        else:
            break
    return "".join(path), idxs, j


def fis_analyze(st, ivs, invariant_ok, pure_extra):
    s = st
    toks = s.toks
    n = len(toks)
    decl_at = fis_is_decl(toks)
    if decl_at is not None:
        k = decl_at
        while k < n:
            if toks[k][0] == "id":
                s.locals.add(toks[k][1])
                s.decl_pos.add(k)
                s.add(s.defs, toks[k][1], "")
            d = 0
            while k < n:
                v = toks[k][1]
                if v in ("(", "[", "{"):
                    d += 1
                elif v in (")", "]", "}"):
                    d -= 1
                elif (v == "," or v == ";") and d == 0:
                    break
                k += 1
            k += 1
            while k < n and toks[k][1] in ("*", "const", "restrict"):
                k += 1
    i = 0
    prev = None
    while i < n:
        kind, v = toks[i]
        if kind != "id" or v in FIS_CTRL or v in FIS_TYPE_WORDS or v in FIS_BASE_TYPES:
            prev = v
            i += 1
            continue
        if prev in ("->", "."):
            prev = v
            i += 1
            continue
        ch = fis_chain(toks, i, n)
        if ch is None:
            return False
        path, idxs, j = ch
        if j < n and toks[j][1] == "(":
            q = fis_match(toks, j, "(", ")")
            if q < 0:
                return False
            name = path
            if "->" in name or "." in name or "[]" in name:
                s.calls.add("(*" + name + ")")
                s.block.append("indirect:" + name)
                s.counter_only = False
            elif (name in FIS_PURE_VEC or name.startswith("__builtin_") or re.fullmatch(r"[A-Z][A-Z0-9_]*", name)
                  or FIS_INTRIN_PURE.fullmatch(name)
                  or name in pure_extra) and i > 0:
                a = j + 1
                while a < q:
                    if toks[a][0] == "id" and toks[a][1] not in ivs and (a == j + 1 or toks[a - 1][1] in (",", "(", "&")):
                        s.read_roots.add(toks[a][1])
                    a += 1
            elif name in FIS_PURE_SCALAR:
                s.block.append("math:" + name)
            else:
                s.calls.add(name)
                s.block.append("call:" + name)
                s.counter_only = False
                if q == j + 1 or (q == j + 2 and toks[j + 1][1] == "void"):
                    s.call_roots.add("*")
                a = j + 1
                while a < q:
                    if toks[a][1] == "&" and a + 1 < q and toks[a + 1][0] == "id":
                        s.call_roots.add(toks[a + 1][1])
                    elif toks[a][0] == "id" and (a == j + 1 or toks[a - 1][1] in (",", "(")) and toks[a][1] not in ivs:
                        s.call_roots.add(toks[a][1])
                    a += 1
            prev = ")"
            i = j + 1
            continue
        if path in FIS_BASE_TYPES:
            prev = v
            i += 1
            continue
        root = re.match(r"\w+", path).group(0)
        if root not in ivs:
            s.roots.add(root)
        unary_star = i not in s.decl_pos and i > 0 and toks[i - 1][1] == "*" and (i == 1 or toks[i - 2][0] == "op" and toks[i - 2][1] not in (")", "]"))
        pre_inc = i > 0 and toks[i - 1][1] in ("++", "--")
        nxt = toks[j][1] if j < n else ""
        post_inc = nxt in ("++", "--")
        after = j + (1 if post_inc else 0)
        assign = toks[after][1] if after < n and toks[after][1] in FIS_ASSIGN else None
        ix_text = "|".join(" ".join(t[1] for t in toks[a:b]) for a, b in idxs)
        if unary_star:
            star_path = "*" + path
            if assign or pre_inc:
                s.add(s.defs, star_path, "")
                s.work.append("store:" + star_path)
                s.counter_only = False
            if assign != "=":
                s.add(s.uses, star_path, "")
            if post_inc:
                s.add(s.defs, path, "")
                s.add(s.uses, path, "")
        else:
            is_def = bool(assign) or pre_inc or post_inc
            if is_def:
                s.add(s.defs, path, ix_text)
                if idxs:
                    s.work.append("store:" + path)
                    s.counter_only = False
                elif "->" in path or "." in path:
                    s.work.append("acc:" + path)
                    s.counter_only = False
                elif assign in ("+=", "-=", "*=", "|=", "&=", "^=") or pre_inc or post_inc:
                    rhs_ids = []
                    if assign:
                        e = after + 1
                        while e < n and toks[e][1] != ";":
                            rhs_ids.append(toks[e])
                            e += 1
                    if assign and not all(t[0] == "num" for t in rhs_ids):
                        s.work.append("reduce:" + path)
                        s.counter_only = False
            if assign != "=" or not is_def:
                s.add(s.uses, path, ix_text)
            if is_def and idxs and s.cond:
                s.block.append("cond-store:" + path)
        for a, b in idxs:
            sub = toks[a:b]
            if any(t[1] in ("[", "->", "(") for t in sub):
                s.block.append(("scatter:" if (assign or pre_inc or post_inc) else "gather:") + path)
            else:
                for t in sub:
                    if t[0] == "id" and t[1] not in ivs and not invariant_ok(t[1]) and t[1] not in FIS_CTRL:
                        s.block.append(("scatter:" if (assign or pre_inc or post_inc) else "gather:") + path)
                        break
        if assign == "=" and not idxs and after + 1 < n:
            r = fis_chain(toks, after + 1, n) if toks[after + 1][0] == "id" else None
            if r and r[0].startswith(path + "->") and r[2] < n and toks[r[2]][1] == ";":
                s.block.append("chase:" + path)
        prev = v
        i += 1
    if s.kind == "switch":
        s.block.append("switch")
    for path, ixs in s.defs.items():
        if "[]" in path and any(u != d for d in ixs for u in s.uses.get(path, ()) if d and u):
            s.block.append("carried:" + path)
    return True


def fis_tarjan(n, adj):
    index, low, onst, st, out = [None] * n, [0] * n, [False] * n, [], []
    counter = [0]

    def visit(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        st.append(v)
        onst[v] = True
        for w in adj[v]:
            if index[w] is None:
                visit(w)
                low[v] = min(low[v], low[w])
            elif onst[w]:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = st.pop()
                onst[w] = False
                comp.append(w)
                if w == v:
                    break
            out.append(sorted(comp))
    for v in range(n):
        if index[v] is None:
            visit(v)
    return out


def fis_loop_parts(kw, header, body):
    if kw not in ("for", "while"):
        return None
    p = body.find("(")
    d = 0
    q = -1
    for j in range(p, len(body)):
        if body[j] == "(":
            d += 1
        elif body[j] == ")":
            d -= 1
            if d == 0:
                q = j
                break
    if p < 0 or q < 0:
        return None
    rest = body[q + 1:].lstrip()
    if not rest.startswith("{") or not body.rstrip().endswith("}"):
        return None
    return rest[1:rest.rstrip().rfind("}")]


FIS_WHY = [None]


def fission(kw, header, body, pure_extra=frozenset()):
    inner = fis_loop_parts(kw, header, body)
    if inner is None:
        FIS_WHY[0] = "if inner is none"
        return None
    toks = fis_tokens(inner)
    if not toks:
        FIS_WHY[0] = "if not toks"
        return None
    stmts_t = fis_split(toks)
    if not stmts_t or len(stmts_t) < 2 or len(stmts_t) > 60:
        FIS_WHY[0] = "if not stmts_t or len stmts_t  < 2 or len stmts_t"
        return None
    if any(t[0][1] in ("for", "while", "do") for t in stmts_t):
        FIS_WHY[0] = "if any t 0 1  in  for   while   do  for t in stmts"
        return None
    for t in stmts_t:
        text = " ".join(v for _, v in t)
        if t[0][1] == "switch":
            text = re.sub(r"\bbreak\s*;", ";", text)
        if FIS_EXIT_RE.search(text):
            FIS_WHY[0] = "if fis_exit_re search text"
            return None
    ivs = set()
    cond_ids = set()
    if kw == "for":
        parts = header.split(";")
        if len(parts) != 3:
            FIS_WHY[0] = "if len parts  != 3"
            return None
        for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|[-+]=)|(?:\+\+|--)\s*([A-Za-z_]\w*)", parts[2]):
            ivs.add(m.group(1) or m.group(2))
        if not ivs:
            FIS_WHY[0] = "if not ivs"
            return None
        cond_ids = set(re.findall(r"(?<![\w.>])([A-Za-z_]\w*)(?!\s*\()", parts[1])) - FIS_CTRL
    else:
        cond_ids = set(re.findall(r"(?<![\w.>])([A-Za-z_]\w*)(?!\s*\()", header)) - FIS_CTRL
    ctrl = set()
    if kw == "while":
        for si, t in enumerate(stmts_t):
            text = " ".join(v for _, v in t)
            m = re.fullmatch(r"(?:(\w+) (?:\+\+|--)|(?:\+\+|--) (\w+)|(\w+) [-+]= [\w ()*+-]+) ;", text)
            if m:
                v = m.group(1) or m.group(2) or m.group(3)
                if v in cond_ids:
                    ivs.add(v)
                    ctrl.add(si)
        if not ivs or any(si < len(stmts_t) - len(ctrl) for si in ctrl):
            FIS_WHY[0] = "if not ivs or any si < len stmts_t    len ctrl  fo"
            return None
    step_ids = set()
    if kw == "for":
        step_ids = set(re.findall(r"(?<![\w.>])([A-Za-z_]\w*)", header.split(";")[2])) - ivs - FIS_CTRL
    for si in ctrl:
        step_ids |= set(v for k, v in stmts_t[si] if k == "id") - ivs
    stmts = []
    for si, t in enumerate(stmts_t):
        if si in ctrl:
            continue
        stmts.append(FisStmt(len(stmts), t))
    if len(stmts) < 2:
        FIS_WHY[0] = "if len stmts  < 2"
        return None
    body_defs = set()
    for s in stmts:
        for path in (fis_quick_defs(s.toks)):
            body_defs.add(path)

    def invariant_ok(name):
        return name not in body_defs

    if step_ids & body_defs:
        FIS_WHY[0] = "if step_ids   body_defs"
        return None

    for s in stmts:
        if not fis_analyze(s, ivs, invariant_ok, pure_extra):
            FIS_WHY[0] = "if not fis_analyze s  ivs  invariant_ok  pure_extr"
            return None
    body_locals = set().union(*[s.locals for s in stmts])
    for s in stmts:
        for path in list(s.defs) + list(s.uses):
            root = re.match(r"\*?(\w+)", path).group(1)
            if root in body_locals and ("[]" in path or path.startswith("*") or "->" in path):
                s.block.append(("scatter:" if path in s.defs else "gather:") + path)
    for s in stmts:
        for p in s.defs:
            rp = re.match(r"\*?(\w+)", p).group(1)
            if (rp in ivs or rp in cond_ids) and not p.startswith("*") and "[]" not in p and "->" not in p and "." not in p:
                FIS_WHY[0] = "if  rp in ivs or rp in cond_ids  and not p startsw"
                return None
    n = len(stmts)
    adj = [set() for _ in range(n)]
    locals_ = {}
    for s in stmts:
        for v in s.locals:
            locals_[v] = s.idx
    paths = set()
    for s in stmts:
        paths |= set(s.defs) | set(s.uses)
    expand = set()
    for p in paths:
        if p in ivs:
            continue
        acc = [(s.idx, ix, True) for s in stmts for ix in s.defs.get(p, ())] + \
              [(s.idx, ix, False) for s in stmts for ix in s.uses.get(p, ())]
        if not any(d for _, _, d in acc):
            continue
        for a, ia, da in acc:
            for b, ib, db in acc:
                if a >= b or not (da or db):
                    continue
                if p in locals_:
                    if locals_[p] == a and da and not db:
                        adj[a].add(b)
                        expand.add(p)
                        continue
                    adj[a].add(b)
                    adj[b].add(a)
                    continue
                same = "[]" in p and ia == ib and ia != "" and all(x not in ia for x in ("[", "->", "("))
                adj[a].add(b)
                if not same:
                    adj[b].add(a)
    for a in stmts:
        for b in stmts:
            adr = {re.match(r"\*?(\w+)", p).group(1) for p in a.defs}
            bdr = {re.match(r"\*?(\w+)", p).group(1) for p in b.defs}
            if a.idx < b.idx and ((a.call_roots & b.roots) or (b.call_roots & a.roots) or (a.calls and b.calls)
                                  or (a.read_roots & bdr) or (b.read_roots & adr)
                                  or ("*" in a.call_roots and b.roots) or ("*" in b.call_roots and a.roots)):
                adj[a.idx].add(b.idx)
                adj[b.idx].add(a.idx)
    comps = fis_tarjan(n, adj)
    if len(comps) < 2:
        FIS_WHY[0] = "if len comps  < 2"
        return None
    comp_of = {}
    for ci, c in enumerate(comps):
        for v in c:
            comp_of[v] = ci
    pieces = []
    for c in sorted(comps, key=lambda c: c[0]):
        work = [w for v in c for w in stmts[v].work]
        block = [b for v in c for b in stmts[v].block]
        counter = all(stmts[v].counter_only for v in c)
        pieces.append({"stmts": c, "work": work, "block": block, "counter": counter,
                       "text": [stmts[v].text[:60] for v in c]})
    links = set()
    for a in range(n):
        for b in adj[a]:
            if comp_of[a] != comp_of[b]:
                links.add((comp_of[a], comp_of[b]))
    order = {tuple(c): k for k, c in enumerate(sorted(comps, key=lambda c: c[0]))}
    piece_of = {v: order[tuple(c)] for c in comps for v in c}
    for k, pc in enumerate(pieces):
        pc["defs"] = set(p for v in pc["stmts"] for p in stmts[v].defs)
        pc["arr_reads"] = set(p for v in pc["stmts"] for p in stmts[v].uses if "[]" in p or p.startswith("*"))
        pc["elems"] = set(re.match(r"[^\[]*\[\]", p).group(0) for v in pc["stmts"] for p in list(stmts[v].defs) + list(stmts[v].uses)
                          if "[]" in p and re.match(r"[^\[]*\[\]", p).group(0) != p)
        pc["linked"] = any(piece_of[a] == k or piece_of[b] == k for a in range(n) for b in adj[a] if piece_of[a] != piece_of[b])
    blocked = [p for p in pieces if p["block"]]
    bdefs = set().union(*[p["defs"] for p in blocked]) if blocked else set()
    belems = set().union(*[p["elems"] for p in blocked]) if blocked else set()
    clean = [p for p in pieces if not p["block"] and any(w.startswith(("store:", "reduce:")) for w in p["work"]) and not p["counter"]
             and not (p["defs"] & bdefs) and not (p["elems"] & belems) and (not p["linked"] or (p["arr_reads"] - bdefs))]
    if not clean or not blocked:
        FIS_WHY[0] = "if not clean or not blocked"
        return None
    cross_locals = sorted(v for v in expand if any(comp_of[locals_[v]] != comp_of[s.idx] for s in stmts if v in s.uses))
    return {"pieces": pieces, "clean": clean, "blocked": blocked, "independent": not links, "expand": cross_locals}


def fis_quick_defs(toks):
    out = set()
    n = len(toks)
    for i, (k, v) in enumerate(toks):
        if k == "op" and (v in FIS_ASSIGN or v in ("++", "--")):
            j = i - 1
            while j >= 0 and toks[j][1] in ("]",):
                d = 0
                while j >= 0:
                    if toks[j][1] == "]":
                        d += 1
                    elif toks[j][1] == "[":
                        d -= 1
                        if d == 0:
                            break
                    j -= 1
                j -= 1
            if j >= 0 and toks[j][0] == "id":
                out.add(toks[j][1])
            if v in ("++", "--") and i + 1 < n and toks[i + 1][0] == "id":
                out.add(toks[i + 1][1])
    d = fis_is_decl(toks)
    if d is not None:
        out.add(toks[d][1])
    return out


def fission_summary(r):
    c = r["clean"][0]
    b = r["blocked"][0]
    cw = sorted(set(w.split(":", 1)[1] for w in c["work"]))[:3]
    bb = sorted(set(b["block"]))[:3]
    tail = "independent" if r["independent"] else ("expand " + ",".join(r["expand"][:3]) + " into a temp array" if r["expand"] else "ordered passes")
    return f"{len(r['pieces'])} pieces: clean {{{','.join(cw)}}} | blocked by {{{','.join(bb)}}} ({tail})"


def fis_decl_names(toks):
    out = set()
    n = len(toks)
    for pos in range(n):
        if pos and toks[pos - 1][1] not in (";", "{", "}") and not (toks[pos - 1][1] == "(" and pos > 1 and toks[pos - 2][1] == "for"):
            continue
        k = fis_is_decl(toks[pos:])
        if k is None:
            continue
        k += pos
        while k < n:
            if toks[k][0] == "id":
                out.add(toks[k][1])
            d = 0
            while k < n:
                v = toks[k][1]
                if v in ("(", "[", "{"):
                    d += 1
                elif v in (")", "]", "}"):
                    d -= 1
                    if d < 0:
                        break
                elif v in (",", ";") and d == 0:
                    break
                k += 1
            if k >= n or toks[k][1] != ",":
                break
            k += 1
            while k < n and toks[k][1] in ("*", "const", "restrict"):
                k += 1
    return out


def fis_pure_funcs(text, funcs):
    info = {}
    for a, b, name in funcs:
        sig = max(text.rfind("}", 0, a), text.rfind(";", 0, a)) + 1
        head = text[sig:a]
        if not re.search(r"\bstatic\b", head):
            continue
        toks = fis_tokens(text[a + 1:b])
        ptoks = fis_tokens(head[head.find("(") + 1:head.rfind(")")]) if "(" in head else []
        if toks is None or ptoks is None:
            continue
        locs = set()
        for part in " ".join(v for _, v in ptoks).split(","):
            w = part.split()
            if w and "*" not in w and "[" not in part and w[-1] not in FIS_CTRL:
                locs.add(w[-1])
        bad = False
        callees = set()
        n = len(toks)
        if any(v == "static" for _, v in toks):
            continue
        locs |= fis_decl_names(toks)
        for i, (k, v) in enumerate(toks):
            if k == "op" and (v in FIS_ASSIGN or v in ("++", "--")):
                j = i - 1 if v in FIS_ASSIGN or (i > 0 and toks[i - 1][0] == "id") else i + 1
                if v in ("++", "--") and not (i > 0 and toks[i - 1][0] == "id"):
                    j = i + 1
                if j < 0 or j >= n or toks[j][0] != "id" or toks[j][1] not in locs:
                    bad = True
                    break
                if j > 0 and toks[j - 1][1] in ("->", ".", "*") and not (toks[j - 1][1] == "*" and j > 1 and toks[j - 2][0] == "id"):
                    bad = True
                    break
            if k == "id" and i + 1 < n and toks[i + 1][1] == "(" and v not in FIS_CTRL and (i == 0 or toks[i - 1][1] not in ("->", ".")):
                if v in ("memcpy", "memmove"):
                    if not (toks[i + 2][1] == "&" and toks[i + 3][1] in locs or toks[i + 2][1] in locs):
                        bad = True
                        break
                elif not (v in FIS_PURE_VEC or v in FIS_PURE_SCALAR or v.startswith("__builtin_") or re.fullmatch(r"[A-Z][A-Z0-9_]*", v)):
                    callees.add(v)
        if not bad:
            info[name] = callees
    pure = set()
    changed = True
    while changed:
        changed = False
        for name, cs in info.items():
            if name not in pure and cs <= pure | {name}:
                pure.add(name)
                changed = True
    return pure


DM_TAG_RE = re.compile(r"(?:^|_)(?:type|kind|tag|op|opcode|cls|class|variant|tid|ty)s?$", re.I)
DM_FLAG_RE = re.compile(r"(?:^|_)(?:active|alive|used|valid|dead|deleted|removed|enabled|disabled|free|visible|dirty|live|"
                        r"in_use|inuse|occupied|empty|tomb\w*|skip|hidden|done|alloc(?:ated)?|present|set)$", re.I)
DM_KNOWN_EXT = set("memcpy memmove memset memcmp strlen strcmp strncmp strchr strrchr strstr malloc calloc realloc free "
                   "printf fprintf snprintf sprintf puts putchar fputs fwrite fread abort assert qsort bsearch".split())
DM_ACC_RE = re.compile(r"(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)((?:\s*\[(?:[^\[\]]|\[[^\[\]]*\])*\])+)")


def dm_dims(sub):
    out, d, cur = [], 0, []
    for ch in sub:
        if ch == "[":
            if d == 0:
                cur = []
            else:
                cur.append(ch)
            d += 1
        elif ch == "]":
            d -= 1
            if d == 0:
                out.append("".join(cur).strip())
            else:
                cur.append(ch)
        elif d > 0:
            cur.append(ch)
    return out


def dm_has(v, e):
    return re.search(r"(?<![\w.>])" + re.escape(v) + r"\b(?!\s*(?:->|\.|\[|\())", e) is not None


def dm_mult(v, e):
    return re.search(r"(?<![\w.>])" + re.escape(v) + r"\s*\)?\s*(?:\*|<<)|\*\s*\(?\s*" + re.escape(v) + r"\b", e) is not None


def dm_stride_class(v, dims):
    hit = [k for k, e in enumerate(dims) if dm_has(v, e)]
    if not hit:
        return None
    if any(k < len(dims) - 1 for k in hit):
        return "strided"
    return "strided" if dm_mult(v, dims[-1]) else "unit"


def dm_for_ivs(header):
    parts = header.split(";")
    if len(parts) != 3:
        return set()
    out = set()
    for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|[-+]=)|(?:\+\+|--)\s*([A-Za-z_]\w*)", parts[2]):
        out.add(m.group(1) or m.group(2))
    return out


def dm_bound(header, defs, eval_dim):
    parts = header.split(";")
    if len(parts) != 3:
        return None
    m = re.search(r"[<>]=?\s*\(?\s*([A-Za-z_]\w*|\d+)\s*\)?\s*$", parts[1].strip())
    return eval_dim(m.group(1), defs) if m else None


def dm_body(text):
    p = text.find("(")
    d = 0
    for j in range(p, len(text)):
        if text[j] == "(":
            d += 1
        elif text[j] == ")":
            d -= 1
            if d == 0:
                rest = text[j + 1:].strip()
                if rest.startswith("{") and rest.endswith("}"):
                    return rest[1:-1]
                return rest
    return ""


def dm_nest(inner, parent, defs, eval_dim):
    j = dm_for_ivs(inner["rawheader"])
    i = dm_for_ivs(parent["rawheader"])
    if len(j) != 1 or len(i) != 1 or inner["kw"] != "for" or parent["kw"] != "for":
        return None
    j, i = next(iter(j)), next(iter(i))
    if i == j:
        return None
    bi = dm_bound(inner["rawheader"], defs, eval_dim)
    bo = dm_bound(parent["rawheader"], defs, eval_dim)
    if bi is not None and bo is not None and bi <= 8 and bo <= 8:
        return None
    if bi is not None and bi <= 16:
        return None
    ph = [x.strip() for x in parent["rawheader"].split(";")]
    if len(ph) == 3:
        bm = re.match(r"\w+\s*<=?\s*(.+)$", ph[1])
        st = re.sub(r"^.*?=", "", ph[0], count=1)
        if bm and re.search(r"(?<![\w.>])" + re.escape(bm.group(1).strip()) + r"(?![\w(])", st):
            return None
    body = dm_body(inner["body"])
    if re.search(r"\[\s*\w+\s*(?:\+\+|--)\s*\]|\[\s*(?:\+\+|--)\s*\w+\s*\]", body):
        return None
    fixes, breaks = [], []
    for m in DM_ACC_RE.finditer(body):
        dims = dm_dims(m.group(2))
        cj, ci = dm_stride_class(j, dims), dm_stride_class(i, dims)
        name = " ".join(m.group(1).split()) + "".join("[" + " ".join(d.split()) + "]" for d in dims)
        if cj == "strided" and ci == "unit":
            fixes.append(name)
        elif cj == "unit" and ci == "strided":
            breaks.append(name)
    if not fixes:
        return None
    pb = dm_body(parent["body"]).strip()
    perfect = pb.replace(" ", "").replace("\n", "").replace("\t", "") == \
        inner["body"].strip().replace(" ", "").replace("\n", "").replace("\t", "")
    if breaks:
        return ("cache-tiling", f"inner {j}: {','.join(sorted(set(fixes))[:2])} strided, {','.join(sorted(set(breaks))[:2])} unit; "
                                f"no loop order makes both unit-stride: block {i}/{j}")
    if not perfect:
        return None
    return ("loop-interchange", f"{','.join(sorted(set(fixes))[:3])} walk the slow dimension with inner {j}: swap the {i}/{j} loops")


def dm_varies(base, L, iv):
    if iv and re.search(r"\[[^\]]*\b" + re.escape(iv) + r"\b[^\]]*\]", base):
        return True
    root = re.match(r"\s*\(?\s*\*?\s*([A-Za-z_]\w*)", base)
    if not root:
        return False
    r = re.escape(root.group(1))
    return bool(re.search(r"(?<![\w.>])" + r + r"\s*(?:=(?!=)|\+\+|--|\+=|-=)|(?:\+\+|--)\s*" + r + r"\b", L["body"]))


def dm_kind_dispatch(L, iv):
    body = dm_body(L["body"])
    hdr = " ".join(L["rawheader"].split())
    if L["kw"] == "for" and re.fullmatch(r"\s*;\s*;\s*", L["rawheader"]) or L["kw"] == "while" and re.fullmatch(r"\s*(?:1|true|TRUE)\s*", hdr):
        return None
    for m in re.finditer(r"(?<![\w.>])switch\s*\(", body):
        q = dm_close(body, m.end() - 1)
        if q < 0:
            continue
        subj = body[m.end():q]
        if re.search(r"\*\s*\w+\s*\+\+|\+\+\s*\w+", subj):
            continue
        fld = re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)", subj)
        if not fld or not DM_TAG_RE.search(fld[-1]):
            continue
        sb = re.sub(r"^.*?=(?!=)", "", subj) if re.search(r"(?<![=!<>])=(?!=)", subj) else subj
        if not dm_varies(sb, L, iv):
            continue
        ob = body.find("{", q)
        cb = dm_close(body, ob, "{", "}") if ob >= 0 else -1
        ncase = len(re.findall(r"\bcase\b", body[ob:cb])) if cb > 0 else 0
        if ncase >= 3:
            return f"switch on {' '.join(subj.split())[:40]} with {ncase} cases per element: bucket or sort the elements by kind, then one tight loop per kind"
    chain = {}
    for m in re.finditer(r"([A-Za-z_][\w\[\]]*(?:(?:->|\.)\w+(?:\[[^\]]*\])*)*?)\s*(?:->|\.)\s*([A-Za-z_]\w*)\s*==\s*([A-Z_][A-Z0-9_]*)\b", body):
        if DM_TAG_RE.search(m.group(2)) and dm_varies(m.group(1), L, iv):
            chain.setdefault(m.group(2), set()).add(m.group(3))
    for f, vals in chain.items():
        if len(vals) >= 3:
            return f"if-chain on .{f} over {len(vals)} kinds per element: bucket or sort by kind first"
    return None


def dm_close(s, i, o="(", c=")"):
    d = 0
    for j in range(i, len(s)):
        if s[j] == o:
            d += 1
        elif s[j] == c:
            d -= 1
            if d == 0:
                return j
    return -1


def dm_flag_filter(L, iv, ftext=""):
    if not iv or L["kw"] != "for":
        return None
    body = dm_body(L["body"]).strip()
    ivr = re.escape(iv)
    local = None
    dm = re.match(r"(?:const\s+)?(?:struct\s+)?\w+\s*\*?\s*(\w+)\s*=\s*&?\s*([A-Za-z_][\w.>\-]*)\s*\[\s*" + ivr + r"\s*\]\s*;", body)
    if dm:
        local, arr = dm.group(1), dm.group(2)
        body = body[dm.end():].strip()
    m = re.match(r"if\s*\(", body)
    if not m:
        return None
    q = dm_close(body, m.end() - 1)
    if q < 0:
        return None
    cond = " ".join(body[m.end():q].split())
    after = body[q + 1:].strip()
    is_skip = bool(re.match(r"continue\s*;", after))
    rest_after_if = ""
    if not is_skip:
        if not after.startswith("{"):
            return None
        cb = dm_close(after, 0, "{", "}")
        rest_after_if = after[cb + 1:].strip() if cb > 0 else "x"
        if rest_after_if:
            return None
    if local:
        pat = r"(!\s*)?\(?\s*" + re.escape(local) + r"\s*(?:->|\.)\s*(\w+)\s*\)?(\s*(==|!=)\s*(0|NULL|false|true))?"
        where = arr + "[]"
    else:
        pat = r"(!\s*)?\(?\s*([A-Za-z_][\w.>\-]*)\s*\[\s*" + ivr + r"\s*\]\s*(?:(?:->|\.)\s*(\w+))?\s*\)?(\s*(==|!=)\s*(0|NULL|false|true))?"
        where = None
    cm = re.fullmatch(pat, cond)
    if not cm:
        return None
    if local:
        field = cm.group(2)
    else:
        where = cm.group(2) + "[]"
        field = cm.group(3)
    if field and not DM_FLAG_RE.search(field) and not cm.group(3 if local else 4):
        return None
    if not field:
        bn = re.escape(where[:-2].split("->")[-1].split(".")[-1])
        decl = re.search(r"([A-Za-z_][\w\s]*?)(\*+)?\s*\b" + bn + r"\s*(\[)?", ftext or "")
        ptrs = (len(decl.group(2) or "") + (1 if decl.group(3) else 0)) if decl else 0
        tyn = decl.group(1).split()[-1] if decl and decl.group(1).split() else ""
        if ptrs < 2 and tyn != "JSValue" and not re.search(re.escape(where[:-2]) + r"\s*\[\s*" + ivr + r"\s*\]\s*->", body):
            return None
    what = f".{field}" if field else " (empty slot)"
    shape = "skip" if is_skip else "guard"
    return f"{shape} on {where}{what} per element: keep a dense array or index list of the live entries and loop over that"


def dm_lut_size(base, text, defs, eval_dim):
    m = re.search(r"(?:^|[;{}\n])\s*(?:static\s+)?(?:const\s+)?(?:unsigned\s+|signed\s+)?[A-Za-z_]\w*(?:\s+const)?\s+" + re.escape(base) +
                  r"\s*((?:\[[^\]]*\])+)\s*(?:=|;)", text)
    if not m:
        return None
    total = 1
    for d in re.findall(r"\[([^\]]*)\]", m.group(1)):
        v = eval_dim(d, defs) if d.strip() else None
        if v is None:
            return None
        total *= v
    return total


def dm_random_gather(L, iv, ftext, text, defs, eval_dim):
    if not iv:
        return None
    body = dm_body(L["body"])
    ivr = re.escape(iv)
    hits = []
    idx_of = {}
    for m in re.finditer(r"(?<![\w.>])(?:const\s+)?(?:\w+\s+)?(\w+)\s*=\s*(?:\(\s*\w+\s*\)\s*)?([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*" + ivr +
                         r"\s*(?:[-+]\s*\d+\s*)?\]\s*(&\s*(?:0x[0-9a-fA-F]+|\d+))?\s*;", body):
        idx_of[m.group(1)] = (" ".join(m.group(2).split()), m.group(3))
    for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*(?:\(\s*\w+\s*\)\s*)?([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*" + ivr +
                         r"\s*(?:[-+]\s*\d+\s*)?\]\s*(&\s*(?:0x[0-9a-fA-F]+|\d+))?\s*\]", body):
        hits.append((" ".join(m.group(1).split()), " ".join(m.group(2).split()), m.group(3), m.end()))
    for k, (src, mask) in idx_of.items():
        for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+)*)\s*\[\s*" + re.escape(k) + r"\s*\]", body):
            hits.append((" ".join(m.group(1).split()), src, mask, m.end()))
    out = []
    for base, idx, mask, end in hits:
        if base == idx:
            continue
        if mask:
            mv = int(mask.replace("&", "").strip(), 0)
            if mv < 4096:
                continue
        lut = dm_lut_size(base.split("->")[-1].split(".")[-1], text, defs, eval_dim)
        if lut is not None and lut <= 4096:
            continue
        root = idx.split("->")[-1].split(".")[-1]
        if re.search(r"\b(?:unsigned\s+char|uint8_t|int8_t|signed\s+char|char|u8|BYTE)\s*(?:const\s*)?\*\s*(?:const\s+)?(?:restrict\s+)?" + re.escape(root) + r"\b", ftext) or \
                re.search(r"\b(?:unsigned\s+char|uint8_t|int8_t|char|u8)\s+" + re.escape(root) + r"\s*\[", ftext):
            continue
        rest = body[end:end + 12]
        kind = "scatter" if re.match(r"\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|\s*(?:\+\+|--)", rest) else "gather"
        out.append(f"{kind} {base}[{idx}[{iv}]]")
    if not out:
        return None
    return sorted(set(out))[:3]


def dm_loop_arrays(body, iv):
    reads, writes = {}, {}
    for m in DM_ACC_RE.finditer(body):
        base = " ".join(m.group(1).split())
        dims = tuple(" ".join(d.split()) for d in dm_dims(m.group(2)))
        rest = body[m.end():m.end() + 12]
        tgt = writes if re.match(r"\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|\s*(?:\+\+|--)", rest) else reads
        tgt.setdefault(base, set()).add(dims)
    return reads, writes


def dm_fusion(loops, s, assigned_names, exit_re):
    out = {}
    fl = sorted([L for L in loops if L["kw"] == "for" and L["inner"]], key=lambda L: L["start"])
    for a, b in zip(fl, fl[1:]):
        if a["func"] != b["func"] or a["depth"] != b["depth"] or a["stop"] >= b["start"]:
            continue
        gap = s[a["stop"] + 1:b["start"]]
        if gap.strip().strip(";").strip():
            continue
        ha = re.sub(r"\s+", "", a["rawheader"])
        hb = re.sub(r"\s+", "", b["rawheader"])
        if ha != hb or ha.startswith(";"):
            continue
        iv = dm_for_ivs(a["rawheader"])
        if len(iv) != 1:
            continue
        iv = next(iter(iv))
        ba, bb = dm_body(a["body"]), dm_body(b["body"])
        if exit_re.search(ba) or exit_re.search(bb):
            continue
        ra, wa = dm_loop_arrays(ba, iv)
        rb, wb = dm_loop_arrays(bb, iv)
        same_ix = ((iv,),)
        bad = False
        for w, r in ((wa, rb), (wb, ra), (wa, wb)):
            for base, ixs in w.items():
                other = r.get(base, set())
                if other and (ixs | other) - set(same_ix):
                    bad = True
        sa = assigned_names(ba) - {iv}
        sb = assigned_names(bb) - {iv}
        ida = set(re.findall(r"(?<![\w.>])([A-Za-z_]\w*)", ba))
        idb = set(re.findall(r"(?<![\w.>])([A-Za-z_]\w*)", bb))
        scal_a = {x for x in sa if x not in wa}
        scal_b = {x for x in sb if x not in wb}
        decl_a = set(re.findall(r"(?<![\w.>])(?:[A-Za-z_]\w*)\s+\**\s*([A-Za-z_]\w*)\s*=", ba))
        decl_b = set(re.findall(r"(?<![\w.>])(?:[A-Za-z_]\w*)\s+\**\s*([A-Za-z_]\w*)\s*=", bb))
        if (scal_a - decl_a) & idb or (scal_b - decl_b) & ida:
            bad = True
        if bad:
            continue
        shared = sorted((set(ra) | set(wa)) & (set(rb) | set(wb)))
        if not shared:
            continue
        out[id(a)] = f"next loop (line {b['line']}) has the same header and shares {','.join(shared[:4])}: fuse into one pass"
    return out


def dm_enums(s, defs, eval_dim):
    out = {}
    for m in re.finditer(r"\benum\s+(\w+)?\s*\{([^{}]*)\}\s*(\w+)?", s):
        vals, cur, ok = {}, -1, True
        for part in m.group(2).split(","):
            part = part.strip()
            if not part:
                continue
            em = re.match(r"([A-Za-z_]\w*)\s*(?:=\s*(.+))?$", part, re.S)
            if not em:
                ok = False
                break
            if em.group(2):
                ex = em.group(2)
                ex = re.sub(r"\b([A-Za-z_]\w*)\b", lambda x: str(vals[x.group(1)]) if x.group(1) in vals else x.group(1), ex)
                v = eval_dim(ex, defs)
                if v is None:
                    try:
                        v = int(eval(ex, {"__builtins__": {}})) if re.fullmatch(r"[\dxXa-fA-F\s+\-*()<|&~]+", ex) else None
                    except Exception:
                        v = None
                if v is None:
                    ok = False
                    break
                cur = v
            else:
                cur += 1
            vals[em.group(1)] = cur
        if not ok or not vals:
            continue
        lo, hi = min(vals.values()), max(vals.values())
        for nm in (m.group(1), m.group(3)):
            if nm:
                out[nm] = (lo, hi)
    return out


def dm_straddle(size):
    if size < 16 or size % 64 == 0 or 64 % size == 0 or size > 256:
        return None
    period = 64 // math.gcd(size, 64)
    need = (size + 63) // 64
    extra = 0
    for k in range(period):
        st = (k * size) % 64
        if (st + size + 63) // 64 > need:
            extra += 1
    return extra / period


def scan_file(path, defs):
    raw = open(path, errors="replace").read()
    s = blank_comments_and_strings(raw)
    relpath = os.path.relpath(path, ROOT)
    line_of = [0] * (len(s) + 1)
    ln = 1
    for i, ch in enumerate(s):
        line_of[i] = ln
        if ch == "\n":
            ln += 1
    line_of[len(s)] = ln
    funcs = []
    depth = 0
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "{":
            if depth == 0:
                end = match_paren(s, i, "{", "}")
                k = i - 1
                while k >= 0 and s[k] in " \t\n":
                    k -= 1
                pre = s[max(0, k - 400):k + 1]
                pre = re.sub(r"__attribute__\s*\(\(.*?\)\)\s*$", "", pre)
                if end > 0 and pre.rstrip().endswith(")"):
                    close = len(pre.rstrip()) - 1
                    op = match_paren(pre[::-1], len(pre) - 1 - close, ")", "(")
                    head = pre[:len(pre) - 1 - op] if op >= 0 else ""
                    m = re.search(r"([A-Za-z_]\w*)\s*$", head)
                    if m and m.group(1) not in KEYWORDS:
                        funcs.append((i, end, m.group(1)))
                if end > 0:
                    i = end + 1
                    continue
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1

    def func_at(pos):
        for a, b, name in funcs:
            if a <= pos <= b:
                return name, a, b
        return "?", -1, -1

    loops = []
    fis_pure = None
    for m in LOOP_RE.finditer(s):
        kw = m.group(1)
        if kw == "while":
            before = s[:m.start()].rstrip()
            if before.endswith("}"):
                k = match_paren(s[::-1], len(s) - len(before), "}", "{")
                if k >= 0:
                    open_pos = len(s) - 1 - k
                    pre = s[:open_pos].rstrip()
                    if pre.endswith("do"):
                        continue
        if kw in ("for", "while"):
            p = s.find("(", m.end())
            if p < 0 or s[m.end():p].strip():
                continue
            q = match_paren(s, p, "(", ")")
            if q < 0:
                continue
            header = s[p + 1:q]
            e = statement_end(s, q + 1)
        else:
            e = statement_end(s, m.end())
            header = ""
            if e > 0:
                w = re.match(r"\s*while\s*\(", s[e + 1:])
                if w:
                    p = e + 1 + w.end() - 1
                    q = match_paren(s, p, "(", ")")
                    header = s[p + 1:q] if q > 0 else ""
        if e < 0:
            continue
        fname, fa, fb = func_at(m.start())
        body = s[m.start():e + 1]
        loops.append({
            "file": relpath, "line": line_of[m.start()], "end": line_of[e], "kw": kw,
            "func": fname, "fa": fa, "fb": fb, "start": m.start(), "stop": e, "rawheader": header,
            "header": " ".join(header.split())[:90], "body": body})
    for L in loops:
        L["depth"] = 1 + sum(1 for o in loops if o is not L and o["start"] < L["start"] and L["stop"] <= o["stop"])
        L["inner"] = not any(o is not L and L["start"] < o["start"] and o["stop"] <= L["stop"] for o in loops)
    fused = dm_fusion(loops, s, assigned_names, FIS_EXIT_RE)
    for L in loops:
        more = {}
        if id(L) in fused:
            more["loop-fusion"] = fused[id(L)]
        if L["inner"]:
            ivs = dm_for_ivs(L["rawheader"]) if L["kw"] == "for" else set()
            iv = next(iter(ivs)) if len(ivs) == 1 else None
            par = [o for o in loops if o is not L and o["start"] < L["start"] and L["stop"] <= o["stop"] and o["depth"] == L["depth"] - 1]
            if par:
                r = dm_nest(L, par[0], defs, eval_dim)
                if r:
                    more[r[0]] = r[1]
            r = dm_kind_dispatch(L, iv)
            if r:
                more["kind-dispatch"] = r
            r = dm_flag_filter(L, iv, s[max(0, s.rfind("}", 0, L["fa"]) + 1):L["start"]] if L["fa"] >= 0 else "")
            if r:
                more["flag-filter"] = r
            if iv and L["fa"] >= 0:
                ftext = s[max(0, s.rfind("}", 0, L["fa"]) + 1):L["start"]]
                r = dm_random_gather(L, iv, ftext, s, defs, eval_dim)
                if r:
                    more["random-gather"] = ", ".join(r)
                xc = set()
                lb = dm_body(L["body"])
                for cm_ in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*\(", lb):
                    nm_ = cm_.group(1)
                    if nm_ in KEYWORDS or nm_ in DM_KNOWN_EXT or re.fullmatch(r"[A-Z][A-Z0-9_]*", nm_) or FIS_INTRIN_PURE.fullmatch(nm_) \
                            or nm_ in FIS_PURE_VEC or nm_ in FIS_PURE_SCALAR or nm_.startswith("__builtin_"):
                        continue
                    q_ = match_paren(lb, cm_.end() - 1, "(", ")")
                    if q_ > 0 and re.search(r"\[[^\]]*\b" + re.escape(iv) + r"\b[^\]]*\]", lb[cm_.end():q_]):
                        xc.add(nm_)
                if xc:
                    L["xcalls"] = sorted(xc)
        if more:
            L["more"] = more
    for L in loops:
        L["depth"] = 1 + sum(1 for o in loops if o is not L and o["start"] < L["start"] and L["stop"] <= o["stop"])
        L["inner"] = not any(o is not L and L["start"] < o["start"] and o["stop"] <= L["stop"] for o in loops)
        b = L["body"]
        hb = b.find("{")
        inner_body = b[hb:] if hb >= 0 else b
        L["alloc"] = sorted(set(x.group(1) for x in ALLOC_RE.finditer(b)))
        L["free"] = sorted(set(x.group(1) for x in FREE_RE.finditer(b)))
        L["push"] = sorted(set(x.group(1) for x in PUSH_RE.finditer(b)))
        L["smallcpy"] = sorted(set(x.group(1) + "(" + x.group(3).replace(" ", "") + ")" for x in SMALLCPY_RE.finditer(b)))
        L["libm"] = sorted(set(x.group(1) for x in LIBM_RE.finditer(b)))
        L["fnptr"] = len([x for x in FNPTR_RE.finditer(b) if not re.match(r"\w+\s*\[", x.group(0)) or "(" in x.group(0)])
        L["div"] = len(DIV_RE.findall(inner_body))
        L["jsapi"] = len(JSAPI_RE.findall(b))
        L["lines"] = L["end"] - L["line"] + 1
        L["gist"] = gist_loop_checks(L, inner_body, L["rawheader"], defs)
        if L.get("_aos_all") and L["fa"] >= 0:
            ftext = s[max(0, L["fa"] - 600):L["start"]]
            typed = {}
            for base in L["_aos_all"]:
                tm = re.search(r"\b(?:struct\s+)?([A-Za-z_]\w*)\s*\*\s*(?:const\s+)?(?:restrict\s+)?" + re.escape(base) + r"\b", ftext) or \
                    re.search(r"\b(?:struct\s+)?([A-Za-z_]\w*)\s+" + re.escape(base) + r"\s*\[", ftext)
                if tm and tm.group(1) not in KEYWORDS:
                    typed[base] = tm.group(1)
            L["_aos_all_type"] = typed
            L["_aos_type"] = {k: v for k, v in typed.items() if k in (L.get("_aos") or {})}
        if L["inner"] and L["fa"] >= 0:
            ftext = s[L["fa"]:L["start"]]
            red = set(re.findall(r"(?<![\w.>\]\)])([A-Za-z_]\w*)\s*(?:\+=|-=)", inner_body))
            fpv = []
            for v in red:
                if v == L["lv"] or not L["lv"] or re.search(r"\b" + re.escape(v) + r"\s*\[", inner_body):
                    continue
                if not re.search(r"\b(?:double|float)\s+(?:[^;{}()]*?[,\s*])?" + re.escape(v) + r"\s*[=;,]", ftext):
                    continue
                upd = list(re.finditer(r"(?<![\w.>])" + re.escape(v) + r"\s*(?:\+=|-=)\s*([^;]*);", inner_body))
                uses = len(re.findall(r"(?<![\w.>])" + re.escape(v) + r"\b", inner_body))
                if not upd or uses != len(upd) or re.search(r"\b(?:double|float|[A-Za-z_]\w*_t)\s+" + re.escape(v) + r"\b", inner_body):
                    continue
                if not any(re.search(r"\[[^\]]*\b" + re.escape(L["lv"]) + r"\b", u.group(1)) for u in upd):
                    continue
                fpv.append(v)
            unrolled = re.search(r"\+=\s*(?:[2-9]|\d\d+|[A-Z_][A-Z0-9_]*)\b", L["header"].split(";")[-1])
            if fpv and not unrolled:
                L["gist"]["fp-serial-reduction"] = sorted(fpv)
        if L["inner"]:
            try:
                if fis_pure is None:
                    fis_pure = frozenset(PROJECT.get("pure", [])) | fis_pure_funcs(s, funcs)
                cm = re.search(r"[<>]=?\s*\(?\s*([A-Za-z_]\w*|\d+)\s*\)?\s*$", L["rawheader"].split(";")[1]) if L["kw"] == "for" and L["rawheader"].count(";") == 2 else None
                trip = eval_dim(cm.group(1), defs) if cm else None
                fr = fission(L["kw"], L["rawheader"], L["body"], fis_pure) if trip is None or trip > 16 else None
            except RecursionError:
                fr = None
            if fr:
                L["fission"] = fission_summary(fr)
        L["noinit"] = L["kw"] == "for" and L["rawheader"].lstrip().startswith(";")
        del L["body"], L["rawheader"], L["fb"]
    for L in loops:
        if L["noinit"] and L["lv"] and any(o is not L and o["func"] == L["func"] and o["lv"] == L["lv"]
                                           and o["start"] < L["start"] and o["stop"] < L["start"] for o in loops):
            L["tail"] = True
            for k in ("single-vector-accumulator", "fp-serial-reduction"):
                L["gist"].pop(k, None)
        else:
            L["tail"] = False
    arrays, fnotes = [], []
    file_has = {"mmap": bool(re.search(r"(?<![\w.>])mmap\s*\(", s)),
                "madvise": bool(re.search(r"\b(?:posix_)?madvise\s*\(", s))}
    for a, b, name in funcs:
        body = s[a:b]
        for mm in LOCALARR_RE.finditer(body):
            if mm.group(0).lstrip().startswith(("static", "return", "case")):
                continue
            ty = mm.group(1).strip()
            if ty.split()[-1] in ("return", "goto", "else", "case", "sizeof"):
                continue
            dims = re.findall(r"\[\s*([^]\[]+?)\s*\]", mm.group(3))
            total = 1
            ok = True
            for dsz in dims:
                v = eval_dim(dsz, defs)
                if v is None:
                    ok = False
                    break
                total *= v
            if not ok:
                continue
            esz = 8 if "*" in ty else TYPE_SIZE.get(ty.replace("const ", "").strip(), None)
            exact = esz is not None
            if esz is None:
                esz = TYPE_SIZE.get(ty.split()[-1], 8)
            nbytes = total * esz
            if nbytes >= 256 and "DYN_IO_SCRATCH" not in mm.group(0):
                arrays.append({"file": relpath, "line": line_of[a + mm.start()], "func": name,
                               "decl": " ".join(mm.group(0).split())[:100], "bytes": nbytes, "exact": exact,
                               "type": ty.replace("const ", "").strip()})
        for mm in MEMSET_RE.finditer(body):
            sz = mm.group(2)
            v = eval_dim(sz, defs)
            if v is None:
                sm = re.fullmatch(r"sizeof\s*\(?\s*([A-Za-z_]\w*)\s*\)?", sz.strip())
                if sm:
                    v = next((x["bytes"] for x in arrays if x["func"] == name and re.search(r"\b" + re.escape(sm.group(1)) + r"\s*\[", x["decl"])), None)
            if v is not None and v >= 1024:
                fnotes.append({"check": "big-memset", "file": relpath, "line": line_of[a + mm.start()], "func": name,
                               "what": f"memset {v} B ({' '.join(sz.split())[:40]})"})
        for mm in CALLOC_RE.finditer(body):
            v = None
            parts = [x.strip() for x in mm.group(2).split(",")]
            if mm.group(1) == "calloc" and len(parts) == 2:
                a1, a2 = eval_dim(parts[0], defs), eval_dim(parts[1], defs)
                v = a1 * a2 if a1 is not None and a2 is not None else None
            if v is not None and v >= 1024:
                fnotes.append({"check": "big-memset", "file": relpath, "line": line_of[a + mm.start()], "func": name,
                               "what": f"{mm.group(1)} {v} B"})
        for mm in SORT_RE.finditer(body):
            q = match_paren(body, mm.end() - 1, "(", ")")
            args = " ".join(body[mm.end():q].split())[:80] if q > 0 else ""
            fnotes.append({"check": "sort-call", "file": relpath, "line": line_of[a + mm.start()], "func": name,
                           "what": f"{mm.group(1)}({args})"})
        ws = collections.Counter(x.group(2) for x in WRITE_RE.finditer(body))
        for fd, n in ws.items():
            if n >= 2:
                fnotes.append({"check": "write-sequence", "file": relpath, "line": line_of[a], "func": name,
                               "what": f"{n} write/send calls on {fd}"})
        for mm in ATOMIC_DEFAULT_RE.finditer(body):
            fnotes.append({"check": "seq-cst-atomic", "file": relpath, "line": line_of[a + mm.start()], "func": name,
                           "what": mm.group(1) or mm.group(0)})
        for mm in LISTWALK_RE.finditer(body):
            fnotes.append({"check": "macro-list-walk", "file": relpath, "line": line_of[a + mm.start()], "func": name,
                           "what": mm.group(1)})
    for a, b, name in funcs:
        body = s[a:b]
        for lm in GOTO_LABEL_RE.finditer(body):
            lab = lm.group(1)
            if lab in ("default", "case", "public", "private"):
                continue
            later = [g for g in re.finditer(r"\bgoto\s+" + re.escape(lab) + r"\s*;", body) if g.start() > lm.end()]
            if later:
                fnotes.append({"check": "goto-loop", "file": relpath, "line": line_of[a + lm.start()], "func": name,
                               "what": f"label {lab}: {len(later)} backward goto(s)", "end": line_of[a + later[-1].start()]})
        allocs = list(ALLOC_RE.finditer(body))
        if len(allocs) >= 4:
            fnotes.append({"check": "alloc-heavy-fn", "file": relpath, "line": line_of[a], "func": name,
                           "what": f"{len(allocs)} allocation calls: " + ",".join(sorted(set(x.group(1) for x in allocs)))[:80]})
        for am in ALLOC_ASSIGN_RE.finditer(body):
            var = am.group(1)
            q = match_paren(body, am.end() - 1, "(", ")")
            arg = " ".join(body[am.end():q].split()) if q > 0 else ""
            if "realloc" in am.group(2):
                continue
            parts = [x.strip() for x in arg.split(",")]
            if "calloc" in am.group(2).lower() and len(parts) == 2:
                arg = " * ".join(parts)
            elif len(parts) >= 2:
                arg = parts[-1]
            const = re.fullmatch(r"(?:\(?\s*(?:\d+|[A-Z_][A-Z0-9_]*|sizeof\s*\(?[^()]*\)?)\s*\)?\s*[*+]?\s*)+", arg or "x")
            after = body[am.end():]
            freed = any(re.search(r"\b" + re.escape(var.split("->")[-1].split(".")[-1]) + r"\s*\)", after[fm.end():fm.end() + 200].split(";")[0])
                        for fm in FREE_RE.finditer(after))
            short = re.escape(var)
            escapes = ("->" in var or "." in var
                       or re.search(r"\breturn\b[^;]*\b" + short + r"\b", body)
                       or re.search(r"=\s*(?:\([^()]*\)\s*)?" + short + r"\s*[;,)]", body[am.end():])
                       or any(not FREE_RE.match(c.group(1) + "(") and c.group(1) not in KEYWORDS
                              and c.group(1) not in ("memcpy", "memset", "memmove")
                              for c in re.finditer(r"\b([A-Za-z_]\w*)\s*\([^;()]*\b" + short + r"\b", body[am.end():])))
            if const and freed and not escapes:
                v = eval_dim(re.sub(r"sizeof\s*\(?[^()]*\)?", "8", arg), defs)
                fnotes.append({"check": "local-heap-temp", "file": relpath, "line": line_of[a + am.start()], "func": name,
                               "what": f"{var} = {am.group(2)}({arg[:50]}) freed locally" + (f", ~{v} B" if v else "")})
        colds = [x for x in COLD_RE.finditer(body) if "unlikely" not in body[max(0, x.start() - 160):x.start()]]
        if colds:
            fnotes.append({"check": "_cold_calls", "file": relpath, "line": line_of[a], "func": name, "what": str(len(colds))})
        sl = collections.Counter(" ".join(x.group(1).split()) for x in re.finditer(r"\bstrlen\s*\(\s*([A-Za-z_][\w.>\-]*)\s*\)", body))
        for var, n in sl.items():
            if n >= 2 and not re.search(r"(?<![\w.>])" + re.escape(var) + r"\s*(?:\[[^\]]*\]\s*)?(?:[-+]?=(?!=)|\+\+|--)", body):
                fnotes.append({"check": "ptr-strlen-repeat", "file": relpath, "line": line_of[a], "func": name,
                               "what": f"strlen({var}) x{n}"})
        head = s[max(0, a - 800):a]
        head = head[max(head.rfind(";"), head.rfind("}")) + 1:]
        hm = re.search(r"([A-Za-z_][\w\s\*]*?)\b" + re.escape(name) + r"\s*\((.*)\)\s*$", head, re.S)
        if hm:
            ret = " ".join(hm.group(1).split())
            params = [" ".join(x.split()) for x in hm.group(2).split(",")]
            byval = [p for p in params if "*" not in p and "[" not in p and re.match(r"(?:const\s+)?(?:struct\s+)?[A-Za-z_]\w*\s+[A-Za-z_]\w*$", p)]
            rtype = re.sub(r"\b(static|inline|extern|const|__attribute__\s*\(\(.*?\)\))\b", "", ret).strip()
            fnotes.append({"check": "_sig", "file": relpath, "line": line_of[a], "func": name,
                           "what": "", "byval": [p.rsplit(" ", 1)[0].replace("const ", "").replace("struct ", "").strip() for p in byval],
                           "ret": rtype if "*" not in rtype else "",
                           "ptrs": [re.sub(r"\bconst\b|\brestrict\b|\s+", " ", p.rsplit("*", 1)[0]).strip() + ("|w" if "const" not in p.split("*")[0] else "|r")
                                    for p in params if "*" in p and "(*" not in p],
                           "restrict": "restrict" in hm.group(2)})
        for fm in FSYNC_RE.finditer(body):
            fnotes.append({"check": "fsync-call", "file": relpath, "line": line_of[a + fm.start()], "func": name,
                           "what": (fm.group(1) or "F_FULLFSYNC")})
    if re.search(r"\bsocket\s*\([^;]*SOCK_STREAM", s) and re.search(r"\b(?:connect|accept4?|listen)\s*\(", s) \
            and "TCP_NODELAY" not in s and "set_nodelay" not in s:
        fnotes.append({"check": "tcp-nodelay-missing", "file": relpath, "line": 1, "func": "-", "what": "SOCK_STREAM sockets without TCP_NODELAY"})
    if file_has["mmap"] and not file_has["madvise"]:
        fnotes.append({"check": "mmap-no-madvise", "file": relpath, "line": 1, "func": "-", "what": "mmap without madvise"})
    structs = parse_structs(s, line_of, relpath)
    members = {}
    members["__ranges__"] = [(relpath, line_of[a], line_of[b], name) for a, b, name in funcs]
    members["__enums__"] = dm_enums(s, defs, eval_dim)
    members["__extern__"] = {}
    if relpath.endswith(".c") and not relpath.endswith(".inc.c"):
        for a, b, name in funcs:
            sig = max(s.rfind("}", 0, a), s.rfind(";", 0, a)) + 1
            if not re.search(r"\bstatic\b", s[sig:a]):
                members["__extern__"][name] = relpath
    members["__types__"] = {}
    for a, b, name in funcs:
        c = collections.Counter(re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)\b(?!\s*\()", s[a:b]))
        if c:
            members[name] = c
            sig = max(s.rfind("}", 0, a), s.rfind(";", 0, a)) + 1
            text = s[sig:b]
            vt = {}
            for dm in re.finditer(r"\b(?:struct\s+|union\s+)?([A-Za-z_]\w*)\s*(\*+)?\s*(?:const\s+|restrict\s+)*([A-Za-z_]\w*)\s*(?=[,;=)\[])", text):
                if dm.group(1) not in KEYWORDS and dm.group(3) not in KEYWORDS:
                    vt.setdefault(dm.group(3), (dm.group(1), bool(dm.group(2))))
            typed = collections.Counter()
            for am in re.finditer(r"\b([A-Za-z_]\w*)\s*(->|\.)\s*([A-Za-z_]\w*)\b(?!\s*\()", text):
                t = vt.get(am.group(1))
                if t and t[1] == (am.group(2) == "->"):
                    typed[(t[0], am.group(3))] += 1
            members["__types__"][name] = typed
    return loops, arrays, fnotes, structs, members


def source_roots():
    roots = PROJECT.get("roots") or (["src"] if os.path.isdir(os.path.join(ROOT, "src")) else ["."])
    return [os.path.join(ROOT, r) for r in roots]


def source_files():
    exts = tuple(PROJECT.get("extensions", [".c", ".h"]))
    out = []
    for top in source_roots():
        for d, _, fs in os.walk(top):
            for f in fs:
                if f.endswith(exts):
                    out.append(os.path.join(d, f))
    return sorted(set(out))


def harden_flags():
    h = PROJECT.get("harden")
    if not h:
        return []
    flags = list(h.get("flags", []))
    try:
        cc = subprocess.run(["clang", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        cc = ""
    want = "_".join(cc.split())
    if h.get("cache_glob"):
        import glob
        for hf in sorted(glob.glob(os.path.join(ROOT, h["cache_glob"]))):
            lines = open(hf).read().splitlines()
            if len(lines) >= 2 and lines[0].startswith(want) and lines[1].startswith("flags;"):
                got = [x for x in lines[1].split(";")[1:] if x and not x.startswith("-W")]
                print(f"hardening flags from {os.path.relpath(hf, ROOT)}: {' '.join(got)}", flush=True)
                return flags + got
    if h.get("probe_file") and h.get("probe_regex") and os.path.exists(os.path.join(ROOT, h["probe_file"])):
        m = re.search(h["probe_regex"], open(os.path.join(ROOT, h["probe_file"])).read(), re.M)
        if m:
            probes = m.group(1).split() + h.get("probe_extra_" + os.uname().machine, [])
            for pr in probes:
                if pr.startswith("-W"):
                    continue
                r = subprocess.run(["clang", "-Werror", pr, "-x", "c", "-o", os.devnull, "-"],
                                   input="int main(void){return 0;}", capture_output=True, text=True)
                if r.returncode == 0:
                    flags.append(pr)
            print(f"hardening flags probed for {cc}: {' '.join(flags)}", flush=True)
    return flags


def remark_cmds():
    harden = harden_flags()
    cmds = []
    ccj = os.path.join(ROOT, PROJECT.get("compile_commands", "compile_commands.json"))
    if os.path.exists(ccj):
        for e in json.load(open(ccj)):
            argv = e.get("arguments") or shlex.split(e["command"])
            src = os.path.normpath(os.path.join(e.get("directory", ROOT), e["file"]))
            cmds.append((src, argv, harden, e.get("directory", ROOT)))
        print(f"compile commands: {os.path.relpath(ccj, ROOT)} ({len(cmds)} entries)", flush=True)
        return cmds
    hook = PROJECT.get("compile_hook")
    if hook:
        lines = subprocess.run(hook, shell=True, cwd=ROOT, capture_output=True, text=True).stdout.splitlines()
        for l in lines:
            argv = shlex.split(l)
            if "-c" not in argv or "-o" not in argv:
                continue
            src = next((a for a in argv if a.endswith(".c")), None)
            if src:
                cmds.append((src, argv, harden, ROOT))
        print(f"compile commands: hook `{hook}` ({len(cmds)} entries)", flush=True)
        return cmds
    inc = []
    for r in source_roots():
        for d, _, _ in os.walk(r):
            inc.append("-I" + d)
    cc = os.environ.get("CC", "clang")
    for f in source_files():
        if f.endswith(".c") and not f.endswith(".inc.c"):
            cmds.append((f, [cc, "-O2", "-c", f, "-o", os.devnull] + inc, harden, ROOT))
    print(f"compile commands: none found; compiling {len(cmds)} sources with {cc} -O2 and the source dirs as includes", flush=True)
    return cmds


def run_remark(job, outdir):
    src, argv, harden, cwd = job
    base = re.sub(r"[^\w.-]", "_", os.path.relpath(os.path.join(cwd, src), ROOT))
    yml = os.path.join(outdir, base + ".opt.yaml")
    a = []
    skip = False
    for i, x in enumerate(argv):
        if skip:
            skip = False
            continue
        if x in ("-o", "-MF"):
            skip = True
            continue
        if x in ("-c", "-MMD", "-MD", "-g") or x.startswith("-W") or x == "-w":
            continue
        a.append(x)
    a = ["nice", "-n", "19"] + a + harden + ["-S", "-o", os.devnull, "-Wno-everything", "-Wpadded",
                                            "-Xclang", "-fdump-record-layouts-complete",
                                            "-fsave-optimization-record=yaml",
                                            "-foptimization-record-passes=" + REMARK_PASSES,
                                            "-foptimization-record-file=" + yml]
    r = subprocess.run(a, cwd=cwd, capture_output=True, text=True)
    with open(os.path.join(outdir, base + ".padded"), "w") as f:
        f.write(r.stderr)
    with open(os.path.join(outdir, base + ".layout"), "w") as f:
        f.write(r.stdout)
    errs = [l for l in r.stderr.splitlines() if " error: " in l]
    return src, r.returncode, "\n".join(errs[-3:])


def parse_remarks(outdir):
    vec = collections.defaultdict(lambda: {"vec": 0, "missed": 0, "reasons": collections.Counter(), "width": set()})
    licm, gvn, spills = [], [], {}
    extra = {"slp_ok": collections.Counter(), "slp_missed": collections.Counter(), "inline_costly": [], "vf": {}}
    doc_re = re.compile(r"^--- !(Passed|Missed|Analysis)\s*$")
    for f in sorted(os.listdir(outdir)):
        if not f.endswith(".opt.yaml"):
            continue
        kind = None
        cur = {}

        def done():
            if not (cur.get("file") and kind):
                return
            key = (os.path.normpath(cur["file"]).replace(ROOT + "/", ""), cur["line"])
            pas, name = cur.get("pass"), cur.get("name")
            if pas == "loop-vectorize":
                flush_vec(vec, kind, cur, key)
            elif pas == "licm" and kind == "Missed":
                licm.append((key[0], key[1], name))
            elif pas == "gvn" and kind == "Missed" and name == "LoadClobbered":
                gvn.append((key[0], key[1], cur["args"].get("ClobberedBy", "?")))
            elif pas == "slp-vectorizer" and cur.get("function"):
                (extra["slp_ok"] if kind == "Passed" else extra["slp_missed"])[(key[0], cur["function"])] += 1
            elif pas == "inline" and kind == "Missed" and name in ("TooCostly", "NeverInline") and cur.get("function"):
                extra["inline_costly"].append((key[0], key[1], cur["function"], cur["args"].get("Callee", "?"),
                                               cur["args"].get("Cost", ""), cur["args"].get("Threshold", ""), name))
            elif pas == "regalloc" and name == "LoopSpillReloadCopies":
                sp = spills.setdefault(key, [0, 0, 0, 0.0])
                sp[0] = max(sp[0], int(cur["args"].get("NumSpills", 0)))
                sp[1] = max(sp[1], int(cur["args"].get("NumReloads", 0)))
                sp[2] += 1
                cost = float(cur["args"].get("TotalSpillsCost", 0) or 0) + float(cur["args"].get("TotalReloadsCost", 0) or 0)
                sp[3] = max(sp[3], cost)
        for line in open(os.path.join(outdir, f), errors="replace"):
            m = doc_re.match(line)
            if m or line.startswith("..."):
                done()
                kind = m.group(1) if m else None
                cur = {"strings": [], "args": {}}
                continue
            if kind is None:
                continue
            m = re.match(r"DebugLoc:\s*\{\s*File:\s*'?([^',]+)'?,\s*Line:\s*(\d+)", line.strip())
            if m and "file" not in cur:
                cur["file"], cur["line"] = m.group(1), int(m.group(2))
                continue
            m = re.match(r"Pass:\s*(\S+)", line)
            if m:
                cur["pass"] = m.group(1)
                continue
            m = re.match(r"Name:\s*(\w+)", line)
            if m:
                cur["name"] = m.group(1)
                continue
            m = re.match(r"Function:\s*'?([^'\s]+)'?", line)
            if m:
                cur["function"] = m.group(1)
                continue
            m = re.match(r"\s*-\s*String:\s*(.*?)\s*$", line)
            if m:
                v = m.group(1)
                if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
                    v = v[1:-1]
                cur["strings"].append(v)
                continue
            m = re.match(r"\s*-\s*(\w+):\s*'?([^']*?)'?\s*$", line)
            if m:
                cur["args"][m.group(1)] = m.group(2)
        done()
    return vec, licm, gvn, spills, extra


def flush_vec(rec, kind, cur, key):
    r = rec[key]
    text = "".join(cur["strings"]).strip()
    if kind == "Passed" and cur.get("name") in ("Vectorized", "Interleaved"):
        r["vec"] += 1
        vf, ic = cur["args"].get("VectorizationFactor"), cur["args"].get("InterleaveCount")
        if cur.get("name") == "Interleaved":
            vf = "1"
        if vf:
            r["width"].add(f"{vf}x{ic or 1}")
    elif kind == "Missed":
        r["missed"] += 1
    elif kind == "Analysis" and text:
        reason = re.sub(r"^loop not vectorized:\s*", "", text)
        reason = (cur.get("name", "?") + ": " + re.sub(r"\d+", "N", reason))[:120]
        r["reasons"][reason] += 1


def parse_padded(outdir):
    seen, out = set(), collections.defaultdict(lambda: {"bytes": 0, "holes": 0, "tail": 0, "file": "", "line": 0})
    pr = re.compile(r"^(.+?):(\d+):(\d+): warning: padding (struct|size of) '(?:struct |union )?([^']+)' with (\d+) bytes? to (align '([^']+)'|alignment boundary)")
    for f in sorted(os.listdir(outdir)):
        if not f.endswith(".padded"):
            continue
        for line in open(os.path.join(outdir, f), errors="replace"):
            m = pr.match(line)
            if not m:
                continue
            path = os.path.normpath(os.path.join(ROOT, m.group(1))).replace(ROOT + "/", "")
            k = (path, m.group(2), m.group(3), m.group(5))
            if k in seen or not path.startswith("src/"):
                continue
            seen.add(k)
            r = out[m.group(5)]
            r["bytes"] += int(m.group(6))
            if m.group(4) == "size of":
                r["tail"] += int(m.group(6))
            else:
                r["holes"] += 1
            if not r["file"]:
                r["file"], r["line"] = path, int(m.group(2))
    return out


def parse_layouts(outdir):
    recs = {}
    for f in sorted(os.listdir(outdir)):
        if not f.endswith(".layout"):
            continue
        text = open(os.path.join(outdir, f), errors="replace").read()
        for blk in text.split("*** Dumping AST Record Layout")[1:]:
            ls = blk.strip("\n").split("\n")
            m = re.match(r"\s*0 \| (struct|union) (.+)$", ls[0])
            if not m:
                continue
            name = m.group(2).strip()
            if "(anonymous" in name or "(unnamed" in name or name in recs:
                continue
            sm = re.search(r"\[sizeof=(\d+)[^\]]*?align=(\d+)", blk)
            if not sm:
                continue
            fields, bitf = [], False
            for l in ls[1:]:
                if "[sizeof=" in l:
                    break
                fm = re.match(r"\s*(\d+)(?::(\d+)-(\d+))? \|   (\S.*)$", l)
                if not fm:
                    continue
                decl = fm.group(4).strip()
                if fm.group(2) is not None:
                    bitf = True
                nm = re.match(r"(.*?)\s*(\w+)$", decl)
                if nm:
                    fields.append((int(fm.group(1)), nm.group(1).strip(), nm.group(2)))
            recs[name] = {"size": int(sm.group(1)), "align": int(sm.group(2)), "fields": fields, "bitfields": bitf,
                          "union": m.group(1) == "union"}
    return recs


PRIM = {"char": 1, "signed char": 1, "unsigned char": 1, "_Bool": 1, "bool": 1, "uint8_t": 1, "int8_t": 1, "BOOL": 4,
        "short": 2, "unsigned short": 2, "uint16_t": 2, "int16_t": 2, "int": 4, "unsigned int": 4, "uint32_t": 4,
        "int32_t": 4, "float": 4, "JSAtom": 4, "long": 8, "unsigned long": 8, "long long": 8, "unsigned long long": 8,
        "uint64_t": 8, "int64_t": 8, "double": 8, "size_t": 8, "ssize_t": 8, "uintptr_t": 8, "intptr_t": 8,
        "off_t": 8, "time_t": 8, "unsigned": 4, "JSClassID": 4, "pid_t": 4, "socklen_t": 4}


def field_geom(ty, recs, aliases):
    ty = re.sub(r"\b(const|volatile|_Atomic|restrict)\b", "", ty).strip()
    am = re.match(r"(.*)\[(\d+)\]$", ty)
    if am:
        g = field_geom(am.group(1), recs, aliases)
        return (g[0] * int(am.group(2)), g[1]) if g else None
    if "*" in ty or "(" in ty:
        return (8, 8)
    if re.match(r"enum\s", ty):
        return (4, 4)
    ty = re.sub(r"^(struct|union|enum)\s+", "", ty).strip()
    if ty in PRIM:
        return (PRIM[ty], PRIM[ty])
    for cand in (ty, aliases.get(ty, "")):
        if cand in recs:
            return (recs[cand]["size"], recs[cand]["align"])
    return None


def reorder(rec, recs, aliases):
    if rec["union"] or rec["bitfields"] or not rec["fields"]:
        return None
    geo = []
    for off, ty, nm in rec["fields"]:
        g = field_geom(ty, recs, aliases)
        if g is None:
            return None
        geo.append((g[0], g[1], nm, off))
    last_flex = [x for x in geo if x[0] == 0]
    body = sorted([x for x in geo if x[0] != 0], key=lambda x: (-x[1], -x[0]))
    off = 0
    for size, al, nm, _ in body:
        off = (off + al - 1) // al * al + size
    a = rec["align"]
    best = (off + a - 1) // a * a
    if last_flex:
        best = max(best, off)
    return best


def verdict(r):
    if not r:
        return "no-remark"
    if r["vec"] and not r["missed"]:
        return "vectorized"
    if r["vec"]:
        return "vectorized-in-some-copies"
    return "not-vectorized"


def attach_compiler(loops, licm, gvn):
    byfile = collections.defaultdict(list)
    for L in loops:
        byfile[L["file"]].append(L)
    for v in byfile.values():
        v.sort(key=lambda L: (L["line"], -L["end"]))
    for L in loops:
        L["licm"] = 0
        L["gvn"] = collections.Counter()

    def innermost(path, line):
        best = None
        for L in byfile.get(path, ()):
            if L["line"] > line:
                break
            if L["line"] <= line <= L["end"] and (best is None or L["end"] - L["line"] < best["end"] - best["line"]):
                best = L
        return best
    for path, line, _ in licm:
        L = innermost(path, line)
        if L:
            L["licm"] += 1
    for path, line, by in gvn:
        L = innermost(path, line)
        if L:
            L["gvn"][by] += 1


def report(outdir, limit=40, want=None):
    files = source_files()
    defs = defines(files)
    have = os.path.isdir(outdir)
    vec, licm, gvn, spills, extra = parse_remarks(outdir) if have else ({}, [], [], {}, {"slp_ok": {}, "slp_missed": {}, "inline_costly": [], "vf": {}})
    padded = parse_padded(outdir) if have else {}
    recs = parse_layouts(outdir) if have else {}
    pfuncs, plines = load_profile(outdir) if have else ({}, {})
    loops, arrays, fnotes, structs, members, franges = [], [], [], [], {}, []
    for p in files:
        l, a, fn, st, mb = scan_file(p, defs)
        loops += l
        arrays += a
        fnotes += fn
        structs += st
        for k, v in mb.items():
            if k == "__ranges__":
                franges.extend(v)
                continue
            if k == "__types__":
                for fn_, ids in v.items():
                    members.setdefault("__types__", {}).setdefault(fn_, collections.Counter()).update(ids)
                continue
            if k in ("__enums__", "__extern__"):
                members.setdefault(k, {}).update(v)
                continue
            members.setdefault(k, collections.Counter()).update(v)
    attach_compiler(loops, licm, gvn)
    for L in loops:
        r = vec.get((L["file"], L["line"]))
        L["verdict"] = verdict(r)
        L["reasons"] = "; ".join(f"{k} x{v}" for k, v in (r["reasons"].most_common(3) if r else []))
        L["vf"] = ",".join(sorted(r["width"])) if r else ""
        sp = spills.get((L["file"], L["line"]))
        L["spills"], L["reloads"], L["spillcost"] = (sp[0], sp[1], sp[3]) if sp else (0, 0, 0.0)
    extern = members.get("__extern__", {})
    enums = members.get("__enums__", {})
    sbyname = {}
    for st in structs:
        sbyname.setdefault(st["name"], st)
        if st["alias"]:
            sbyname.setdefault(st["alias"], st)
    findings = collections.defaultdict(list)
    aliases = {st["alias"]: st["name"] for st in structs if st["alias"]}
    project_structs = {st["name"] for st in structs} | set(aliases)

    def range_heat(file, a, b):
        fl = plines.get(file)
        if not fl:
            return 0.0
        return sum(v for ln, v in fl.items() if a <= ln <= b)

    for L in loops:
        L["heat"] = range_heat(L["file"], L["line"], L["end"])
    fself = collections.Counter()
    for fpath, a, b, name in franges:
        fself[name] += range_heat(fpath, a, b)
    HOT = 0.005

    def add(check, file, line, func, what, rank, heat=None):
        if heat is None:
            heat = fself.get(func, 0.0)
        findings[check].append({"file": file, "line": line, "func": func, "what": what, "rank": rank, "heat": heat})

    def lrank(L):
        return (0 if L["inner"] else 1, 0 if L["verdict"] == "not-vectorized" else 1, -L["depth"], L["lines"])
    for L in loops:
        g = L["gist"]
        lh = L["heat"]

        def add(check, file, line, func, what, rank, heat=None, _lh=lh):
            findings[check].append({"file": file, "line": line, "func": func, "what": what, "rank": rank,
                                    "heat": _lh if heat is None else heat})
        for k, v in g.items():
            add(k, L["file"], L["line"], L["func"], ",".join(v)[:110] + f"  [{L['verdict']}]", lrank(L))
        managed = PROJECT.get("managed_prefixes", [])
        heap_a = [x for x in L["alloc"] if not x.startswith(tuple(managed))] if managed else list(L["alloc"])
        heap_f = [x for x in L["free"] if not x.startswith(tuple(managed))] if managed else list(L["free"])
        if heap_a and heap_f:
            add("alloc-churn", L["file"], L["line"], L["func"], f"alloc={','.join(heap_a)} free={','.join(heap_f)}", (0 if L["inner"] else 1, -L["depth"]))
        elif L["alloc"] and L["inner"]:
            add("alloc-in-loop", L["file"], L["line"], L["func"], ",".join(L["alloc"])[:80], lrank(L))
        if L["push"] and L["inner"]:
            add("push-per-element", L["file"], L["line"], L["func"], ",".join(L["push"])[:80], lrank(L))
        if L["smallcpy"] and L["inner"] and L["verdict"] == "not-vectorized":
            add("smallcpy-blocks-vec", L["file"], L["line"], L["func"], ",".join(L["smallcpy"]) + " :: " + L["reasons"][:70], lrank(L))
        if L["libm"] and L["inner"] and L["verdict"] == "not-vectorized":
            add("libm-in-loop", L["file"], L["line"], L["func"], ",".join(L["libm"]) + " :: " + L["reasons"][:70], lrank(L))
        for k_, v_ in (L.get("more") or {}).items():
            add(k_, L["file"], L["line"], L["func"], v_[:160] + f"  [{L['verdict']}]", lrank(L))
        for cal in L.get("xcalls") or []:
            home = extern.get(cal)
            if home and home != L["file"]:
                add("xtu-call", L["file"], L["line"], L["func"], f"{cal}() per element, defined in {home}  [{L['verdict']}]", lrank(L))
        if L.get("fission") and L["verdict"] != "vectorized" and not L.get("tail"):
            add("loop-fission", L["file"], L["line"], L["func"], L["fission"] + f"  [{L['verdict']}]", lrank(L))
        if L["fnptr"] and L["inner"] and L["lines"] <= 12:
            add("indirect-call-in-loop", L["file"], L["line"], L["func"], f"{L['fnptr']} indirect :: {L['header'][:60]}", lrank(L))
        for base, flds in (L.get("_aos") or {}).items():
            ty = (L.get("_aos_type") or {}).get(base)
            rec = recs.get(ty) or recs.get(aliases.get(ty or "", ""))
            if not rec or rec["size"] < 32:
                continue
            used = 0
            for off, fty, fnm in rec["fields"]:
                if fnm in flds:
                    g = field_geom(fty, recs, aliases)
                    used += g[0] if g else 8
            if used and used * 4 <= rec["size"]:
                add("aos-partial-touch", L["file"], L["line"], L["func"],
                    f"{base}[i].{{{','.join(flds)}}}: {used} of {rec['size']} B per element ({ty})", lrank(L))
        if L["licm"]:
            add("licm-blocked", L["file"], L["line"], L["func"], f"{L['licm']} load(s) not hoisted  [{L['verdict']}]", (-L["licm"],))
        if L["gvn"]:
            tot = sum(L["gvn"].values())
            add("gvn-clobbered", L["file"], L["line"], L["func"],
                f"{tot} reload(s): " + ",".join(f"{k}={v}" for k, v in L["gvn"].most_common()), (-tot,))
        if L["vf"] and all(w.startswith("1x") for w in L["vf"].split(",")):
            add("interleave-only", L["file"], L["line"], L["func"], f"width {L['vf']}", lrank(L))
        if L["spills"] or L["reloads"]:
            add("loop-spills", L["file"], L["line"], L["func"],
                f"{L['spills']} spills {L['reloads']} reloads, weighted cost {L['spillcost']:.0f}  [{L['verdict']}, depth {L['depth']}]",
                (-L["spillcost"],))
    def add(check, file, line, func, what, rank, heat=None):
        if heat is None:
            heat = fself.get(func, 0.0)
        findings[check].append({"file": file, "line": line, "func": func, "what": what, "rank": rank, "heat": heat})

    for n in [x for x in fnotes if x["check"] == "_cold_calls"]:
        if fself.get(n["func"], 0.0) >= HOT:
            add("cold-path-in-hot-fn", n["file"], n["line"], n["func"], f"{n['what']} cold call(s) not under unlikely()", (0,))
    fnotes = [x for x in fnotes if x["check"] != "_cold_calls"]
    seen_inl = set()
    for fpath, ln, caller, callee, cost, thr, why in extra["inline_costly"]:
        if why != "TooCostly" or (caller, callee, ln) in seen_inl:
            continue
        site = range_heat(fpath, ln, ln)
        if site < 0.001:
            continue
        seen_inl.add((caller, callee, ln))
        add("inline-too-costly", fpath, ln, caller, f"{callee} not inlined (cost {cost} / threshold {thr}); call-site line self-time", (0,), site)
    slp_m, slp_o = collections.Counter(), collections.Counter()
    for (fpath, fn), n in extra["slp_missed"].items():
        slp_m[fn] += n
    for (fpath, fn), n in extra["slp_ok"].items():
        slp_o[fn] += n
    fhome = {name: (fpath, a) for fpath, a, b, name in franges}
    for fn, n in slp_m.items():
        if fself.get(fn, 0.0) >= HOT:
            fp, ln = fhome.get(fn, ("?", 0))
            add("slp-missed-hot", fp, ln, fn, f"{n} SLP attempts missed, {slp_o.get(fn, 0)} vectorized (incl. inlined code)", (0,))
    sfile = {st["name"]: st for st in structs}
    sfile.update({st["alias"]: st for st in structs if st["alias"]})
    hot_funcs = {f: h for f, h in fself.items() if h >= 0.01}
    ftypes = members.get("__types__", {})
    for name, rec in recs.items():
        if name not in project_structs:
            continue
        st = sfile.get(name)
        best = reorder(rec, recs, aliases)
        if best is not None and best < rec["size"]:
            lines_now, lines_best = (rec["size"] + 63) // 64, (best + 63) // 64
            if rec["size"] - best >= 8 or lines_best < lines_now:
                add("reorder-savings", st["file"] if st else "?", st["line"] if st else 0, name,
                    f"{rec['size']} -> {best} B by alignment order ({lines_now} -> {lines_best} cache lines)", (best - rec["size"],), 0.0)
        if rec["size"] >= 128 and hot_funcs and not rec["union"]:
            weight = collections.Counter()
            tnames = {name} | ({st["alias"]} if st and st.get("alias") else set()) | ({st["name"]} if st else set())
            users = {}
            for f, h in hot_funcs.items():
                tc = ftypes.get(f)
                if not tc:
                    continue
                for off, fty, fnm in rec["fields"]:
                    k = sum(tc.get((t, fnm), 0) for t in tnames)
                    if k:
                        weight[fnm] += h * k
                        users[f] = h
            hot = [(off, fty, fnm) for off, fty, fnm in rec["fields"] if weight.get(fnm)]
            if len(hot) >= 2:
                touched = {off // 64 for off, _, _ in hot}
                need = 0
                for off, fty, fnm in hot:
                    g = field_geom(fty, recs, aliases)
                    need += g[0] if g else 8
                need_lines = max(1, (need + 63) // 64)
                if len(touched) >= need_lines + 2:
                    add("hot-fields-spread", st["file"] if st else "?", st["line"] if st else 0, name,
                        f"{len(hot)} hot fields ({need} B) span {len(touched)} of {(rec['size'] + 63) // 64} lines: "
                        + ",".join(f for _, _, f in sorted(hot, key=lambda x: -weight[x[2]])[:8]),
                        (-(len(touched) - need_lines),),
                        max(users.values(), default=0.0))
    type_heat = {}
    for L in loops:
        for _b, ty in (L.get("_aos_all_type") or {}).items():
            type_heat[ty] = max(type_heat.get(ty, 0.0), L["heat"])

    def relayout(rec, repl):
        geo = []
        for off, ty, nm in rec["fields"]:
            g = field_geom(repl.get(nm, ty), recs, aliases)
            if g is None or g[0] == 0:
                return None
            geo.append(g)
        off = 0
        for sz, al in sorted(geo, key=lambda g: (-g[1], -g[0])):
            off = (off + al - 1) // al * al + sz
        a = max([g[1] for g in geo] or [1])
        return (off + a - 1) // a * a

    def users_heat(names):
        best = max([type_heat.get(n, 0.0) for n in names] or [0.0])
        for f, h in fself.items():
            tc = ftypes.get(f)
            if tc and h > best and any(t in names for (t, _) in tc):
                best = h
        return best

    for name, rec in recs.items():
        if name not in project_structs or rec["union"] or rec["bitfields"] or not rec["fields"]:
            continue
        st = sfile.get(name)
        tn = {name} | ({st["alias"]} if st and st.get("alias") else set()) | ({st["name"]} if st else set())
        now = relayout(rec, {})
        if now is None:
            continue
        now = min(now, rec["size"])
        links = [nm for off, ty, nm in rec["fields"]
                 if (lambda m: m and m.group(1) in tn)(re.fullmatch(r"(?:struct\s+)?(\w+)\s*\*", ty.strip()))]
        if len(links) >= 2 and len(links) < len(rec["fields"]):
            new = relayout(rec, {nm: "uint32_t" for nm in links})
            if new is not None and (now - new >= max(8, now // 8) or (new + 63) // 64 < (now + 63) // 64):
                add("ptr-to-index", st["file"] if st else "?", st["line"] if st else 0, name,
                    f"{len(links)} self-links ({','.join(links[:4])}): {now} -> {new} B per node with uint32_t pool indices",
                    (new - now,), users_heat(tn))
        narrow = {}
        for off, ty, nm in rec["fields"]:
            et = re.sub(r"^enum\s+", "", ty.strip())
            rng = enums.get(et)
            if rng and field_geom(ty, recs, aliases) == (4, 4) and rng[0] >= 0:
                if rng[1] < 256:
                    narrow[nm] = "uint8_t"
                elif rng[1] < 65536:
                    narrow[nm] = "uint16_t"
        if narrow:
            new = relayout(rec, narrow)
            if new is not None and (now - new >= 8 or (new + 63) // 64 < (now + 63) // 64):
                add("wide-enum-field", st["file"] if st else "?", st["line"] if st else 0, name,
                    f"{','.join(f'{k}->{v}' for k, v in list(narrow.items())[:4])}: {now} -> {new} B",
                    (new - now,), users_heat(tn))
    for ty, h in type_heat.items():
        rec = recs.get(ty) or recs.get(aliases.get(ty, ""))
        if not rec or rec["union"] or (ty not in project_structs):
            continue
        frac = dm_straddle(rec["size"])
        if not frac or frac < 0.25:
            continue
        sz = rec["size"]
        up = 1 << (sz - 1).bit_length() if sz < 64 else (sz + 63) // 64 * 64
        down = 1 << (sz.bit_length() - 1) if sz < 64 else sz // 64 * 64
        best = relayout(rec, {}) or sz
        st = sfile.get(ty) or sfile.get(aliases.get(ty, ""))
        add("line-straddle", st["file"] if st else "?", st["line"] if st else 0, ty,
            f"{sz} B elements: {frac:.0%} touch an extra cache line (64-B aligned base); pad to {up} B or shrink to {down} B"
            f" (best field order gives {best} B)", (-frac,), h)
    sigs = {}
    for n in [x for x in fnotes if x["check"] == "_sig"]:
        sigs[n["func"]] = n
        for ty in n["byval"]:
            rec = recs.get(ty) or recs.get(aliases.get(ty, ""))
            if rec and rec["size"] >= 32 and ty in project_structs:
                add("ptr-struct-by-value", n["file"], n["line"], n["func"], f"parameter of type {ty} ({rec['size']} B) passed by value", (-rec["size"],))
        rt = n["ret"].split()[-1] if n["ret"] else ""
        rec = recs.get(rt) or recs.get(aliases.get(rt, ""))
        if rec and rec["size"] >= 32 and rt in project_structs:
            add("ptr-struct-by-value", n["file"], n["line"], n["func"], f"returns {rt} ({rec['size']} B) by value", (-rec["size"],))
    fnotes = [x for x in fnotes if x["check"] != "_sig"]
    for L in loops:
        if L.get("_copytypes"):
            for ty in L["_copytypes"]:
                rec = recs.get(ty) or recs.get(aliases.get(ty, ""))
                if rec and rec["size"] >= 32:
                    findings["ptr-struct-copy-in-loop"].append({"file": L["file"], "line": L["line"], "func": L["func"],
                        "what": f"{ty} ({rec['size']} B) copied per iteration", "rank": (-rec["size"],), "heat": L["heat"]})
        alias_evidence = re.search(r"unsafe dependent memory|UnsafeDep|runtime pointer checks|cannot identify array bounds", L["reasons"]) \
            or L["licm"] or L["gvn"].get("store", 0)
        if alias_evidence and L["func"] in sigs:
            sg = sigs[L["func"]]
            types = collections.Counter(p.split("|")[0] for p in sg["ptrs"])
            same = [t for t, c in types.items() if c >= 2]
            writes = [p for p in sg["ptrs"] if p.endswith("|w")]
            if same and writes and not sg["restrict"]:
                findings["ptr-restrict-candidate"].append({"file": L["file"], "line": L["line"], "func": L["func"],
                    "what": f"{len(sg['ptrs'])} pointer params ({', '.join(same)} x2+); aliasing evidence: licm {L['licm']}, store-clobbered reloads {L['gvn'].get('store', 0)}; {L['reasons'][:50]}", "rank": (0,), "heat": L["heat"]})
    for a in arrays:
        add("stack-array", a["file"], a["line"], a["func"], f"{a['bytes']}{'' if a['exact'] else '?'} B :: {a['decl']}", (-a["bytes"],))
    for n in fnotes:
        h = range_heat(n["file"], n["line"], n["end"]) if n.get("end") else None
        add(n["check"], n["file"], n["line"], n["func"], n["what"], (n["file"], n["line"]), h)
    for st in structs:
        if st["bools"] >= 3:
            add("bool-fields", st["file"], st["line"], st["name"], f"{st['bools']} bool fields of {st['nfields']}", (-st["bools"],))
        if (st["atomics"] >= 2 or (st["atomics"] and st["locks"])) and not st["aligned"]:
            add("false-sharing-review", st["file"], st["line"], st["name"],
                f"{st['atomics']} atomics, {st['locks']} locks, {st['nfields']} fields, no 64-B alignment", (-st["atomics"],))
        if st["dptr"]:
            for d in st["dptr"]:
                m = re.match(r"(?:struct\s+)?([A-Za-z_]\w*)\s*\*\s*\*", d)
                node = sbyname.get(m.group(1)) if m else None
                if node and node["selfnext"]:
                    add("chained-hash", st["file"], st["line"], st["name"], f"{d} -> node {node['name']} ({node['file']}:{node['line']})", (st["file"],))
    for name, r in padded.items():
        st = sbyname.get(name)
        add("padding", r["file"], r["line"], name,
            f"{r['bytes']} B padding ({r['holes']} holes, tail {r['tail']})" + (f", {st['nfields']} fields" if st else ""), (-r["bytes"],))
    os.makedirs(outdir, exist_ok=True)
    tsv = os.path.join(outdir, "loops.tsv")
    cols = ["file", "line", "end", "func", "kw", "depth", "inner", "lines", "heat", "verdict", "vf", "spills", "reloads", "spillcost", "licm",
            "alloc", "free", "push", "smallcpy", "libm", "fnptr", "div", "jsapi", "header", "reasons", "gist"]
    with open(tsv, "w") as f:
        f.write("\t".join(cols) + "\n")
        for L in loops:
            row = []
            for c in cols:
                v = L[c]
                if c == "gist":
                    v = ";".join(f"{k}={','.join(x)}" for k, x in v.items())
                elif isinstance(v, list):
                    v = ",".join(v)
                row.append(str(v))
            f.write("\t".join(row) + "\n")
    with open(os.path.join(outdir, "findings.tsv"), "w") as f:
        f.write("check\tsection\theat_pct\tfile\tline\tfunc\twhat\n")
        for cid, sec, _ in CHECKS:
            for x in sorted(findings.get(cid, []), key=lambda x: (-x["heat"], x["rank"])):
                f.write(f"{cid}\t{sec}\t{100 * x['heat']:.3f}\t{x['file']}\t{x['line']}\t{x['func']}\t{x['what']}\n")
    with open(os.path.join(outdir, "stack-arrays.tsv"), "w") as f:
        f.write("file\tline\tfunc\tbytes\texact\tdecl\n")
        for a in sorted(arrays, key=lambda a: -a["bytes"]):
            f.write(f"{a['file']}\t{a['line']}\t{a['func']}\t{a['bytes']}\t{a['exact']}\t{a['decl']}\n")
    v = collections.Counter(L["verdict"] for L in loops)
    reasons = collections.Counter()
    for L in loops:
        if L["verdict"] == "not-vectorized" and L["inner"]:
            for part in L["reasons"].split("; "):
                if part:
                    reasons[part.rsplit(" x", 1)[0]] += 1
    print(f"loops: {len(loops)} in {len(files)} files; structs {len(structs)}; remarks for "
          f"{sum(1 for L in loops if L['verdict'] != 'no-remark')} loops; licm {len(licm)}, gvn {len(gvn)}, "
          f"loops with spills {sum(1 for L in loops if L['spills'])}, padded structs {len(padded)}")
    for k, n in v.most_common():
        print(f"  {k:28s} {n}")
    print("why innermost loops were not vectorized (top 12):")
    for k, n in reasons.most_common(12):
        print(f"  {n:5d}  {k}")
    print("\nfindings per check (section of AGENT.md):")
    for cid, sec, desc in CHECKS:
        print(f"  {len(findings.get(cid, [])):6d}  {cid:24s} {sec:16s} {desc}")
    if pfuncs:
        skip = {"stack-array", "padding", "alloc-heavy-fn", "macro-list-walk", "seq-cst-atomic", "mmap-no-madvise",
                "reorder-savings", "fsync-call", "tcp-nodelay-missing", "sort-call", "big-memset"}
        allrows = [(cid, x) for cid, rows in findings.items() if cid not in skip for x in rows if x["heat"] > 0]
        allrows.sort(key=lambda r: -r[1]["heat"])
        print(f"\n## HOTTEST FINDINGS (share of profile self-time inside the loop / function body / call-site line; {len(allrows)} with heat)")
        for cid, x in allrows[:max(limit, 80) if limit else 0]:
            print(f"  {100 * x['heat']:6.2f}%  {cid:24s} {x['file']}:{x['line']} {x['func']} :: {x['what'][:100]}")
    for cid, sec, desc in CHECKS:
        if want and cid not in want:
            continue
        rows = sorted(findings.get(cid, []), key=lambda x: (-x["heat"], x["rank"]))
        if not rows:
            continue
        print(f"\n## {cid} [{sec}] ({len(rows)}): {desc}")
        for x in rows[:limit]:
            hp = f"{100 * x['heat']:6.2f}%  " if pfuncs else ""
            print(f"  {hp}{x['file']}:{x['line']} {x['func']} :: {x['what']}")
    if fself:
        print("\n## TOP FUNCTIONS BY SELF TIME (inlined callees counted at their call lines)")
        for fn, h in fself.most_common(30):
            fp, ln = fhome.get(fn, ("?", 0))
            print(f"  {100 * h:6.2f}%  {fp}:{ln} {fn}")
    print(f"\nfull tables: {tsv}, {os.path.join(outdir, 'findings.tsv')}, {os.path.join(outdir, 'stack-arrays.tsv')}")
    return findings


def workloads():
    return [(w["name"], w["args"] if "args" in w else ["@cat"] + w["cat"]) for w in PROJECT.get("workloads", [])]


def _children_sum(nodes, k):
    d = nodes[k][0]
    s, j, cd = 0, k + 1, None
    while j < len(nodes) and nodes[j][0] > d:
        if cd is None:
            cd = nodes[j][0]
        if nodes[j][0] == cd:
            s += nodes[j][1]
        j += 1
    return s


def sample_tree(path, image):
    lines = open(path, errors="replace").read().split("\n")
    node_re = re.compile(r"^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\) \+ \d+  \[(0x[0-9a-f]+)\]")
    nodes = []
    for l in lines:
        if l.startswith("Total number in stack") or l.startswith("Sort by top of stack"):
            break
        m = node_re.match(l)
        if m:
            nodes.append((len(m.group(1)), int(m.group(2)), m.group(3).strip(), m.group(4), int(m.group(5), 16)))
    load = None
    for l in lines:
        m = re.match(r"\s*(0x[0-9a-f]+) -\s+0x[0-9a-f]+ \+?(\S+)", l)
        if m and os.path.basename(m.group(2)) == image and load is None:
            load = int(m.group(1), 16)
    leaf, incl, total, stack = collections.Counter(), collections.Counter(), 0, []
    for k, (d, n, fn, img, addr) in enumerate(nodes):
        while stack and stack[-1][0] >= d:
            stack.pop()
        if not stack:
            total += n
        if fn not in {x[1] for x in stack}:
            incl[fn] += n
        stack.append((d, fn))
        self_n = n - _children_sum(nodes, k)
        if self_n > 0 and img == image:
            leaf[addr] += self_n
    return leaf, incl, total, load


def symbolizer():
    for c in ("llvm-symbolizer", "/opt/homebrew/opt/llvm/bin/llvm-symbolizer", "/usr/local/opt/llvm/bin/llvm-symbolizer"):
        try:
            subprocess.run([c, "--version"], capture_output=True, check=True)
            return c
        except (OSError, subprocess.CalledProcessError):
            continue
    sys.exit("dod_scan profile: llvm-symbolizer not found (it separates addresses and expands inlined frames; atos does not)")


def text_vmaddr(binary):
    r = subprocess.run(["otool", "-l", binary], capture_output=True, text=True).stdout
    m = re.search(r"segname __TEXT\s+vmaddr (0x[0-9a-f]+)", r)
    return int(m.group(1), 16) if m else 0x100000000


def symbolize(binary, load, addrs):
    """address -> [(func, file, line), ...] innermost inlined frame first."""
    base = text_vmaddr(binary)
    addrs = sorted(addrs)
    inp = "".join(f"0x{a - load + base:x}\n" for a in addrs)
    r = subprocess.run([symbolizer(), "--obj=" + binary, "--inlining"], input=inp, capture_output=True, text=True)
    blocks = r.stdout.strip("\n").split("\n\n")
    out = {}
    if len(blocks) != len(addrs):
        sys.exit(f"dod_scan profile: symbolizer returned {len(blocks)} blocks for {len(addrs)} addresses")
    for a, blk in zip(addrs, blocks):
        ls = blk.split("\n")
        frames = []
        for k in range(0, len(ls) - 1, 2):
            m = re.match(r"(.+):(\d+):\d+$", ls[k + 1])
            if m:
                frames.append((ls[k], m.group(1), int(m.group(2))))
        out[a] = frames
    return out


def profile(outdir, binary, seconds):
    if not workloads():
        sys.exit("dod_scan profile: no workloads; list them under \"workloads\" in dod_scan.json")
    binary = os.path.abspath(binary)
    pdir = os.path.join(outdir, "profile")
    os.makedirs(pdir, exist_ok=True)
    for name, spec in workloads():
        if spec[0] == "@cat":
            if not all(os.path.exists(os.path.join(ROOT, x)) for x in spec[1:]):
                print(f"  SKIP {name}: missing input", flush=True)
                continue
            script = os.path.join(pdir, name + ".js")
            with open(script, "w") as f:
                for x in spec[1:]:
                    f.write(open(os.path.join(ROOT, x)).read() + "\n")
            args = [script]
        else:
            if not os.path.exists(os.path.join(ROOT, spec[0])):
                print(f"  SKIP {name}: missing {spec[0]}", flush=True)
                continue
            args = [os.path.join(ROOT, x) for x in spec]
        rep = os.path.join(pdir, name + ".sample.txt")
        if os.path.exists(rep):
            os.remove(rep)
        proc = subprocess.Popen([binary] + PROJECT.get("binary_args", []) + args, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["sample", str(proc.pid), str(seconds), "1", "-mayDie", "-file", rep], capture_output=True)
        try:
            proc.wait(timeout=600)
        except subprocess.TimeoutExpired:
            proc.kill()
        if not os.path.exists(rep):
            print(f"  SKIP {name}: no samples (process ended before sampling began)", flush=True)
    aggregate(outdir, binary)


def aggregate(outdir, binary):
    binary = os.path.abspath(binary)
    pdir = os.path.join(outdir, "profile")
    func_share, line_share = collections.Counter(), collections.Counter()
    used = 0
    for name, _ in workloads():
        rep = os.path.join(pdir, name + ".sample.txt")
        if not os.path.exists(rep):
            continue
        leaf, incl, total, load = sample_tree(rep, os.path.basename(binary))
        if not total or load is None:
            print(f"  SKIP {name}: empty report", flush=True)
            continue
        syms = symbolize(binary, load, list(leaf))
        own = sum(leaf.values())
        for a, n in leaf.items():
            seen = set()
            for fn, fl, ln in syms.get(a, []):
                key = (os.path.normpath(fl).replace(ROOT + "/", ""), ln)
                if key not in seen:
                    seen.add(key)
                    line_share[key] += n / total
        for fn, n in incl.items():
            func_share[fn] += n / total
        used += 1
        print(f"  {name}: {total} samples, {100.0 * own / total:.0f}% in the engine binary", flush=True)
    if not used:
        sys.exit("profile: no workload produced samples")
    with open(os.path.join(outdir, "profile_funcs.tsv"), "w") as f:
        f.write("func\tinclusive_share\n")
        for fn, v in func_share.most_common():
            f.write(f"{fn}\t{v / used:.6f}\n")
    with open(os.path.join(outdir, "profile_lines.tsv"), "w") as f:
        f.write("file\tline\tself_share\n")
        for (fl, ln), v in line_share.most_common():
            f.write(f"{fl}\t{ln}\t{v / used:.6f}\n")
    print(f"profile: {used} workloads -> {outdir}/profile_funcs.tsv, profile_lines.tsv", flush=True)


def load_profile(outdir):
    funcs, lines = {}, collections.defaultdict(dict)
    pf, pl = os.path.join(outdir, "profile_funcs.tsv"), os.path.join(outdir, "profile_lines.tsv")
    if os.path.exists(pf):
        for l in open(pf).read().splitlines()[1:]:
            a, b = l.split("\t")
            funcs[a] = float(b)
    if os.path.exists(pl):
        byname = collections.defaultdict(set)
        for f in source_files():
            rel = os.path.relpath(f, ROOT)
            byname[os.path.basename(rel)].add(rel)
        ambiguous = set()
        for l in open(pl).read().splitlines()[1:]:
            a, b, c = l.split("\t")
            if int(b) == 0:
                continue
            if "/" not in a:
                cands = byname.get(a, set())
                if len(cands) != 1:
                    if cands:
                        ambiguous.add(a)
                    continue
                a = next(iter(cands))
            lines[a][int(b)] = lines[a].get(int(b), 0.0) + float(c)
        if ambiguous:
            print(f"profile: {len(ambiguous)} basenames map to several sources and were dropped: {sorted(ambiguous)[:6]}")
    return funcs, lines


SELFTEST_SRC = r"""
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <math.h>
typedef int BOOL;
struct list_head { struct list_head *next, *prev; };
#define list_for_each(el, head) for (el = (head)->next; el != (head); el = el->next)
void vec_push(int);
int lre_compile(int);
struct S { int n; double* a; long total; };
struct P { char a; long b; char c; int d; };
void g(void);
void lf(struct S* s, const int* items, int m) { for (int i = 0; i < m; i++) { s->a[i] = items[i]; g(); } }
double fp(const double* a, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
int pk(struct P* p) { return p->a + p->c; }
typedef struct { double v[2]; } f64x2;
f64x2 vaddq_f64(f64x2 a, f64x2 b);
f64x2 vld1q_f64(const double* p);
double fx_vsum(const double* x, int n) { f64x2 acc = { { 0, 0 } }; for (int i = 0; i + 2 <= n; i += 2) acc = vaddq_f64(acc, vld1q_f64(&x[i])); return acc.v[0] + acc.v[1]; }
struct node { int key; struct node* next; };
struct table { struct node** buckets; int n; };
struct flags { BOOL a; BOOL b; BOOL c; int x; };
struct shared { _Atomic long hits; _Atomic long misses; pthread_mutex_t mu; };
struct item { int a; int b; double c; long d; double e[2]; };
struct big { int a; char pad1[100]; int b; char pad2[100]; int c; };
struct big2 { int a; char pad1[100]; int b; char pad2[100]; int c; };
void note_f(float);
void fx_fis(float* o, const float* a, const float* x, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f + 1.0f; note_f(x[i]); } }
int fx_big2(struct big2* q) { return q->a; }
struct node2 { int key; struct node2* next; };
int fx_big(struct big* p) { return p->a + p->b + p->c; }
int fx_goto(int n) { int i = 0; again: i++; if (i < n) goto again; return i; }
int fx_tmp(int n) { int* t = malloc(sizeof(int) * 64); if (!t) return 0; t[0] = n; n = t[0]; free(t); return n; }
void fx_heavy(void) { void* a = malloc(1); void* b = malloc(2); void* c = calloc(1, 3); void* d = realloc(a, 4); free(b); free(c); free(d); }
void fx_sync(int fd) { fsync(fd); }
int fx_sock(void) { int s = socket(2, SOCK_STREAM, 0); return connect(s, 0, 0); }
struct acc { long total; int n; struct { long sum; } *st; };
void use(void*);
int g_fn(int);
int fx_churn(int n) { for (int i = 0; i < n; i++) { char* p = malloc(16); use(p); free(p); } return 0; }
void fx_bound(struct acc* s, int* a) { for (int i = 0; i < s->n; i++) { a[i] = 0; use(a); } }
void fx_strlen(const char* s, char* o) { for (size_t i = 0; i < strlen(s); i++) o[i] = s[i]; }
void fx_unswitch(int* a, int n, int mode) { for (int i = 0; i < n; i++) { if (mode) a[i] = 1; else a[i] = 2; } }
int fx_cold(int* a, int n) { for (int i = 0; i < n; i++) { if (a[i] < 0) { fprintf(stderr, "bad"); return -1; } } return 0; }
void fx_mod(int* a, int n, int m) { for (int i = 0; i < n; i++) a[i] = i % m + i % 1000 + i % 1024; }
void fx_stride(int* a, const int* b, int n, int k) { for (int i = 0; i < n; i++) a[i] = b[i * k]; }
void fx_accum(struct acc* s, const int* it, int n) { for (int i = 0; i < n; i++) s->total += it[i]; }
void fx_deep(struct acc* s, const int* it, int n) { for (int i = 0; i < n; i++) s->st->sum = it[i]; }
long fx_tr(long* o, const long* a, int n) { long sum = 0; for (int i = 0; i < n; i++) { o[i] = a[i] * 2; sum += o[i]; } return sum; }
void fx_streams(int* a, int* b, int* c, int* d, int* e, int* f, int n) { for (int i = 0; i < n; i++) a[i] = b[i] + c[i] + d[i] + e[i] + f[i]; }
void fx_carried(int* a, int n) { for (int i = 1; i < n; i++) a[i] = a[i - 1] + 1; }
void fx_sys(int fd, const char* b, int n) { for (int i = 0; i < n; i++) write(fd, b + i, 1); }
void fx_lock(pthread_mutex_t* m, _Atomic long* c, int n) { for (int i = 0; i < n; i++) { pthread_mutex_lock(m); atomic_fetch_add(c, 1); } }
void fx_fmt(char* o, const int* a, int n) { for (int i = 0; i < n; i++) o += sprintf(o, "%d,", a[i]); }
int fx_chase(struct node* p) { int s = 0; while (p) { s += p->key; p = p->next; } return s; }
const char* fx_scan(const char* p, const char* e) { while (p < e && *p != '\n') p++; return p; }
int fx_bs(const int* a, int n, int x) { int lo = 0, hi = n; while (lo < hi) { int mid = (lo + hi) >> 1; if (a[mid] < x) lo = mid + 1; else hi = mid; } return lo; }
int cmp(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }
void fx_sort(int* a, int n) { qsort(a, n, sizeof(int), cmp); }
void fx_memset(void) { char big[4096]; memset(big, 0, sizeof big); use(big); }
void fx_realloc(int** p, int n) { int cap = 0; for (int i = 0; i < n; i++) { *p = realloc(*p, (cap + 1) * sizeof(int)); cap++; } }
void fx_writes(int fd) { write(fd, "a", 1); write(fd, "b", 1); }
void* fx_map(int fd) { return mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0); }
long fx_atomic(_Atomic long* c) { return atomic_load(c); }
long fx_aos(const struct item* it, int n) { long s = 0; for (int i = 0; i < n; i++) s += it[i].a; return s; }
void fx_push(int n) { for (int i = 0; i < n; i++) vec_push(i); }
void fx_cpy(char* d, const int* s, int n) { for (int i = 0; i < n; i++) memcpy(d + 4 * i, &s[i], 4); }
double fx_libm(const double* a, int n) { double s = 0; for (int i = 0; i < n; i++) s += sqrt(a[i]) * (double)g_fn(i); return s; }
struct ops { int (*fn)(int); };
void fx_ind(struct ops* o, int* a, int n) { for (int i = 0; i < n; i++) a[i] = o->fn(a[i]); }
void fx_div(int* a, int n, int d) { for (int i = 0; i < n; i++) a[i] = a[i] / d; }
void fx_alloc(char** o, int n) { for (int i = 0; i < n; i++) o[i] = malloc(8); }
void fx_regex(int n) { for (int i = 0; i < n; i++) lre_compile(0); }
int fx_merge(const int* x, int na, const int* y, int nb, int* o) { int p = 0, q = 0, c = 0; while (p < na && q < nb) { if (x[p] < y[q]) p++; else if (y[q] < x[p]) q++; else { o[c++] = x[p]; p++; q++; } } return c; }
void fx_list(struct list_head* h) { struct list_head* el; list_for_each(el, h) { use(el); } }
struct grid { int w; float* cell; struct grid* parent; struct { struct grid* g; } *meta; };
struct rec48 { double a, b, c, d, e, f; };
double fx_rec(struct rec48 r) { return r.a + r.f; }
double fx_copy(const struct rec48* rs, int n) { double s = 0; for (int i = 0; i < n; i++) { struct rec48 r = rs[i]; s += r.a; } return s; }
float fx_sub(float* a, const int* idx, int n) { float t = 0; for (int i = 0; i < n; i++) { a[idx[i] * 2] += 1; t += a[idx[i] * 2]; a[idx[i] * 2] *= t; } return t; }
float fx_chain(struct grid* g, int n) { float t = 0; for (int i = 0; i < n; i++) { t += g->meta->g->cell[i]; g->meta->g->w++; t += g->meta->g->w; } return t; }
void fx_mbase(struct grid* g, int* out, int n) { for (int i = 0; i < n; i++) { out[i] = (int)g->cell[i]; use(out); } }
float fx_row(const float* a, int m, int n) { float t = 0; for (int i = 0; i < m; i++) for (int j = 0; j < n; j++) t += a[i * n + j]; return t; }
float fx_aop(struct grid** items, int n) { float t = 0; for (int i = 0; i < n; i++) t += items[i]->w; return t; }
void fx_unlink(struct node** head, int key) { struct node *p = *head, *prev = NULL; while (p) { if (p->key == key) { if (prev) prev->next = p->next; else *head = p->next; return; } prev = p; p = p->next; } }
void fx_alias(float* d, float* s, int n) { for (int i = 0; i < n; i++) d[i] = s[0] * d[i]; }
size_t fx_slen(const char* s) { return strlen(s) + strlen(s); }
void fx_fuse(float* a, const float* b, float* c, int n) {
    for (int i = 0; i < n; i++) { a[i] = b[i] * 2.0f; }
    for (int i = 0; i < n; i++) { c[i] = b[i] + 1.0f; }
}
float fx_col(float a[][1024], int n) { float t = 0; for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) { t += a[j][i]; } } return t; }
void fx_tile(float b[][1024], float a[][1024], int n) { for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) { b[j][i] = a[i][j]; } } }
struct ev { int kind; float v; };
void ka(float); void kb(float); void kc(float);
void fx_kind(const struct ev* ev, int n) { for (int i = 0; i < n; i++) { switch (ev[i].kind) { case 0: ka(ev[i].v); break; case 1: kb(ev[i].v); break; case 2: kc(ev[i].v); break; } } }
struct ent { int active; float x, vx; };
void fx_flag(struct ent* e, int n) { for (int i = 0; i < n; i++) { if (!e[i].active) continue; e[i].x += e[i].vx; } }
double fx_gather(const int* idx, const double* val, int n) { double t = 0; for (int i = 0; i < n; i++) { t += val[idx[i]]; } return t; }
void ng_xtu(float);
void fx_xtu(const float* a, int n) { for (int i = 0; i < n; i++) { ng_xtu(a[i]); } }
struct tnode { struct tnode* left; struct tnode* right; struct tnode* parent; int key; };
enum shade { SH_A, SH_B, SH_C };
struct cell { enum shade s0; double w; enum shade s1; int k; enum shade s2; };
struct rec40 { double a, b, c, d; int e; int f; };
double fx_straddle(const struct rec40* r, int n) { double t = 0; for (int i = 0; i < n; i++) { t += r[i].a + r[i].f; } return t; }
int fx_tsum(const struct tnode* t, const struct cell* c) { return t->key + c->k + (int)c->s0 + (int)c->s1 + (int)c->s2; }
"""

SELFTEST_NEG = r"""
void ng_unswitch(int* a, int n) { for (int i = 0; i < n; i++) { if (a[i]) a[i] = 1; } }
void ng_mod(int* a, int n) { for (int i = 0; i < n; i++) a[i] = i % 64; }
void ng_stride(int* a, const int* b, int n) { for (int i = 0; i < n; i++) a[i] = b[i]; }
double ng_fp(const double* a, int n) { double s0 = 0, s1 = 0; for (int i = 0; i + 1 < n; i += 2) { s0 += a[i]; s1 += a[i + 1]; } return s0 + s1; }
double ng_vsum(const double* x, int n) { f64x2 a0 = { { 0, 0 } }, a1 = a0; for (int i = 0; i + 4 <= n; i += 4) { a0 = vaddq_f64(a0, vld1q_f64(&x[i])); a1 = vaddq_f64(a1, vld1q_f64(&x[i + 2])); } return a0.v[0] + a1.v[0]; }
double ng_series(int n) { double t = 1, s = 0; for (int i = 1; i < n; i++) { t *= 0.5; s += t; } return s; }
int ng_merge(const int* x, int na, const int* y, int nb) { int p = 0, q = 0, c = 0; while (p < na && q < nb) { int a = x[p], b = y[q]; c += a == b; p += a <= b; q += b <= a; } return c; }
struct ng_ordered { double d; long l; int i; short s; char c; char c2; };
int* ng_tmp(void) { int* t = malloc(64); return t; }
void ng_row(int* o, const int* a, int m, int n) { for (int i = 0; i < m; i++) { const int* row = a + i * n; for (int j = 0; j < n; j++) o[j] = row[j]; } }
void ng_restrict(float* restrict d, const float* restrict s, int n) { for (int i = 0; i < n; i++) d[i] = s[0] * d[i]; }
double ng_byptr(const struct rec48* r) { return r->a; }
size_t ng_slen(const char* s) { size_t n = strlen(s); return n + n; }
void ng_unlink(struct node** head, int key) { struct node** pp = head; while (*pp) { if ((*pp)->key == key) { *pp = (*pp)->next; return; } pp = &(*pp)->next; } }
void ng_accum(int** ps, int n) { for (int i = 0; i < n; i++) { int* p = ps[i]; p[0]++; } }
void ng_elem(struct node* h) { struct node* p; for (p = h; p; p = p->next) p->key++; }
const char* ng_scan(const char* p, const char* e) { return memchr(p, '\n', e - p); }
void ng_sink(float*, int);
void ng_fis(float* o, const float* a, int n) { for (int i = 0; i < n; i++) { o[i] = a[i] * 2.0f; ng_sink(o, i); } }
void ng_xtu(float v) { (void)v; }
void ng_nofuse(float* a, const float* b, float* c, int n) {
    for (int i = 0; i < n; i++) { a[i] = b[i] * 2.0f; }
    for (int i = 0; i < n; i++) { c[i] = a[i + 1]; }
}
int ng_rowwalk(int a[][1024], int n) { int t = 0; for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) { t += a[i][j]; } } return t; }
void ng_twokinds(const struct ev* ev, int n) { for (int i = 0; i < n; i++) { const struct ev* q = &ev[i]; switch (q->kind) { case 0: ka(q->v); break; default: kb(q->v); } } }
void ng_thresh(struct ent* e, int n, float t) { for (int i = 0; i < n; i++) { struct ent* q = &e[i]; if (q->x > t) continue; q->x += q->vx; } }
void ng_lut(int* o, const unsigned char* s, const int* tab, int n) { for (int i = 0; i < n; i++) { o[i] = tab[s[i]]; } }
static void ng_local(float v) { (void)v; }
void ng_samefile(const float* a, int n) { for (int i = 0; i < n; i++) { ng_local(a[i]); } }
"""

def selftest():
    must = {"alloc-churn", "bound-reload", "call-in-bound", "invariant-branch", "cold-in-loop", "modulo-in-loop",
            "non-unit-stride", "accum-through-pointer", "deep-chain", "transform-and-reduce", "many-streams",
            "loop-carried", "syscall-in-loop", "lock-in-loop", "format-in-loop", "pointer-chase", "bytewise-scan",
            "binary-search", "sort-call", "big-memset", "linear-realloc", "write-sequence", "mmap-no-madvise",
            "seq-cst-atomic", "bool-fields", "false-sharing-review", "chained-hash", "stack-array",
            "aos-partial-touch", "push-per-element", "regex-compile-in-loop", "div-in-loop", "alloc-in-loop",
            "indirect-call-in-loop", "macro-list-walk", "single-vector-accumulator", "branchy-merge",
            "goto-loop", "local-heap-temp", "alloc-heavy-fn", "fsync-call", "tcp-nodelay-missing", "cold-path-in-hot-fn",
            "ptr-repeated-subscript", "ptr-repeated-chain", "ptr-member-base-index", "ptr-row-pointer", "ptr-array-of-pointers",
            "ptr-prev-unlink", "ptr-strlen-repeat", "loop-fission", "loop-fusion", "loop-interchange", "cache-tiling",
            "kind-dispatch", "flag-filter", "random-gather", "xtu-call"}
    compiler_must = {"licm-blocked", "gvn-clobbered", "padding", "fp-serial-reduction", "smallcpy-blocks-vec", "libm-in-loop",
                     "reorder-savings", "hot-fields-spread", "ptr-struct-by-value", "ptr-struct-copy-in-loop", "ptr-restrict-candidate",
                     "ptr-to-index", "wide-enum-field", "line-straddle"}
    neg_allowed = {"stack-array", "padding", "licm-blocked", "gvn-clobbered", "loop-spills", "alloc-in-loop", "pointer-chase"}
    global ROOT
    old = ROOT
    bad = 0
    with tempfile.TemporaryDirectory(prefix="dodscan") as d:
        os.makedirs(os.path.join(d, "src"))
        open(os.path.join(d, "src", "fx.c"), "w").write(SELFTEST_SRC)
        open(os.path.join(d, "src", "ng.c"), "w").write(SELFTEST_NEG)
        out = os.path.join(d, "out")
        os.makedirs(out)
        ROOT = d
        configure(d, os.path.join(d, "none.json"))
        try:
            r = subprocess.run(["clang", "-O2", "-std=gnu17", "-S", "-o", os.devnull, "-Wno-everything", "-Wpadded",
                                "-Xclang", "-fdump-record-layouts-complete",
                                "-ftrivial-auto-var-init=zero", "-fstack-protector-strong",
                                "-fsave-optimization-record=yaml", "-foptimization-record-passes=" + REMARK_PASSES,
                                "-foptimization-record-file=" + os.path.join(out, "fx.c.opt.yaml"), "src/fx.c"],
                               cwd=d, capture_output=True, text=True)
            open(os.path.join(out, "fx.c.padded"), "w").write(r.stderr)
            open(os.path.join(out, "fx.c.layout"), "w").write(r.stdout)
            src_lines = SELFTEST_SRC.split("\n")
            cold_line = next(i for i, l in enumerate(src_lines, 1) if l.startswith("int fx_cold("))
            big_line = next(i for i, l in enumerate(src_lines, 1) if l.startswith("int fx_big("))
            with open(os.path.join(out, "profile_funcs.tsv"), "w") as pf:
                pf.write("func\tinclusive_share\nfx_cold\t0.25\nfx_big\t0.2\n")
            with open(os.path.join(out, "profile_lines.tsv"), "w") as pl:
                pl.write(f"file\tline\tself_share\nsrc/fx.c\t{cold_line}\t0.125\nsrc/fx.c\t{big_line}\t0.05\n")
            if r.returncode:
                print("selftest: the fixture does not compile:\n" + r.stderr[-800:])
                return 1
            import io
            import contextlib
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                findings = report(out, limit=0, want=set())
        finally:
            ROOT = old
            configure(old)
    pos = collections.defaultdict(set)
    neg = collections.defaultdict(list)
    heat_ok = any(x["func"] == "fx_cold" and abs(x["heat"] - 0.125) < 1e-9 for x in findings.get("cold-in-loop", []))
    print(f"  {'ok  ' if heat_ok else 'MISS'} heat join (loop self-time from profile_lines.tsv)")
    bad += not heat_ok
    typed_ok = not any(x["func"] == "big2" for x in findings.get("hot-fields-spread", []))
    print(f"  {'ok  ' if typed_ok else 'FALSE'} hot fields are bound to the accessing variable's type (big2 shares names, has no hot user)")
    bad += not typed_ok
    for cid, rows in findings.items():
        for x in rows:
            if x["file"].endswith("ng.c"):
                neg[cid].append(f"{x['func']}:{x['what']}")
            else:
                pos[cid].add(x["func"])
    for cid in sorted(must | compiler_must):
        hit = bool(pos.get(cid))
        print(f"  {'ok  ' if hit else 'MISS'} {cid:24s} {','.join(sorted(pos.get(cid, ())))[:80]}")
        bad += not hit
    for cid, rows in sorted(neg.items()):
        if cid in neg_allowed or (cid == "alloc-heavy-fn"):
            continue
        print(f"  FALSE {cid:24s} {rows}")
        bad += 1
    unknown = set(findings) - set(CHECK_IDS)
    if unknown:
        print("  checks emitted but not declared:", sorted(unknown))
        bad += 1
    print(f"selftest: {'ok' if not bad else str(bad) + ' failure(s)'}")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["remarks", "profile", "aggregate", "report", "all", "selftest"])
    ap.add_argument("--root", default=None, help="project root (default: nearest dir with dod_scan.json, compile_commands.json or .git)")
    ap.add_argument("--project", default=None, help="project config JSON (default: <root>/dod_scan.json or <root>/tools/dod_scan.json)")
    ap.add_argument("--binary", default=None)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--out", default=None)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--check", action="append", help="print only these checks' tables")
    a = ap.parse_args()
    configure(a.root, a.project)
    if a.out is None:
        a.out = DEFAULT_OUT
    if a.binary is None:
        a.binary = os.path.join(ROOT, PROJECT.get("binary", "a.out"))
    if a.cmd == "selftest":
        sys.exit(1 if selftest() else 0)
    os.makedirs(a.out, exist_ok=True)
    if a.cmd in ("remarks", "all"):
        cmds = remark_cmds()
        print(f"remarks: {len(cmds)} translation units, {a.jobs} at a time, nice 19, passes {REMARK_PASSES}", flush=True)
        bad = 0
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for src, rc, err in ex.map(lambda j: run_remark(j, a.out), cmds):
                if rc:
                    bad += 1
                    print(f"  FAIL {src}: {err.strip()[-300:]}", flush=True)
        print(f"remarks: done, {bad} failed", flush=True)
        if bad:
            sys.exit(1)
    if a.cmd in ("profile", "all"):
        profile(a.out, a.binary, a.seconds)
    if a.cmd == "aggregate":
        aggregate(a.out, a.binary)
    if a.cmd in ("report", "all"):
        report(a.out, a.limit, set(a.check) if a.check else None)


if __name__ == "__main__":
    main()
