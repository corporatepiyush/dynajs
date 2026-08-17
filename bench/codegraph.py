#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Piyush Katariya
#
# @author Piyush Katariya
"""codegraph_c.py -- parse a C tree into a graph and query it.

Targets C11/C17 with the GNU and Clang extensions real code actually uses.

There is no compiler frontend here and that is deliberate, not a shortcut. A
real C parse needs the preprocessor, which needs the include path, which needs
the build system -- so a "proper" C analyzer only works on a tree you can
already build, on the machine that builds it. This one works on any directory
of C you can read. Comments and string/char literals are blanked (byte offsets
preserved, so every line number stays exact), functions are found by matching
braces, and a call is any identifier followed by `(`.

What that buys, and nothing else in this repo has: byte-accurate struct layout.
`layout` and `struct_size` carry the LP64 offset, size and padding of every
top-level field, so "which structs waste memory to alignment holes" and "which
hot object needs two cache lines" are answerable facts rather than guesses.

WHAT IT CANNOT SEE, stated once and then measured per function:
  * calls through function pointers -- `n_fnptr_calls`, and query 1
  * the preprocessor's own control flow -- `config_blocks` lists the gates
  * bodies produced by macro expansion -- `macros.n_uses` counts the sites

Every call site lands in exactly one of four buckets, because conflating them
is how a C repo comes to read as far blinder than it is:
  edge              resolved to a definition in this tree
  n_macro_calls     a macro defined in this tree; a preprocessor construct,
                    not a hole in the graph
  n_external_calls  libc/POSIX/compiler builtin, or a prototype declared here
                    and defined outside the tree -- a boundary we chose
  unresolved_calls  genuinely unknown; this is the honest blindness column

Usage:
  python3 codegraph_c.py /path/to/repo --report
  python3 codegraph_c.py /path/to/repo --list
  python3 codegraph_c.py /path/to/repo 1 19 --limit 20
  python3 codegraph_c.py --deps          # (none -- pure standard library)"""
__author__ = "Piyush Katariya"
__license__ = "MIT"

# ---------------------------------------------------------------------------
# Self-contained on purpose: this one file is the whole tool. Copy it anywhere
# and run it. Requires CPython 3.14+ and its bundled SQLite 3.37 or newer --
# 3.37 for STRICT tables, which the schema uses throughout.
#
# Dependencies are declared in DEPS with a reason and installed with
# --install-deps. A grammar-backed analyzer REFUSES to run without its grammar:
# there is no regex fallback, and an empty graph reads exactly like a clean
# repository. codegraph_python.py and codegraph_c.py need no grammar at all.
#
# Nothing here is imported from a sibling file, and the schema below is this
# language's own. Other analyzers in this repo differ wherever their languages
# differ. Edit this file directly.
# ---------------------------------------------------------------------------

import sys as _sys

if _sys.version_info < (3, 14):                          # noqa: E402
    _sys.exit(
        "codegraph needs CPython 3.14 or newer; this is %d.%d.%d at %s.\n"
        "The schema uses STRICT tables (SQLite 3.37+) and codegraph_python.py\n"
        "parses with the running interpreter's own grammar, so on an older\n"
        "Python it would silently see less of a repository than is there.\n"
        "The other analyzers share this floor rather than each having one."
        % (_sys.version_info[0], _sys.version_info[1], _sys.version_info[2],
           _sys.executable))

import argparse
import array
import bisect
import csv
import hashlib
import importlib
import importlib.util
import os
import re
import sqlite3
import stat
import subprocess
import sys
import time
from dataclasses import dataclass
from dataclasses import dataclass, field
from itertools import accumulate
from typing import Any, Callable, Iterable, Optional, Sequence
from typing import Any, Callable, Iterator, Optional
from typing import Any, Optional


# ==========================================================================
# _deps.py
# Dependency declaration and optional installation.
#
# Every language analyzer declares exactly what it needs and why. Nothing is
# installed behind the user's back: `ensure()` only reports, and only
# `--install-deps` actually runs pip.
#
# An analyzer must still RUN with nothing installed. A missing grammar downgrades
# the parse from a syntax tree to regex scanning; it never aborts the run. Which
# mode was used is recorded in the `meta` table so a query result can never be
# mistaken for something more precise than it is.
# ==========================================================================

@dataclass(frozen=True)
class Dep:
    """One importable module and the pip requirement that provides it."""
    module: str
    pip: str
    why: str
    optional: bool = True
    #: Minimum version we have actually verified against, for the record.
    verified: str = ""

    @property
    def present(self) -> bool:
        try:
            return importlib.util.find_spec(self.module) is not None
        except (ImportError, ValueError):
            return False

    def version(self) -> str:
        try:
            mod = importlib.import_module(self.module)
        except Exception:
            return ""
        for attr in ("__version__", "VERSION", "version"):
            v = getattr(mod, attr, None)
            if isinstance(v, str):
                return v
            if isinstance(v, tuple):
                return ".".join(str(p) for p in v)
        try:
            from importlib.metadata import version as _v
            return _v(self.pip.split("[")[0].split("=")[0].split(">")[0])
        except Exception:
            return "?"

@dataclass
class DepSet:
    """The dependency surface of one analyzer."""
    lang: str
    deps: list[Dep] = field(default_factory=list)

    def missing(self) -> list[Dep]:
        return [d for d in self.deps if not d.present]

    def present(self) -> list[Dep]:
        return [d for d in self.deps if d.present]

    def required_missing(self) -> list[Dep]:
        return [d for d in self.missing() if not d.optional]

    # -- reporting ---------------------------------------------------------
    def describe(self) -> str:
        out = ["dependencies for codegraph-%s:" % self.lang]
        if not self.deps:
            out.append("  (none -- pure standard library)")
            return "\n".join(out)
        for d in self.deps:
            mark = "ok " if d.present else ("MISSING" if not d.optional else "absent ")
            ver = d.version() if d.present else ""
            tag = "required" if not d.optional else "optional"
            out.append("  [%-7s] %-28s %-10s %s" % (mark, d.pip, ver, tag))
            out.append("             %s" % d.why)
            if d.verified:
                out.append("             verified against %s" % d.verified)
        miss = self.missing()
        if miss:
            out.append("")
            out.append("install with:")
            out.append("  %s" % self.pip_command())
            out.append("or let the tool do it:")
            out.append("  python3 %s --install-deps" % _script_name())
        return "\n".join(out)

    def pip_command(self, missing_only: bool = True) -> str:
        want = self.missing() if missing_only else self.deps
        if not want:
            return "(nothing to install)"
        return "%s -m pip install %s" % (
            sys.executable, " ".join(sorted(d.pip for d in want)))

    # -- installation ------------------------------------------------------
    def install(self, quiet: bool = False, only_binary: bool = True) -> bool:
        """pip-install everything missing. Returns True if all present after."""
        want = self.missing()
        if not want:
            if not quiet:
                print("all dependencies already present")
            return True
        cmd = [sys.executable, "-m", "pip", "install"]
        if only_binary:
            # Source builds of a tree-sitter grammar need a C toolchain and
            # take minutes. If there is no wheel we would rather fail loudly
            # and fall back to regex than silently start compiling.
            cmd += ["--only-binary", ":all:"]
        cmd += sorted(d.pip for d in want)
        if not quiet:
            print("running: %s" % " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        rc = proc.returncode
        out = (proc.stdout or "") + (proc.stderr or "")
        if not quiet and out.strip():
            print(out.rstrip())

        if rc != 0 and "externally-managed-environment" in out:
            # A Homebrew or distro Python refuses to be written to (PEP 668).
            # Telling the user to pass --break-system-packages would be
            # advising them to damage the interpreter their OS depends on.
            print(_pep668_advice(self))
            return False
        if rc != 0 and only_binary:
            if not quiet:
                print("no wheel available for this interpreter; "
                      "retrying without --only-binary (needs a C compiler)")
            rc = subprocess.call([c for c in cmd
                                  if c not in ("--only-binary", ":all:")])
        importlib.invalidate_caches()
        still = self.missing()
        if still and not quiet:
            print("still missing: %s" % ", ".join(d.pip for d in still))
            print("the analyzer will run in degraded (regex) mode")
        return not still

def _script_name() -> str:
    import os
    return os.path.basename(sys.argv[0] or "codegraph_<lang>.py")

def _pep668_advice(ds: "DepSet") -> str:
    return (
        "\nthis Python is externally managed (PEP 668) -- pip will not write "
        "to it,\nand overriding that with --break-system-packages can break "
        "the interpreter\nyour OS depends on. Use a virtual environment "
        "instead:\n\n"
        "  python3 -m venv .venv\n"
        "  .venv/bin/pip install %s\n"
        "  .venv/bin/python %s <repo>\n\n"
        "There is no way to run without the grammar: an analyzer with no\n"
        "parser refuses rather than emitting an empty graph, because an\n"
        "empty graph reads exactly like a clean repository."
        % (" ".join(sorted(d.pip for d in ds.missing())), _script_name()))

TREE_SITTER = Dep(
    module="tree_sitter",
    pip="tree-sitter>=0.25",
    why="incremental parser runtime. Without it a grammar-backed analyzer "
        "will NOT run -- there is no regex fallback, because an empty graph "
        "reads exactly like a clean repository",
    verified="0.26.0 (cp314 macOS arm64 wheel)",
)

def grammar(lang: str, module: str, pip: str, verified: str = "") -> Dep:
    return Dep(
        module=module,
        pip=pip,
        why="tree-sitter grammar for %s. Required: without it this analyzer "
            "refuses to run rather than produce an empty graph" % lang,
        verified=verified,
    )


# ==========================================================================
# _ts.py
# tree-sitter loading, with an honest fallback.
#
# Two rules govern this module.
#
# 1. A missing grammar is not an error. The analyzer degrades to regex scanning
#    and says so. A tool that refuses to start is worth less than a tool that
#    tells you which of its answers are approximate.
#
# 2. The parse mode is recorded, per run, in the `meta` table. Every report
#    prints it. `n_parse_errors` on `files` counts tree-sitter ERROR nodes, so a
#    file the grammar could not handle is visible rather than silently thin.
#
# The py-tree-sitter API changed incompatibly at 0.22/0.23 (`Language(ptr, name)`
# became `Language(ptr)`, `Parser.set_language()` became the `parser.language`
# property). Everything here targets the >=0.25 API and probes for the old one so
# an older wheel already on the box does not produce a confusing AttributeError.
# ==========================================================================

MODE_TREE_SITTER = "tree-sitter"

MODE_REGEX = "regex-fallback"

MODE_NATIVE = "native-ast"

MODE_BRACE_SCAN = "brace-scan"

@dataclass
class ParserHandle:
    """A parser plus the story of how we got it."""
    mode: str
    parser: Any = None
    language: Any = None
    lang_name: str = ""
    grammar_pip: str = ""
    grammar_version: str = ""
    runtime_version: str = ""
    note: str = ""

    @property
    def ok(self) -> bool:
        return self.parser is not None

    def parse(self, src: bytes):
        return self.parser.parse(src)

    def banner(self) -> str:
        if self.mode == MODE_TREE_SITTER:
            return "parser: tree-sitter %s + %s %s" % (
                self.runtime_version, self.grammar_pip, self.grammar_version)
        if self.mode == MODE_NATIVE:
            return "parser: %s" % self.note
        if self.mode == MODE_BRACE_SCAN:
            # The INTENDED parser for C, not a degradation: there is no
            # grammar to install and nothing approximate about the result.
            # Calling it a fallback teaches readers to distrust correct
            # answers (the setup() docstring's exact argument).
            return "parser: brace-scan (%s)" % self.note
        return "parser: REGEX FALLBACK (%s) -- spans and nesting are approximate" % self.note

def load(lang_name: str, grammar_module: str, grammar_pip: str,
         symbol: str = "language") -> ParserHandle:
    """Build a tree-sitter parser for `lang_name`, or explain why not."""
    try:
        ts = importlib.import_module("tree_sitter")
    except ImportError:
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip,
                            note="tree_sitter not installed")
    try:
        gm = importlib.import_module(grammar_module)
    except ImportError:
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip,
                            note="%s not installed" % grammar_pip)

    fn = getattr(gm, symbol, None) or getattr(gm, "language", None)
    if fn is None:
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip,
                            note="%s exposes no %s()" % (grammar_module, symbol))
    try:
        ptr = fn()
    except Exception as exc:                                   # pragma: no cover
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip,
                            note="%s() raised %s" % (symbol, exc))

    language = _make_language(ts, ptr, lang_name)
    if language is None:
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip,
                            note="ABI mismatch between tree-sitter runtime and "
                                 "%s -- upgrade both together" % grammar_pip)

    abi_note = _check_abi(ts, language, grammar_pip)
    if abi_note:
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip, note=abi_note)
    parser = _make_parser(ts, language)
    if parser is None:
        return ParserHandle(mode=MODE_REGEX, lang_name=lang_name,
                            grammar_pip=grammar_pip,
                            note="could not attach language to parser")

    return ParserHandle(
        mode=MODE_TREE_SITTER, parser=parser, language=language,
        lang_name=lang_name, grammar_pip=grammar_pip,
        grammar_version=_ver(gm, grammar_pip),
        runtime_version=_ver(ts, "tree-sitter"),
    )

def _make_language(ts: Any, ptr: Any, name: str) -> Optional[Any]:
    if isinstance(ptr, getattr(ts, "Language", ())):
        return ptr
    for args in ((ptr,), (ptr, name)):          # >=0.22 first, then legacy
        try:
            return ts.Language(*args)
        except (TypeError, ValueError):
            continue
        except Exception:
            return None
    return None

def _check_abi(ts: Any, language: Any, grammar_pip: str) -> str:
    """Refuse a grammar the runtime cannot speak, with a message that says why.

    Several grammars have not been rebuilt in well over a year and sit at an
    older ABI than the runtime's floor. When that floor rises, construction
    fails somewhere deep in the C extension with nothing naming the culprit.
    Checking here turns that into one sentence naming the package and the two
    numbers involved.
    """
    abi = getattr(language, "abi_version", None)
    if abi is None:
        abi = getattr(language, "version", None)
    if abi is None:
        return ""
    lo = getattr(ts, "MIN_COMPATIBLE_LANGUAGE_VERSION", None)
    hi = getattr(ts, "LANGUAGE_VERSION", None)
    if lo is not None and abi < lo:
        return ("%s is ABI %d but this tree-sitter runtime accepts %d-%s; "
                "upgrade the grammar or pin tree-sitter lower"
                % (grammar_pip, abi, lo, hi if hi is not None else "?"))
    if hi is not None and abi > hi:
        return ("%s is ABI %d, newer than this tree-sitter runtime supports "
                "(max %d); upgrade tree-sitter" % (grammar_pip, abi, hi))
    return ""

def _make_parser(ts: Any, language: Any) -> Optional[Any]:
    try:                                        # >=0.22
        return ts.Parser(language)
    except TypeError:
        pass
    except Exception:
        return None
    try:
        p = ts.Parser()
        try:
            p.language = language               # >=0.22 property
        except AttributeError:
            p.set_language(language)            # <=0.21
        return p
    except Exception:                                          # pragma: no cover
        return None

def _ver(mod: Any, pip_name: str) -> str:
    v = getattr(mod, "__version__", None)
    if isinstance(v, str):
        return v
    try:
        from importlib.metadata import version
        return version(pip_name.split(">")[0].split("=")[0])
    except Exception:
        return "?"

def walk(node: Any) -> Iterator[Any]:
    """Every node in the subtree, parents before children.

    Uses an explicit stack rather than recursion: a minified bundle or a
    generated parser table nests deep enough to blow the Python stack, and a
    RecursionError halfway through a repo scan is indistinguishable from a
    crash.
    """
    stack = [node]
    while stack:
        n = stack.pop()
        yield n
        stack.extend(reversed(n.children))

def walk_cursor(node: Any) -> Iterator[tuple[Any, int]]:
    """Every node with its depth, using a TreeCursor (much faster than
    touching `.children`, which materialises a Python list per node)."""
    cursor = node.walk()
    depth = 0
    while True:
        yield cursor.node, depth
        if cursor.goto_first_child():
            depth += 1
            continue
        while not cursor.goto_next_sibling():
            if not cursor.goto_parent():
                return
            depth -= 1

def named_children(node: Any, *types: str) -> list[Any]:
    if not types:
        return [c for c in node.named_children]
    want = set(types)
    return [c for c in node.named_children if c.type in want]

def child_by_field(node: Any, field: str) -> Optional[Any]:
    return node.child_by_field_name(field)

def text_of(node: Any, src: bytes) -> str:
    return src[node.start_byte:node.end_byte].decode("utf-8", "replace")

def field_text(node: Any, field: str, src: bytes, default: str = "") -> str:
    c = node.child_by_field_name(field)
    return text_of(c, src) if c is not None else default

def descendants_of_type(node: Any, *types: str) -> Iterator[Any]:
    want = set(types)
    for n in walk(node):
        if n.type in want:
            yield n

def count_types(node: Any, counter_types: dict[str, str]) -> dict[str, int]:
    """One pass over a subtree, counting node types into named buckets.

    `counter_types` maps a tree-sitter node type to the metric it feeds. One
    walk for all metrics: walking a large function body once per metric is the
    difference between a repo scan taking seconds and taking minutes.
    """
    out: dict[str, int] = {}
    for n in walk(node):
        key = counter_types.get(n.type)
        if key is not None:
            out[key] = out.get(key, 0) + 1
    return out

def has_error(node: Any) -> bool:
    return node.has_error

def count_errors(root: Any) -> tuple[int, int]:
    """(error nodes, missing nodes) in the tree.

    A file with errors is still indexed -- tree-sitter recovers and the symbols
    around the damage are real. The count travels with the file row so a query
    can exclude, or specifically hunt, the parts we got wrong.
    """
    if not root.has_error:
        return 0, 0
    errs = miss = 0
    for n in walk(root):
        if n.type == "ERROR":
            errs += 1
        elif n.is_missing:
            miss += 1
    return errs, miss

class Query:
    """A compiled tree-sitter query, tolerant of the 0.24->0.25 API split.

    `Language.query()` was removed in 0.25 in favour of a standalone `Query`
    class and a `QueryCursor` for execution. Both spellings are probed so one
    analyzer source works across the wheels people actually have installed.
    """

    def __init__(self, handle: ParserHandle, source: str):
        self.ok = False
        self._q = None
        self._cursor_cls = None
        if not handle.ok:
            return
        try:
            ts = importlib.import_module("tree_sitter")
            qcls = getattr(ts, "Query", None)
            if qcls is not None:
                try:
                    self._q = qcls(handle.language, source)
                except TypeError:
                    self._q = handle.language.query(source)
            else:
                self._q = handle.language.query(source)
            self._cursor_cls = getattr(ts, "QueryCursor", None)
            self.ok = True
        except Exception:
            self.ok = False
            self._q = None

    def captures(self, node: Any) -> dict[str, list[Any]]:
        if not self.ok:
            return {}
        try:
            if self._cursor_cls is not None:
                return self._cursor_cls(self._q).captures(node)
            return self._q.captures(node)
        except Exception:
            return {}

    def matches(self, node: Any) -> list[Any]:
        if not self.ok:
            return []
        try:
            if self._cursor_cls is not None:
                return self._cursor_cls(self._q).matches(node)
            return self._q.matches(node)
        except Exception:
            return []


# ==========================================================================
# _core.py
# The part of a code graph that does not depend on the language.
#
# Every analyzer in this repo re-reads and re-parses the tree on every run and
# builds the whole graph in a `:memory:` database. A graph file on disk gets read
# after the code it describes has moved on, and a stale graph is worse than none:
# it answers confidently and wrongly.
#
# What lives here: the file walk, the universal schema, the build driver, the
# aggregate pass, the renderer and the CLI. What does not: anything that knows
# what a function looks like. That is the analyzer's job, and it is the only part
# that needs writing per language.
#
# The universal schema is deliberately wider than any one language needs. A
# column that is always zero for Go costs nothing and keeps one query catalogue
# readable across nine languages; a column that exists only for Rust would force
# every shared query to branch.
# ==========================================================================

SCHEMA_VERSION = 1

COMMON_SKIP_DIRS = {
    ".git", ".hg", ".svn", ".jj", ".idea", ".vscode", ".vs", ".claude",
    "node_modules", "bower_components", "vendor", "third_party", "thirdparty",
    "external", "externals", "deps", "Godeps", "_vendor",
    "__pycache__", ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox",
    ".venv", "venv", "env", ".env", "virtualenv",
    "build", "_build", "dist", "out", "target", "bin", "obj", ".gradle",
    ".next", ".nuxt", ".svelte-kit", ".parcel-cache", ".turbo", ".cache",
    "coverage", "htmlcov", ".nyc_output", "site-packages",
}

GENERATED_MARKERS = (
    "@generated", "DO NOT EDIT", "Code generated by", "AUTO-GENERATED",
    "autogenerated", "This file was automatically generated",
    "Generated by the protocol buffer compiler", "@flow-generated",
)

GENERATED_NAME_RE = re.compile(
    r'(\.min\.|\.bundle\.|[-_.](gen|generated|pb|g)\.|_pb2|\.g\.dart$'
    r'|\.designer\.|^zz_generated)', re.I)

TEST_PATH_RE = re.compile(
    r'(^|/)(tests?|test-d|spec|specs|__tests__|__snapshots__|testing|'
    r'e2e|integration[-_]tests?|testdata|test_data|test-data|'
    r'fixtures?)(/|$)', re.I)

TEST_NAME_RE_BY_LANG: dict[str, "re.Pattern[str]"] = {
    "python": re.compile(r'(^test_|_test\.py$|^conftest\.py$)'),
    "go": re.compile(r'_test\.go$'),
    "rust": re.compile(r'(^tests?\.rs$|_test\.rs$)'),
    "java": re.compile(r'(^Test[A-Z]|Tests?\.java$|TestCase\.java$|IT\.java$)'),
    "javascript": re.compile(r'(\.test\.|\.spec\.|^test-|-test\.)'),
    "typescript": re.compile(r'(\.test\.|\.spec\.|\.test-d\.|^test-|-test\.)'),
    "php": re.compile(r'(Test\.php$|^test_)'),
    "ruby": re.compile(r'(_spec\.rb$|_test\.rb$|^test_)'),
    "c": re.compile(r'(^test_|_test\.[ch]$|^t_)'),
}

TEST_NAME_RE = re.compile(r'(^test_|_test\.|\.test\.|\.spec\.)')

VENDOR_PATH_RE = re.compile(
    r'(^|/)(vendor|third_party|thirdparty|external|node_modules|deps)(/|$)', re.I)

MARKER_RE = re.compile(
    r'\b(TODO|FIXME|XXX|HACK|BUG|NOTE|WARNING|OPTIMIZE|REVIEW|DEPRECATED|'
    r'SAFETY|PANIC|UNSAFE)\b[ \t]*[:\-(]', re.I)

MAGIC_OK = {0, 1, 2, -1, 10, 100, 1000, 8, 16, 32, 64, 128, 256, 512, 1024,
            255, 65535, 4096, 24, 60, 365, 7, 12, 3, 4, 6}

def module_of(rel: str, depth: int = 2) -> str:
    """A stable grouping key for a path.

    Two levels, not one: `src/` alone puts an entire repo in one bucket, and
    the full directory makes every leaf its own module. Two levels is what
    actually separates subsystems in the repos this was tested against.
    """
    parts = rel.replace(os.sep, "/").split("/")
    if len(parts) <= 1:
        return "(root)"
    head = parts[:-1]
    if head and head[0] in ("src", "lib", "source", "internal", "pkg", "app"):
        head = head[:depth + 1]
    else:
        head = head[:depth]
    return "/".join(head) or "(root)"

def is_generated(name: str, head: str) -> bool:
    if GENERATED_NAME_RE.search(name):
        return True
    return any(m in head for m in GENERATED_MARKERS)

@dataclass
class FileRec:
    """One source file, already read and classified."""
    fid: int
    mid: int
    rel: str
    abspath: str
    text: str
    data: bytes
    lang: str
    is_test: bool
    is_generated: bool
    is_vendored: bool

@dataclass
class Buffers:
    """Row accumulators.

    Everything is buffered and flushed with `executemany`. Per-row `INSERT`
    across a million-symbol repo spends more time in the sqlite3 binding layer
    than in parsing.
    """
    params: list[tuple] = field(default_factory=list)
    fields: list[tuple] = field(default_factory=list)
    locals: list[tuple] = field(default_factory=list)
    literals: list[tuple] = field(default_factory=list)
    markers: list[tuple] = field(default_factory=list)
    attributes: list[tuple] = field(default_factory=list)
    imports: list[tuple] = field(default_factory=list)
    hazards: list[tuple] = field(default_factory=list)
    enum_members: list[tuple] = field(default_factory=list)
    edges: dict[tuple[int, int], list[int]] = field(default_factory=dict)
    callsites: set[tuple[int, int, int]] = field(default_factory=set)
    unresolved: dict[tuple[int, str], list[int]] = field(default_factory=dict)
    extra: dict[str, list[tuple]] = field(default_factory=dict)

    def rows(self, table: str) -> list[tuple]:
        """Accumulator for a language-specific table."""
        return self.extra.setdefault(table, [])

    def add_edge(self, caller: int, callee: int, same_file: bool,
                 same_module: bool, line: int = 0) -> None:
        key = (caller, callee)
        e = self.edges.get(key)
        if e is None:
            self.edges[key] = [1, int(same_file), int(same_module),
                               int(caller == callee)]
        else:
            e[0] += 1
        if line:
            self.callsites.add((caller, callee, line))

    def add_unresolved(self, caller: int, name: str, line: int) -> None:
        """A call we saw but could not point at a definition.

        This is the honesty column. Dynamic dispatch, reflection, function
        pointers and cross-language calls all land here, and a query that
        reasons over the call graph can check how blind it is before trusting
        its own answer.
        """
        key = (caller, name)
        u = self.unresolved.get(key)
        if u is None:
            self.unresolved[key] = [1, line]
        else:
            u[0] += 1

    def add_hazard(self, sid: int, pattern: str, category: str,
                   n: int = 1, line: int = 0) -> None:
        self.hazards.append((sid, pattern, category, n, line))

PRAGMAS = """
PRAGMA journal_mode=OFF;
PRAGMA synchronous=OFF;
PRAGMA page_size=16384;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-262144;
PRAGMA foreign_keys=OFF;
"""

BASE_SCHEMA = r"""
CREATE TABLE meta(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID, STRICT;

CREATE TABLE modules(
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    kind TEXT NOT NULL DEFAULT 'source',
    n_files INT NOT NULL DEFAULT 0,
    n_symbols INT NOT NULL DEFAULT 0,
    n_public INT NOT NULL DEFAULT 0,
    sloc INT NOT NULL DEFAULT 0,
    fan_in INT NOT NULL DEFAULT 0,
    fan_out INT NOT NULL DEFAULT 0,
    instability REAL NOT NULL DEFAULT 0.0
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
    doc_lines INT NOT NULL DEFAULT 0,
    max_line_len INT NOT NULL DEFAULT 0,
    sha1 TEXT NOT NULL,
    parsed INT NOT NULL DEFAULT 0,
    is_test INT NOT NULL DEFAULT 0,
    is_generated INT NOT NULL DEFAULT 0,
    is_vendored INT NOT NULL DEFAULT 0,
    n_parse_errors INT NOT NULL DEFAULT 0,
    n_missing_nodes INT NOT NULL DEFAULT 0,
    parse_ms REAL NOT NULL DEFAULT 0.0,
    n_symbols INT NOT NULL DEFAULT 0,
    n_functions INT NOT NULL DEFAULT 0,
    n_types INT NOT NULL DEFAULT 0,
    n_imports INT NOT NULL DEFAULT 0,
    total_cyclo INT NOT NULL DEFAULT 0,
    max_cyclo INT NOT NULL DEFAULT 0,
    total_risk INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE symbols(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    module_id INT REFERENCES modules(id),
    parent_id INT REFERENCES symbols(id),
    name TEXT NOT NULL,
    qual_name TEXT NOT NULL DEFAULT '',
    kind TEXT NOT NULL,
    line_start INT NOT NULL,
    line_end INT NOT NULL,
    n_lines INT NOT NULL DEFAULT 0,
    byte_start INT NOT NULL DEFAULT 0,
    byte_end INT NOT NULL DEFAULT 0,
    signature TEXT,
    return_type TEXT,
    visibility TEXT NOT NULL DEFAULT '',

    -- shape
    n_params INT NOT NULL DEFAULT 0,
    n_optional_params INT NOT NULL DEFAULT 0,
    n_generic_params INT NOT NULL DEFAULT 0,
    n_overloads INT NOT NULL DEFAULT 0,
    arity_rank INT NOT NULL DEFAULT 0,

    -- flags
    is_public INT NOT NULL DEFAULT 0,
    is_static INT NOT NULL DEFAULT 0,
    is_async INT NOT NULL DEFAULT 0,
    is_generator INT NOT NULL DEFAULT 0,
    is_abstract INT NOT NULL DEFAULT 0,
    is_override INT NOT NULL DEFAULT 0,
    is_exported INT NOT NULL DEFAULT 0,
    is_test INT NOT NULL DEFAULT 0,
    is_deprecated INT NOT NULL DEFAULT 0,
    is_entrypoint INT NOT NULL DEFAULT 0,
    is_generated INT NOT NULL DEFAULT 0,

    -- size
    sloc INT NOT NULL DEFAULT 0,
    body_bytes INT NOT NULL DEFAULT 0,
    n_comment_lines INT NOT NULL DEFAULT 0,
    n_doc_lines INT NOT NULL DEFAULT 0,
    has_doc INT NOT NULL DEFAULT 0,

    -- complexity
    cyclomatic INT NOT NULL DEFAULT 0,
    cognitive INT NOT NULL DEFAULT 0,
    max_nesting INT NOT NULL DEFAULT 0,
    n_tokens INT NOT NULL DEFAULT 0,
    n_operators INT NOT NULL DEFAULT 0,
    n_operands INT NOT NULL DEFAULT 0,
    n_distinct_operators INT NOT NULL DEFAULT 0,
    n_distinct_operands INT NOT NULL DEFAULT 0,
    halstead_volume INT NOT NULL DEFAULT 0,
    maintainability INT NOT NULL DEFAULT 0,

    -- control flow
    n_loops INT NOT NULL DEFAULT 0,
    n_branches INT NOT NULL DEFAULT 0,
    n_returns INT NOT NULL DEFAULT 0,
    n_early_returns INT NOT NULL DEFAULT 0,
    n_switch INT NOT NULL DEFAULT 0,
    n_cases INT NOT NULL DEFAULT 0,
    n_ternary INT NOT NULL DEFAULT 0,
    n_logical INT NOT NULL DEFAULT 0,
    n_try INT NOT NULL DEFAULT 0,
    n_catch INT NOT NULL DEFAULT 0,
    n_catch_broad INT NOT NULL DEFAULT 0,
    n_catch_empty INT NOT NULL DEFAULT 0,
    n_finally INT NOT NULL DEFAULT 0,
    n_throw INT NOT NULL DEFAULT 0,
    n_labels INT NOT NULL DEFAULT 0,
    n_gotos INT NOT NULL DEFAULT 0,

    -- what sits inside a loop
    max_loop_depth INT NOT NULL DEFAULT 0,
    call_in_loop INT NOT NULL DEFAULT 0,
    alloc_in_loop INT NOT NULL DEFAULT 0,
    io_in_loop INT NOT NULL DEFAULT 0,
    await_in_loop INT NOT NULL DEFAULT 0,
    lock_in_loop INT NOT NULL DEFAULT 0,
    concat_in_loop INT NOT NULL DEFAULT 0,
    regex_in_loop INT NOT NULL DEFAULT 0,
    query_in_loop INT NOT NULL DEFAULT 0,
    branch_in_loop INT NOT NULL DEFAULT 0,

    -- data texture
    n_locals INT NOT NULL DEFAULT 0,
    n_assign INT NOT NULL DEFAULT 0,
    n_compound_assign INT NOT NULL DEFAULT 0,
    n_incdec INT NOT NULL DEFAULT 0,
    n_cmp INT NOT NULL DEFAULT 0,
    n_bitop INT NOT NULL DEFAULT 0,
    n_shift INT NOT NULL DEFAULT 0,
    n_arith INT NOT NULL DEFAULT 0,
    n_string_lit INT NOT NULL DEFAULT 0,
    n_regex_lit INT NOT NULL DEFAULT 0,
    n_float_lit INT NOT NULL DEFAULT 0,
    n_magic INT NOT NULL DEFAULT 0,
    n_null_check INT NOT NULL DEFAULT 0,
    n_subscript INT NOT NULL DEFAULT 0,
    n_member_access INT NOT NULL DEFAULT 0,
    n_lambda INT NOT NULL DEFAULT 0,
    n_closure_capture INT NOT NULL DEFAULT 0,

    -- the call graph
    n_calls INT NOT NULL DEFAULT 0,
    n_unique_calls INT NOT NULL DEFAULT 0,
    n_dynamic_calls INT NOT NULL DEFAULT 0,
    n_unresolved_calls INT NOT NULL DEFAULT 0,
    fan_in INT NOT NULL DEFAULT 0,
    fan_out INT NOT NULL DEFAULT 0,
    n_callsites INT NOT NULL DEFAULT 0,
    is_recursive INT NOT NULL DEFAULT 0,
    is_leaf INT NOT NULL DEFAULT 0,
    is_root INT NOT NULL DEFAULT 0,

    -- hazards
    n_hazards INT NOT NULL DEFAULT 0,
    risk_score INT NOT NULL DEFAULT 0
    {EXTRA_SYMBOL_COLS}
) STRICT;

CREATE TABLE params(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    pos INT NOT NULL,
    name TEXT,
    type TEXT NOT NULL DEFAULT '',
    default_value TEXT,
    is_optional INT NOT NULL DEFAULT 0,
    is_variadic INT NOT NULL DEFAULT 0,
    is_ref INT NOT NULL DEFAULT 0,
    is_mutable INT NOT NULL DEFAULT 0,
    is_nullable INT NOT NULL DEFAULT 0,
    is_generic INT NOT NULL DEFAULT 0,
    is_untyped INT NOT NULL DEFAULT 0,
    type_depth INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, pos)
) WITHOUT ROWID, STRICT;

CREATE TABLE fields(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    name TEXT NOT NULL,
    type TEXT NOT NULL DEFAULT '',
    visibility TEXT NOT NULL DEFAULT '',
    line INT NOT NULL DEFAULT 0,
    is_static INT NOT NULL DEFAULT 0,
    is_const INT NOT NULL DEFAULT 0,
    is_mutable INT NOT NULL DEFAULT 0,
    is_nullable INT NOT NULL DEFAULT 0,
    is_collection INT NOT NULL DEFAULT 0,
    is_untyped INT NOT NULL DEFAULT 0,
    has_default INT NOT NULL DEFAULT 0,
    type_depth INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

CREATE TABLE locals(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    name TEXT NOT NULL,
    type TEXT NOT NULL DEFAULT '',
    line INT NOT NULL DEFAULT 0,
    is_const INT NOT NULL DEFAULT 0,
    is_mutable INT NOT NULL DEFAULT 0,
    is_untyped INT NOT NULL DEFAULT 0,
    has_init INT NOT NULL DEFAULT 0,
    in_loop INT NOT NULL DEFAULT 0,
    scope_depth INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

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

CREATE TABLE unresolved_calls(
    caller_id INT NOT NULL REFERENCES symbols(id),
    name TEXT NOT NULL,
    n INT NOT NULL DEFAULT 1,
    first_line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(caller_id, name)
) WITHOUT ROWID, STRICT;

CREATE TABLE imports(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    target TEXT NOT NULL,
    target_id INT REFERENCES files(id),
    alias TEXT,
    kind TEXT NOT NULL DEFAULT 'import',
    line INT NOT NULL DEFAULT 0,
    is_external INT NOT NULL DEFAULT 0,
    is_relative INT NOT NULL DEFAULT 0,
    is_wildcard INT NOT NULL DEFAULT 0,
    is_type_only INT NOT NULL DEFAULT 0,
    is_dynamic INT NOT NULL DEFAULT 0,
    n_names INT NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE hazards(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    pattern TEXT NOT NULL,
    category TEXT NOT NULL,
    n INT NOT NULL DEFAULT 1,
    first_line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, pattern)
) WITHOUT ROWID, STRICT;

CREATE TABLE attributes(
    id INTEGER PRIMARY KEY,
    symbol_id INT REFERENCES symbols(id),
    file_id INT NOT NULL REFERENCES files(id),
    name TEXT NOT NULL,
    args TEXT,
    line INT NOT NULL DEFAULT 0
) STRICT;

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
    n_fields INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

CREATE TABLE markers(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    symbol_id INT REFERENCES symbols(id),
    kind TEXT NOT NULL,
    line INT NOT NULL,
    text TEXT
) STRICT;

CREATE VIRTUAL TABLE sym_fts USING fts5(name, qual_name, signature, content='');
"""

BASE_INDEXES = r"""
-- Every query in every catalogue filters `f.is_test=0`, and without this
-- SQLite built the index at run time, per query. Measured on go/kubernetes:
-- package-state-concurrent 2.73s -> 0.037s. `files` is small, so the index
-- costs almost nothing to carry.
CREATE INDEX idx_files_test ON files(is_test);
CREATE INDEX idx_files_gen ON files(is_generated);
CREATE INDEX idx_sym_static_fn ON symbols(is_static) WHERE kind='function' AND is_static=0;
CREATE INDEX idx_sym_name ON symbols(name);
CREATE INDEX idx_sym_qual ON symbols(qual_name);
CREATE INDEX idx_sym_file_line ON symbols(file_id, line_start);
CREATE INDEX idx_sym_module_kind ON symbols(module_id, kind);
CREATE INDEX idx_sym_parent ON symbols(parent_id) WHERE parent_id IS NOT NULL;
CREATE INDEX idx_sym_kind ON symbols(kind, name);

CREATE INDEX idx_fn_fanin ON symbols(fan_in DESC, name, file_id, cyclomatic, sloc, fan_out) WHERE kind='function';
CREATE INDEX idx_fn_cyclo ON symbols(cyclomatic DESC, name, file_id, sloc, max_nesting, cognitive) WHERE kind='function';
CREATE INDEX idx_fn_cog ON symbols(cognitive DESC, name, file_id, cyclomatic, max_nesting) WHERE kind='function';
CREATE INDEX idx_fn_risk ON symbols(risk_score DESC, name, file_id, cyclomatic) WHERE kind='function';
CREATE INDEX idx_fn_sloc ON symbols(sloc DESC, name, file_id, cyclomatic) WHERE kind='function';
CREATE INDEX idx_fn_nest ON symbols(max_nesting DESC, name, file_id, cyclomatic) WHERE kind='function';
CREATE INDEX idx_fn_rec ON symbols(cyclomatic DESC, name, file_id) WHERE is_recursive=1;
CREATE INDEX idx_fn_leaf ON symbols(fan_in DESC, name, file_id) WHERE is_leaf=1;
CREATE INDEX idx_fn_public ON symbols(fan_in DESC, name, file_id) WHERE is_public=1;
CREATE INDEX idx_fn_loopdepth ON symbols(max_loop_depth DESC, name, file_id, sloc) WHERE max_loop_depth>1;
CREATE INDEX idx_fn_callinloop ON symbols(call_in_loop DESC, name, file_id) WHERE call_in_loop>0;
CREATE INDEX idx_fn_awaitloop ON symbols(await_in_loop DESC, name, file_id) WHERE await_in_loop>0;
CREATE INDEX idx_fn_allocloop ON symbols(alloc_in_loop DESC, name, file_id) WHERE alloc_in_loop>0;
CREATE INDEX idx_fn_ioloop ON symbols(io_in_loop DESC, name, file_id) WHERE io_in_loop>0;
CREATE INDEX idx_fn_queryloop ON symbols(query_in_loop DESC, name, file_id) WHERE query_in_loop>0;
CREATE INDEX idx_fn_dyn ON symbols(n_dynamic_calls DESC, name, file_id) WHERE n_dynamic_calls>0;
CREATE INDEX idx_fn_unres ON symbols(n_unresolved_calls DESC, name, file_id) WHERE n_unresolved_calls>0;
CREATE INDEX idx_fn_catch ON symbols(n_catch_broad DESC, name, file_id) WHERE n_catch_broad>0;
CREATE INDEX idx_fn_magic ON symbols(n_magic DESC, name, file_id) WHERE n_magic>0;
CREATE INDEX idx_fn_nodoc ON symbols(cyclomatic DESC, name, file_id) WHERE has_doc=0 AND kind='function';
CREATE INDEX idx_fn_async ON symbols(name, file_id) WHERE is_async=1;
CREATE INDEX idx_fn_untested ON symbols(fan_in DESC, name) WHERE is_test=0;

CREATE INDEX idx_edge_callee ON edges(callee_id, caller_id);
CREATE INDEX idx_edge_xmod ON edges(caller_id) WHERE same_module=0;
CREATE INDEX idx_cs_callee ON callsites(callee_id, line);
CREATE INDEX idx_unres_name ON unresolved_calls(name, n DESC);

CREATE INDEX idx_haz_cat ON hazards(category, n DESC);
CREATE INDEX idx_haz_pattern ON hazards(pattern, symbol_id);

CREATE INDEX idx_imp_target ON imports(target);
CREATE INDEX idx_imp_file ON imports(file_id, target);
CREATE INDEX idx_imp_resolved ON imports(target_id) WHERE target_id IS NOT NULL;
CREATE INDEX idx_imp_external ON imports(target) WHERE is_external=1;

CREATE INDEX idx_params_sym ON params(symbol_id, pos);
CREATE INDEX idx_params_type ON params(type);
CREATE INDEX idx_params_untyped ON params(symbol_id) WHERE is_untyped=1;
CREATE INDEX idx_fields_sym ON fields(symbol_id, ordinal);
CREATE INDEX idx_fields_type ON fields(type);
CREATE INDEX idx_locals_sym ON locals(symbol_id, ordinal);
CREATE INDEX idx_lit_val ON literals(value, file_id) WHERE is_magic=1;
CREATE INDEX idx_lit_sym ON literals(symbol_id, kind);
CREATE INDEX idx_attr_sym ON attributes(symbol_id, name);
CREATE INDEX idx_attr_name ON attributes(name);
CREATE INDEX idx_mark_kind ON markers(kind, file_id);
CREATE INDEX idx_enum_sym ON enum_members(symbol_id, ordinal);

CREATE INDEX idx_files_module ON files(module_id, sloc DESC);
CREATE INDEX idx_files_lang ON files(lang, sloc DESC);
CREATE INDEX idx_files_err ON files(n_parse_errors DESC) WHERE n_parse_errors>0;
CREATE INDEX idx_files_risk ON files(total_risk DESC, path);
"""

BASE_VIEWS = r"""
CREATE VIEW v_fn AS
SELECT s.id, s.name, s.qual_name, f.path, m.name AS module, s.line_start,
    s.line_end, s.sloc, s.cyclomatic, s.cognitive, s.max_nesting,
    s.fan_in, s.fan_out, s.n_calls, s.n_unresolved_calls, s.is_recursive,
    s.is_public, s.is_async, s.is_test, s.has_doc, s.n_params,
    s.max_loop_depth, s.call_in_loop, s.n_hazards, s.risk_score,
    f.is_test AS in_test_file, f.is_generated AS in_generated_file,
    f.path || ':' || s.line_start AS at
FROM symbols s
JOIN files f ON f.id = s.file_id
LEFT JOIN modules m ON m.id = s.module_id
WHERE s.kind IN ('function','method','constructor','closure');

CREATE VIEW v_type AS
SELECT s.id, s.name, s.qual_name, s.kind, f.path, m.name AS module,
    s.line_start, s.n_lines, s.is_public, s.visibility,
    (SELECT COUNT(*) FROM fields fl WHERE fl.symbol_id=s.id) AS n_fields,
    (SELECT COUNT(*) FROM symbols c WHERE c.parent_id=s.id) AS n_members,
    f.path || ':' || s.line_start AS at
FROM symbols s
JOIN files f ON f.id = s.file_id
LEFT JOIN modules m ON m.id = s.module_id
WHERE s.kind IN ('class','struct','interface','trait','enum','union','record',
                 'protocol','type','module','impl','object','mixin');

CREATE VIEW v_hotspot AS
SELECT *, (cyclomatic*2 + cognitive + max_nesting*5 + call_in_loop*4
    + n_hazards*6 + fan_in) AS heat
FROM v_fn
WHERE in_generated_file = 0
ORDER BY heat DESC;

CREATE VIEW v_blindspot AS
SELECT name, path, module, n_calls, n_unresolved_calls, fan_out,
    CAST(100.0 * n_unresolved_calls / NULLIF(n_calls,0) AS INT) AS pct_blind, at
FROM v_fn
WHERE n_unresolved_calls > 0
ORDER BY n_unresolved_calls DESC;

CREATE VIEW v_untested AS
SELECT * FROM v_fn
WHERE in_test_file = 0 AND is_test = 0 AND in_generated_file = 0
  AND id NOT IN (
    SELECT e.callee_id FROM edges e
    JOIN symbols cs ON cs.id = e.caller_id
    JOIN files cf ON cf.id = cs.file_id
    WHERE cf.is_test = 1 OR cs.is_test = 1);
"""

MATERIALIZE_INDEXES = r"""
CREATE INDEX IF NOT EXISTS ix_mat_edge_caller ON edges(caller_id, is_self);
CREATE INDEX IF NOT EXISTS ix_mat_edge_callee ON edges(callee_id, is_self);
CREATE INDEX IF NOT EXISTS ix_mat_cs_callee ON callsites(callee_id);
CREATE INDEX IF NOT EXISTS ix_mat_unres ON unresolved_calls(caller_id);
CREATE INDEX IF NOT EXISTS ix_mat_haz ON hazards(symbol_id, category);
CREATE INDEX IF NOT EXISTS ix_mat_sym_file ON symbols(file_id, kind);
CREATE INDEX IF NOT EXISTS ix_mat_sym_mod ON symbols(module_id, is_public);
CREATE INDEX IF NOT EXISTS ix_mat_imp_file ON imports(file_id);
CREATE INDEX IF NOT EXISTS ix_mat_files_mod ON files(module_id);
CREATE INDEX IF NOT EXISTS ix_mat_sym_parent ON symbols(parent_id);
"""

MATERIALIZE_BASE = r"""
UPDATE symbols AS s SET fan_out = x.c FROM
    (SELECT caller_id AS id, COUNT(*) AS c FROM edges WHERE is_self=0
     GROUP BY caller_id) AS x WHERE x.id = s.id;

UPDATE symbols AS s SET fan_in = x.c FROM
    (SELECT callee_id AS id, COUNT(*) AS c FROM edges WHERE is_self=0
     GROUP BY callee_id) AS x WHERE x.id = s.id;

UPDATE symbols AS s SET n_callsites = x.c FROM
    (SELECT callee_id AS id, COUNT(*) AS c FROM callsites
     GROUP BY callee_id) AS x WHERE x.id = s.id;

UPDATE symbols AS s SET is_recursive = 1 FROM
    (SELECT DISTINCT caller_id AS id FROM edges WHERE is_self=1) AS x
    WHERE x.id = s.id;

UPDATE symbols AS s SET n_unresolved_calls = x.n FROM
    (SELECT caller_id AS id, SUM(n) AS n FROM unresolved_calls
     GROUP BY caller_id) AS x WHERE x.id = s.id;

UPDATE symbols AS s SET n_hazards = x.n FROM
    (SELECT symbol_id AS id, SUM(n) AS n FROM hazards
     GROUP BY symbol_id) AS x WHERE x.id = s.id;

UPDATE symbols SET is_leaf = (fan_out = 0), is_root = (fan_in = 0);

UPDATE files AS f SET
    n_symbols = x.n_symbols, n_functions = x.n_functions, n_types = x.n_types,
    total_cyclo = x.total_cyclo, max_cyclo = x.max_cyclo, total_risk = x.total_risk
FROM (
    SELECT file_id AS id, COUNT(*) AS n_symbols,
        SUM(kind IN ('function','method','constructor','closure')) AS n_functions,
        SUM(kind IN ('class','struct','interface','trait','enum','union',
                     'record','protocol','type','impl')) AS n_types,
        COALESCE(SUM(cyclomatic),0) AS total_cyclo,
        COALESCE(MAX(cyclomatic),0) AS max_cyclo,
        COALESCE(SUM(risk_score),0) AS total_risk
    FROM symbols GROUP BY file_id) AS x
WHERE x.id = f.id;

UPDATE files AS f SET n_imports = x.c FROM
    (SELECT file_id AS id, COUNT(*) AS c FROM imports GROUP BY file_id) AS x
    WHERE x.id = f.id;

UPDATE modules AS m SET n_symbols = x.n, n_public = x.p FROM
    (SELECT module_id AS id, COUNT(*) AS n, SUM(is_public) AS p
     FROM symbols WHERE module_id IS NOT NULL GROUP BY module_id) AS x
    WHERE x.id = m.id;

UPDATE modules AS m SET n_files = x.c, sloc = x.s FROM
    (SELECT module_id AS id, COUNT(*) AS c, COALESCE(SUM(sloc),0) AS s
     FROM files WHERE module_id IS NOT NULL GROUP BY module_id) AS x
    WHERE x.id = m.id;

UPDATE modules AS m SET fan_out = x.c FROM
    (SELECT s1.module_id AS id, COUNT(DISTINCT s2.module_id) AS c
     FROM edges e JOIN symbols s1 ON s1.id=e.caller_id
     JOIN symbols s2 ON s2.id=e.callee_id
     WHERE s1.module_id <> s2.module_id GROUP BY s1.module_id) AS x
    WHERE x.id = m.id;

UPDATE modules AS m SET fan_in = x.c FROM
    (SELECT s2.module_id AS id, COUNT(DISTINCT s1.module_id) AS c
     FROM edges e JOIN symbols s1 ON s1.id=e.caller_id
     JOIN symbols s2 ON s2.id=e.callee_id
     WHERE s1.module_id <> s2.module_id GROUP BY s2.module_id) AS x
    WHERE x.id = m.id;

UPDATE modules SET instability =
    CASE WHEN (fan_in + fan_out) = 0 THEN 0.0
         ELSE CAST(fan_out AS REAL) / (fan_in + fan_out) END;
"""

class Analyzer:
    """What one language must supply.

    Subclass, fill in the class attributes, implement `parse_file`, and the
    driver below does discovery, schema, aggregation, indexing and the CLI.
    """

    #: short name, e.g. "rust"
    LANG = "?"
    #: what version of the language this was written against, shown in --version
    TARGET = ""
    #: file extensions to parse
    EXTS: tuple[str, ...] = ()
    #: extra directories to skip on top of COMMON_SKIP_DIRS
    SKIP_DIRS: set[str] = set()
    #: files larger than this are counted but not parsed. A 30 MB generated
    #: blob will otherwise dominate the run and teach nobody anything.
    MAX_FILE_BYTES = 4 * 1024 * 1024
    #: A file under the byte cap can still cost gigabytes: 3.99 MB on one
    #: line took 3.36 GB of RSS, because the cap bounds what is read and not
    #: what parsing it allocates. Minified bundles and generated tables are
    #: exactly this shape.
    MAX_LINE_BYTES = 1024 * 1024
    #: what this analyzer needs, and how to install it
    DEPS: DepSet = None            # type: ignore[assignment]
    #: hazard categories -> generates an `n_<cat>` column on symbols
    HAZARD_CATEGORIES: tuple[str, ...] = ()
    #: (column_name, sql_type_and_default) added to symbols
    EXTRA_SYMBOL_COLS: tuple[tuple[str, str], ...] = ()
    #: additional CREATE TABLE statements
    SCHEMA_EXT = ""
    #: additional CREATE INDEX statements, applied after the bulk load
    INDEX_EXT = ""
    #: additional CREATE VIEW statements
    VIEW_EXT = ""
    #: extra UPDATE statements run after the base aggregate pass
    MATERIALIZE_EXT = ""
    #: the risk formula, an SQL expression over `symbols`
    RISK_SQL = "cyclomatic*2 + cognitive + max_nesting*5 + n_hazards*6"
    #: the query catalogue: (name, title, notes, sql)
    QUERIES: list[tuple[str, str, str, str]] = []
    #: triage/metrics queries, kept separate so a bug-fixing agent can
    #: run only QUERIES. Reach them with --metrics.
    METRICS: list[tuple[str, str, str, str]] = []
    #: manifest files worth parsing for module/dependency facts
    MANIFESTS: tuple[str, ...] = ()

    def __init__(self) -> None:
        self.parser: ParserHandle = ParserHandle(mode=MODE_REGEX,
                                                 note="not initialised")
        self.file_id: dict[str, int] = {}

    # -- lifecycle ---------------------------------------------------------
    def setup(self) -> ParserHandle:
        """Build the parser. Called once, before any file is read."""
        raise NotImplementedError

    def parse_file(self, rec: FileRec, db: sqlite3.Connection,
                   bufs: Buffers) -> None:
        """Extract every symbol in one file. Called once per file."""
        raise NotImplementedError

    def resolve_calls(self, db: sqlite3.Connection, bufs: Buffers) -> None:
        """Second pass: turn recorded call names into edges.

        Split from `parse_file` because a call can only be resolved once every
        file has been seen -- forward references are the normal case, not the
        exception.
        """
        raise NotImplementedError

    def parse_manifests(self, root: str, db: sqlite3.Connection) -> None:
        """Optional: read go.mod / Cargo.toml / package.json and friends."""

    def post_build(self, db: sqlite3.Connection) -> None:
        """Optional: anything that needs the finished graph."""

    # -- schema assembly ---------------------------------------------------
    def flush_symbols(self, db: sqlite3.Connection) -> None:
        """Write any buffered symbol rows. A no-op where there are none.

        This analyzer inserts symbols directly rather than buffering them, so
        there is nothing to flush -- but `build()` calls this unconditionally,
        and a missing method is an AttributeError at run time rather than a
        clear signal that the two halves disagree.
        """

    def symbol_columns(self) -> list[tuple[str, str]]:
        cols = [("n_%s" % c, "INT NOT NULL DEFAULT 0")
                for c in self.HAZARD_CATEGORIES]
        cols += list(self.EXTRA_SYMBOL_COLS)
        seen: set[str] = set()
        out: list[tuple[str, str]] = []
        for name, decl in cols:
            if name in seen:
                continue
            seen.add(name)
            out.append((name, decl))
        return out

    def schema_sql(self) -> str:
        extra = self.symbol_columns()
        block = ""
        if extra:
            block = ",\n    " + ",\n    ".join(
                "%s %s" % (n, d) for n, d in extra)
        return (BASE_SCHEMA.replace("{EXTRA_SYMBOL_COLS}", block)
                + "\n" + self.SCHEMA_EXT)

    def materialize_sql(self) -> str:
        parts = [MATERIALIZE_BASE]
        for cat in self.HAZARD_CATEGORIES:
            parts.append(
                "UPDATE symbols AS s SET n_%s = x.n FROM "
                "(SELECT symbol_id AS id, SUM(n) AS n FROM hazards "
                "WHERE category='%s' GROUP BY symbol_id) AS x "
                "WHERE x.id = s.id;" % (cat, cat))
        parts.append(self.MATERIALIZE_EXT)
        parts.append("UPDATE symbols SET risk_score = %s;" % self.RISK_SQL)
        parts.append(
            "UPDATE symbols SET halstead_volume = CAST("
            "(n_operators + n_operands) * "
            "(CASE WHEN (n_distinct_operators + n_distinct_operands) > 1 "
            "THEN 1.0 * (n_distinct_operators + n_distinct_operands) "
            "ELSE 2.0 END) AS INT) WHERE n_tokens > 0;")
        parts.append(
            "UPDATE symbols SET maintainability = MAX(0, CAST("
            "171 - 0.23 * cyclomatic - 16.2 * "
            "(CASE WHEN sloc > 1 THEN 1.0 * sloc / 20.0 ELSE 0.05 END) "
            "AS INT)) WHERE kind IN "
            "('function','method','constructor','closure');")
        return "\n".join(p for p in parts if p.strip())

def _gil_enabled() -> bool:
    """False on a free-threaded (PEP 703) build.

    Worth recording per run: the same source on a free-threaded interpreter has
    genuinely concurrent access to any shared state, so a concurrency finding
    that was theoretical under the GIL is reachable there.
    """
    probe = getattr(sys, "_is_gil_enabled", None)
    return probe() if probe is not None else True

def _concurrency_note(mode: str) -> str:
    """Why this run is single-threaded -- true for THIS analyzer, not in general.

    The tree-sitter measurement does not apply to the analyzers that never load
    it, and asserting it there was a claim about code that is not running.
    """
    if mode == MODE_TREE_SITTER:
        return ("serial: tree-sitter holds the GIL for the whole of parse() "
                "(4 threads measured at 3.8x wall time for 4x work) and its "
                "_binding extension refuses to load in a subinterpreter, so "
                "neither threads nor PEP 734 help. Process-level parallelism "
                "would need symbol ids assigned outside SQLite.")
    return ("serial: this analyzer parses in-process with no third-party "
            "extension. Parallelism would need symbol ids assigned outside "
            "SQLite, which is where they come from today.")

def discover(analyzer: Analyzer, root: str, db: sqlite3.Connection,
             include_tests: bool, include_generated: bool,
             include_vendored: bool, quiet: bool) -> list[FileRec]:
    """Walk the tree, insert every file row, return the parseable ones."""
    skip = COMMON_SKIP_DIRS | set(analyzer.SKIP_DIRS)
    exts = set(analyzer.EXTS)
    mod_id: dict[str, int] = {}
    out: list[FileRec] = []
    n_seen = n_skipped_big = 0

    def module(rel: str) -> int:
        name = module_of(rel)
        mid = mod_id.get(name)
        if mid is None:
            kind = ("test" if TEST_PATH_RE.search(name) else
                    "vendor" if VENDOR_PATH_RE.search(name) else
                    "example" if re.search(r'(^|/)(examples?|samples?|demos?)(/|$)', name, re.I) else
                    "tool" if re.search(r'(^|/)(tools?|scripts?|cmd|bin)(/|$)', name, re.I) else
                    "source")
            mid = db.execute(
                "INSERT INTO modules(name,kind) VALUES(?,?)",
                (name, kind)).lastrowid
            mod_id[name] = mid
        return mid

    real_root = os.path.realpath(root)
    n_skipped_special = 0
    n_skipped_escape = 0
    n_skipped_denied = 0
    n_walk_errors = 0
    n_files = 0
    file_rows: list[tuple] = []

    def _walk_error(exc: OSError) -> None:
        nonlocal n_walk_errors
        n_walk_errors += 1

    for dirpath, dirnames, filenames in os.walk(root, onerror=_walk_error):
        dirnames[:] = [d for d in sorted(dirnames)
                       if d not in skip and not d.startswith(".")]
        for fn in sorted(filenames):
            ext = os.path.splitext(fn)[1]
            if ext not in exts:
                continue
            n_seen += 1
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root)
            too_big = False
            try:
                st = os.stat(full)
                # A FIFO reports st_size 0, passes the size cap, and then
                # open() blocks forever waiting for a writer. A symlink to
                # /dev/zero reports 0 and reads until the machine dies. Both
                # are storable in a tarball; the symlink is storable in git.
                if not stat.S_ISREG(st.st_mode):
                    n_skipped_special += 1
                    continue
                # A symlink named passwd.c pointing at /etc/passwd was read,
                # hashed, parsed, and any TODO-ish line echoed to stdout. This
                # tool is pointed at code the user did not write and its output
                # gets pasted into reports.
                if os.path.realpath(full) != full and not \
                        os.path.realpath(full).startswith(real_root + os.sep):
                    n_skipped_escape += 1
                    continue
                if st.st_size > analyzer.MAX_FILE_BYTES:
                    n_skipped_big += 1
                    data = b""
                    text = ""
                    too_big = True
                else:
                    with open(full, "rb") as fh:
                        data = fh.read()
                    text = data.decode("utf-8", "replace")
            except PermissionError:
                n_skipped_denied += 1
                continue
            except OSError:
                continue

            # A single 3.99 MB line took 3.36 GB of RSS: the byte cap bounds
            # what is read, not what parsing it costs.
            if not too_big and data:
                longest = max((len(l) for l in data.split(b"\n")), default=0)
                if longest > analyzer.MAX_LINE_BYTES:
                    n_skipped_big += 1
                    too_big = True

            lines = text.splitlines()
            blank = sum(1 for l in lines if not l.strip())
            cmt = sum(1 for l in lines
                      if l.lstrip()[:3] in ("//", "#", "/*", "*", "*/", '"""',
                                            "'''", "--", ";;", "%") and l.strip())
            name_re = TEST_NAME_RE_BY_LANG.get(analyzer.LANG,
                                              TEST_NAME_RE)
            test = bool(TEST_PATH_RE.search(rel) or name_re.search(fn))
            gen = is_generated(fn, text[:2000])
            vend = bool(VENDOR_PATH_RE.search(rel))
            mid = module(rel)
            parse = (not too_big and bool(text)
                     and (include_tests or not test)
                     and (include_generated or not gen)
                     and (include_vendored or not vend))

            # A non-UTF-8 filename arrives surrogate-escaped and sqlite3
            # rejects surrogates, which used to kill the whole scan from
            # outside the try above.
            if _has_surrogates(rel):
                rel = rel.encode("utf-8", "replace").decode("utf-8", "replace")
                fn = fn.encode("utf-8", "replace").decode("utf-8", "replace")
            # The id is assigned here rather than read back from
            # `lastrowid`: a counter from 1 produces exactly the rowids SQLite
            # would have handed out, and it lets every file row go in as one
            # `executemany` after the walk instead of one INSERT per file --
            # 31,157 statements on elasticsearch.
            n_files += 1
            fid = n_files
            file_rows.append(
                (fid, rel, os.path.dirname(rel) or ".", fn, ext, analyzer.LANG,
                 mid, st.st_size, len(lines),
                 sum(1 for l in lines if l.strip()),
                 blank, cmt, max((len(l) for l in lines), default=0),
                 hashlib.sha1(data).hexdigest() if data else "",
                 int(parse), int(test), int(gen), int(vend)))
            analyzer.file_id[rel] = fid
            if parse:
                out.append(FileRec(fid, mid, rel, full, text, data,
                                   analyzer.LANG, test, gen, vend))

    if file_rows:
        db.executemany(
            "INSERT INTO files(id,path,dir,basename,ext,lang,module_id,bytes,"
            "lines,sloc,blank_lines,comment_lines,max_line_len,sha1,parsed,"
            "is_test,is_generated,is_vendored) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", file_rows)
        file_rows.clear()

    for n, why in ((n_skipped_big,
                    "too large or with a pathologically long line -- "
                    "catalogued, not parsed"),
                   (n_skipped_special,
                    "not regular files (fifo, socket, device) -- skipped"),
                   (n_skipped_escape,
                    "symlinks pointing OUTSIDE the tree -- skipped"),
                   (n_skipped_denied, "unreadable (permission denied)"),
                   (n_walk_errors, "director(ies) could not be listed")):
        if n and not quiet:
            print("  %d %s" % (n, why))
    db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
               ("files_skipped",
                "big=%d special=%d escaping_symlink=%d denied=%d walk_errors=%d"
                % (n_skipped_big, n_skipped_special, n_skipped_escape,
                   n_skipped_denied, n_walk_errors)))
    return out

def _has_surrogates(text: str) -> bool:
    return any("\ud800" <= ch <= "\udfff" for ch in text)

def scan_markers(rec: FileRec, bufs: Buffers) -> None:
    """TODO/FIXME/HACK and friends, with their line and text."""
    for i, line in enumerate(rec.text.splitlines(), 1):
        m = MARKER_RE.search(line)
        if m and ("//" in line or "#" in line or "*" in line or "--" in line):
            bufs.markers.append(
                (rec.fid, None, m.group(1).upper(), i, line.strip()[:200]))

#: Files between incremental flushes of the per-file row buffers. The
#: accumulators used to hold every row until the end of the parse: on
#: elasticsearch that is 444k params, 508k imports and 498k more besides,
#: all live at the moment peak RSS is set. Draining them periodically
#: costs nothing -- the inserts happen either way -- and they are pure
#: appends keyed by an already-assigned symbol_id, so batching does not
#: change a single row or id.
FLUSH_EVERY = 2000

def flush_rows(db: sqlite3.Connection, bufs: Buffers) -> None:
    """Write and CLEAR the per-file tables. Safe to call mid-parse.

    Only tables keyed by a symbol_id that already exists. Edges,
    callsites and unresolved calls are NOT here: they are not known
    until `resolve_calls` has seen every file.
    """
    ex = db.executemany
    if bufs.params:
        ex("INSERT OR IGNORE INTO params(symbol_id,pos,name,type,default_value,"
           "is_optional,is_variadic,is_ref,is_mutable,is_nullable,is_generic,"
           "is_untyped,type_depth) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", bufs.params)
    if bufs.fields:
        ex("INSERT OR IGNORE INTO fields(symbol_id,ordinal,name,type,visibility,"
           "line,is_static,is_const,is_mutable,is_nullable,is_collection,"
           "is_untyped,has_default,type_depth) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", bufs.fields)
    if bufs.locals:
        ex("INSERT OR IGNORE INTO locals(symbol_id,ordinal,name,type,line,"
           "is_const,is_mutable,is_untyped,has_init,in_loop,scope_depth) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?)", bufs.locals)
    if bufs.literals:
        ex("INSERT INTO literals(symbol_id,file_id,kind,value,line,is_magic) "
           "VALUES(?,?,?,?,?,?)", bufs.literals)
    if bufs.markers:
        ex("INSERT INTO markers(file_id,symbol_id,kind,line,text) "
           "VALUES(?,?,?,?,?)", bufs.markers)
    if bufs.attributes:
        ex("INSERT INTO attributes(symbol_id,file_id,name,args,line) "
           "VALUES(?,?,?,?,?)", bufs.attributes)
    if bufs.imports:
        ex("INSERT INTO imports(file_id,target,target_id,alias,kind,line,"
           "is_external,is_relative,is_wildcard,is_type_only,is_dynamic,n_names) "
           "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", bufs.imports)
    if bufs.hazards:
        ex("INSERT INTO hazards(symbol_id,pattern,category,n,first_line) "
           "VALUES(?,?,?,?,?) ON CONFLICT(symbol_id,pattern) DO UPDATE "
           "SET n = n + excluded.n", bufs.hazards)
    if bufs.enum_members:
        ex("INSERT OR IGNORE INTO enum_members(symbol_id,ordinal,name,value,"
           "n_fields) VALUES(?,?,?,?,?)", bufs.enum_members)
    bufs.params.clear()
    bufs.fields.clear()
    bufs.locals.clear()
    bufs.literals.clear()
    bufs.markers.clear()
    bufs.attributes.clear()
    bufs.imports.clear()
    bufs.hazards.clear()
    bufs.enum_members.clear()

def flush(db: sqlite3.Connection, bufs: Buffers) -> None:
    """Final flush: the per-file tables, then everything resolve_calls produced."""
    flush_rows(db, bufs)
    ex = db.executemany
    if bufs.edges:
        ex("INSERT OR IGNORE INTO edges(caller_id,callee_id,n_calls,same_file,"
           "same_module,is_self) VALUES(?,?,?,?,?,?)",
           # A generator, not a list comprehension: `executemany` takes any
           # iterable, and materialising one costs a fresh tuple per edge --
           # 770k of them on elasticsearch, held alongside the dict they were
           # built from, at exactly the moment peak RSS is set. Measured
           # +530,005 live blocks vs +5, and streaming is no slower.
           ((a, b, v[0], v[1], v[2], v[3]) for (a, b), v in bufs.edges.items()))
    if bufs.callsites:
        ex("INSERT OR IGNORE INTO callsites(caller_id,callee_id,line) "
           "VALUES(?,?,?)", bufs.callsites)
    if bufs.unresolved:
        ex("INSERT OR IGNORE INTO unresolved_calls(caller_id,name,n,first_line) "
           "VALUES(?,?,?,?)",
           ((c, n, v[0], v[1]) for (c, n), v in bufs.unresolved.items()))

_IMPORT_SUFFIXES = ("", ".py", ".pyi", ".ts", ".tsx", ".d.ts", ".mts", ".cts",
                    ".js", ".jsx", ".mjs", ".cjs", ".rb", ".php", ".go",
                    ".rs", ".java")

_IMPORT_INDEXES = ("__init__.py", "index.ts", "index.tsx", "index.js",
                   "index.mjs", "mod.rs", "lib.rs")

def resolve_import_targets(db: sqlite3.Connection, analyzer: "Analyzer") -> int:
    """Point each import row at the file it names, where that file is here.

    Left NULL, `imports.target_id` silently turns every query built on it into
    a tautology: `import-cycles` finds none because the join never matches,
    a barrel's `importers` count is always zero, and "nothing imports this
    export" is true of everything. Those queries did not fail -- they returned
    confident, empty, wrong answers.

    Relative specifiers are resolved against the importing file's directory;
    everything else is tried as a dotted or slashed path from the tree root.
    A bare package name resolves to nothing, which is correct: it is external.
    """
    by_path: dict[str, int] = {}
    for fid, path in db.execute("SELECT id, path FROM files"):
        norm = path.replace(os.sep, "/")
        by_path[norm] = fid
        stem = norm.rsplit(".", 1)[0]
        by_path.setdefault(stem, fid)

    def look(cand: str) -> Optional[int]:
        cand = cand.strip("/")
        if not cand:
            return None
        for suf in _IMPORT_SUFFIXES:
            hit = by_path.get(cand + suf)
            if hit is not None:
                return hit
        for idx in _IMPORT_INDEXES:
            hit = by_path.get("%s/%s" % (cand, idx))
            if hit is not None:
                return hit
        return None

    rows: list[tuple[int, int]] = []
    for iid, fid, target, path in db.execute(
            "SELECT i.id, i.file_id, i.target, f.path FROM imports i "
            "JOIN files f ON f.id=i.file_id WHERE i.target_id IS NULL"):
        if not target:
            continue
        t = target.replace(os.sep, "/").strip()
        here = os.path.dirname(path.replace(os.sep, "/"))
        hit = None
        if t.startswith("."):
            # Python's `..pkg.mod` and JS's `../pkg/mod` both count leading
            # dots, but Python counts one extra: `.` is this package.
            n_up = len(t) - len(t.lstrip("."))
            rest = t[n_up:].replace(".", "/") if "/" not in t else t.lstrip("./")
            base = here
            for _ in range(max(0, n_up - 1)):
                base = os.path.dirname(base)
            hit = look("%s/%s" % (base, rest) if base else rest)
        else:
            hit = look(t.replace(".", "/")) or look("%s/%s" % (here, t))
        if hit is not None and hit != fid:
            rows.append((hit, iid))
    if rows:
        db.executemany("UPDATE imports SET target_id=? WHERE id=?", rows)
    db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
               ("imports_resolved",
                "%d of %d import rows point at a file in this tree"
                % (len(rows),
                   db.execute("SELECT COUNT(*) FROM imports").fetchone()[0])))
    return len(rows)

def build(analyzer: Analyzer, root: str, db: sqlite3.Connection, *,
          include_tests: bool = True, include_generated: bool = False,
          include_vendored: bool = False, quiet: bool = False) -> int:
    """Parse `root` into the open connection `db`. Returns files parsed."""
    db.executescript(PRAGMAS)
    db.executescript(analyzer.schema_sql())

    handle = analyzer.setup()
    analyzer.parser = handle
    if not quiet:
        print("  " + handle.banner())

    t0 = time.time()
    recs = discover(analyzer, root, db, include_tests, include_generated,
                    include_vendored, quiet)
    t_discover = time.time() - t0
    if not quiet:
        print("  %d %s files discovered in %.1fs"
              % (len(recs), analyzer.LANG, t_discover))

    bufs = Buffers()
    t1 = time.time()
    n_err = 0
    parse_failed: list[tuple[int]] = []
    step = max(1, len(recs) // 20)
    for i, rec in enumerate(recs):
        try:
            scan_markers(rec, bufs)
            analyzer.parse_file(rec, db, bufs)
        except RecursionError:
            n_err += 1
            parse_failed.append((rec.fid,))
        except Exception as exc:
            # One pathological file must not cost the other 40,000. The failure
            # is recorded on the file row so `--report` can show it rather than
            # the run quietly covering less than it claims.
            n_err += 1
            parse_failed.append((rec.fid,))
            if os.environ.get("CODEGRAPH_DEBUG"):
                import traceback
                print("  parse failed: %s: %s" % (rec.rel, exc), file=sys.stderr)
                traceback.print_exc()
        # The source is never read again: from here `recs` is used only for
        # len(). Holding text AND data for every file to the end of the run
        # was 594 MB of elasticsearch's 3.9 GB peak. Outside the try above on
        # purpose -- a failure here must not be swallowed as a parse error.
        rec.text = ""
        rec.data = b""
        if (i + 1) % FLUSH_EVERY == 0:
            flush_rows(db, bufs)
        if not quiet and (i + 1) % step == 0:
            print("  ... %d/%d files" % (i + 1, len(recs)))
    if parse_failed:
        db.executemany("UPDATE files SET parsed=0, "
                       "n_parse_errors=n_parse_errors+1 WHERE id=?",
                       parse_failed)
        parse_failed.clear()
    # Symbols reach the database HERE -- before anything counts or queries the
    # table. Flushing after the count made `n_syms` read 0 and printed
    # "3554 file(s) produced NO symbols" over a perfectly good graph.
    analyzer.flush_symbols(db)
    n_syms = db.execute("SELECT COUNT(*) FROM symbols").fetchone()[0]
    # A parse failure is a correctness signal, not progress noise: a bug in one
    # analyzer once dropped 620 of Django's 2,103 files and 30% of its symbols,
    # and --quiet hid the only line that said so. Failures above a handful are
    # always reported, and the exit path records them for a query to find.
    if n_err and (n_err > len(recs) // 100 or not quiet):
        print("  WARNING: %d of %d file(s) FAILED to parse and contributed"
              " nothing." % (n_err, len(recs)), file=sys.stderr)
        print("           Re-run with CODEGRAPH_DEBUG=1 for the tracebacks.",
              file=sys.stderr)
    if not quiet:
        print("  %d symbols parsed in %.1fs%s"
              % (n_syms, time.time() - t1,
                 " (%d file(s) failed)" % n_err if n_err else ""))
    if recs and n_syms == 0:
        # Not fatal -- a repo really can hold only declarations -- but it is
        # never what the user expected, and silence here reads as success.
        print("  WARNING: %d file(s) were read and produced NO symbols. Every"
              % len(recs))
        print("           query below will be empty for that reason, not"
              " because the")
        print("           code is clean. Check --report for parse errors.")

    t2 = time.time()
    analyzer.resolve_calls(db, bufs)
    flush(db, bufs)
    if not quiet:
        print("  call graph built in %.1fs" % (time.time() - t2))

    resolve_import_targets(db, analyzer)
    try:
        analyzer.parse_manifests(root, db)
    except Exception as exc:
        # Manifests are supplementary. A malformed one must never discard a
        # parse that already succeeded -- a non-object tsconfig in vscode once
        # threw away 3.6 minutes of work at the last step.
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                   ("manifest_error", "%s: %s" % (type(exc).__name__, exc)))
        if not quiet:
            print("  manifest parsing failed (%s); continuing" % exc,
                  file=sys.stderr)

    t3 = time.time()
    db.executescript(MATERIALIZE_INDEXES)
    db.executescript(analyzer.materialize_sql())
    if not quiet:
        print("  aggregates materialized in %.1fs" % (time.time() - t3))

    db.execute("INSERT INTO sym_fts(rowid,name,qual_name,signature) "
               "SELECT id,name,qual_name,COALESCE(signature,'') FROM symbols")

    t4 = time.time()
    db.executescript(BASE_INDEXES)
    if analyzer.INDEX_EXT:
        db.executescript(analyzer.INDEX_EXT)
    db.executescript(BASE_VIEWS)
    if analyzer.VIEW_EXT:
        db.executescript(analyzer.VIEW_EXT)
    if not quiet:
        print("  indexed in %.1fs" % (time.time() - t4))

    analyzer.post_build(db, root)      # DYN PROJECT: root threaded for the pass

    meta_rows = (
        ("schema_version", str(SCHEMA_VERSION)),
        ("lang", analyzer.LANG),
        ("target", analyzer.TARGET),
        ("root", os.path.abspath(root)),
        ("parse_mode", handle.mode),
        ("parser", handle.banner()),
        ("built_at", time.strftime("%Y-%m-%dT%H:%M:%S")),
        ("files_parsed", str(len(recs))),
        ("files_failed", str(n_err)),
        ("python", sys.version.split()[0]),
        ("sqlite", sqlite3.sqlite_version),
        ("free_threading",
         "yes -- GIL disabled" if not _gil_enabled() else "no -- GIL enabled"),
        ("parse_concurrency", _concurrency_note(handle.mode)),
    )
    db.executemany("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)", meta_rows)

    db.commit()
    db.execute("ANALYZE")
    return len(recs)

MAX_CELL = 72

def _cell(v: Any) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        t = "%.2f" % v
    else:
        t = str(v)
    return t if len(t) <= MAX_CELL else t[:MAX_CELL - 3] + "..."

def render(rows: Sequence[Sequence[Any]], cols: Sequence[str],
           out: Any = sys.stdout) -> None:
    if not rows:
        print(" (no rows)", file=out)
        return
    body = [[_cell(v) for v in r] for r in rows]
    w = [max(len(str(cols[i])), max(len(r[i]) for r in body))
         for i in range(len(cols))]
    print(" " + " ".join(str(cols[i]).ljust(w[i]) for i in range(len(cols))), file=out)
    print(" " + " ".join("-" * w[i] for i in range(len(cols))), file=out)
    for r in body:
        print(" " + " ".join(r[i].ljust(w[i]) for i in range(len(r))), file=out)

def report(db: sqlite3.Connection, analyzer: Analyzer) -> None:
    """A short narrative: how much we saw, and how much we missed."""
    q = lambda s, *a: db.execute(s, a).fetchall()
    one = lambda s, *a: (db.execute(s, a).fetchone() or [0])[0]

    print("\n" + "=" * 78)
    print("OVERVIEW")
    print("-" * 78)
    meta = dict(q("SELECT key,value FROM meta"))
    for k in ("lang", "target", "parser", "root", "built_at"):
        if meta.get(k):
            print(" %-14s %s" % (k, meta[k]))

    files = one("SELECT COUNT(*) FROM files")
    parsed = one("SELECT COUNT(*) FROM files WHERE parsed=1")
    sloc = one("SELECT COALESCE(SUM(sloc),0) FROM files WHERE parsed=1")
    print(" %-14s %d catalogued, %d parsed, %d sloc" % ("files", files, parsed, sloc))
    print(" %-14s %s" % ("symbols", ", ".join(
        "%s=%d" % (k, v) for k, v in
        q("SELECT kind,COUNT(*) FROM symbols GROUP BY kind "
          "ORDER BY COUNT(*) DESC LIMIT 12"))))
    print(" %-14s %d edges, %d call sites, %d unresolved"
          % ("call graph", one("SELECT COUNT(*) FROM edges"),
             one("SELECT COUNT(*) FROM callsites"),
             one("SELECT COALESCE(SUM(n),0) FROM unresolved_calls")))

    print("\n" + "=" * 78)
    print("HOW MUCH OF THIS TO TRUST")
    print("-" * 78)
    err_files = one("SELECT COUNT(*) FROM files WHERE n_parse_errors>0")
    tot_calls = one("SELECT COALESCE(SUM(n_calls),0) FROM symbols")
    unres = one("SELECT COALESCE(SUM(n),0) FROM unresolved_calls")
    if parsed == 0:
        print(" NOTHING WAS PARSED. Every number below is zero because no file")
        print(" was read, not because this repository is empty or clean.")
    elif meta.get("parse_mode") == MODE_REGEX:
        print(" Parsed WITHOUT a grammar: spans and nesting are approximate and")
        print(" call edges are absent. Only the file inventory is reliable.")
    print(" %-30s %d file(s)" % ("files with parse errors", err_files))
    if tot_calls:
        print(" %-30s %d of %d call sites (%d%%)"
              % ("calls we could NOT resolve", unres, tot_calls,
                 100 * unres // tot_calls))
    else:
        print(" %-30s no calls were recorded at all -- this is the absence of"
              % "call resolution")
        print(" %-30s data, not a clean result" % "")
    print(" A high unresolved share means the call-graph queries below see less")
    print(" than they imply. `v_blindspot` lists exactly where.")

    for label, sql in (
        ("BIGGEST MODULES",
         "SELECT name, n_files AS files, sloc, n_symbols AS syms, "
         "ROUND(instability,2) AS instab FROM modules "
         "WHERE n_files>0 ORDER BY sloc DESC LIMIT 12"),
        ("HEAVIEST FUNCTIONS",
         "SELECT name, sloc, cyclomatic AS cyclo, cognitive AS cog, "
         "max_nesting AS nest, fan_in, at FROM v_fn "
         "ORDER BY cyclomatic DESC LIMIT 12"),
        ("MOST DEPENDED ON",
         "SELECT name, fan_in, fan_out, cyclomatic AS cyclo, sloc, at "
         "FROM v_fn ORDER BY fan_in DESC LIMIT 12"),
        ("MARKERS LEFT IN THE CODE",
         "SELECT kind, COUNT(*) AS n FROM markers GROUP BY kind ORDER BY n DESC"),
    ):
        print("\n" + "=" * 78)
        print(label)
        print("-" * 78)
        cur = db.execute(sql)
        render(cur.fetchall(), [d[0] for d in cur.description])

def main(analyzer: Analyzer, argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        prog="codegraph_%s.py" % analyzer.LANG,
        description="Parse a %s tree into an in-memory graph and query it in "
                    "one shot. Target: %s" % (analyzer.LANG, analyzer.TARGET),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="every run re-parses from source; nothing is cached, so an "
               "answer can never describe code that has moved on")
    ap.add_argument("root", nargs="?", default=".", help="tree to parse")
    ap.add_argument("which", nargs="*", type=int, help="1-based query numbers")
    ap.add_argument("--module", default="%", help="module-name LIKE filter")
    ap.add_argument("--limit", type=int, default=-1,
                    help="rows per query; -1 (default) is every row")
    ap.add_argument("--list", action="store_true", help="list the queries")
    ap.add_argument("--metrics", action="store_true",
                    help="run/list the METRICS section instead of QUERIES")
    ap.add_argument("--schema", action="store_true", help="dump the schema")
    ap.add_argument("--report", action="store_true", help="narrative overview")
    ap.add_argument("--sql", help="ad-hoc query against the graph")
    ap.add_argument("--csv", type=int, metavar="N", help="emit query N as CSV")
    ap.add_argument("--json", type=int, metavar="N", help="emit query N as JSON")
    ap.add_argument("--save", metavar="PATH", help="also write the graph to a file")
    ap.add_argument("--force", action="store_true",
                    help="allow --save to overwrite an existing file")
    ap.add_argument("--deps", action="store_true",
                    help="show dependencies and how to install them")
    ap.add_argument("--install-deps", action="store_true",
                    help="pip-install the missing dependencies, then continue")
    ap.add_argument("--include-generated", action="store_true",
                    help="parse generated files too (off by default)")
    ap.add_argument("--include-vendored", action="store_true",
                    help="parse vendored trees too (off by default)")
    ap.add_argument("--no-tests", action="store_true",
                    help="skip test files")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--version", action="store_true")
    a = ap.parse_args(argv)

    if a.version:
        print("codegraph_%s.py  target=%s  schema=v%d  python=%s  sqlite=%s"
              % (analyzer.LANG, analyzer.TARGET, SCHEMA_VERSION,
                 sys.version.split()[0], sqlite3.sqlite_version))
        return 0
    if a.install_deps:
        analyzer.DEPS.install(quiet=a.quiet)
        if a.deps:
            print()
            print(analyzer.DEPS.describe())
            return 0
    elif a.deps:
        print(analyzer.DEPS.describe())
        return 0
    if a.schema:
        print(analyzer.schema_sql())
        print(BASE_INDEXES)
        print(analyzer.INDEX_EXT)
        print(BASE_VIEWS)
        print(analyzer.VIEW_EXT)
        return 0
    if a.list:
        qs = analyzer.METRICS if a.metrics else analyzer.QUERIES
        for i, (name, title, _, _) in enumerate(qs, 1):
            print("%2d. %-26s %s" % (i, name, title))
        return 0

    # --csv and --json are consumed by other programs; progress lines on
    # stdout made the output unparseable, and --quiet was the only escape.
    if a.csv is not None or a.json is not None:
        a.quiet = True

    missing = analyzer.DEPS.missing() if analyzer.DEPS else []
    if missing and not a.quiet:
        print("note: %d dependency/ies absent -- run with --deps to see them"
              % len(missing))

    if not os.path.isdir(a.root):
        # os.walk on a missing path yields nothing silently, so the run used
        # to print a complete, successful-looking report and then traceback in
        # parse_manifests.
        print("not a directory: %s" % a.root, file=sys.stderr)
        return 2

    t0 = time.time()
    db = sqlite3.connect(":memory:")
    # Repo contract (CLAUDE.md Appendix A): the graph covers src/ only.
    # dev.sh fans the gate into whole-tree copies under .dev/trees/, so
    # walking the repo root would parse the sources once per copy -- and the
    # tree also carries tests/, tools/ and third_party/ that belong to the
    # gate, not the source graph. A root without src/ is walked as given.
    scan_root = os.path.join(os.path.abspath(a.root), "src")
    if not os.path.isdir(scan_root):
        scan_root = os.path.abspath(a.root)
    n = build(analyzer, scan_root, db,
              include_tests=not a.no_tests,
              include_generated=a.include_generated,
              include_vendored=a.include_vendored,
              quiet=a.quiet)
    took = time.time() - t0
    p = {"mod": a.module, "lim": a.limit}

    if a.sql:
        try:
            cur = db.execute(a.sql, p) if ":" in a.sql else db.execute(a.sql)
            if cur.description is None:
                # A statement that returns no rows used to traceback on
                # cur.description AFTER being applied, throwing away the run.
                print(" %d row(s) affected" % cur.rowcount)
            else:
                render(cur.fetchall(), [d[0] for d in cur.description])
        except sqlite3.Error as exc:
            print(" query failed: %s" % exc, file=sys.stderr)
            db.close()
            return 2
        db.close()
        return 0
    if a.csv is not None or a.json is not None:
        idx = (a.csv if a.csv is not None else a.json) - 1
        qs_csv = analyzer.METRICS if a.metrics else analyzer.QUERIES
        if not (0 <= idx < len(qs_csv)):
            print("no query %d" % (idx + 1), file=sys.stderr)
            return 2
        cur = db.execute(qs_csv[idx][3], p)
        cols = [d[0] for d in cur.description]
        rows = cur.fetchall()
        if a.csv:
            w = csv.writer(sys.stdout)
            w.writerow(cols)
            w.writerows(rows)
        else:
            import json as _json
            _json.dump([dict(zip(cols, r)) for r in rows], sys.stdout,
                       indent=2, default=str)
            print()
        db.close()
        return 0

    if not a.quiet:
        print("codegraph-%s: %d files parsed into memory in %.1fs "
              "module=%s limit=%s" % (analyzer.LANG, n, took, a.module,
                                      "all" if a.limit < 0 else a.limit))
    if a.report:
        report(db, analyzer)

    qs = analyzer.METRICS if a.metrics else analyzer.QUERIES
    sel = a.which or range(1, len(qs) + 1)
    for k in sel:
        if not (1 <= k <= len(qs)):
            continue
        name, title, notes, sql = qs[k - 1]
        print("\n" + "=" * 78)
        print("Q%d. %s -- %s" % (k, name, title))
        print("-" * 78)
        for line in notes.splitlines():
            print(" " + line)
        print()
        try:
            cur = db.execute(sql, p)
            render(cur.fetchall(), [d[0] for d in cur.description])
        except sqlite3.Error as exc:
            print(" query failed: %s" % exc)

    if a.save:
        # backup() replaces the destination's entire contents. Overwriting an
        # unrelated database on a path typo is not recoverable, and following
        # a symlink to do it is worse.
        try:
            if os.path.lexists(a.save) and not a.force:
                print("\nrefusing to overwrite %s (pass --force)%s"
                      % (a.save,
                         " -- it is a symlink to %s" % os.path.realpath(a.save)
                         if os.path.islink(a.save) else ""),
                      file=sys.stderr)
            else:
                if os.path.islink(a.save):
                    os.unlink(a.save)
                dest = sqlite3.connect(a.save)
                db.backup(dest)
                dest.close()
                print("\n(graph also written to %s)" % a.save)
        except (sqlite3.Error, OSError) as exc:
            # The graph is finished and the answers are already printed; a
            # failed save must not discard them.
            print("\ncould not write %s: %s" % (a.save, exc), file=sys.stderr)
    db.close()
    return 0


# ==========================================================================
# lang_c.py
# codegraph_c.py -- parse a C tree into a graph and query it.
#
# Targets C11/C17 with the GNU and Clang extensions real code actually uses.
#
# There is no compiler frontend here and that is deliberate, not a shortcut. A
# real C parse needs the preprocessor, which needs the include path, which needs
# the build system -- so a "proper" C analyzer only works on a tree you can
# already build, on the machine that builds it. This one works on any directory
# of C you can read. Comments and string/char literals are blanked (byte offsets
# preserved, so every line number stays exact), functions are found by matching
# braces, and a call is any identifier followed by `(`.
#
# What that buys, and nothing else in this repo has: byte-accurate struct layout.
# `layout` and `struct_size` carry the LP64 offset, size and padding of every
# top-level field, so "which structs waste memory to alignment holes" and "which
# hot object needs two cache lines" are answerable facts rather than guesses.
#
# WHAT IT CANNOT SEE, stated once and then measured per function:
#   * calls through function pointers -- `n_fnptr_calls`, and query 1
#   * the preprocessor's own control flow -- `config_blocks` lists the gates
#   * bodies produced by macro expansion -- `macros.n_uses` counts the sites
#
# Every call site lands in exactly one of four buckets, because conflating them
# is how a C repo comes to read as far blinder than it is:
#   edge              resolved to a definition in this tree
#   n_macro_calls     a macro defined in this tree; a preprocessor construct,
#                     not a hole in the graph
#   n_external_calls  libc/POSIX/compiler builtin, or a prototype declared here
#                     and defined outside the tree -- a boundary we chose
#   unresolved_calls  genuinely unknown; this is the honest blindness column
#
# Usage:
#   python3 codegraph_c.py /path/to/repo --report
#   python3 codegraph_c.py /path/to/repo --list
#   python3 codegraph_c.py /path/to/repo 1 19 --limit 20
#   python3 codegraph_c.py --deps          # (none -- pure standard library)
# ==========================================================================

DEPS = DepSet(lang="c", deps=[])

GRAMMAR_NOTE = (
    "no compiler frontend and no third-party package: C is analysed by "
    "blanking comments and literals, matching braces, and scanning tokens. "
    "This is the intended parser for C in this tool, not a fallback -- there "
    "is nothing to install and nothing to keep in ABI lockstep."
)

HAZARD_CATEGORIES = (
    "memory", "alloc", "io", "stdio", "exec", "libm", "integer",
    "concurrency", "control",
    # CERT CON33-C / MSC24-C: libc calls with a shared static buffer.
    # Correct single-threaded, a data race the moment it is not.
    "reentrancy",
)

HAZARD_FUNCS: dict[str, str] = {
    # length-carrying memory and string operations -- CWE-119/120/787
    "memcpy": "memory", "memmove": "memory", "memset": "memory",
    "memcmp": "memory", "memchr": "memory", "mempcpy": "memory",
    "strcpy": "memory", "strncpy": "memory", "strcat": "memory",
    "strncat": "memory", "strlen": "memory", "strnlen": "memory",
    "strcmp": "memory", "strncmp": "memory", "strchr": "memory",
    "strrchr": "memory", "strstr": "memory", "strtok": "memory",
    "strtok_r": "memory", "strlcpy": "memory", "strlcat": "memory",
    "sprintf": "memory", "snprintf": "memory", "vsprintf": "memory",
    "vsnprintf": "memory", "alloca": "memory", "gets": "memory",
    "scanf": "memory", "sscanf": "memory", "fscanf": "memory",
    "vfscanf": "memory", "vsscanf": "memory",
    "bcopy": "memory", "bzero": "memory",
    # allocation -- ownership, not danger. `free` is separated out downstream.
    "malloc": "alloc", "calloc": "alloc", "realloc": "alloc", "free": "alloc",
    "aligned_alloc": "alloc", "posix_memalign": "alloc", "memalign": "alloc",
    "valloc": "alloc", "reallocarray": "alloc",
    "strdup": "alloc", "strndup": "alloc",
    # buffered stdio: no guarantee of one syscall per call
    "fwrite": "stdio", "fread": "stdio", "fprintf": "stdio", "fputs": "stdio",
    "fputc": "stdio", "fgets": "stdio", "fgetc": "stdio", "printf": "stdio",
    "puts": "stdio", "putchar": "stdio", "fflush": "stdio", "fopen": "stdio",
    "fclose": "stdio", "vfprintf": "stdio", "syslog": "stdio",
    "vprintf": "stdio", "vsnprintf": "stdio",
    # raw descriptors: where untrusted bytes enter
    "recv": "io", "send": "io", "read": "io", "write": "io", "pread": "io",
    "pwrite": "io", "readv": "io", "writev": "io", "recvfrom": "io",
    "sendto": "io", "recvmsg": "io", "sendmsg": "io", "accept": "io",
    "accept4": "io", "connect": "io", "bind": "io", "listen": "io",
    "open": "io", "openat": "io", "close": "io", "lseek": "io",
    "mmap": "io", "munmap": "io", "sendfile": "io", "splice": "io",
    "getaddrinfo": "io", "gethostbyname": "io", "inet_pton": "io",
    # handing bytes to another program
    "system": "exec", "popen": "exec", "execve": "exec", "execl": "exec",
    "execlp": "exec", "execvp": "exec", "execv": "exec", "fork": "exec",
    "posix_spawn": "exec", "dlopen": "exec", "dlsym": "exec",
    # a libm call in a loop body is a hard stop for the vectoriser
    "sqrt": "libm", "sqrtf": "libm", "exp": "libm", "expf": "libm",
    "log": "libm", "logf": "libm", "log2": "libm", "log10": "libm",
    "pow": "libm", "powf": "libm", "sin": "libm", "cos": "libm",
    "tan": "libm", "atan": "libm", "atan2": "libm", "asin": "libm",
    "acos": "libm", "tgamma": "libm", "lgamma": "libm", "erf": "libm",
    "fmod": "libm", "cbrt": "libm", "hypot": "libm", "round": "libm",
    "floor": "libm", "ceil": "libm", "trunc": "libm",
    # anything two threads can reach at once
    "pthread_mutex_lock": "concurrency", "pthread_mutex_unlock": "concurrency",
    "pthread_mutex_trylock": "concurrency", "pthread_rwlock_rdlock": "concurrency",
    "pthread_rwlock_wrlock": "concurrency", "pthread_rwlock_unlock": "concurrency",
    "pthread_cond_wait": "concurrency", "pthread_cond_signal": "concurrency",
    "pthread_cond_broadcast": "concurrency",
    "pthread_create": "concurrency", "pthread_join": "concurrency",
    "pthread_detach": "concurrency", "sem_wait": "concurrency",
    "sem_post": "concurrency",
    "atomic_load": "concurrency", "atomic_store": "concurrency",
    "atomic_fetch_add": "concurrency", "atomic_fetch_sub": "concurrency",
    "atomic_exchange": "concurrency",
    "atomic_compare_exchange_strong": "concurrency",
    "atomic_compare_exchange_weak": "concurrency",
    # non-local control flow: the paths a reviewer forgets
    "setjmp": "control", "sigsetjmp": "control", "longjmp": "control",
    "siglongjmp": "control", "abort": "control", "exit": "control",
    "_exit": "control", "assert": "control", "raise": "control",
    "signal": "control", "sigaction": "control",
    # -- CERT, clang-tidy, cppcheck and flawfinder name these. They
    # land in `hazards` per-pattern, and no rule fires here: whether an
    # `atoi` matters depends on where its input came from, which is a
    # join, not a lookup.
    "stpcpy": "memory",
    "stpncpy": "memory",
    "wcscpy": "memory",
    "wcscat": "memory",
    "wcsncpy": "memory",
    "memccpy": "memory",
    "strsep": "memory",
    "realpath": "memory",
    "getwd": "memory",
    "getcwd": "memory",
    "swprintf": "memory",
    "asprintf": "memory",
    "vasprintf": "memory",
    "reallocf": "alloc",
    "strdupa": "alloc",
    "strndupa": "alloc",
    "xmalloc": "alloc",
    "xrealloc": "alloc",
    "xcalloc": "alloc",
    "xstrdup": "alloc",
    "g_malloc": "alloc",
    "g_free": "alloc",
    "cfree": "alloc",
    "execle": "exec",
    "execvpe": "exec",
    "fexecve": "exec",
    "wordexp": "exec",
    "posix_spawnp": "exec",
    "vfork": "exec",
    "dlmopen": "exec",
    "dlclose": "exec",
    "tmpnam": "io",
    "tempnam": "io",
    "mktemp": "io",
    "mkstemp": "io",
    "mkdtemp": "io",
    "freopen": "io",
    "chmod": "io",
    "fchmod": "io",
    "chown": "io",
    "fchown": "io",
    "umask": "io",
    "access": "io",
    "faccessat": "io",
    "link": "io",
    "symlink": "io",
    "unlink": "io",
    "unlinkat": "io",
    "rename": "io",
    "remove": "io",
    "truncate": "io",
    "ftruncate": "io",
    "fsync": "io",
    "fdatasync": "io",
    "readlink": "io",
    "opendir": "io",
    "readdir": "io",
    "pthread_cancel": "concurrency",
    "pthread_kill": "concurrency",
    "sem_trywait": "concurrency",
    "atomic_thread_fence": "concurrency",
    "sched_yield": "concurrency",
    "pthread_barrier_wait": "concurrency",
    "atexit": "control",
    "at_quick_exit": "control",
    "quick_exit": "control",
    "kill": "control",
    "alarm": "control",
    "sigprocmask": "control",
    "sigsuspend": "control",
    "pause": "control",
    "atoi": "integer",
    "atol": "integer",
    "atoll": "integer",
    "atof": "integer",
    "strtol": "integer",
    "strtoul": "integer",
    "strtoll": "integer",
    "strtoull": "integer",
    "strtod": "integer",
    "strtof": "integer",
    "strtoimax": "integer",
    "strtoumax": "integer",
    "gmtime": "reentrancy",
    "localtime": "reentrancy",
    "ctime": "reentrancy",
    "asctime": "reentrancy",
    "getenv": "reentrancy",
    "setenv": "reentrancy",
    "putenv": "reentrancy",
    "strerror": "reentrancy",
    "getpwnam": "reentrancy",
    "getpwuid": "reentrancy",
    "getgrnam": "reentrancy",
    "getgrgid": "reentrancy",
    "gethostbyaddr": "reentrancy",
    "setlocale": "reentrancy",
    "ttyname": "reentrancy",
    "crypt": "reentrancy",
    "basename": "reentrancy",
    "dirname": "reentrancy",
    "tmpfile": "reentrancy",
    "rand": "reentrancy",
    "srand": "reentrancy",
    "random": "reentrancy",
    "srandom": "reentrancy",
    "drand48": "reentrancy",
    "lrand48": "reentrancy",
    "mrand48": "reentrancy",
    "initstate": "reentrancy",
}

HAZARD_RE: list[tuple[str, str, "re.Pattern[str]"]] = [
    ("ptr_cast", "integer", re.compile(r'\(\s*(?:const\s+)?[A-Za-z_]\w*\s*\*+\s*\)')),
    ("shift", "integer", re.compile(r'<<|>>')),
    ("mul_sizeof", "integer", re.compile(r'\*\s*sizeof|sizeof\s*\([^)]*\)\s*\*')),
    ("fixed_buffer", "memory",
     re.compile(r'\b(?:char|uint8_t|unsigned\s+char|int8_t)\s+\w+\s*\[\s*\d+\s*\]')),
    ("vla", "memory",
     re.compile(r'\b(?:char|int|uint8_t|double|float)\s+\w+\s*\[\s*[a-z_]\w*\s*\]')),
    ("signed_cmp", "integer", re.compile(r'\bint\s+\w+\s*=\s*\w+\s*-\s*\w+')),
]

LIBC_ALLOC = ("malloc", "calloc", "realloc", "free", "aligned_alloc",
              "posix_memalign", "memalign", "valloc", "reallocarray",
              "strdup", "strndup")

ALLOC_NAME_RE = re.compile(r'(?:alloc|free|strdup|memdup)$', re.I)

LIBC_KNOWN: frozenset[str] = frozenset("""
malloc calloc realloc free aligned_alloc posix_memalign memalign valloc
reallocarray strdup strndup abort exit _exit atexit getenv setenv unsetenv
putenv system qsort bsearch rand srand random srandom arc4random abs labs llabs
div ldiv lldiv atoi atol atoll strtol strtoll strtoul strtoull strtod strtof
strtold
memcpy memmove memset memcmp memchr memrchr mempcpy bcopy bzero
strcpy strncpy strcat strncat strlen strnlen strcmp strncmp strcasecmp
strncasecmp strchr strrchr strstr strcasestr strtok strtok_r strspn strcspn
strpbrk strerror strerror_r strsignal strlcpy strlcat strsep strdupa
sprintf snprintf vsprintf vsnprintf asprintf vasprintf
printf fprintf vfprintf vprintf dprintf vdprintf puts fputs putchar fputc putc
scanf sscanf fscanf vsscanf getchar getc fgetc gets fgets ungetc
fopen fdopen freopen fclose fflush fread fwrite fseek fseeko ftell ftello
rewind feof ferror clearerr setvbuf setbuf fileno tmpfile popen pclose remove
rename perror
open openat creat close read write pread pwrite readv writev lseek dup dup2
pipe pipe2 fcntl ioctl fsync fdatasync ftruncate truncate stat fstat lstat
fstatat access chmod chown fchmod fchown umask mkdir rmdir unlink unlinkat
symlink readlink link rename realpath getcwd chdir opendir readdir closedir
rewinddir scandir alphasort mmap munmap mprotect madvise msync mlock munlock
sbrk brk isatty ttyname sync
socket bind listen accept accept4 connect shutdown send sendto sendmsg recv
recvfrom recvmsg getsockopt setsockopt getsockname getpeername socketpair
select pselect poll ppoll epoll_create epoll_create1 epoll_ctl epoll_wait
kqueue kevent getaddrinfo freeaddrinfo gai_strerror gethostbyname
gethostbyname_r getnameinfo inet_pton inet_ntop inet_addr inet_ntoa htons htonl
ntohs ntohl
fork vfork execve execl execlp execle execv execvp execvpe posix_spawn
posix_spawnp wait waitpid wait3 wait4 kill killpg raise signal sigaction
sigemptyset sigfillset sigaddset sigdelset sigismember sigprocmask sigsuspend
sigwait alarm pause getpid getppid getuid geteuid getgid getegid setuid setgid
seteuid setegid setsid setpgid getpgid nice sched_yield sched_setaffinity
sched_getaffinity setrlimit getrlimit getrusage sysconf uname daemon
pthread_create pthread_join pthread_detach pthread_exit pthread_self
pthread_equal pthread_cancel pthread_kill pthread_once pthread_atfork
pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock
pthread_mutex_trylock pthread_mutex_unlock pthread_mutexattr_init
pthread_mutexattr_settype pthread_mutexattr_destroy
pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait
pthread_cond_signal pthread_cond_broadcast
pthread_rwlock_init pthread_rwlock_destroy pthread_rwlock_rdlock
pthread_rwlock_wrlock pthread_rwlock_tryrdlock pthread_rwlock_trywrlock
pthread_rwlock_unlock pthread_key_create pthread_key_delete
pthread_getspecific pthread_setspecific pthread_attr_init pthread_attr_destroy
pthread_attr_setstacksize pthread_attr_setdetachstate pthread_setname_np
pthread_getname_np sem_init sem_destroy sem_wait sem_trywait sem_post
sem_getvalue
atomic_load atomic_store atomic_exchange atomic_fetch_add atomic_fetch_sub
atomic_fetch_or atomic_fetch_and atomic_fetch_xor atomic_flag_test_and_set
atomic_compare_exchange_strong atomic_compare_exchange_weak
atomic_thread_fence atomic_init atomic_is_lock_free
time gmtime localtime gmtime_r localtime_r mktime timegm strftime strptime
difftime clock clock_gettime clock_getres clock_settime gettimeofday
settimeofday nanosleep usleep sleep asctime ctime asctime_r ctime_r
sqrt sqrtf sqrtl cbrt exp expf exp2 expm1 log logf log2 log10 log1p pow powf
sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh hypot fmod
fmodf remainder fabs fabsf fmax fmin fdim floor ceil round lround llround trunc
rint nearbyint modf frexp ldexp copysign nan isnan isinf isfinite signbit
tgamma lgamma erf erfc
isalpha isdigit isalnum isspace isupper islower ispunct isxdigit isprint
isgraph iscntrl toupper tolower isblank
setjmp longjmp sigsetjmp siglongjmp
va_start va_end va_arg va_copy offsetof sizeof alignof
dlopen dlsym dlclose dlerror backtrace backtrace_symbols
iconv iconv_open iconv_close wcslen wcscpy mbstowcs wcstombs
syslog openlog closelog getopt getopt_long basename dirname glob globfree
crypt getpwnam getpwuid getgrnam getgrgid
mkstemp mkdtemp tmpnam tempnam ftok shmget shmat shmdt shmctl semget semop
msgget msgsnd msgrcv
""".split())

BUILTIN_PREFIXES = ("__builtin_", "__atomic_", "__sync_", "__asan_",
                    "__msan_", "__tsan_", "__c11_atomic_", "_InterlockedExchange")

INTRIN_NAME_RE = re.compile(
    r'^(?:v[a-z0-9_]+q?_[a-z0-9_]+|_mm\d*_[a-z0-9_]+|sv[a-z0-9_]+_[a-z0-9_]+'
    r'|vec_[a-z0-9_]+|_bit_[a-z0-9_]+)$')

# One alternation scan replaces the seven per-keyword scans metrics() used to
# run; the tokens are disjoint so a single pass counts every category exactly.
METRIC_KW = re.compile(
    r'\b(for|while|do|if|switch|case|goto|return|catch)\b|&&|\|\||\?')

IDENT = re.compile(r'[A-Za-z_]\w*')

OPERATOR = re.compile(r'[+\-*/%=<>!&|^~]+|\[|\]|\.|->')

INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"]+)[>"]', re.M)

DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)(\([^)]*\))?[ \t]*(.*)$', re.M)

TYPEDEF_SIMPLE = re.compile(r'^[ \t]*typedef[ \t]+(.+?)\b(\w+)[ \t]*;', re.M)

TAG_BODY = re.compile(r'^[ \t]*(?:typedef[ \t]+)?(struct|enum|union)[ \t]*(\w*)[ \t]*\{', re.M)

STRING_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')

#: G07: a string literal that names a credential (mirrors the other packs).
SECRET_RE = re.compile(
    r'(api[_-]?key|apikey|secret|password|passwd|pwd|token|bearer|'
    r'access[_-]?key|private[_-]?key|client[_-]?secret|'
    r'auth[_-]?token|jwt|credential|smtp[_-]?pass|db[_-]?pass|'
    r'sk_live|rk_live|pk_live|ghp_|xoxb-|AKIA)', re.I)
SECRET_MIN_LEN = 12

#: Symbols are buffered and written in batches of this many (bounded so the
#: buffer never shows up in peak RSS), then flushed once after the parse loop.
SYMBOL_BATCH = 4000

_STORAGE = (r'(?:const[ \t]+|volatile[ \t]+|_Atomic[ \t]+|atomic_\w+[ \t]+|'
            r'unsigned[ \t]+|signed[ \t]+|long[ \t]+|short[ \t]+|struct[ \t]+|'
            r'enum[ \t]+|union[ \t]+)*')

def file_scope_depth(blank: str) -> list[int]:
    """Brace depth at the START of each line, from blanked source.

    A declaration is FILE-SCOPE only at depth 0: struct/union/enum fields sit
    inside their definition's braces, and function bodies inside theirs. The
    globals collector once recorded every field of every struct as mutable
    file-scope state, which made race-surface and global-state-mutation list
    `buckets`, `adj`, `rt` and a thousand other members as if each were a
    global variable -- a false positive per struct definition.

    Preprocessor lines are depth-neutral: `#define F(x) {` would otherwise
    open a brace that never closes at file scope and hide every real global
    after it. A multiline macro's continuation lines are NOT #-prefixed, so
    braces there still count -- a BALANCED macro nets to zero, which is the
    shape that matters.

    LOCAL PATCH on top of upstream codegraph_c.py -- send back when it works.
    """
    lines = blank.split("\n")
    depths = [0] * len(lines)
    d = 0
    for idx, line in enumerate(lines):
        depths[idx] = d
        if line.lstrip().startswith("#"):
            continue
        for ch in line:
            if ch == "{":
                d += 1
            elif ch == "}" and d > 0:
                d -= 1
    return depths


GLOBAL_RE = re.compile(
    r'^[ \t]*(static[ \t]+|extern[ \t]+)?'
    + r'(' + _STORAGE + r'[A-Za-z_]\w*)'
    + r'[ \t]+(\*+)?[ \t]*(\w+)[ \t]*(\[[^\]]*\])?[ \t]*(=[^;]*)?;',
    re.M)

LOCAL_RE = re.compile(
    r'^[ \t]*((?:const[ \t]+|volatile[ \t]+|static[ \t]+|register[ \t]+|'
    r'unsigned[ \t]+|signed[ \t]+|long[ \t]+|short[ \t]+|struct[ \t]+|'
    r'enum[ \t]+|union[ \t]+)*)'
    r'([A-Za-z_]\w*)[ \t]+(\*+)?[ \t]*(\w+)[ \t]*(\[[^\]]*\])?[ \t]*(=[^;]*)?;',
    re.M)

PROTO_RE = re.compile(
    r'^[ \t]*(?:extern[ \t]+|static[ \t]+)?'
    r'[A-Za-z_][\w \t*]*?\b([A-Za-z_]\w*)[ \t]*\([^;{]*\)[ \t]*;', re.M)

NUM_RE = re.compile(r'\b(0[xX][0-9a-fA-F]+|\d+[uUlL]*)\b')

IFDEF_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif)[ \t]+(.+)$', re.M)

IFEND_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|endif)\b', re.M)

LABEL_RE = re.compile(r'^[ \t]*([A-Za-z_]\w*)[ \t]*:(?![:=])', re.M)

CASE_RE = re.compile(r'\bcase\b[ \t]+([^:]+):')

ATTR_RE = re.compile(r'__attribute__\s*\(\(([^)]*)\)\)')

INTRIN_RE = re.compile(
    r'\b(v[a-z0-9_]+q?_[a-z0-9_]+|_mm\d*_[a-z0-9_]+|sv[a-z0-9_]+_[a-z0-9_]+)\s*\(')

RETURN_SHAPE = re.compile(r'\breturn\b([^;]*);')

FUNC_SCAN = re.compile(r'[{}]|\b([A-Za-z_]\w*)\s*\(')

FNPTR_CALL_RE = re.compile(r'\(\s*\*\s*\w+\s*\)\s*\(')

#: `(T*)name` -- a cast to a pointer type applied to an identifier. Feeds the
#: const-cast-away capture (the name must be const-declared to count).
CAST_RE = re.compile(r'\(\s*[^()]*\*\s*\)\s*([A-Za-z_]\w*)')

#: access(X) / open(X) with a variable argument: the TOCTOU pair shape.
TOCTOU_ACCESS_RE = re.compile(r'\baccess\s*\(\s*([A-Za-z_]\w*)')
TOCTOU_OPEN_RE = re.compile(r'\bopen\s*\(\s*([A-Za-z_]\w*)')

#: `&fn` -- address-of an identifier in a body. A name whose address is taken
#: is used even when no direct call edge exists.
ADDR_TAKEN_RE = re.compile(r'&\s*([A-Za-z_]\w*)')

MEMBER_CALL_RE = re.compile(r'(?:->|(?<![.\d])\.)\s*\w+\s*\(')

MAKE_RULE_RE = re.compile(r'^([A-Za-z0-9_./$()%-]+)\s*:[^=]')

MAKE_VAR_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*[:+?]?=\s*(.*)$')

MAKE_OBJ_RE = re.compile(r'[\w/$(){}.-]+\.o\b')

MAKE_SRC_RE = re.compile(r'[\w/.-]+\.c\b')

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

TYPE_WORDS = {"const", "volatile", "static", "inline", "extern", "restrict",
              "unsigned", "signed", "struct", "enum", "union", "_Atomic",
              "long", "short"}

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

def blank_c(src: str) -> str:
    """Comments and string/char literals -> spaces.

    Newlines are preserved so every byte offset in the result still maps to the
    same line in the original. That invariant is what lets the whole rest of
    this file scan the blanked text and still report exact line numbers.
    """
    n = len(src)
    i = 0
    segments: list[str] = []
    prev = 0

    def emit_blank(start: int, end: int) -> None:
        segments.append(src[prev:start])
        segments.append(''.join('\n' if ch == '\n' else ' '
                                for ch in src[start:end]))

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

def scan_defects(raw: str, blank: str) -> tuple[int, int]:
    """(n_parse_errors, n_missing_nodes) for one file.

    A regex scanner never raises on malformed C, so without this a file that
    the analyzer silently mis-read would be indistinguishable from one it read
    correctly -- and `--report` would claim coverage it does not have.

    An error is something that makes the brace/paren structure unusable, which
    is exactly what function discovery depends on. A missing node is a
    preprocessor conditional that never closes.
    """
    errors = 0
    depth = 0
    negative = False
    for ch in blank:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                negative = True
                depth = 0
    if depth != 0 or negative:
        errors += 1
    if blank.count("(") != blank.count(")"):
        errors += 1
    last_open = raw.rfind("/*")
    if last_open >= 0 and raw.find("*/", last_open + 2) < 0:
        errors += 1

    opens = closes = 0
    for m in IFEND_RE.finditer(raw):
        if m.group(1) == "endif":
            closes += 1
        else:
            opens += 1
    return errors, abs(opens - closes)

def split_params(sig: str) -> list[tuple[int, str, str, int, int, int, int]]:
    """(pos, type, name, ptr_depth, is_array, is_const, is_varargs) per param."""
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

TYPE_SIZE: dict[str, int] = {
    "char": 1, "signed char": 1, "unsigned char": 1, "uint8_t": 1, "int8_t": 1,
    "_Bool": 1, "bool": 1,
    "short": 2, "unsigned short": 2, "uint16_t": 2, "int16_t": 2,
    "short int": 2, "unsigned short int": 2,
    "int": 4, "unsigned": 4, "unsigned int": 4, "uint32_t": 4, "int32_t": 4,
    "float": 4, "mode_t": 4, "pid_t": 4, "uid_t": 4, "gid_t": 4,
    "long": 8, "unsigned long": 8, "uint64_t": 8, "int64_t": 8, "double": 8,
    "size_t": 8, "ssize_t": 8, "long long": 8, "unsigned long long": 8,
    "long int": 8, "unsigned long int": 8,
    "intptr_t": 8, "uintptr_t": 8, "off_t": 8, "time_t": 8, "pthread_t": 8,
    "ptrdiff_t": 8, "long double": 16,
}

TYPE_ALIGN: dict[str, int] = {"long double": 16}

PTR_SIZE = 8

def type_size(ctype: str, ptr_depth: int, array_len: int) -> tuple[int, int, int]:
    """(size, align, exact). A pointer is 8/8 and always exact under LP64.

    `exact=0` means the type is a struct or typedef this scanner cannot size.
    Every layout query filters on `exact=1`, so an unknown type produces no
    answer rather than a wrong one.
    """
    if ptr_depth > 0:
        base, align, exact = PTR_SIZE, PTR_SIZE, 1
    else:
        t = re.sub(r'\b(const|volatile|struct|union|enum|_Atomic|register)\b',
                   ' ', ctype)
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
    """Assign byte offsets to top-level fields.

    -> (rows, total_size, tail_pad, all_exact, max_align). Nested and union-arm
    fields get offset -1: a union arm has no single offset, and reporting one
    would be a lie a reader would act on.
    """
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

def expr_counts(body: str) -> dict[str, int]:
    """Expression-level texture: what the code is made of, not how big it is."""
    arrow = body.count("->")
    return dict(
        n_deref=len(re.findall(r'(?<![->])\*\s*[A-Za-z_(]', body)) + arrow,
        n_arrow=arrow,
        n_subscript=body.count("["),
        n_member_access=arrow + len(re.findall(r'(?<![.\d])\.(?=[A-Za-z_])', body)),
        n_addrof=len(re.findall(r'(?<!\w)&[A-Za-z_]', body)),
        n_cast=len(re.findall(r'\(\s*(?:const\s+)?[A-Za-z_]\w*\s*\*+\s*\)', body)),
        n_sizeof=len(re.findall(r'\bsizeof\b', body)),
        n_ternary=body.count('?'),
        n_bitop=sum(body.count(c) for c in '&|^~'),
        n_shift=body.count('<<') + body.count('>>'),
        n_arith=sum(body.count(c) for c in '+-*/%'),
        n_cmp=len(re.findall(r'==|!=|<=|>=|(?<![=<])<(?![<=])|(?<![=>])>(?![>=])', body)),
        n_assign=len(re.findall(r'(?<![+\-*/%&|^<>!=])=(?!=)', body)),
        n_compound_assign=sum(body.count(s) for s in ('+=','-=','*=','/=','%=','&=','|=','^=','<<=','>>=')),
        n_incdec=body.count('++') + body.count('--'),
        n_logical=body.count('&&') + body.count('||'),
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
    """How a function reports failure.

    C has no error type, so the convention is per-function and invisible at the
    call site. A function that returns both NULL and -1 is one every caller has
    an even chance of checking wrongly.
    """
    r: dict[str, int] = dict(ret_null=0, ret_neg=0, ret_zero=0, ret_val=0,
                             ret_void=0)
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
    """The keyword that opened the block at `brace_pos`, skipping its `(...)`."""
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

LOOP_ALLOC = ("malloc", "calloc", "realloc", "strdup", "strndup",
              "aligned_alloc", "reallocarray")

LOOP_LIBM = ("sqrt", "exp", "log", "log2", "log10", "pow", "sin", "cos", "tan",
             "atan", "atan2", "tgamma", "lgamma", "erf", "fmod", "cbrt",
             "hypot", "floor", "ceil", "round")

def loop_analysis(body: str) -> dict[str, int]:
    """Per-iteration cost: what actually sits INSIDE a loop body.

    Counting loops tells you a function iterates. Counting what is inside them
    tells you what it iterates over -- and a `strlen` or a `malloc` there is
    the difference between O(n) and O(n^2).
    """
    r = dict(max_loop_depth=0, switch_in_loop=0, alloc_in_loop=0,
             call_in_loop=0, libm_in_loop=0, div_in_loop=0, strlen_in_loop=0,
             branch_in_loop=0, io_in_loop=0, lock_in_loop=0)
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
            if ch == "/" and i + 1 < n and body[i + 1] not in "/=":
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
                    elif tok in ("if", "case"):
                        r["branch_in_loop"] += 1
                    elif called and tok not in KEYWORDS:
                        r["call_in_loop"] += 1
                        if tok in LOOP_ALLOC:
                            r["alloc_in_loop"] += 1
                        if tok in LOOP_LIBM:
                            r["libm_in_loop"] += 1
                        if tok in ("strlen", "strnlen"):
                            r["strlen_in_loop"] += 1
                        cat = HAZARD_FUNCS.get(tok)
                        if cat == "io":
                            r["io_in_loop"] += 1
                        elif cat == "concurrency":
                            r["lock_in_loop"] += 1
                    i = m.end()
                    continue
        i += 1
    return r

def metrics(body: str) -> dict[str, int]:
    """Size and control-flow shape."""
    nl = nb = ns = ng = nr = nc = nlog = 0
    for m in METRIC_KW.finditer(body):
        t = m.group(0)
        if t == '&&' or t == '||' or t == '?':
            nlog += 1
            continue
        if t in ('if', 'for', 'while', 'case', 'catch'):
            nc += 1
        if t in ('for', 'while', 'do'):
            nl += 1
        if t == 'if' or t == 'switch' or t == 'case':
            nb += 1
        if t == 'switch':
            ns += 1
        if t == 'goto':
            ng += 1
        if t == 'return':
            nr += 1
    cyclo = 1 + nc + nlog
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
    idents = IDENT.findall(body)
    ops = OPERATOR.findall(body)
    return dict(
        cyclomatic=cyclo, cognitive=cog, max_nesting=mx,
        sloc=sum(1 for l in body.splitlines() if l.strip()),
        n_loops=nl,
        n_branches=nb,
        n_switch=ns,
        n_returns=nr,
        n_early_returns=max(0, nr - 1),
        n_gotos=ng,
        n_tokens=len(idents) + len(ops),
        n_operators=len(ops),
        n_operands=len(idents),
        n_distinct_operators=len(set(ops)),
        n_distinct_operands=len(set(idents)),
    )

def struct_fields(blank: str, raw: str, open_brace: int) -> tuple[list[tuple[Any, ...]], int]:
    """Members of the struct/union body whose `{` is at `open_brace`.

    Tracks a stack of nested tags so a field inside an anonymous union is
    marked `in_union`, which the layout pass needs: union arms overlap and must
    not be laid out end to end.
    """
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
        if "(" in stmt:                       # function-pointer member
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

def has_leading_comment(raw_lines: list[str], line_start: int) -> tuple[int, int]:
    """(has_doc, n_doc_lines) for the comment block just above a definition."""
    i = line_start - 2                        # 0-based index of previous line
    n = 0
    while i >= 0:
        s = raw_lines[i].strip()
        if not s and n == 0:
            i -= 1
            continue
        if s.startswith(("/*", "*", "*/", "//")):
            n += 1
            i -= 1
            continue
        break
    return (1 if n else 0), n

class CAnalyzer(Analyzer):
    LANG = "c"
    TARGET = "C11/C17 + GNU and Clang extensions (no preprocessor required)"
    EXTS = (".c", ".h", ".inc", ".cc")
    SKIP_DIRS = {"CMakeFiles", ".deps", ".libs", "autom4te.cache"}
    DEPS = DEPS
    HAZARD_CATEGORIES = HAZARD_CATEGORIES
    MANIFESTS = ("Makefile", "makefile", "GNUmakefile", "CMakeLists.txt",
                 "meson.build", "configure.ac")
    QUERIES = []          # filled in below, after the catalogue is defined

    EXTRA_SYMBOL_COLS = (
        ("is_inline", "INT NOT NULL DEFAULT 0"),
        ("is_variadic", "INT NOT NULL DEFAULT 0"),
        ("n_ptr_params", "INT NOT NULL DEFAULT 0"),
        ("n_ptr_locals", "INT NOT NULL DEFAULT 0"),
        ("n_deref", "INT NOT NULL DEFAULT 0"),
        ("n_arrow", "INT NOT NULL DEFAULT 0"),
        ("n_addrof", "INT NOT NULL DEFAULT 0"),
        ("n_cast", "INT NOT NULL DEFAULT 0"),
        ("n_sizeof", "INT NOT NULL DEFAULT 0"),
        ("n_intrinsic", "INT NOT NULL DEFAULT 0"),
        ("n_atomic", "INT NOT NULL DEFAULT 0"),
        ("n_volatile", "INT NOT NULL DEFAULT 0"),
        ("n_restrict", "INT NOT NULL DEFAULT 0"),
        ("n_likely", "INT NOT NULL DEFAULT 0"),
        ("n_builtin", "INT NOT NULL DEFAULT 0"),
        ("n_static_assert", "INT NOT NULL DEFAULT 0"),
        ("switch_in_loop", "INT NOT NULL DEFAULT 0"),
        ("libm_in_loop", "INT NOT NULL DEFAULT 0"),
        ("div_in_loop", "INT NOT NULL DEFAULT 0"),
        ("strlen_in_loop", "INT NOT NULL DEFAULT 0"),
        ("ret_null", "INT NOT NULL DEFAULT 0"),
        ("ret_neg", "INT NOT NULL DEFAULT 0"),
        ("ret_zero", "INT NOT NULL DEFAULT 0"),
        ("ret_val", "INT NOT NULL DEFAULT 0"),
        ("ret_void", "INT NOT NULL DEFAULT 0"),
        ("n_fnptr_calls", "INT NOT NULL DEFAULT 0"),
        ("n_macro_calls", "INT NOT NULL DEFAULT 0"),
        ("n_external_calls", "INT NOT NULL DEFAULT 0"),
        ("n_extern_decl_calls", "INT NOT NULL DEFAULT 0"),
        ("n_free", "INT NOT NULL DEFAULT 0"),
        # -- P2 pack: const-cast, toctou ------------------------------------
        ("n_const_cast", "INT NOT NULL DEFAULT 0"),
        ("n_toctou", "INT NOT NULL DEFAULT 0"),
    )

    SCHEMA_EXT = r"""
-- Byte-accurate field placement under LP64. `exact=0` means a field type this
-- scanner could not size, and every layout query filters those out rather than
-- reporting a number it cannot stand behind.
CREATE TABLE layout(
    symbol_id INT NOT NULL REFERENCES symbols(id),
    ordinal INT NOT NULL,
    byte_off INT NOT NULL,          -- -1 for a nested or union-arm field
    byte_size INT NOT NULL,
    pad_before INT NOT NULL,
    exact INT NOT NULL,
    ptr_depth INT NOT NULL DEFAULT 0,
    array_len INT NOT NULL DEFAULT 0,
    is_fnptr INT NOT NULL DEFAULT 0,
    depth INT NOT NULL DEFAULT 0,
    in_union INT NOT NULL DEFAULT 0,
    PRIMARY KEY(symbol_id, ordinal)
) WITHOUT ROWID, STRICT;

 CREATE TABLE struct_size(
     symbol_id INT NOT NULL PRIMARY KEY REFERENCES symbols(id),
     total_size INT NOT NULL,
     tail_pad INT NOT NULL,
     total_pad INT NOT NULL,
     max_align INT NOT NULL,
     exact INT NOT NULL,
     n_lines_64 INT NOT NULL       -- 64-byte cache lines this object spans
 ) WITHOUT ROWID, STRICT;

 -- Prototypes and extern declarations: names promised in this file but not
 -- (necessarily) defined anywhere in the tree. Feeds extern-symbol-asymmetry.
 CREATE TABLE declarations(
     id INTEGER PRIMARY KEY,
     file_id INT NOT NULL REFERENCES files(id),
     name TEXT NOT NULL,
     line INT NOT NULL DEFAULT 0
 ) STRICT;

 -- `&fn` expressions: addresses taken of in-tree functions. A function whose
 -- only uses are address-taken is invisible to the call graph (the call goes
 -- through a pointer) -- fnptr-blindspot-callers ranks exactly those.
 CREATE TABLE addr_taken(
     id INTEGER PRIMARY KEY,
     symbol_id INT REFERENCES symbols(id),
     file_id INT NOT NULL REFERENCES files(id),
     name TEXT NOT NULL,
     line INT NOT NULL DEFAULT 0
 ) STRICT;

 CREATE TABLE secret_candidates(
     id INTEGER PRIMARY KEY,
     symbol_id INT REFERENCES symbols(id),
     file_id INT NOT NULL REFERENCES files(id),
     value TEXT NOT NULL,
     line INT NOT NULL
 ) STRICT;


-- The bodies the call graph cannot see into. `n_uses` is filled in during call
-- resolution, so "which macro is doing the most work" is answerable.
CREATE TABLE macros(
    symbol_id INT NOT NULL PRIMARY KEY REFERENCES symbols(id),
    is_functionlike INT NOT NULL DEFAULT 0,
    n_params INT NOT NULL DEFAULT 0,
    body TEXT,
    body_len INT NOT NULL DEFAULT 0,
    is_multiline INT NOT NULL DEFAULT 0,
    n_uses INT NOT NULL DEFAULT 0
) WITHOUT ROWID, STRICT;

-- File-scope objects. Kept out of `symbols` deliberately: the questions asked
-- of a C global are about storage and mutability, not about calls.
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

-- The preprocessor's control flow: regions a plain build silently omits.
CREATE TABLE config_blocks(
    id INTEGER PRIMARY KEY,
    file_id INT NOT NULL REFERENCES files(id),
    directive TEXT NOT NULL,
    expr TEXT NOT NULL,
    line INT NOT NULL,
    is_config INT NOT NULL DEFAULT 0
) STRICT;

-- Transitive caller counts computed in Python after resolve_calls (the call
-- graph is a DAG-plus-cycles, so SQL recursion would re-expand per path).
-- `n_transitive` is the number of DISTINCT functions that can reach the
-- symbol through resolved edges at any depth.
CREATE TABLE reach(
    symbol_id INT NOT NULL PRIMARY KEY REFERENCES symbols(id),
    n_transitive INT NOT NULL DEFAULT 0
) WITHOUT ROWID, STRICT;

CREATE TABLE makefile_rules(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    rule TEXT NOT NULL,
    line INT NOT NULL,
    n_objs INT NOT NULL DEFAULT 0,
    n_srcs INT NOT NULL DEFAULT 0,
    uses_ar INT NOT NULL DEFAULT 0
) STRICT;

-- ===== DYN PROJECT (dynajs repo only; not upstream) ======================
-- Runtime-API call sites per function: JS_*/js_* names (JS_FreeValue,
-- JS_NewObject, JS_SetPropertyStr, JS_Throw*, ...). The brace scanner counts
-- them only as extern/unresolved; this table carries the NAMES, filled by a
-- post-parse text pass over each function's byte range. False positives are
-- possible where a comment or string contains call-shaped text -- rare.
CREATE TABLE dyn_api_calls(
    caller_id INT NOT NULL REFERENCES symbols(id),
    name TEXT NOT NULL,
    n INT NOT NULL DEFAULT 1,
    first_line INT NOT NULL DEFAULT 0,
    PRIMARY KEY(caller_id, name)
) WITHOUT ROWID, STRICT;
-- ===== END DYN PROJECT ===================================================
"""

    INDEX_EXT = r"""
CREATE INDEX idx_layout_pad ON layout(symbol_id, pad_before DESC) WHERE pad_before>0;
CREATE INDEX idx_layout_ptr ON layout(symbol_id) WHERE ptr_depth>0;
CREATE INDEX idx_layout_top ON layout(symbol_id, ptr_depth) WHERE in_union=0 AND depth=0;
CREATE INDEX idx_ss_pad ON struct_size(total_pad DESC, total_size);
CREATE INDEX idx_ss_lines ON struct_size(n_lines_64 DESC, total_size DESC);
CREATE INDEX idx_macro_uses ON macros(n_uses DESC) WHERE is_functionlike=1;
CREATE INDEX idx_glob_shared ON globals(module_id) WHERE is_static=1 AND is_const=0;
CREATE INDEX idx_glob_name ON globals(name);
CREATE INDEX idx_symbols_concurrency ON symbols(module_id) WHERE n_concurrency>0;
CREATE INDEX idx_cfg_expr ON config_blocks(expr) WHERE is_config=1;
CREATE INDEX idx_reach_fan ON reach(n_transitive DESC) WHERE n_transitive>0;
CREATE INDEX idx_decl_name ON declarations(name);
CREATE INDEX idx_addrtaken ON addr_taken(name);
CREATE INDEX idx_secret_sym ON secret_candidates(symbol_id);
CREATE INDEX idx_mk_rule ON makefile_rules(n_objs DESC, n_srcs DESC);
CREATE INDEX idx_fn_fnptr ON symbols(n_fnptr_calls DESC, name, file_id) WHERE n_fnptr_calls>0;
CREATE INDEX idx_fn_extern ON symbols(n_external_calls DESC, name, file_id) WHERE n_external_calls>0;
CREATE INDEX idx_fn_libm ON symbols(n_libm DESC, name, file_id) WHERE n_libm>0;
CREATE INDEX idx_fn_leak ON symbols(n_alloc DESC, name, file_id) WHERE n_alloc>0 AND n_free=0;
CREATE INDEX idx_fn_swloop ON symbols(switch_in_loop DESC, name, file_id) WHERE switch_in_loop>0;
CREATE INDEX idx_fn_libmlp ON symbols(libm_in_loop DESC, name, file_id) WHERE libm_in_loop>0;
CREATE INDEX idx_fn_divlp ON symbols(div_in_loop DESC, name, file_id) WHERE div_in_loop>0;
CREATE INDEX idx_fn_strlen ON symbols(strlen_in_loop DESC, name, file_id) WHERE strlen_in_loop>0;
CREATE INDEX idx_fn_intrin ON symbols(n_intrinsic DESC, name, file_id) WHERE n_intrinsic>0;
CREATE INDEX idx_fn_cast ON symbols(n_cast DESC, name, file_id) WHERE n_cast>0;
CREATE INDEX idx_fn_inline ON symbols(fan_in DESC, name, file_id) WHERE is_inline=1 AND is_static=1;
"""

    VIEW_EXT = r"""
-- Every C-specific blindness signal in one place, per function.
CREATE VIEW v_c_blind AS
SELECT s.id, s.name, f.path, m.name AS module, s.n_calls,
    s.n_fnptr_calls, s.n_macro_calls, s.n_external_calls,
    s.n_extern_decl_calls, s.n_unresolved_calls, s.fan_out,
    f.path || ':' || s.line_start AS at
FROM symbols s JOIN files f ON f.id=s.file_id
LEFT JOIN modules m ON m.id=s.module_id
WHERE s.kind='function';

-- Field-level shape of every struct and union, top-level members only:
-- a union arm is an alternative, not an additional field.
CREATE VIEW v_struct_shape AS
SELECT s.id, s.name AS struct_name, s.kind, f.path, m.name AS module,
    COUNT(*) AS n_fields,
    SUM(l.ptr_depth>0) AS n_pointers,
    SUM(l.is_fnptr) AS n_fnptrs,
    SUM(l.array_len>0) AS n_arrays,
    SUM(l.pad_before) AS inner_pad,
    f.path || ':' || s.line_start AS at
FROM symbols s JOIN layout l ON l.symbol_id=s.id
JOIN files f ON f.id=s.file_id
LEFT JOIN modules m ON m.id=s.module_id
WHERE s.kind IN ('struct','union') AND l.in_union=0 AND l.depth=0
GROUP BY s.id;

CREATE VIEW v_attack_surface AS
SELECT * FROM v_fn WHERE n_io>0 OR n_exec>0 OR (n_memory>0 AND fan_in=0);
"""

    MATERIALIZE_EXT = r"""
UPDATE symbols AS s SET n_unique_calls = x.c FROM
    (SELECT caller_id AS id, COUNT(*) AS c FROM edges GROUP BY caller_id) AS x
    WHERE x.id = s.id;

-- `n_alloc` arrives from the generic hazard-category pass, which cannot know
-- that `free` is the opposite of allocation. Split them: an allocation without
-- a matching free in the same function is a review item, and lumping the two
-- together makes every careful function look like a leak.
UPDATE symbols AS s SET n_free = x.n FROM
    (SELECT symbol_id AS id, SUM(n) AS n FROM hazards
     WHERE category='alloc' AND (pattern='free' OR pattern LIKE '%free')
     GROUP BY symbol_id) AS x WHERE x.id = s.id;

UPDATE symbols SET n_alloc = 0 WHERE n_alloc > 0;

UPDATE symbols AS s SET n_alloc = x.n FROM
    (SELECT symbol_id AS id, SUM(n) AS n FROM hazards
     WHERE category='alloc' AND pattern<>'free' AND pattern NOT LIKE '%free'
     GROUP BY symbol_id) AS x WHERE x.id = s.id;
"""

    #: Weighted for C's actual failure modes: memory and I/O dominate, and a
    #: recursive function on attacker-controlled depth is a stack DoS.
    RISK_SQL = (
        "cyclomatic*2 + cognitive + max_nesting*5"
        " + n_memory*10 + n_io*8 + n_exec*15 + n_integer*1"
        " + n_alloc*2 + n_concurrency*3"
        " + (CASE WHEN is_recursive THEN 25 ELSE 0 END)"
        " + (CASE WHEN n_alloc>0 AND n_free=0 THEN 10 ELSE 0 END)"
    )

    # -- setup -------------------------------------------------------------
    def __init__(self) -> None:
        super().__init__()
        #: function name -> [(symbol_id, file_id, module_id)]
        self.fn_by_name: dict[str, list[tuple[int, int, int]]] = {}
        #: macro name -> symbol_id of its first definition
        self.macro_sid: dict[str, int] = {}
        #: names with a prototype somewhere in this tree
        self.declared: set[str] = set()
        #: pending calls: (caller_sid, file_id, module_id, name, line, n)
        #: (caller, file, module, callee name, EVERY call line, count)
        #: Unresolved calls, column-wise. C carries the call LINES per site
        #: rather than one line, so that column stays a list of lists; the
        #: three integer columns still become typed arrays. Same reasoning as
        #: the other analyzers: these rows are live when peak RSS is set.
        self.pend_sid: array.array = array.array("i")
        self.pend_fid: array.array = array.array("i")
        self.pend_mid: array.array = array.array("i")
        self.pend_cnt: array.array = array.array("i")
        self.pend_name: list[str] = []
        self.pend_lines: list[list[int]] = []
        #: buffered symbol rows, bucketed by column shape (go-style batching)
        self._sym_buckets: dict[tuple[str, ...], list[tuple[Any, ...]]] = {}
        self._sym_sqls: dict[tuple[str, ...], str] = {}
        self._n_sym = 0
        #: basename -> file_id, built once, for #include resolution
        self.by_basename: dict[str, int] = {}
        self.n_edges = self.n_macro = self.n_extern = self.n_unres = 0

    def setup(self) -> ParserHandle:
        """A parser handle that tells the truth about what this is.

        MODE_BRACE_SCAN, not MODE_REGEX. The distinction is not cosmetic: the
        report prints a "spans and nesting are approximate, install the
        grammar" warning for MODE_REGEX, and for C there is no grammar to
        install and nothing degraded about the result. Labelling the intended
        parser a fallback teaches every future reader to distrust correct
        answers.
        """
        return ParserHandle(
            mode=MODE_BRACE_SCAN, parser=blank_c, lang_name="c",
            note="regex + brace matching (no compiler frontend required)")

    # -- parsing -----------------------------------------------------------
    def parse_file(self, rec: FileRec, db: sqlite3.Connection,
                   bufs: Buffers) -> None:
        t0 = time.time()
        raw = rec.text
        blank = blank_c(raw)
        errors, missing = scan_defects(raw, blank)

        self._includes(rec, raw, bufs)
        self._config_blocks(rec, raw, db)
        self._macros(rec, raw, db, bufs)
        self._typedefs(rec, raw, db)
        self._tags(rec, raw, blank, db, bufs)

        funcs = find_functions(blank, raw)
        spans = sorted((ls, le) for _, _, ls, le, _, _, _, _ in funcs)
        starts = [s for s, _ in spans]
        ends = [e for _, e in spans]

        def in_function(ln: int) -> bool:
            idx = bisect.bisect_right(starts, ln) - 1
            return idx >= 0 and ln <= ends[idx]

        self._prototypes(rec, blank, in_function, bufs)
        self._globals(rec, blank, db, in_function, file_scope_depth(blank))
        self._functions(rec, raw, blank, funcs, db, bufs)

        db.execute("UPDATE files SET n_parse_errors=?, n_missing_nodes=?, "
                   "parse_ms=? WHERE id=?",
                   (errors, missing, (time.time() - t0) * 1000.0, rec.fid))

    # -- includes ----------------------------------------------------------
    def _includes(self, rec: FileRec, raw: str, bufs: Buffers) -> None:
        """`#include` becomes an import edge, resolved against the tree.

        A `<system>` include that resolves to a file in this tree is not
        external -- projects put their own headers on the include path all the
        time, and calling those external would hide the real coupling.
        """
        if not self.by_basename:
            for rel, fid in self.file_id.items():
                self.by_basename.setdefault(os.path.basename(rel), fid)
        here = os.path.dirname(rec.rel)
        for m in INCLUDE.finditer(raw):
            tgt = m.group(2)
            is_sys = 1 if m.group(1) == "<" else 0
            cand = os.path.normpath(os.path.join(here, tgt))
            tid = (self.file_id.get(cand) or self.file_id.get(tgt)
                   or self.by_basename.get(os.path.basename(tgt)))
            bufs.imports.append(
                (rec.fid, tgt, tid, None, "include", line_of(raw, m.start()),
                 0 if tid else 1, 0 if is_sys else 1, 0, 0, 0, 1))

    # -- preprocessor ------------------------------------------------------
    def _config_blocks(self, rec: FileRec, raw: str,
                       db: sqlite3.Connection) -> None:
        rows = []
        for cb in IFDEF_RE.finditer(raw):
            expr = cb.group(2).strip()[:160]
            rows.append((rec.fid, cb.group(1), expr, line_of(raw, cb.start()),
                         1 if ("CONFIG_" in expr or "HAVE_" in expr
                               or "USE_" in expr) else 0))
        if rows:
            db.executemany(
                "INSERT INTO config_blocks(file_id,directive,expr,line,is_config)"
                " VALUES(?,?,?,?,?)", rows)

    def _macros(self, rec: FileRec, raw: str, db: sqlite3.Connection,
                bufs: Buffers) -> None:
        for m in DEFINE.finditer(raw):
            name, args, body = m.group(1), m.group(2), (m.group(3) or "")
            ln = line_of(raw, m.start())
            sid = self._new_symbol(
                db,
                ["file_id", "module_id", "name", "qual_name", "kind",
                 "line_start", "line_end", "n_lines", "signature",
                 "visibility"],
                [rec.fid, rec.mid, name, name, "macro", ln, ln, 1,
                 m.group(0)[:200], ""])
            bufs.rows("macros").append(
                (sid, 1 if args else 0,
                 len([a for a in (args or "()")[1:-1].split(",") if a.strip()]),
                 body[:500], len(body),
                 1 if body.rstrip().endswith("\\") else 0))
            self.macro_sid.setdefault(name, sid)

    def _typedefs(self, rec: FileRec, raw: str, db: sqlite3.Connection) -> None:
        for m in TYPEDEF_SIMPLE.finditer(raw):
            if "(" in m.group(1):
                continue
            ln = line_of(raw, m.start())
            self._new_symbol(
                db,
                ["file_id", "module_id", "name", "qual_name", "kind",
                 "line_start", "line_end", "n_lines", "return_type",
                 "is_public"],
                [rec.fid, rec.mid, m.group(2), m.group(2), "typedef", ln, ln,
                 1, re.sub(r'\\s+', ' ', m.group(1)).strip()[:120], 1])

    # -- aggregates: struct / union / enum ---------------------------------
    def _tags(self, rec: FileRec, raw: str, blank: str,
              db: sqlite3.Connection, bufs: Buffers) -> None:
        for m in TAG_BODY.finditer(blank):
            kind, tag = m.group(1), m.group(2)
            ob = blank.index("{", m.start())
            flds, endline = struct_fields(blank, raw, ob)
            name = tag
            if not name:
                close = blank.find("}", ob)
                mm = re.search(r'}\s*(\w+)\s*;', raw[close:close + 120])
                name = mm.group(1) if mm else "(anon@%d)" % line_of(raw, m.start())
            ln = line_of(raw, m.start())
            doc, ndoc = has_leading_comment(rec.text.splitlines(), ln)
            sid = self._new_symbol(
                db,
                ["file_id", "module_id", "name", "qual_name", "kind",
                 "line_start", "line_end", "n_lines", "is_public",
                 "has_doc", "n_doc_lines"],
                [rec.fid, rec.mid, name, name, kind, ln, endline,
                 endline - ln + 1, 1, doc, ndoc])

            if kind in ("struct", "union"):
                for f in flds:
                    ordinal, ftype, fname, ptr, alen, isfn, depth, inu, fline = f
                    bufs.fields.append(
                        (sid, ordinal, fname, ftype, "", fline, 0,
                         1 if "const" in ftype else 0,
                         0 if "const" in ftype else 1,
                         1 if ptr else 0, 1 if alen else 0, 0, 0, ptr))
                lay, tot, tail, ex, mal = layout_struct(flds, kind == "union")
                for row, f in zip(lay, flds):
                    bufs.rows("layout").append(
                        (sid,) + row + (f[3], f[4], f[5], f[6], f[7]))
                tpad = sum(r[3] for r in lay) + tail
                bufs.rows("struct_size").append(
                    (sid, tot, tail, tpad, mal, ex, (tot + 63) // 64 if tot else 0))
            elif kind == "enum":
                close = blank.find("}", ob)
                inner = raw[ob + 1:close] if close > ob else ""
                for i2, part in enumerate(inner.split(",")):
                    em = re.match(r'\s*([A-Za-z_]\w*)\s*(?:=\s*([^,]+))?\s*', part)
                    if em:
                        bufs.enum_members.append(
                            (sid, i2, em.group(1)[:80],
                             (em.group(2) or "").strip()[:60] or None, 0))

    # -- prototypes and globals -------------------------------------------
    def _prototypes(self, rec: FileRec, blank: str, in_function,
                    bufs: Buffers) -> None:
        """Names declared here but possibly defined outside the tree.

        A call to one of these is a boundary, not a blind spot: the definition
        exists, it is simply in a library or a directory this run skipped.
        Each declaration is also recorded (extern-symbol-asymmetry asks which
        of them has NO definition anywhere in the tree).
        """
        for m in PROTO_RE.finditer(blank):
            if in_function(line_of(blank, m.start())):
                continue
            nm = m.group(1)
            if nm not in KEYWORDS:
                self.declared.add(nm)
                bufs.rows("declarations").append(
                    (rec.fid, nm, line_of(blank, m.start())))

    def _globals(self, rec: FileRec, blank: str, db: sqlite3.Connection,
                 in_function, depths) -> None:
        rows = []
        for m in GLOBAL_RE.finditer(blank):
            ln = line_of(blank, m.start())
            if in_function(ln) or depths[ln] != 0:
                continue
            storage, ctype, stars, gname, arr, init = m.groups()
            if gname in KEYWORDS or ctype.strip() in (
                    "return", "typedef", "case", "goto", "else", "do"):
                continue
            rows.append((
                rec.fid, rec.mid, gname, re.sub(r'\s+', ' ', ctype).strip()[:120],
                ln,
                1 if (storage or "").strip() == "static" else 0,
                1 if "const" in ctype else 0,
                1 if "volatile" in ctype else 0,
                1 if ("_Atomic" in ctype or "atomic_" in ctype) else 0,
                1 if arr else 0, len(stars or ""), 1 if init else 0))
        if rows:
            db.executemany(
                "INSERT INTO globals(file_id,module_id,name,type,line,is_static,"
                "is_const,is_volatile,is_atomic,is_array,ptr_depth,has_init) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", rows)

    # -- functions ---------------------------------------------------------
    def _functions(self, rec: FileRec, raw: str, blank: str,
                   funcs: list[tuple[Any, ...]], db: sqlite3.Connection,
                   bufs: Buffers) -> None:
        raw_lines = raw.splitlines()
        for name, sig, ls, le, st, inl, body, boff in funcs:
            mt = metrics(body)
            la = loop_analysis(body)
            ec = expr_counts(body)
            rs = return_shapes(body)
            ps = split_params(sig)
            raw_body = raw[boff:boff + len(body)]
            n_cmt = sum(1 for l in raw_body.splitlines()
                        if l.strip().startswith(("/*", "//", "*")))
            doc, ndoc = has_leading_comment(raw_lines, ls)

            locs: list[tuple[Any, ...]] = []
            for lm in LOCAL_RE.finditer(body):
                stor, lty, lstars, lname = (lm.group(1), lm.group(2),
                                            lm.group(3), lm.group(4))
                if lname in KEYWORDS or lty in ("return", "case", "goto", "else"):
                    continue
                ptr = len(lstars or "")
                ltype = (stor + lty + (" " + lstars if lstars else "")).strip()
                locs.append((len(locs), lname[:80], ltype[:120],
                             line_of(body, lm.start()) + ls - 1,
                             1 if "const" in stor else 0,
                             0 if "const" in stor else 1, 0,
                             1 if lm.group(6) else 0, 0, 0, ptr))
            n_ptr_locals = sum(1 for l in locs if l[10] > 0)

            mags = [m for m in NUM_RE.finditer(body)
                    if not _is_ok_magic(m.group(1))]
            calls, n_calls, fnptr = self._scan_calls(body, boff, raw)

            # -- P2 pack ----------------------------------------------------
            # const-cast-away (CERT EXP05-C): a `(T*)` cast applied to a
            # const-declared local or const parameter drops the const.
            const_names = {l[1] for l in locs if l[4]}
            const_names |= {p[2] for p in ps if p[2] and p[5]}
            n_const_cast = 0
            for cm in CAST_RE.finditer(body):
                if cm.group(1) in const_names:
                    n_const_cast += 1
            # toctou-access-open: access(X) and open(X) in the same function
            # with a shared variable name -- the check and the use race.
            acc_vars = {m.group(1) for m in TOCTOU_ACCESS_RE.finditer(body)}
            opn_vars = {m.group(1) for m in TOCTOU_OPEN_RE.finditer(body)}
            n_toctou = len(acc_vars & opn_vars)
            # addr_taken: `&fn` in this body -- a use the call graph cannot
            # see; fnptr-blindspot-callers joins these against fan_in=0.
            # sid is assigned below; buffer the pairs until it exists.
            addr_rows = [(am.group(1), line_of(body, am.start()) + ls - 1)
                         for am in ADDR_TAKEN_RE.finditer(body)]

            m: dict[str, Any] = {}
            m.update(mt)
            m.update({k: v for k, v in la.items()})
            m.update(ec)
            m.update(rs)
            m["n_string_lit"] = len(STRING_LIT.findall(raw_body))
            m["n_locals"] = len(locs)
            m["n_ptr_locals"] = n_ptr_locals
            m["n_magic"] = len(mags)
            m["n_comment_lines"] = n_cmt
            m["has_doc"] = doc
            m["n_doc_lines"] = ndoc
            m["body_bytes"] = len(body)
            m["n_params"] = len(ps)
            m["n_ptr_params"] = sum(1 for p in ps if p[3] > 0)
            m["is_variadic"] = 1 if any(p[6] for p in ps) else 0
            m["is_static"] = st
            m["is_inline"] = inl
            m["is_public"] = 0 if st else 1
            m["is_test"] = 1 if rec.is_test else 0
            m["is_generated"] = 1 if rec.is_generated else 0
            m["is_entrypoint"] = 1 if name in ("main", "LLVMFuzzerTestOneInput") else 0
            m["n_calls"] = n_calls
            m["n_fnptr_calls"] = fnptr
            m["n_dynamic_calls"] = fnptr
            m["n_const_cast"] = n_const_cast
            m["n_toctou"] = n_toctou

            qual = ("%s:%s" % (rec.rel, name)) if st else name
            sid = self._insert_symbol(
                db, rec, name, "function", ls, le, qual, sig,
                return_type_of(sig, name), "static" if st else "extern", m)

            for tname, tline in addr_rows:
                bufs.rows("addr_taken").append(
                    (sid, rec.fid, tname, tline))

            for l in locs:
                bufs.locals.append((sid,) + l[:10])
            for m2 in mags[:200]:
                v = m2.group(1)
                bufs.literals.append(
                    (sid, rec.fid, "hex" if v[:2].lower() == "0x" else "int",
                     v[:40], line_of(body, m2.start()) + ls - 1, 1))
            for sm in STRING_LIT.finditer(raw_body):
                sl = sm.group(0)
                if len(sl) >= SECRET_MIN_LEN and " " not in sl \
                        and SECRET_RE.search(sl):
                    # G07: credential-shaped literal -- candidate, not verdict
                    bufs.rows("secret_candidates").append(
                        (sid, rec.fid, sl[:200],
                         line_of(body, sm.start()) + ls - 1))
            for am in ATTR_RE.finditer(sig):
                bufs.attributes.append((sid, rec.fid, am.group(1)[:80], None, ls))
            for pos, ptype, pname, stars, is_arr, is_const, is_var in ps:
                bufs.params.append(
                    (sid, pos, pname or None, ptype, None, 0, is_var,
                     1 if stars else 0, 0 if is_const else 1,
                     1 if stars else 0, 0, 0, stars))

            self._hazards(sid, body, calls, bufs)
            for nm, call_lines in calls.items():
                self.pend_sid.append(sid)
                self.pend_fid.append(rec.fid)
                self.pend_mid.append(rec.mid)
                self.pend_cnt.append(len(call_lines))
                self.pend_name.append(sys.intern(nm))
                self.pend_lines.append(call_lines)
            self.fn_by_name.setdefault(name, []).append((sid, rec.fid, rec.mid))

    def _scan_calls(self, body: str, boff: int,
                    raw: str) -> tuple[dict[str, list[int]], int, int]:
        """Every `ident(` in a body, with the line of EVERY occurrence.

        This used to keep only the first line per callee, which cost 35% of a
        real C repo's call sites and made `n_callsites` a synonym for `fan_in`.
        The counts were always right; it was the positions that were dropped.

        An identifier reached through `->` or `.` is deliberately NOT recorded
        as a call to a global of that name. `state->handler(x)` is an indirect
        call through a struct member; pretending it is a call to some unrelated
        top-level `handler` invents an edge and, worse, hides the indirection
        the reader most needs to know about.
        """
        calls: dict[str, list[int]] = {}
        n_calls = 0
        n = len(body)
        for mm in IDENT.finditer(body):
            tok = mm.group(0)
            e = mm.end()
            while e < n and body[e] in " \t\r\n":
                e += 1
            if e >= n or body[e] != "(":
                continue
            if tok in KEYWORDS:
                continue
            s = mm.start()
            j = s - 1
            while j >= 0 and body[j] in " \t\r\n":
                j -= 1
            if j >= 0 and (body[j] == "." or (j >= 1 and body[j] == ">"
                                              and body[j - 1] == "-")):
                continue                      # member call: counted as indirect
            n_calls += 1
            calls.setdefault(tok, []).append(line_of(raw, boff + s))
        fnptr = (len(FNPTR_CALL_RE.findall(body))
                 + len(MEMBER_CALL_RE.findall(body)))
        return calls, n_calls + fnptr, fnptr

    def _hazards(self, sid: int, body: str,
                 calls: dict[str, list[int]], bufs: Buffers) -> None:
        for tok, lines in calls.items():
            cnt, line = len(lines), lines[0]
            cat = HAZARD_FUNCS.get(tok)
            if cat:
                bufs.add_hazard(sid, tok, cat, cnt, line)
        for pat, cat, rx in HAZARD_RE:
            c = len(rx.findall(body))
            if c:
                bufs.add_hazard(sid, pat, cat, c, 0)

    # -- insertion ---------------------------------------------------------
    def _new_symbol(self, db: sqlite3.Connection, cols: list[str],
                    vals: list[Any]) -> int:
        """Buffer one symbol; ids come from a counter, not `lastrowid`.

        `id INTEGER PRIMARY KEY` accepts an explicit value, and a counter from
        1 hands out exactly the rowids SQLite would have assigned -- which is
        what makes `executemany` possible at all (Python's sqlite3 refuses
        `executemany` with `RETURNING`). Rows are bucketed by their column
        shape (four shapes exist: function, macro, typedef, tag) and flushed
        per bucket, so every INSERT in a batch is the same width -- the byte
        identity of the graph is preserved because absent metric columns take
        the DDL default either way.
        """
        self._n_sym += 1
        sid = self._n_sym
        key = tuple(cols)
        rows = self._sym_buckets.setdefault(key, [])
        rows.append((sid,) + tuple(vals))
        if len(rows) >= SYMBOL_BATCH:
            self._flush_sym_bucket(db, key)
        return sid

    def _flush_sym_bucket(self, db: sqlite3.Connection,
                          key: tuple[str, ...]) -> None:
        rows = self._sym_buckets.get(key)
        if not rows:
            return
        sql = self._sym_sqls.get(key)
        if sql is None:
            sql = self._sym_sqls[key] = (
                "INSERT INTO symbols(id,%s) VALUES(%s)"
                % (",".join(key), ",".join("?" * (1 + len(key)))))
        db.executemany(sql, rows)
        self._sym_buckets[key] = []

    def _insert_symbol(self, db: sqlite3.Connection, rec: FileRec, name: str,
                       kind: str, line_start: int, line_end: int, qual: str,
                       signature: str, return_type: str, visibility: str,
                       m: dict[str, Any]) -> int:
        """Buffer a function symbol (fixed column order, so one batch shape).

        The metric columns are those the schema carries (`_SYMBOL_COLS`), in a
        fixed order, defaulting absent metrics to 0 -- byte-identical to the
        old variable-width per-row INSERT because every metric column is
        `INT NOT NULL DEFAULT 0`.
        """
        cols = ["file_id", "module_id", "name", "qual_name", "kind",
                "line_start", "line_end", "n_lines", "signature",
                "return_type", "visibility"]
        metric_cols = sorted(c for c in _SYMBOL_COLS
                             if c not in cols and c != "id")
        full_cols = cols + metric_cols
        vals: list[Any] = [rec.fid, rec.mid, name, qual[:400], kind,
                           line_start, line_end, line_end - line_start + 1,
                           signature[:400], return_type[:200], visibility]
        vals += [int(m.get(c, 0)) for c in metric_cols]
        return self._new_symbol(db, full_cols, vals)

    def flush_symbols(self, db: sqlite3.Connection) -> None:
        """Write every buffered symbol row once, after the parse loop.

        Called unconditionally by `build()` before anything counts the table;
        a parser must never leave a buffered row unwritten or `n_syms` reads 0.
        """
        for key in list(self._sym_buckets):
            self._flush_sym_bucket(db, key)

    # -- call resolution ---------------------------------------------------
    def resolve_calls(self, db: sqlite3.Connection, bufs: Buffers) -> None:
        """Four buckets, and every call site lands in exactly one.

          1. a function defined in this tree            -> edge
          2. a macro defined in this tree               -> n_macro_calls
          3. libc / POSIX / builtin / intrinsic, or a
             prototype declared here but defined outside -> n_external_calls
          4. anything left                              -> unresolved_calls

        Bucket 3 is the one the original file did not have, and without it a
        normal C file reads as almost totally blind -- every `memcpy` and
        `printf` counted against the graph. Blindness that includes the entire
        standard library is not a measurement, it is noise.

        Bucket 1 prefers a definition in the SAME FILE. C has file-scope
        `static` linkage, so two files can each define `getSize` and mean
        different functions; the same-file candidate is the one the compiler
        would pick.
        """
        by_name = self.fn_by_name
        n_res = n_macro = n_extern = n_decl = n_unres = 0
        macro_uses: dict[int, int] = {}
        ext: dict[int, int] = {}
        decl: dict[int, int] = {}
        mac: dict[int, int] = {}
        alloc_hz: list[tuple[int, str, str, int, int]] = []

        for caller_sid, fid, mid, name, call_lines, cnt in zip(
                self.pend_sid, self.pend_fid, self.pend_mid,
                self.pend_name, self.pend_lines, self.pend_cnt):
            line = call_lines[0]
            cands = by_name.get(name)
            if cands:
                target = cands[0]
                if len(cands) > 1:
                    for t in cands:
                        if t[1] == fid:
                            target = t
                            break
                # One `add_edge` per LINE: the edge's n_calls comes out
                # right and `callsites` gets a row for each site, which is
                # what it means in every other analyzer here.
                for site in call_lines:
                    bufs.add_edge(caller_sid, target[0], target[1] == fid,
                                  target[2] == mid, site)
                n_res += cnt
                if ALLOC_NAME_RE.search(name):
                    # A project allocator wrapper. Counting it as allocation is
                    # what makes ownership and per-iteration-alloc queries work
                    # on a codebase that never calls malloc directly.
                    alloc_hz.append((caller_sid, name[:80], "alloc", cnt, line))
                continue
            msid = self.macro_sid.get(name)
            if msid is not None:
                mac[caller_sid] = mac.get(caller_sid, 0) + cnt
                macro_uses[msid] = macro_uses.get(msid, 0) + cnt
                n_macro += cnt
                continue
            if self._is_external(name):
                ext[caller_sid] = ext.get(caller_sid, 0) + cnt
                n_extern += cnt
                continue
            if name in self.declared:
                ext[caller_sid] = ext.get(caller_sid, 0) + cnt
                decl[caller_sid] = decl.get(caller_sid, 0) + cnt
                n_extern += cnt
                n_decl += cnt
                continue
            bufs.add_unresolved(caller_sid, name[:160], line)
            if cnt > 1:
                bufs.unresolved[(caller_sid, name[:160])][0] += cnt - 1
            n_unres += cnt

        for sid, pat, cat, n, ln in alloc_hz:
            bufs.add_hazard(sid, pat, cat, n, ln)
        # The buffered macros rows must hit the table before the n_uses
        # UPDATE below, or the UPDATE matches nothing and every macro shows 0.
        _macros_rows = bufs.rows("macros")
        if _macros_rows:
            db.executemany(
                "INSERT INTO macros(symbol_id,is_functionlike,n_params,body,"
                "body_len,is_multiline) VALUES(?,?,?,?,?,?)",
                list(_macros_rows))
            _macros_rows.clear()
        for col, data in (("n_external_calls", ext), ("n_macro_calls", mac),
                          ("n_extern_decl_calls", decl)):
            if data:
                db.executemany("UPDATE symbols SET %s=? WHERE id=?" % col,
                               [(v, k) for k, v in data.items()])
        if macro_uses:
            db.executemany("UPDATE macros SET n_uses=? WHERE symbol_id=?",
                           [(v, k) for k, v in macro_uses.items()])

        self.n_edges, self.n_macro = n_res, n_macro
        self.n_extern, self.n_unres = n_extern, n_unres
        total = n_res + n_macro + n_extern + n_unres
        db.execute(
            "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
            ("calls_resolved",
             "%d in-tree / %d via macro / %d external (%d of them "
             "declared-here-defined-elsewhere) / %d unresolved -- "
             "%d%% of %d call sites accounted for"
             % (n_res, n_macro, n_extern, n_decl, n_unres,
                100 * (total - n_unres) // max(1, total), total)))
        self.flush_extra(db, bufs)

    def _is_external(self, name: str) -> bool:
        """True where a call leaves the tree by design rather than by defeat."""
        if name in LIBC_KNOWN:
            return True
        if name.startswith(BUILTIN_PREFIXES):
            return True
        return bool(INTRIN_NAME_RE.match(name))

    def flush_extra(self, db: sqlite3.Connection, bufs: Buffers) -> None:
        """One `executemany` per language-specific table."""
        for tbl, sql in (
            ("layout",
             "INSERT OR IGNORE INTO layout(symbol_id,ordinal,byte_off,byte_size,"
             "pad_before,exact,ptr_depth,array_len,is_fnptr,depth,in_union) "
             "VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
            ("struct_size",
             "INSERT OR IGNORE INTO struct_size(symbol_id,total_size,tail_pad,"
             "total_pad,max_align,exact,n_lines_64) VALUES(?,?,?,?,?,?,?)"),
            ("declarations",
             "INSERT INTO declarations(file_id,name,line) VALUES(?,?,?)"),
            ("addr_taken",
             "INSERT INTO addr_taken(symbol_id,file_id,name,line) "
             "VALUES(?,?,?,?)"),
            ("secret_candidates",
             "INSERT INTO secret_candidates(symbol_id,file_id,value,line) "
             "VALUES(?,?,?,?)"),
        ):
            rows = bufs.extra.get(tbl)
            if rows:
                db.executemany(sql, rows)

    # -- build files -------------------------------------------------------
    def parse_manifests(self, root: str, db: sqlite3.Connection) -> None:
        """Link rules that enumerate their objects by hand.

        These are the rules a new call from a shared source silently breaks:
        the compile succeeds, the link fails, and the error names a symbol
        nobody edited.
        """
        rows: list[tuple[Any, ...]] = []
        seen = 0
        for base in (root,) + tuple(
                os.path.join(root, d) for d in sorted(os.listdir(root))[:64]
                if os.path.isdir(os.path.join(root, d))):
            for nm in ("Makefile", "makefile", "GNUmakefile"):
                p = os.path.join(base, nm)
                if not os.path.isfile(p):
                    continue
                seen += 1
                rel = os.path.relpath(p, root)
                cont, start = "", 0
                try:
                    fh = open(p, errors="replace")
                except OSError:
                    continue
                with fh:
                    for ln_no, ln in enumerate(fh, 1):
                        ln = ln.rstrip('\n')
                        if cont:
                            ln, cont = cont + " " + ln.lstrip(), ""
                        if ln.endswith("\\"):
                            cont, start = ln[:-1], start or ln_no
                            continue
                        mm = MAKE_RULE_RE.match(ln)
                        if mm:
                            pre = ln.split(":", 1)[1]
                            rows.append((rel, mm.group(1), start or ln_no,
                                         len(MAKE_OBJ_RE.findall(pre)),
                                         len(MAKE_SRC_RE.findall(pre)),
                                         1 if ".a" in pre else 0))
                            start = 0
                            continue
                        vm = MAKE_VAR_RE.match(ln)
                        if vm:
                            val = vm.group(2)
                            objs = len(MAKE_OBJ_RE.findall(val))
                            srcs = len(MAKE_SRC_RE.findall(val))
                            if objs or srcs:
                                rows.append((rel, vm.group(1) + " =",
                                             start or ln_no, objs, srcs,
                                             1 if ".a" in val else 0))
                        start = 0
                break
        if rows:
            db.executemany(
                "INSERT INTO makefile_rules(path,rule,line,n_objs,n_srcs,uses_ar)"
                " VALUES(?,?,?,?,?,?)", rows)
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                   ("makefiles_read", str(seen)))

    def post_build(self, db: sqlite3.Connection,
                   root: "str | None" = None) -> None:
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                   ("grammar_note", GRAMMAR_NOTE))
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                   ("layout_model", "LP64: pointer 8/8, long 8, int 4; "
                                    "sizes reported only where exact=1"))
        self._transitive_fan(db)
        _dynajs_project_pass(db, root)      # DYN PROJECT

    def _transitive_fan(self, db: sqlite3.Connection) -> None:
        """Count distinct callers that can reach each symbol through resolved
        edges, at any depth.

        The call graph is a DAG with cycles, so a SQL walk over edges would
        re-expand every path; a traversal of the REVERSE adjacency in Python
        is O(V+E) per node and exact on the resolved edges: each symbol gets
        the set of distinct callers (fan-in) that can reach it. Results feed
        `blast-radius`, whose answer is the transitive CALLER set.
        """
        edges = db.execute(
            "SELECT caller_id, callee_id FROM edges").fetchall()
        if not edges:
            return
        callers: dict[int, list[int]] = {}
        symbols = {c for c, _ in edges} | {k for _, k in edges}
        for caller, callee in edges:
            callers.setdefault(callee, []).append(caller)
        rows: list[tuple[int, int]] = []
        for sym in symbols:
            seen: set[int] = set()
            stack = [sym]
            while stack:
                cur = stack.pop()
                for nxt in callers.get(cur, ()):
                    if nxt not in seen and nxt != sym:
                        seen.add(nxt)
                        stack.append(nxt)
            if seen:
                rows.append((sym, len(seen)))
        if rows:
            db.executemany(
                "INSERT OR REPLACE INTO reach(symbol_id, n_transitive) "
                "VALUES(?,?)", rows)

def _is_ok_magic(tok: str) -> bool:
    """Integers nobody should be asked to name."""
    t = tok.rstrip("uUlL")
    try:
        return int(t, 0) in MAGIC_OK
    except ValueError:
        return False

# ==========================================================================
# DYN PROJECT EXTENSION (dynajs repo only; not upstream)
#
# Records the NAMES of dynajs-runtime API calls per function (the brace
# scanner counts them only as extern/unresolved buckets). One re-read of each
# parsed file, scoped to each function's byte range; byte offsets are into
# the raw file bytes, which the scanner preserves exactly.
#
# If the upstream codegraph_c.py moves ahead, re-copy it and re-apply:
#   1. the src/-only shim in main()
#   2. this block
#   3. the dyn_api_calls CREATE TABLE in SCHEMA_EXT
#   4. the _dynajs_project_pass(db) call in CAnalyzer.post_build
#   5. DYN_QUERIES/DYN_METRICS and the CAnalyzer assignments at the tail
# ==========================================================================

_DYN_API_CALL_RE = re.compile(r'\b(JS_[A-Za-z0-9_]*|js_[a-z0-9_]+)\s*\(')
_DYN_API_SKIP_RE = re.compile(r'^JS_VALUE_GET_')   # macros, not API calls

def _dynajs_project_pass(db: sqlite3.Connection,
                         root: "str | None" = None) -> None:
    """Populate dyn_api_calls from raw source text per function line range.

    The C analyzer records line_start/line_end but not byte offsets, so the
    pass locates each function's span by counting newlines in the raw file
    bytes and slices that range. files.path is relative to the scan root
    (src/), so the root is joined here."""
    import bisect
    root = root or "."
    files = db.execute(
        "SELECT id, path FROM files WHERE parsed=1 AND is_test=0").fetchall()
    syms = db.execute(
        "SELECT id, file_id, line_start, line_end FROM symbols "
        "WHERE kind='function' AND line_end >= line_start").fetchall()
    by_file: dict[int, list[tuple[int, int, int]]] = {}
    for sid, fid, ls, le in syms:
        by_file.setdefault(fid, []).append((sid, ls, le))
    if not by_file:
        db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                   ("dyn_api_pass", "no function symbols"))
        return
    n_names = n_read = n_miss = 0
    for fid, path in files:
        blocks = by_file.get(fid)
        if not blocks:
            continue
        try:
            with open(os.path.join(root, path), "rb") as fh:
                data = fh.read()
        except OSError:
            n_miss += 1
            continue
        n_read += 1
        newlines = [i for i, b in enumerate(data) if b == 0x0A]
        for sid, ls, le in blocks:
            bs = newlines[ls - 1] + 1 if 0 < ls - 1 < len(newlines) else 0
            be = newlines[le - 1] + 1 if 0 < le - 1 < len(newlines) else len(data)
            if be <= bs or be > len(data):
                continue
            counts: dict[str, int] = {}
            chunk = data[bs:be].decode("latin-1")   # byte-exact, offsets hold
            for m in _DYN_API_CALL_RE.finditer(chunk):
                name = m.group(1)
                if _DYN_API_SKIP_RE.match(name):
                    continue
                counts[name] = counts.get(name, 0) + 1
            for name, n in counts.items():
                first = bs + chunk.find(name + "(")
                line = bisect.bisect_left(newlines, first) + 1
                db.execute(
                    "INSERT OR REPLACE INTO dyn_api_calls(caller_id,name,n,"
                    "first_line) VALUES(?,?,?,?)",
                    (sid, name, n, line))
                n_names += 1
    db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
               ("dyn_api_pass",
                "%d files scanned, %d api names recorded, %d files unreadable"
                % (n_read, n_names, n_miss)))

# ==========================================================================
# END DYN PROJECT EXTENSION
# ==========================================================================

QUERIES: list[tuple[str, str, str, str]] = [
(
    "untrusted-frontier",
    "Parses attacker bytes AND does pointer/size arithmetic",
    "ANSWERS the functions where a memory-safety bug is actually reachable.\n"
    "ACT this is the CWE-190 shape: a length from the wire, a shift or a cast,\n"
    "     then a copy. Review these before anything else on the risk list.\n"
    "MISLEADS io counts RAW descriptor calls only; a function fed by a caller\n"
    "     that did the read is just as exposed and is invisible here.",
    """SELECT s.name, s.n_io AS io, s.n_memory AS mem, s.n_integer AS int_,
        s.n_alloc AS alloc, s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_io>0 AND (s.n_memory>0 OR s.n_integer>0)
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_memory + s.n_integer) DESC, s.n_io DESC LIMIT :lim"""),
(
    "stack-exhaustion",
    "Self-recursive functions: unbounded input depth is a stack DoS",
    "ANSWERS where a deeply nested input can exhaust the C stack.\n"
    "ACT every one of these needs a depth cap that is TESTED at the cap.\n"
    "MISLEADS DIRECT self-recursion only. Mutual recursion through two\n"
    "     functions, or through a function pointer, does not appear here.",
    """SELECT s.name, s.cyclomatic AS cyclo, s.max_nesting AS nest,
        s.n_io AS io, s.n_locals AS locals, s.fan_in, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.is_recursive=1 AND s.kind='function'
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.cyclomatic DESC LIMIT :lim"""),
(
    "ownership-review",
    "Allocates but never frees in the same function",
    "ANSWERS where allocation ownership crosses a function boundary.\n"
    "ACT each row must have a NAMED owner that frees it on EVERY path --\n"
    "     including the early returns and the goto-out ladder.\n"
    "MISLEADS this is a REVIEW list, not a leak list. Transferring ownership\n"
    "     to the caller is the normal C idiom and looks identical here.",
    """SELECT s.name, s.n_alloc AS allocs, s.n_free AS frees, s.fan_in,
        s.n_returns AS returns_, s.n_gotos AS gotos, s.n_labels AS labels,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_alloc>0 AND s.n_free=0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_alloc DESC, s.fan_in DESC LIMIT :lim"""),
(
    "allocator-mixing",
    "Files that use libc malloc AND a project allocator",
    "ANSWERS which files split their memory across two accounting systems.\n"
    "ACT pick ONE per file. A project allocator usually exists to track or cap\n"
    "     usage, and a libc allocation in the same file is invisible to it.\n"
    "MISLEADS a project allocator is recognised by NAME SHAPE (anything ending\n"
    "     in alloc/free/strdup), so an unconventionally named one is missed and\n"
    "     an unrelated `list_free` is counted. Also, mixing is sometimes right:\n"
    "     tracked buffers through the wrapper, private scratch through libc.",
    """SELECT f.path,
        SUM(CASE WHEN h.pattern IN ('malloc','calloc','realloc','free',
            'aligned_alloc','posix_memalign','memalign','valloc',
            'reallocarray','strdup','strndup') THEN h.n ELSE 0 END) AS libc,
        SUM(CASE WHEN h.pattern NOT IN ('malloc','calloc','realloc','free',
            'aligned_alloc','posix_memalign','memalign','valloc',
            'reallocarray','strdup','strndup') THEN h.n ELSE 0 END) AS project,
        COUNT(DISTINCT h.pattern) AS distinct_fns,
        GROUP_CONCAT(DISTINCT h.pattern) AS via
    FROM hazards h JOIN symbols s ON s.id=h.symbol_id
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE h.category='alloc' AND COALESCE(m.name,'') LIKE :mod
    GROUP BY f.path HAVING libc>0 AND project>0
    ORDER BY (libc+project) DESC LIMIT :lim"""),
(
    "alloc-per-iteration",
    "malloc/realloc inside a loop body",
    "ANSWERS which loops pay the allocator once per item.\n"
    "ACT hoist the allocation, or reserve capacity before the loop.\n"
    "MISLEADS a realloc-grow loop is amortised O(1) and belongs here anyway;\n"
    "     the count is of SITES, and one site in a hot loop beats ten cold ones.",
    """SELECT s.name, s.alloc_in_loop AS allocs, s.max_loop_depth AS depth,
        s.n_free AS frees, s.call_in_loop AS calls, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.alloc_in_loop>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.alloc_in_loop DESC, s.max_loop_depth DESC LIMIT :lim"""),
(
    "bypass-tax",
    "Allocates BEFORE it knows the fast path applies",
    "ANSWERS which functions pay setup cost on inputs they then refuse.\n"
    "ACT probe FIRST, allocate second. This is the classic bypass candidate.\n"
    "MISLEADS an allocation before a loop is usually just the output buffer,\n"
    "     which has to be allocated up front and is not a tax at all.",
    """SELECT s.name, s.n_alloc AS allocs, s.n_free AS frees,
        s.n_loops AS loops, s.n_branches AS brs, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.n_alloc>0 AND s.n_free>0
    AND s.n_loops>0 AND s.sloc<=120 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_loops*3 + s.n_branches + s.n_alloc*2) DESC,
        s.sloc ASC LIMIT :lim"""),
(
    "race-surface",
    "Mutable, non-atomic, non-const file-scope state",
    "ANSWERS what two threads could be writing at the same time.\n"
    "ACT every row needs a lock, an atomic, a thread-local, or a written proof\n"
    "     that only one thread ever touches it.\n"
    "MISLEADS it does not know which modules are threaded. The last column is\n"
    "     the only evidence offered: how many functions in the same module use\n"
    "     a concurrency primitive at all.",
    """WITH mod_conc AS (
        SELECT module_id AS mid, COUNT(*) AS n
        FROM symbols WHERE n_concurrency > 0
        GROUP BY module_id),
    surf AS (
        SELECT g.id, g.name, g.type, g.ptr_depth, g.is_array, g.is_static,
            g.has_init, g.module_id, g.line, g.file_id,
            COALESCE(mc.n, 0) AS mod_conc_fns
        FROM globals g LEFT JOIN mod_conc mc ON mc.mid = g.module_id
        WHERE g.is_const = 0 AND g.is_atomic = 0 AND g.is_volatile = 0)
    SELECT s.name AS name_, s.type AS type_, s.ptr_depth AS ptr,
        s.is_array AS arr, s.is_static AS stat, s.has_init AS init,
        m.name AS module, f.path || ':' || s.line AS at, s.mod_conc_fns
    FROM surf s JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE f.is_test = 0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY mod_conc_fns DESC, ptr_depth DESC, name_ LIMIT :lim"""),
(
    "per-element-dispatch",
    "A switch INSIDE a loop: type dispatch paid once per element",
    "ANSWERS where a loop re-decides the same thing on every iteration.\n"
    "ACT hoist the switch out, or specialise the loop per case.\n"
    "MISLEADS a bytecode interpreter's dispatch loop is exactly this shape and\n"
    "     is correct by design. So is a state machine.",
    """SELECT s.name, s.switch_in_loop AS sw, s.max_loop_depth AS depth,
        s.call_in_loop AS calls, s.n_cases AS cases, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.switch_in_loop>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.switch_in_loop DESC, s.call_in_loop DESC LIMIT :lim"""),
(
    "loop-invariant-strlen",
    "strlen() inside a loop: accidental O(n^2)",
    "ANSWERS where a length is recomputed that cannot have changed.\n"
    "ACT hoist it, or carry the length beside the string.\n"
    "MISLEADS a strlen over a SHORT string is a few cycles, and a loop whose\n"
    "     body mutates the string has to recompute it.",
    """SELECT s.name, s.strlen_in_loop AS strlens, s.max_loop_depth AS depth,
        s.n_loops AS loops, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.strlen_in_loop>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.max_loop_depth DESC, s.strlen_in_loop DESC LIMIT :lim"""),
(
    "error-shape-mix",
    "Functions that report failure in more than one shape",
    "ANSWERS where a caller can plausibly check the wrong thing.\n"
    "ACT a function returning both NULL and -1 is one that callers get wrong\n"
    "     roughly half the time. Pick one convention per function and document\n"
    "     it in the header, where the caller is actually looking.\n"
    "MISLEADS returning 0 for success AND 0 as a legitimate value is the worst\n"
    "     case of all and is completely invisible here.",
    """SELECT s.name, s.ret_null AS r_null, s.ret_neg AS r_neg,
        s.ret_zero AS r_0, s.ret_val AS r_val, s.ret_void AS r_void,
        s.return_type AS ret_type, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    AND ((s.ret_null>0) + (s.ret_neg>0) + (s.ret_zero>0)) >= 2
    ORDER BY s.fan_in DESC LIMIT :lim"""),
(
    "dead-code",
    "Nothing in this tree calls these",
    "ANSWERS what might be deletable.\n"
    "ACT grep the name as a STRING before deleting anything: a registry entry,\n"
    "     a config value or a reflective call keeps a symbol alive with no edge\n"
    "     to show for it.\n"
    "MISLEADS this is the query most likely to be wrong, and `graph-blindspots`\n"
    "     measures by how much. Public symbols are excluded because a caller\n"
    "     outside this tree cannot be seen at all, so what is left is private\n"
    "     and unreferenced -- a much weaker claim than dead.",
    """SELECT s.name, s.kind, s.sloc, s.cyclomatic AS cyclo,
        s.n_external_calls AS ext_calls,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.fan_in=0 AND s.is_public=0 AND s.is_test=0
      AND s.is_entrypoint=0 AND s.is_override=0 AND s.is_abstract=0
      AND s.kind IN ('function','method','closure')
      AND s.name NOT IN ('(anonymous)','<module>')
      AND f.is_test=0 AND f.is_generated=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.sloc DESC LIMIT :lim"""),
("nonreentrant-under-threads", "a libc call with a shared static buffer, in a function that also touches threads",
    "ANSWERS CERT CON33-C and MSC24-C, which clang-tidy states as a per-call\n"
    "     rule and cppcheck barely states at all: `localtime`, `strerror`,\n"
    "     `getenv`, `basename` and friends return a pointer into one static\n"
    "     buffer. Single-threaded that is correct and cheap. In a function that\n"
    "     also creates threads or takes locks it is a data race that corrupts\n"
    "     the OTHER thread\'s result, silently, under load.\n"
    "ACT use the _r form -- localtime_r, strerror_r, getpwnam_r -- or copy the\n"
    "     result before releasing the lock. `patterns` names exactly which\n"
    "     calls, so the fix is mechanical.\n"
    "MISLEADS a function that touches threads is not necessarily called from\n"
    "     more than one, and pthread_create in main() next to a getenv() at\n"
    "     startup is fine. This sees categories in the same body, not the\n"
    "     happens-before between them.",
    """SELECT s.name, m.name AS module,
        COUNT(DISTINCT h.pattern) AS n_patterns,
        GROUP_CONCAT(DISTINCT h.pattern) AS patterns,
        s.n_reentrancy AS reentrancy_calls,
        s.n_concurrency AS concurrency_calls,
        s.n_atomic AS atomics, s.fan_in, s.cyclomatic AS cyclo,
        f.path || \':\' || s.line_start AS at
    FROM symbols s
    JOIN hazards h ON h.symbol_id = s.id AND h.category = \'reentrancy\'
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.n_concurrency > 0 AND f.is_test = 0
      AND COALESCE(m.name,\'\') LIKE :mod
    GROUP BY s.id
    ORDER BY concurrency_calls DESC, n_patterns DESC, s.fan_in DESC
    LIMIT :lim"""),
("unchecked-conversion-on-an-io-path", "atoi/strtol in a function that also reads input",
    "ANSWERS CERT ERR34-C, which flawfinder and clang-tidy report on every\n"
    "     `atoi` in the tree. `atoi(\"42\")` on a literal is fine. `atoi` on\n"
    "     bytes that just came off a socket or a file returns 0 for the string\n"
    "     \"0\" and for garbage alike, and sets no errno to distinguish them.\n"
    "     The graph supplies the missing half: whether the same function also\n"
    "     performs I/O.\n"
    "ACT use strtol with an end pointer and check both `end` and `errno`, or\n"
    "     reject the input outright. `conversions` names the exact calls;\n"
    "     `returns_value` says whether the function can even report a failure\n"
    "     to its caller, and a 0 there means the error has nowhere to go.\n"
    "MISLEADS co-occurrence in one body is not dataflow -- a function may read\n"
    "     a file and separately parse a constant. A wrapper that validates\n"
    "     before calling this one makes the row correct and harmless, and that\n"
    "     wrapper is not visible in the row.",
    """SELECT s.name, m.name AS module,
        COUNT(DISTINCT h.pattern) AS n_conversions,
        GROUP_CONCAT(DISTINCT h.pattern) AS conversions,
        SUM(h.n) AS conversion_calls,
        s.n_io AS io_calls, s.n_stdio AS stdio_calls,
        s.n_memory AS memory_calls, s.ret_val AS returns_value, s.fan_in,
        f.path || \':\' || s.line_start AS at
    FROM symbols s
    JOIN hazards h ON h.symbol_id = s.id AND h.category = \'integer\'
        AND h.pattern IN (\'atoi\',\'atol\',\'atoll\',\'atof\',\'strtol\',
            \'strtoul\',\'strtoll\',\'strtoull\',\'strtod\',\'strtof\',
            \'strtoimax\',\'strtoumax\')
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE (s.n_io > 0 OR s.n_stdio > 0) AND f.is_test = 0
      AND COALESCE(m.name,\'\') LIKE :mod
    GROUP BY s.id
    ORDER BY conversion_calls DESC, io_calls DESC, s.fan_in DESC
    LIMIT :lim"""),
(
    "buffer-overflow-surface",
    "sprintf/strcpy/strcat/gets without bounds (CERT STR31-C)",
    "ANSWERS where unbounded buffer operations are used: sprintf, strcpy,\n"
    "     strcat, gets. Each can write past the end of a buffer if the input\n"
    "     is larger than expected.\n"
    "ACT use snprintf, strncpy (or strlcpy), strncat, fgets. Check return values.\n"
    "MISLEADS a sprintf on a buffer known to be large enough is safe, but the\n"
    "     graph cannot prove the buffer size.",
    """SELECT s.name,
        COUNT(DISTINCT h.pattern) AS n_patterns,
        GROUP_CONCAT(DISTINCT h.pattern) AS patterns,
        SUM(h.n) AS dangerous_calls,
        s.n_memory AS memory_calls, s.n_io AS io_calls,
        s.fan_in, s.cyclomatic AS cyclo,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN hazards h ON h.symbol_id=s.id
        AND h.pattern IN ('sprintf','strcpy','strcat','gets','vsprintf',
            'scanf','sscanf','fscanf','vfscanf','vsscanf')
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE f.is_test=0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id
    ORDER BY dangerous_calls DESC, s.fan_in DESC LIMIT :lim"""),
(
    "format-string-injection",
    "printf with non-literal format string (CERT STR30-C)",
    "ANSWERS where printf/fprintf/sprintf is called with a format string that is\n"
    "     not a string literal, enabling format string attacks if the string\n"
    "     contains user-controlled %s or %n.\n"
    "ACT use printf('%s', user_string) not printf(user_string).\n"
    "MISLEADS a format string from a trusted constant array is safe. The graph\n"
    "     sees the call but not the argument source.",
    """SELECT s.name,
        COUNT(DISTINCT h.pattern) AS n_patterns,
        GROUP_CONCAT(DISTINCT h.pattern) AS patterns,
        SUM(h.n) AS format_calls,
        s.n_io AS io_calls, s.n_stdio AS stdio_calls,
        s.fan_in, s.cyclomatic AS cyclo,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN hazards h ON h.symbol_id=s.id
        AND h.pattern IN ('printf','fprintf','sprintf','snprintf','vprintf',
            'vfprintf','vsprintf','vsnprintf','syslog')
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE f.is_test=0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id
    ORDER BY format_calls DESC, s.fan_in DESC LIMIT :lim"""),
(
    "memory-leak-surface",
    "malloc/calloc/realloc without matching free (CERT MEM31-C)",
    "ANSWERS where a function allocates memory but has no matching free, which\n"
    "     is a leak if the allocation outlives the function and is not stored.\n"
    "ACT free the allocation before returning, or document ownership transfer.\n"
    "MISLEADS a function that allocates and returns the pointer to the caller is\n"
    "     not leaking; the caller is responsible. The graph sees alloc/free in\n"
    "     the same function, not across functions.",
    """SELECT s.name, s.n_alloc AS allocs, s.n_free AS frees,
        s.n_alloc - s.n_free AS imbalance,
        s.n_memory AS memory_ops, s.fan_in,
        s.cyclomatic AS cyclo, s.n_returns AS returns,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_alloc > 0 AND s.n_free=0 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_alloc DESC, s.fan_in DESC LIMIT :lim"""),
(
    "double-free-surface",
    "free called more than once on same pointer (CERT MEM40-C)",
    "ANSWERS where a function has more free calls than alloc calls, which may\n"
    "     indicate a double-free if the same pointer is freed on multiple paths.\n"
    "ACT track the pointer state; set to NULL after free and check before.\n"
    "MISLEADS a function that frees pointers allocated in other functions has\n"
    "     more frees than allocs by design. The imbalance is a signal, not proof.",
    """SELECT s.name, s.n_alloc AS allocs, s.n_free AS frees,
        s.n_free - s.n_alloc AS excess_frees,
        s.n_returns AS return_paths,
        s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_free > s.n_alloc AND s.n_free > 1 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY excess_frees DESC, s.fan_in DESC LIMIT :lim"""),
(
    "null-deref-surface",
    "Pointer dereference without null check (CERT ERR30-C)",
    "ANSWERS where a function dereferences pointers but has no null checks,\n"
    "     which means a NULL pointer will crash the program.\n"
    "ACT check for NULL before dereferencing, especially after malloc/calloc.\n"
    "MISLEADS a pointer known to be non-null (from a trusted source) does not\n"
    "     need a check. The graph sees n_null_check but not which pointers.",
    """SELECT s.name, s.n_memory AS memory_ops,
        s.n_null_check AS null_checks,
        s.n_alloc AS allocs,
        s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_memory > 5 AND s.n_null_check=0 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_memory DESC, s.fan_in DESC LIMIT :lim"""),
(
    "integer-overflow-surface",
    "Arithmetic without bounds checking (CERT INT32-C)",
    "ANSWERS where integer arithmetic is performed without overflow checking,\n"
    "     which is UB for signed integers and wraps for unsigned.\n"
    "ACT use checked arithmetic (__builtin_add_overflow) or validate inputs.\n"
    "MISLEADS arithmetic on known-small constants is safe. The graph counts\n"
    "     arithmetic sites but not the operand ranges.",
    """SELECT s.name, s.n_arith AS arith_ops,
        s.n_integer AS integer_ops,
        s.n_cmp AS comparisons,
        s.n_loops AS loops, s.cyclomatic AS cyclo,
        s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_arith > 20 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_arith DESC, s.fan_in DESC LIMIT :lim"""),
(
    "division-by-zero-surface",
    "Division without zero check (CERT INT33-C)",
    "ANSWERS where division is performed but the function has no comparison\n"
    "     that could be a zero-check on the divisor.\n"
    "ACT check for zero before dividing.\n"
    "MISLEADS a division by a constant is safe. The graph sees n_arith but\n"
    "     cannot distinguish division from other arithmetic.",
    """SELECT s.name, s.n_arith AS arith_ops,
        s.div_in_loop, s.n_cmp AS comparisons,
        s.n_null_check AS null_checks,
        s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_arith > 0 AND s.n_cmp < s.n_arith AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_arith DESC, s.fan_in DESC LIMIT :lim"""),
(
    "switch-no-default",
    "Switch without a default case (MISRA-C 16.4)",
    "ANSWERS where a switch statement has no default case, so unhandled values\n"
    "     fall through silently. MISRA-C requires a default in every switch.\n"
    "ACT add a default case, even if it only asserts or does nothing.\n"
    "MISLEADS a switch over an exhaustive enum may not need a default, but a\n"
    "     future value will be silently ignored.",
    """SELECT s.name, s.n_switch AS switches,
        s.n_cases AS cases,
        s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_switch > 0 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_switch DESC, s.fan_in DESC LIMIT :lim"""),
(
    "race-condition-surface",
    "Shared static variable accessed from concurrent context (CERT CON33-C)",
    "ANSWERS where a function reads/writes a static variable and also touches\n"
    "     threads, locks, or atomics, indicating potential data races.\n"
    "ACT protect the shared variable with a mutex, or use thread-local storage.\n"
    "MISLEADS a static variable set once at startup and only read afterward is\n"
    "     safe. The graph sees co-occurrence, not the happens-before relation.",
    """SELECT s.name, s.n_concurrency AS concurrency_ops,
        s.n_atomic AS atomic_ops,
        s.n_reentrancy AS reentrancy_calls,
        s.n_memory AS memory_ops,
        s.fan_in, s.cyclomatic AS cyclo,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_concurrency > 0 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_concurrency DESC, s.fan_in DESC LIMIT :lim"""),
(
    "signal-handler-unsafe",
    "Unsafe function called inside a signal handler (CERT SIG30-C)",
    "ANSWERS where a function that may be called as a signal handler calls\n"
    "     non-async-signal-safe functions (malloc, printf, syslog).\n"
    "ACT only call async-signal-safe functions inside signal handlers.\n"
    "MISLEADS relies on function name matching (contains 'sig' or 'handler'),\n"
    "     which is crude; a function registered via signal(2) at runtime with a\n"
    "     non-obvious name is not detected.",
    """SELECT s.name,
        COUNT(DISTINCT h.pattern) AS n_unsafe_patterns,
        GROUP_CONCAT(DISTINCT h.pattern) AS unsafe_patterns,
        s.n_io AS io_calls, s.n_memory AS memory_calls,
        s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN hazards h ON h.symbol_id=s.id
        AND h.pattern IN ('malloc','calloc','realloc','free','printf',
            'fprintf','sprintf','snprintf','syslog','fopen','fclose',
            'fread','fwrite','malloc','strdup')
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE (instr(lower(s.name), 'sig') > 0
           OR instr(lower(s.name), 'handler') > 0
           OR instr(lower(s.name), 'signal') > 0)
      AND f.is_test=0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id
    ORDER BY n_unsafe_patterns DESC, s.fan_in DESC LIMIT :lim"""),
(
    "infinite-loop",
    "Functions with loops but no return statements (CERT MSC53-C)",
    "ANSWERS where a function has loops but no return paths at all, which may\n"
    "     indicate an infinite loop with no exit condition.\n"
    "ACT ensure there is a break, return, or condition that exits the loop.\n"
    "MISLEADS an event loop or a scheduler is intentionally infinite. The graph\n"
    "     sees n_loops and n_returns but not the exit condition. A function that\n"
    "     exits via a called function's longjmp is not captured.",
    """SELECT s.name, s.n_loops AS loops,
        s.n_returns AS returns,
        s.cyclomatic AS cyclo, s.fan_in, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_loops > 0 AND s.n_returns = 0 AND s.fan_in > 0
      AND s.kind='function' AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_loops DESC, s.fan_in DESC LIMIT :lim"""),
(
    "unused-return-value",
    "Function whose return value is ignored by callers (CERT EXP12-C)",
    "ANSWERS where a function returns a value but its callers do not check it,\n"
    "     so errors are silently lost. fan_in says how many call sites exist;\n"
    "     a high fan_in with no error-checking callers is a systemic problem.\n"
    "ACT check the return value at every call site, or use a must-check attribute.\n"
    "MISLEADS a function that returns void has no return value to check. The\n"
    "     graph sees fan_in but not whether individual callers check the result.",
    """SELECT s.name, s.ret_val AS returns_value,
        s.fan_in, s.cyclomatic AS cyclo, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.ret_val=1 AND s.fan_in > 5 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.fan_in DESC LIMIT :lim"""),
(
    "macro-side-effect",
    "Function-like macro that may evaluate arguments twice (CERT PRE31-C)",
    "ANSWERS where a function-like macro is defined, which may evaluate its\n"
    "     arguments more than once. If an argument has side effects (i++, f()),\n"
    "     the result is undefined.\n"
    "ACT use a static inline function instead of a macro.\n"
    "MISLEADS a macro that does not reference its arguments more than once is\n"
    "     safe. The graph sees the macro but not its body.",
    """SELECT s.name, s.kind,
        s.n_macro_calls AS macro_calls,
        s.n_calls, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='macro' AND s.fan_in > 3 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.fan_in DESC LIMIT :lim"""),
(
    "vtable-risk",
    "Functions reached through function pointers or dynamic member calls",
    "ANSWERS where runtime target ambiguity is densest: the reads through\n"
    "     `ops->`-style members and `(*fp)()` calls that a static call graph\n"
    "     cannot resolve. Every one is a site whose callee is decided at runtime.\n"
    "ACT audit the dispatch site: is the function-pointer slot ever written with\n"
    "     something other than the one obvious initializer? If so, the edge here\n"
    "     is a security boundary, not an abstraction.\n"
    "MISLEADS counts CALL SITES, not distinct targets, and the brace scanner\n"
    "     sees the `->`-shaped member call syntax only; a struct passed around\n"
    "     and invoked through a local alias (`ops->read` copied into a local\n"
    "     `fp`) shows up as the alias's direct call instead.",
    """SELECT s.name, s.n_fnptr_calls AS fnptr_calls,
        s.n_dynamic_calls AS dyn_calls,
        MAX(s.n_calls - s.n_fnptr_calls - s.n_dynamic_calls, 0) AS direct_calls,
        s.fan_in, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE (s.n_fnptr_calls + s.n_dynamic_calls) > 0
      AND s.kind='function' AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_fnptr_calls + s.n_dynamic_calls) DESC LIMIT :lim"""),
(
    "header-scope-ratio",
    "User-header vs system-header include ratio per file",
    "ANSWERS which files lean almost entirely on system headers (<...>) and so\n"
    "     carry little project-local coupling -- and which pull in mostly user\n"
    "     headers (\"...\"), marking them as tightly coupled to the tree.\n"
    "ACT a file at ~0%% user headers is often a portability shim; a file at\n"
    "     100%% user headers that is itself widely included deserves a look for\n"
    "     layering violations.\n"
    "MISLEADS a `<system>` include that RESOLVES inside the tree is counted as\n"
    "     user here (the analyzer treats project headers on the include path as\n"
    "     internal), so the ratio is about the include SPELLING, not about where\n"
    "     the file actually lives.",
    """SELECT f.path,
        COUNT(*) FILTER (WHERE i.is_relative=1) AS user_headers,
        COUNT(*) FILTER (WHERE i.is_relative=0) AS sys_headers,
        CAST(100.0 * COUNT(*) FILTER (WHERE i.is_relative=1)
             / NULLIF(COUNT(*), 0) AS INT) AS pct_user,
        f.sloc
    FROM imports i JOIN files f ON f.id=i.file_id
    LEFT JOIN modules m ON m.id=f.module_id
    WHERE i.kind='include' AND COALESCE(m.name,'') LIKE :mod
    GROUP BY f.id
    ORDER BY pct_user ASC, COUNT(*) DESC LIMIT :lim"""),
(
    "recursion-loops",
    "Mutually recursive function pairs (a calls b, b calls a)",
    "ANSWERS the call pairs that can recurse unboundedly even though no single\n"
    "     function calls itself. Each row is a 2-cycle in the resolved edge set.\n"
    "ACT add a depth cap or an iteration guard on the LOWER-LEVEL member of the\n"
    "     pair; the higher one is where the recursion is entered.\n"
    "MISLEADS finds direct 2-cycles only. Cycles of length >= 3 (a->b->c->a)\n"
    "     need a full SCC walk and do NOT appear; self-recursion is covered by\n"
    "     `stack-exhaustion`, not here. Edges are name-resolved, so two functions\n"
    "     that share one name are conflated.",
    """SELECT a.name AS fn_a, b.name AS fn_b,
        fa.path || ':' || a.line_start AS at_a,
        fb.path || ':' || b.line_start AS at_b,
        ea.n_calls AS a_calls_b, eb.n_calls AS b_calls_a
    FROM edges ea
    JOIN edges eb ON ea.caller_id=eb.callee_id
                AND ea.callee_id=eb.caller_id
    JOIN symbols a ON a.id=ea.caller_id
    JOIN symbols b ON b.id=ea.callee_id
    JOIN files fa ON fa.id=a.file_id
    JOIN files fb ON fb.id=b.file_id
    LEFT JOIN modules m ON m.id=a.module_id
    WHERE ea.caller_id < ea.callee_id
      AND ea.caller_id <> ea.callee_id
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY (ea.n_calls + eb.n_calls) DESC LIMIT :lim"""),
(
    "global-state-mutation",
    "Non-static, non-const globals: the state every translation unit shares",
    "ANSWERS the file-scope objects without internal linkage (no `static`), so\n"
    "     any translation unit that declares them extern can mutate them. Where\n"
    "     `race-surface` asks which mutable globals two THREADS could clobber\n"
    "     (it includes statics and singles out non-atomic ones), this asks\n"
    "     which globals merely EXIST as cross-TU seams -- the ones that make a\n"
    "     function untestable in isolation and a refactor a hunt through every\n"
    "     declaring file.\n"
    "ACT make them static and route access through one setter, or move them\n"
    "     into a context struct passed explicitly.\n"
    "MISLEADS the brace scanner cannot see WHICH functions write the global,\n"
    "     only that it exists and is shared (`race-surface` is the same data\n"
    "     through a thread-race lens); `has_init` being 0 does not prove there\n"
    "     is no initializer (a forward-declared extern has none by\n"
    "     construction). Config-style `extern const` tables are correctly\n"
    "     excluded (is_const). The scanner records every file-scope declarator\n"
    "     including function parameters of that shape, so private locals and\n"
    "     pointer parameters can surface as rows; shared mutable file-scope\n"
    "     objects are the rows that matter.",
    """SELECT f.path, g.name, g.type, g.line,
        g.has_init, g.is_volatile, g.is_atomic, g.ptr_depth
    FROM globals g JOIN files f ON f.id=g.file_id
    LEFT JOIN modules m ON m.id=g.module_id
    WHERE g.is_static=0 AND g.is_const=0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY g.is_atomic DESC, g.is_volatile DESC, f.path, g.line LIMIT :lim"""),
(
    "unreferenced-includes",
    "Headers included but none of their functions are ever called",
    "ANSWERS the #include lines whose target file's functions have zero\n"
    "     resolved callers anywhere in the tree -- the include-graph shape that\n"
    "     slows rebuilds without contributing edges.\n"
    "ACT drop the include if the header only carried types you no longer use;\n"
    "     otherwise expect one of the covered-by-macro / config-gated cases.\n"
    "MISLEADS a header used ONLY for types, macros, or constants looks dead\n"
    "     here by construction (the scanner sees function calls only); a header\n"
    "     whose functions are CALLED THROUGH POINTERS is also invisible. This is\n"
    "     a candidate list, not a delete list.",
    """SELECT f.path AS header, f.sloc,
        (SELECT COUNT(*) FROM symbols s WHERE s.file_id=f.id) AS n_syms,
        (SELECT COUNT(*) FROM symbols s
          WHERE s.file_id=f.id AND s.kind='function') AS n_fns,
        (SELECT COUNT(*) FROM edges e JOIN symbols s ON s.id=e.callee_id
          WHERE s.file_id=f.id) AS inbound_calls,
        COUNT(DISTINCT i.file_id) AS included_by
    FROM files f
    JOIN imports i ON i.target_id=f.id AND i.kind='include'
    LEFT JOIN modules m ON m.id=f.module_id
    WHERE f.n_symbols > 0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY f.id
    HAVING inbound_calls = 0 AND n_fns > 0
    ORDER BY included_by DESC, f.sloc DESC LIMIT :lim"""),
(
    "blast-radius",
    "Symbols with the largest transitive caller sets",
    "ANSWERS which functions, if changed, can disturb the most of the tree:\n"
    "     the count of DISTINCT functions that can reach this symbol through\n"
    "     resolved edges at any depth.\n"
    "ACT treat the top rows as API changes requiring a caller sweep: every\n"
    "     row's callers are the blast zone. For a public entry point the number\n"
    "     is meaningless by design -- see MISLEADS.\n"
    "MISLEADS `n_transitive` counts callers THROUGH RESOLVED EDGES ONLY; a\n"
    "     symbol called dynamically (function pointer) or across a name\n"
    "     collision undercounts, and a widely-included header's inline helpers\n"
    "     are undercounted for the same reason. The pass is a per-symbol\n"
    "     traversal over the reverse adjacency: O(V*(V+E)) in total, which is\n"
    "     fine at redis scale and slow on very large C corpora -- it is exact\n"
    "     on the edges that exist, not memoised.",
    """SELECT s.name, r.n_transitive AS transitive_callers,
        s.fan_in, s.n_calls, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM reach r JOIN symbols s ON s.id=r.symbol_id
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY r.n_transitive DESC LIMIT :lim"""),
(
    "cross-file-struct-coupling",
    "Struct types that span translation units via layout surface",
    "ANSWERS the struct definitions whose shape (size, pointer fields, padding)\n"
    "     makes them load-bearing across files: a struct this large or this\n"
    "     pointer-heavy, defined once, is almost certainly passed between\n"
    "     translation units and is a compatibility surface.\n"
    "ACT treat layout changes (field reorder, pointer widening) as ABI changes\n"
    "     for every included_by file; the pad columns show where the wasted\n"
    "     bytes are.\n"
    "MISLEADS the brace scanner cannot see WHERE a struct is instantiated; this\n"
    "     ranks by DEFINED shape, not by measured usage, so a huge struct that\n"
    "     never leaves its file appears here too. `exact=0` sizes (unknown field\n"
    "     types) are excluded rather than guessed.",
    """SELECT s.name, ss.total_size, ss.tail_pad, ss.total_pad,
        ss.n_lines_64, ss.exact,
        (SELECT COUNT(*) FROM layout l WHERE l.symbol_id=ss.symbol_id
          AND l.depth=0) AS n_fields,
        (SELECT COUNT(*) FROM layout l WHERE l.symbol_id=ss.symbol_id
          AND l.ptr_depth>0) AS n_ptr_fields,
        f.path || ':' || s.line_start AS at
    FROM struct_size ss JOIN symbols s ON s.id=ss.symbol_id
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE ss.exact=1 AND ss.total_size >= 64
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY ss.total_size DESC, ss.total_pad DESC LIMIT :lim"""),
(
    "extern-linkage-density",
    "Translation units leaning on extern declarations",
    "ANSWERS where file boundaries are load-bearing on bare extern promises:\n"
    "     the count of extern-declared globals a file relies on, and the volume\n"
    "     of calls resolved only because a prototype declared the callee (no\n"
    "     definition in this unit).\n"
    "ACT high extern + low definition density is where a symbol rename or a\n"
    "     signature change breaks the tree without the compiler naming every\n"
    "     victim; consider moving the shared declarations into a header.\n"
    "MISLEADS n_external_calls counts calls whose callee was declared but not\n"
    "     defined in the unit -- libc and POSIX calls dominate any realistic\n"
    "     file, so compare DENSITY across files, not the raw number.",
     """WITH eg AS (
        SELECT g.file_id AS fid, COUNT(*) AS n
        FROM globals g WHERE g.is_static=0 AND g.is_const=0 GROUP BY g.file_id),
    ec AS (
        SELECT s.file_id AS fid, COALESCE(SUM(s.n_external_calls),0) AS ext,
               COALESCE(SUM(s.n_calls),0) AS tot
        FROM symbols s WHERE s.kind='function' GROUP BY s.file_id),
    fc AS (
        SELECT s.file_id AS fid, COUNT(*) AS n FROM symbols s GROUP BY s.file_id)
    SELECT f.path,
        COALESCE(eg.n, 0) AS extern_globals,
        COALESCE(ec.ext, 0) AS extern_calls,
        COALESCE(ec.tot, 0) AS total_calls,
        CAST(100.0 * COALESCE(ec.ext,0) / NULLIF(COALESCE(ec.tot,0), 0) AS INT)
            AS pct_external
    FROM files f
    LEFT JOIN modules m ON m.id=f.module_id
    LEFT JOIN eg ON eg.fid = f.id
    LEFT JOIN ec ON ec.fid = f.id
    LEFT JOIN fc ON fc.fid = f.id
    WHERE fc.n > 0
      AND (COALESCE(ec.ext,0) > 0 OR COALESCE(eg.n,0) > 0)
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY extern_calls DESC LIMIT :lim"""),
(
    "cross-tu-signature-drift",
    "One function name, two different definitions across TUs",
    "ANSWERS function names defined with >= 2 different signatures across\n"
    "     translation units: UB per C99 6.2.7 (MISRA 8.3). The linker picks\n"
    "     one definition and every caller of the other shape is miscompiled.\n"
    "ACT pick one signature; rename or delete the other definition. The row\n"
    "     names every file involved.\n"
    "MISLEADS typedef-equivalent types compare different textually, so a\n"
    "     benign `int foo(int)` vs `int foo(int32_t)` pair is reported;\n"
    "     K&R definitions yield empty signature text and are excluded, and a\n"
    "     static fn in one file plus an extern fn of the same name in another\n"
    "     is NOT a link conflict yet still reads as drift here.",
    """SELECT s.name,
        COUNT(DISTINCT s.signature) AS n_sigs,
        COUNT(DISTINCT s.file_id)   AS n_files,
        GROUP_CONCAT(DISTINCT f.path) AS where_defined
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function' AND s.signature IS NOT NULL AND s.signature != ''
      AND f.is_generated = 0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.name
    HAVING n_sigs > 1
    ORDER BY n_files DESC, n_sigs DESC
    LIMIT :lim"""),
(
    "linkage-scope-mismatch",
    "External-linkage functions whose callers all live in one TU",
    "ANSWERS non-static functions whose resolved callers all sit in one\n"
    "     translation unit: they should be static (MISRA 8.7, cppcheck). An\n"
    "     external-linkage symbol is a coupling surface for the whole\n"
    "     binary; a one-TU usage pattern says the author forgot the keyword.\n"
    "ACT make it static; if fan_in is also 0, `dead-code` owns the deletion\n"
    "     question instead.\n"
    "MISLEADS name-based resolution undercounts callers, so a function used\n"
    "     from a second TU through a macro or a function pointer reads as\n"
    "     one-TU here; fnptr-dispatched users are invisible; a header inline\n"
    "     absorbed elsewhere never appears. When in doubt, the compiler's\n"
    "     own -Wmissing-prototypes is the tiebreaker.",
    """SELECT s.name, f.path, s.line_start, s.fan_in,
        COUNT(DISTINCT cf.id) AS n_caller_files
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    LEFT JOIN edges e ON e.callee_id = s.id
    LEFT JOIN symbols cs ON cs.id = e.caller_id
    LEFT JOIN files cf ON cf.id = cs.file_id
    WHERE s.kind = 'function' AND s.is_static = 0
      AND f.is_generated = 0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id
    HAVING s.fan_in > 0 AND n_caller_files <= 1
    ORDER BY s.fan_in DESC
    LIMIT :lim"""),
(
    "risky-process-apis",
    "Sites of dangerous process/temp APIs (CERT ENV33-C, flawfinder)",
    "ANSWERS reachable sites of the dangerous process and temp-file API set:\n"
    "     system/popen/exec for process boundaries, mktemp/tmpnam/tempnam for\n"
    "     race-prone temp files, access for TOCTOU-prone checks.\n"
    "ACT replace mktemp with mkstemp; interrogate every system/popen -- each\n"
    "     is a command-injection review item when the argument is not a\n"
    "     constant; prefer execve with an explicit argv over execl/execvp\n"
    "     when the argument list is built at runtime.\n"
    "MISLEADS the capture is the call scanner, independent of resolution, so\n"
    "     libc calls that would otherwise classify as external still appear;\n"
    "     the denylist is the fixed set above -- rand/setenv are NOT captured\n"
    "     and are absent by design (their risk is caller-context, which this\n"
    "     query does not model); errno-checking callers are not distinguished.",
    """SELECT f.path, s.name AS caller, h.pattern AS api, h.category, h.n AS sites,
        h.first_line, s.fan_in
    FROM hazards h
    JOIN symbols s ON s.id=h.symbol_id
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE h.pattern IN ('system','popen','execve','execl','execlp','execvp',
                        'execv','fork','posix_spawn','posix_spawnp','vfork',
                        'wordexp','dlopen','dlmopen','mktemp','tmpnam',
                        'tempnam','access')
      AND f.is_generated = 0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.fan_in DESC, h.n DESC
    LIMIT :lim"""),
(
    "hardcoded-secret-candidates",
    "Credential-shaped string literals (OWASP G07)",
    "ANSWERS string literals at least 12 chars long whose text names a\n"
    "     credential (password, token, api_key, secret, bearer, jwt, ...) --\n"
    "     the literal that a committed secret looks like.\n"
    "ACT rotate and move to a secret manager; never commit the literal.\n"
    "MISLEADS a format string or test fixture containing the WORD token/pass\n"
    "     reads as a candidate (the filter is the literal's own text, not its\n"
    "     use); values over 200 chars are truncated at capture; a secret\n"
    "     built from parts or read from an env var is invisible here. This\n"
    "     is a candidate list, not a verdict.",
    """SELECT s.name, sc.value AS candidate, sc.line,
        f.path || ':' || sc.line AS at
    FROM secret_candidates sc
    JOIN symbols s ON s.id = sc.symbol_id
    JOIN files f ON f.id = sc.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE f.is_test = 0 AND f.is_generated = 0 AND COALESCE(m.name,'') LIKE :mod
      AND sc.value NOT LIKE '/%' AND instr(sc.value, '|') = 0
      AND instr(sc.value, '%') = 0
    ORDER BY length(sc.value) DESC LIMIT :lim"""),
(
    "include-cycles",
    "User headers that include each other, directly or via a chain",
    "ANSWERS user headers that include each other: the include graph is a\n"
    "     DAG in any well-formed project, so a cycle means the headers are\n"
    "     mutually dependent and the build order is an accident of include\n"
    "     guards.\n"
    "ACT break the cycle with a forward declaration or by shrinking one\n"
    "     header; rows name both endpoints and the cycle length.\n"
    "MISLEADS include guards make cycles harmless at compile time, so this\n"
    "     is a maintainability smell, not a defect; depth is capped at 8;\n"
    "     is_relative=1 means user \"...\" includes, and a `<...>` include\n"
    "     that happens to resolve in-tree is treated as system-style and\n"
    "     excluded from the walk.",
    """WITH RECURSIVE walk(root, dep, depth, path) AS (
        SELECT i.file_id, i.target_id, 1,
               '>' || i.file_id || '>' || i.target_id || '>'
        FROM imports i
        WHERE i.target_id IS NOT NULL AND i.is_relative = 1
          AND i.kind='include'
        UNION
        SELECT w.root, i.target_id, w.depth + 1, w.path || i.target_id || '>'
        FROM walk w
        JOIN imports i ON i.file_id = w.dep
        WHERE i.target_id IS NOT NULL AND i.is_relative = 1
          AND i.kind='include'
          AND w.depth < 8
          AND (i.target_id = w.root
               OR instr(w.path, '>' || i.target_id || '>') = 0)
    )
    SELECT DISTINCT f1.path AS header, f3.path AS partner, w.depth
    FROM walk w
    JOIN files f1 ON f1.id = w.root
    JOIN imports i0 ON i0.file_id = w.root AND i0.target_id IS NOT NULL
         AND i0.is_relative = 1 AND i0.kind = 'include'
    JOIN files f3 ON f3.id = i0.target_id
    WHERE w.dep = w.root
    ORDER BY w.depth, header
    LIMIT :lim"""),
(
    "const-cast-away",
    "Casts that drop const from const-declared names (CERT EXP05-C)",
    "ANSWERS `(T*)name` casts applied to a name declared const: the\n"
    "     const-qualified promise is stripped by the cast, and any write\n"
    "     through the result is UB in the caller's face.\n"
    "ACT remove the cast, or drop const from the declaration if the\n"
    "     function genuinely mutates (and say why).\n"
    "MISLEADS the const test is against the local/param declaration in the\n"
    "     SAME function: a const GLOBAL or a const from another translation\n"
    "     unit is invisible here; `(void*)` casts and casts of expressions\n"
    "     (not bare names) are not counted; a cast of a name that is NOT\n"
    "     const-declared reads clean by construction.",
    """SELECT s.name, s.n_const_cast AS const_casts, s.n_cast AS casts_total,
        s.fan_in, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_const_cast > 0 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_const_cast DESC, s.fan_in DESC LIMIT :lim"""),
(
    "fnptr-blindspot-callers",
    "Functions used only through &fn -- invisible to the call graph",
    "ANSWERS functions with fan_in 0 whose address is taken somewhere:\n"
    "     they ARE used, but every call goes through a function pointer, so\n"
    "     dead-code and every fan-in-based number understate them.\n"
    "ACT read these as live API surface; a fnptr-dispatched function is a\n"
    "     plugin point or a table-driven dispatch entry.\n"
    "MISLEADS &fn text capture catches address-takes in function bodies; an\n"
    "     address taken in a struct initializer at file scope (the dominant\n"
    "     dispatch-table pattern) is NOT captured -- the table rows are a\n"
    "     floor, and `linkage-scope-mismatch` and dead-code must be read\n"
    "     with this page open.",
    """SELECT s.name, f.path, s.line_start, s.sloc,
        (SELECT COUNT(*) FROM addr_taken a WHERE a.name=s.name) AS addr_taken
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.fan_in=0 AND s.is_test=0
      AND EXISTS (SELECT 1 FROM addr_taken a WHERE a.name=s.name)
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY addr_taken DESC, s.sloc DESC LIMIT :lim"""),
(
    "extern-symbol-asymmetry",
    "Declared in a prototype, never defined in the tree",
    "ANSWERS names with a prototype/extern declaration but no definition\n"
    "     anywhere in the tree: either the definition lives in a library\n"
    "     this run skipped (fine), or the symbol is promised but missing\n"
    "     (a link error waiting for the first caller).\n"
    "ACT for each row decide: library boundary (ignore) or genuinely\n"
    "     missing (define or delete the prototype).\n"
    "MISLEADS a definition behind `#ifdef` that the scan excluded reads as\n"
    "     missing; static functions are excluded from the definition side\n"
    "     only when their name matches -- a same-named static in one file\n"
    "     does NOT satisfy an extern promise in another; libc prototypes in\n"
    "     system headers are not scanned (only this tree's files are).",
    """SELECT d.name, f.path, d.line,
        (SELECT COUNT(*) FROM symbols s
          WHERE s.name = d.name AND s.kind='function') AS defined_count
    FROM declarations d JOIN files f ON f.id=d.file_id
    LEFT JOIN modules m ON m.id=f.module_id
    WHERE (SELECT COUNT(*) FROM symbols s
            WHERE s.name = d.name AND s.kind='function') = 0
      AND f.is_generated = 0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY d.name, f.path
    LIMIT :lim"""),
(
    "toctou-access-open",
    "access(X) then open(X) on the same variable (CERT POS01-C)",
    "ANSWERS functions that both access(X) and open(X) with the same\n"
    "     variable: the permission check and the use are two system calls,\n"
    "     and the file can be swapped between them. The window is the whole\n"
    "     race.\n"
    "ACT open first, then fstat the descriptor; never trust access for\n"
    "     security decisions.\n"
    "MISLEADS same-function variable-name pairing, not data flow: access on\n"
    "     a path built from a different variable than the open is missed,\n"
    "     and access+open on a CONSTANT path (no race on most filesystems\n"
    "     in practice) reads the same as the race; `faccessat` with\n"
    "     AT_EACCESS is a different capture and is absent.",
    """SELECT s.name, s.n_toctou AS pairs, s.n_io AS io_calls, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_toctou > 0 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_toctou DESC, s.fan_in DESC LIMIT :lim""")
]

METRICS = [
(
    "graph-blindspots",
    "Read this first: where the call graph cannot see",
    "ANSWERS how much of every other answer below is a lower bound.\n"
    "ACT fnptr is the real blindness -- a callback table, a vtable-by-hand, a\n"
    "     dispatch array. Reachability results for a module high on this list\n"
    "     are floors, not facts. `via_macro` and `external` are NOT blindness:\n"
    "     one is the preprocessor, the other is libc, and both are known.\n"
    "MISLEADS this matches on NAME and body size only -- it never checks that\n"
    "     the definitions sit behind a preprocessor conditional. Two\n"
    "     same-named static functions in unrelated translation units, or one\n"
    "     entry point implemented once per example module, look identical to\n"
    "     a real backend pair. Confirm with grep -n '#if' on both files.\n"
    "     fnptr also counts ordinary struct-member calls, and `unresolved`\n"
    "     includes names from headers of libraries this run never walked into.",
    """SELECT m.name AS module, COUNT(*) AS fns,
        COALESCE(SUM(s.n_fnptr_calls),0) AS fnptr,
        COALESCE(SUM(s.n_macro_calls),0) AS via_macro,
        COALESCE(SUM(s.n_external_calls),0) AS external,
        COALESCE(SUM(s.n_unresolved_calls),0) AS unresolved,
        CAST(100.0*(COALESCE(SUM(s.n_fnptr_calls),0)
                    + COALESCE(SUM(s.n_unresolved_calls),0))
             / NULLIF(SUM(s.n_calls),0) AS INT) AS pct_unseen
    FROM symbols s JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND m.name LIKE :mod
    GROUP BY m.id ORDER BY fnptr DESC, unresolved DESC LIMIT :lim"""),
(
    "hot-multipliers",
    "Where one fix multiplies: highest fan-in, ranked with complexity",
    "ANSWERS which functions the rest of the tree leans on hardest.\n"
    "ACT a win in a high-fan-in leaf pays back once per caller.\n"
    "MISLEADS fan_in counts STATIC call sites, not dynamic frequency, and it\n"
    "     cannot see a caller that reaches this through a function pointer.",
    """SELECT s.name, s.fan_in, s.n_callsites AS sites, s.cyclomatic AS cyclo,
        s.sloc, s.is_static AS stat, s.is_inline AS inl, m.name AS module,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.fan_in DESC, s.cyclomatic DESC LIMIT :lim"""),
(
    "risk-ranked",
    "Security review order: complexity x hazard x recursion",
    "ANSWERS if you can only review N functions this week, which N.\n"
    "ACT risk = 2*cyclo + cognitive + 5*nest + 10*memory + 8*io + 15*exec\n"
    "     + integer + 2*alloc + 3*concurrency + 25 if recursive\n"
    "     + 10 if it allocates and never frees.\n"
    "MISLEADS it is a heuristic for ORDERING, not a list of findings.",
    """SELECT s.name, s.risk_score AS risk, s.cyclomatic AS cyclo,
        s.cognitive AS cog, s.max_nesting AS nest, s.n_memory AS mem,
        s.n_io AS io, s.n_integer AS int_, s.is_recursive AS rec,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.risk_score DESC LIMIT :lim"""),
(
    "alloc-cost",
    "Allocations per call, TRANSITIVELY",
    "ANSWERS what one call to this function really costs the allocator.\n"
    "ACT `direct` counts allocation written in the body; `xalloc` walks the\n"
    "     call graph and multiplies by static call sites.\n"
    "MISLEADS the multiplier is STATIC call sites, not trip count, and the\n"
    "     walk stops at depth 3 -- deeper allocation is not counted at all.",
    """-- depth bound: 3 call levels; the multiplier is capped at 4096 so a
    -- densely-called leaf cannot explode the intermediate table.
    WITH RECURSIVE
    edge(caller, callee, mult) AS (
        SELECT caller_id, callee_id, COUNT(*) FROM callsites
        GROUP BY caller_id, callee_id),
    direct(sym, n) AS (
        SELECT symbol_id, SUM(n) FROM hazards
        WHERE category='alloc' AND pattern<>'free'
          AND lower(substr(pattern, -4))<>'free'
        GROUP BY symbol_id),
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
    WHERE s.kind='function' AND COALESCE(m.name,'') LIKE :mod
    GROUP BY w.root HAVING xalloc >= 8
    ORDER BY xalloc DESC LIMIT :lim"""),
(
    "module-coupling",
    "Cross-module call edges: where a seam would actually cut",
    "ANSWERS how entangled the subsystems are.\n"
    "ACT a heavy one-way edge is a real seam; a heavy pair is a cycle and the\n"
    "     two modules are one module that has not admitted it.\n"
    "MISLEADS counts DISTINCT caller/callee pairs, not runtime frequency, and\n"
    "     misses every edge that goes through a callback.",
    """SELECT mc.name AS from_module, mt.name AS to_module,
        COUNT(*) AS edges, SUM(e.n_calls) AS calls,
        COUNT(DISTINCT e.callee_id) AS distinct_targets
    FROM edges e
    JOIN symbols sc ON sc.id=e.caller_id
    JOIN symbols st ON st.id=e.callee_id
    JOIN modules mc ON mc.id=sc.module_id
    JOIN modules mt ON mt.id=st.module_id
    WHERE e.same_module=0 AND mc.name LIKE :mod
    GROUP BY mc.id, mt.id ORDER BY edges DESC LIMIT :lim"""),
(
    "header-fanout",
    "Headers whose change rebuilds the most of the tree",
    "ANSWERS which header is the build's bottleneck, transitively.\n"
    "ACT split the widely-included header, or move the hot declarations into a\n"
    "     narrow one. This is the cheapest build-time win in a C repo.\n"
    "MISLEADS include depth is not compile cost, and a header included by\n"
    "     everything but changed once a year costs nothing at all.",
    """-- depth bound: 4 levels of transitive #include. Deeper chains are counted
    -- at the depth they are first reached; C include graphs are shallow but do
    -- contain cycles, and UNION plus the bound is what makes this terminate.
    WITH RECURSIVE inc(src, hdr, depth) AS (
        SELECT i.file_id, i.target_id, 1 FROM imports i
        WHERE i.target_id IS NOT NULL
        UNION
        SELECT c.src, i.target_id, c.depth+1
        FROM inc c JOIN imports i ON i.file_id=c.hdr
        WHERE i.target_id IS NOT NULL AND c.depth < 4 AND i.target_id <> c.src)
    SELECT f.path AS header, COUNT(DISTINCT c.src) AS rebuilt_files,
        f.sloc, f.n_symbols AS syms, MIN(c.depth) AS min_depth
    FROM inc c JOIN files f ON f.id=c.hdr
    LEFT JOIN modules m ON m.id=f.module_id
    WHERE COALESCE(m.name,'') LIKE :mod
    GROUP BY c.hdr ORDER BY rebuilt_files DESC, f.sloc DESC LIMIT :lim"""),
(
    "nested-loops",
    "Loop depth >= 2: the O(n^k) candidates, with their per-iteration cost",
    "ANSWERS where cost grows super-linearly in the input.\n"
    "ACT check what bounds the INNER trip count. `divs` and `calls` are the\n"
    "     two per-iteration costs people forget; a divide is ~20-40 cycles and\n"
    "     a loop-invariant one should be a reciprocal multiply.\n"
    "MISLEADS depth counts LEXICAL nesting, not asymptotics: an inner loop\n"
    "     bounded by a constant is O(1).",
    """SELECT s.name, s.max_loop_depth AS depth, s.n_loops AS loops,
        s.call_in_loop AS calls, s.div_in_loop AS divs,
        s.libm_in_loop AS libm, s.branch_in_loop AS brs, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.max_loop_depth>1 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.max_loop_depth DESC, s.call_in_loop DESC LIMIT :lim"""),
(
    "vectorisation-blocked",
    "Loops that CANNOT vectorise: a libm call in the body",
    "ANSWERS which loops are categorically unvectorizable as written.\n"
    "ACT a libm call in a loop body is a hard stop for the auto-vectoriser --\n"
    "     it cannot prove the call is pure or replace it with a vector form.\n"
    "     Use a vectorised math library, or a polynomial approximation.\n"
    "MISLEADS it does not know the loop is hot, and -ffast-math plus a vector\n"
    "     libm changes the answer entirely.",
    """SELECT s.name, s.n_libm AS libm, s.libm_in_loop AS libm_in_loop,
        s.n_loops AS loops, s.max_loop_depth AS depth, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_libm>0 AND s.n_loops>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.libm_in_loop DESC, s.n_libm DESC LIMIT :lim"""),
(
    "explicit-simd",
    "Hand-written intrinsics and branch hints",
    "ANSWERS where the code already commits to a specific ISA.\n"
    "ACT every intrinsic site needs a scalar fallback that is BUILT in CI, not\n"
    "     merely present -- an unbuilt fallback is an unrun fallback.\n"
    "MISLEADS a high intrinsic count is not a fast function, and `likely` hints\n"
    "     are frequently wrong and never re-measured.",
    """SELECT s.name, s.n_intrinsic AS intrin, s.n_likely AS hints,
        s.n_restrict AS restrict_, s.n_builtin AS builtins,
        s.max_loop_depth AS depth, s.n_atomic AS atomics,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE (s.n_intrinsic>0 OR s.n_likely>0) AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_intrinsic DESC, s.n_likely DESC LIMIT :lim"""),
(
    "struct-padding",
    "Byte-accurate layout: bytes lost to alignment holes",
    "ANSWERS which structs waste memory on padding, and exactly how much.\n"
    "ACT reorder fields largest-alignment-first. This is free: no algorithm\n"
    "     changes, no API changes, and it compounds across every instance.\n"
    "MISLEADS LP64 model, and exact=1 rows only -- a struct containing another\n"
    "     struct cannot be sized here and is simply absent from this list.",
    """SELECT s.name AS struct_, s.kind, ss.total_size AS sz,
        ss.total_pad AS pad, ss.tail_pad AS tail,
        CAST(100.0*ss.total_pad/NULLIF(ss.total_size,0) AS INT) AS pct_waste,
        ss.n_lines_64 AS lines64, ss.max_align AS algn,
        f.path || ':' || s.line_start AS at
    FROM struct_size ss JOIN symbols s ON s.id=ss.symbol_id
    JOIN files f ON f.id=s.file_id LEFT JOIN modules m ON m.id=s.module_id
    WHERE ss.exact=1 AND ss.total_pad>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY ss.total_pad DESC LIMIT :lim"""),
(
    "cache-line-crossers",
    "Structs just over a 64-byte cache line",
    "ANSWERS which hot objects need two cache lines where one would do.\n"
    "ACT 65-128 bytes is the painful band: shrink below 64 and every access\n"
    "     halves its memory traffic. Removing the padding is often enough.\n"
    "MISLEADS a struct that is never hot does not care, and an object always\n"
    "     touched in full needs both lines anyway.",
    """SELECT s.name AS struct_, ss.total_size AS sz, ss.total_pad AS pad,
        ss.n_lines_64 AS lines64, ss.max_align AS algn,
        (SELECT COUNT(*) FROM layout l WHERE l.symbol_id=s.id) AS fields,
        f.path || ':' || s.line_start AS at
    FROM struct_size ss JOIN symbols s ON s.id=ss.symbol_id
    JOIN files f ON f.id=s.file_id LEFT JOIN modules m ON m.id=s.module_id
    WHERE ss.exact=1 AND ss.total_size BETWEEN 65 AND 128
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY ss.total_size ASC LIMIT :lim"""),
(
    "cache-hostile-layout",
    "Pointer-dense structs: each pointer field defeats the prefetcher",
    "ANSWERS which structures drag a whole cache line to reach one field, and\n"
    "     then send you somewhere else in memory to read it.\n"
    "ACT candidates for splitting the hot fields into a parallel array (SoA).\n"
    "MISLEADS TOP-LEVEL fields only -- union arms are alternatives, not extra\n"
    "     fields -- and a node in a linked structure is pointer-dense by nature.",
    """SELECT v.struct_name AS struct_, v.n_fields AS fields,
        v.n_pointers AS ptrs, v.n_fnptrs AS fnptr, v.n_arrays AS arrays,
        CAST(100.0*v.n_pointers/v.n_fields AS INT) AS pct_ptr, v.at
    FROM v_struct_shape v
    WHERE COALESCE(v.module,'') LIKE :mod
    AND v.n_fields>=6 AND (100.0*v.n_pointers/v.n_fields)>=50
    ORDER BY v.n_pointers DESC LIMIT :lim"""),
(
    "stack-pressure",
    "Functions with the most locals, and the most pointer locals",
    "ANSWERS which frames are large enough to matter.\n"
    "ACT a big frame in a RECURSIVE function multiplies by depth, and that\n"
    "     product is what actually overflows the stack.\n"
    "MISLEADS counts DECLARATIONS, not simultaneous liveness -- the compiler\n"
    "     reuses slots -- and it cannot see arrays sized at run time.",
    """SELECT s.name, s.n_locals AS locals, s.n_ptr_locals AS ptrs,
        s.is_recursive AS rec, s.max_loop_depth AS depth, s.sloc,
        s.n_params AS params, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.n_locals>0
    AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_locals DESC, s.is_recursive DESC LIMIT :lim"""),
(
    "cast-density",
    "Pointer casts: where the type system was overruled",
    "ANSWERS the places a wrong assumption becomes a memory bug.\n"
    "ACT each cast is a claim the compiler cannot check. A cast next to I/O\n"
    "     and shifting is where attacker-controlled bytes become a pointer.\n"
    "MISLEADS the pattern counts some compound literals and some macro\n"
    "     parameter lists as casts.",
    """SELECT s.name, s.n_cast AS casts, s.n_deref AS derefs,
        s.n_shift AS shifts, s.n_sizeof AS sizeofs, s.n_memory AS mem,
        s.n_io AS io, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_cast>0 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_cast DESC LIMIT :lim"""),
(
    "macro-machinery",
    "Function-like macros, by how much work they do",
    "ANSWERS the code the call graph cannot see into at all.\n"
    "ACT a heavily-used multi-line macro is a function that skipped review: no\n"
    "     type checking, no breakpoint, no stack frame, and its cost is\n"
    "     multiplied by every use site rather than shared.\n"
    "MISLEADS `uses` counts sites where the name was called and no function of\n"
    "     that name exists in the tree, so a macro shadowed by a real function\n"
    "     somewhere reads as unused.",
    """SELECT s.name, mc.n_uses AS uses, mc.n_params AS params,
        mc.body_len AS body_len, mc.is_multiline AS multiline,
        f.path || ':' || s.line_start AS at
    FROM macros mc JOIN symbols s ON s.id=mc.symbol_id
    JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE mc.is_functionlike=1 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY mc.n_uses DESC, mc.body_len DESC LIMIT :lim"""),
(
    "config-gated",
    "Code behind a CONFIG_/HAVE_/USE_ flag",
    "ANSWERS which regions a plain build silently omits.\n"
    "ACT build each flag in CI, or the code inside it is unrun and unreviewed\n"
    "     and will not compile the day someone needs it.\n"
    "MISLEADS lists DIRECTIVES, not region sizes: one #ifdef can gate a\n"
    "     thousand lines or a single semicolon.",
    """SELECT cb.expr, COUNT(*) AS sites, COUNT(DISTINCT cb.file_id) AS files,
        GROUP_CONCAT(DISTINCT cb.directive) AS forms,
        GROUP_CONCAT(DISTINCT f.basename) AS in_files
    FROM config_blocks cb JOIN files f ON f.id=cb.file_id
    LEFT JOIN modules m ON m.id=f.module_id
    WHERE cb.is_config=1 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY cb.expr ORDER BY sites DESC LIMIT :lim"""),
(
    "backend-parity",
    "One name, two definitions: which #if-selected backend is the STUB",
    "ANSWERS where a compile-time alternative silently drops a feature.\n"
    "ACT a body a fraction of its sibling's size is usually the stub, and the\n"
    "     platform that selects it is the platform the feature does not work on.\n"
    "MISLEADS a small body can be complete -- a one-line platform wrapper is\n"
    "     the whole implementation on that platform.",
    """SELECT a.name, MIN(a.sloc) AS small, MAX(a.sloc) AS large,
        COUNT(*) AS defs, GROUP_CONCAT(DISTINCT f2.basename) AS files
    FROM symbols a
    JOIN files f2 ON f2.id=a.file_id
    JOIN modules m ON m.id=a.module_id
    WHERE a.kind='function' AND COALESCE(m.name,'') LIKE :mod
    AND m.kind NOT IN ('test','tool') AND f2.is_test=0
    AND a.name NOT IN ('main','LLVMFuzzerTestOneInput','usage','help')
    GROUP BY a.name
    HAVING COUNT(*)>1 AND MAX(a.sloc) >= 3*MIN(a.sloc)+3
    AND (COUNT(DISTINCT a.file_id)=1 OR MIN(f2.basename) <> MAX(f2.basename))
    ORDER BY (MAX(a.sloc)-MIN(a.sloc)) DESC LIMIT :lim"""),
(
    "profiler-invisible",
    "static inline with real fan-in: zero self-time is not zero cost",
    "ANSWERS which functions a sampling profiler CANNOT attribute cost to.\n"
    "ACT never conclude one of these is cold from a flat profile -- its time\n"
    "     is charged to whoever inlined it. `hidden` is fan_in * sloc, a rough\n"
    "     measure of how much code the inliner is duplicating.\n"
    "MISLEADS `static inline` is a request, not a guarantee, and a big one is\n"
    "     often refused.",
    """SELECT s.name, s.fan_in, s.sloc, s.cyclomatic AS cyclo,
        s.fan_in*s.sloc AS hidden, f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.is_inline=1 AND s.is_static=1
    AND s.fan_in>=3 AND s.sloc>=4 AND COALESCE(m.name,'') LIKE :mod
    ORDER BY hidden DESC LIMIT :lim"""),
(
    "undocumented-complexity",
    "Complex functions with almost no comments",
    "ANSWERS where the next reader has to reconstruct intent from the code,\n"
    "     and where that reconstruction costs the most.\n"
    "ACT a useful comment carries a CONSTRAINT or a non-obvious fact -- why\n"
    "     the bound is 4096, which caller guarantees the pointer is non-NULL.\n"
    "MISLEADS comment COUNT is not comment quality, and a genuinely obvious\n"
    "     500-line switch needs no prose at all.",
    """SELECT s.name, s.cyclomatic AS cyclo, s.sloc,
        s.n_comment_lines AS cmts, s.has_doc AS doc,
        s.max_nesting AS nest, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.kind='function' AND s.cyclomatic>=20
    AND s.n_comment_lines*20 < s.sloc AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.cyclomatic DESC LIMIT :lim"""),
(
    "hand-linked-objects",
    "Build rules and object lists that enumerate their inputs by hand",
    "ANSWERS which link targets name their objects individually -- both in a\n"
    "     rule's prerequisites and in the OBJ= variable that feeds it, which is\n"
    "     where a C project usually keeps the list.\n"
    "ACT these are the lists a NEW CALL from a shared source silently breaks:\n"
    "     the compile succeeds and the link fails naming a symbol nobody edited.\n"
    "MISLEADS a hand-written list is perfectly correct until a dependency\n"
    "     changes, so this is a fragility ranking, not a defect list. A probe\n"
    "     snippet in a configure check looks the same and matters not at all.",
    """SELECT path, rule, n_objs AS objs, n_srcs AS srcs,
        uses_ar AS archives, line AS at_line
    FROM makefile_rules
    WHERE n_objs+n_srcs >= 2
    ORDER BY n_objs+n_srcs DESC LIMIT :lim"""),
(
    "parse-coverage",
    "How much of the tree this run actually read",
    "ANSWERS whether any answer above is missing a chunk of the codebase.\n"
    "ACT `errors` means braces or parentheses did not balance, so functions in\n"
    "     that file may be merged, truncated or missed entirely. `unclosed` is\n"
    "     an #if without an #endif. Both are worth reading before trusting a\n"
    "     ranking that says a module is small.\n"
    "MISLEADS a file with zero errors can still be mis-read: a macro that\n"
    "     opens a brace and another that closes it balances perfectly and\n"
    "     produces nonsense spans.",
    """SELECT COALESCE(m.name,'(none)') AS module, COUNT(*) AS files,
        SUM(f.parsed) AS parsed, SUM(f.n_parse_errors) AS errors,
        SUM(f.n_missing_nodes) AS unclosed, SUM(f.sloc) AS sloc,
        SUM(f.n_functions) AS fns, SUM(f.n_imports) AS includes,
        CAST(ROUND(SUM(f.parse_ms)) AS INT) AS ms
    FROM files f LEFT JOIN modules m ON m.id=f.module_id
    WHERE COALESCE(m.name,'') LIKE :mod
    GROUP BY f.module_id ORDER BY sloc DESC LIMIT :lim"""),
(
    "goto-spaghetti",
    "Functions with excessive goto usage (MISRA-C 15.1)",
    "ANSWERS where a function has more than 3 goto statements, making control\n"
    "     flow hard to follow and verify.\n"
    "ACT restructure with structured control flow (if/else, while, break).\n"
    "MISLEADS goto for cleanup-on-error (the only acceptable use in Linux kernel\n"
    "     style) is correct but should be the only pattern.",
    """SELECT s.name, s.n_gotos AS gotos,
        s.n_labels AS labels,
        s.cyclomatic AS cyclo, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_gotos > 3 AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_gotos DESC, s.cyclomatic DESC LIMIT :lim"""),
(
    "deep-nesting",
    "Functions with excessive nesting depth (MISRA-C)",
    "ANSWERS where a function has max_nesting > 4, making it hard to verify.\n"
    "ACT extract nested blocks into helper functions; use early returns.\n"
    "MISLEADS C's max_nesting is +1 vs tree-sitter languages because it counts\n"
    "     the function's own brace. Adjust thresholds accordingly.",
    """SELECT s.name, s.max_nesting AS nesting,
        s.cyclomatic AS cyclo,
        s.n_loops AS loops, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.max_nesting > 5 AND s.kind='function' AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.max_nesting DESC, s.cyclomatic DESC LIMIT :lim"""),
(
    "too-many-params",
    "Functions with too many parameters (MISRA-C)",
    "ANSWERS where a function has more than 6 parameters.\n"
    "ACT use a struct parameter.\n"
    "MISLEADS a variadic function has n_params that does not count the ellipsis.",
    """SELECT s.name, s.n_params,
        s.sloc, s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_params > 6 AND s.kind='function' AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_params DESC, s.fan_in DESC LIMIT :lim"""),
(
    "scattered-concerns",
    "A function called from many different modules (shotgun surgery)",
    "ANSWERS which functions are called from many distinct modules.\n"
    "ACT consider splitting or stabilizing the contract.\n"
    "MISLEADS a utility like memcpy or printf is called from everywhere.",
    """SELECT s.name, COUNT(DISTINCT m.id) AS n_caller_modules,
        s.fan_in, s.cyclomatic AS cyclo, s.sloc,
        GROUP_CONCAT(DISTINCT m.name) AS modules,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN edges e ON e.callee_id=s.id
    JOIN symbols caller ON caller.id=e.caller_id
    LEFT JOIN modules m ON m.id=caller.module_id
    JOIN files f ON f.id=s.file_id
    WHERE s.kind='function' AND f.is_test=0
      AND e.is_self=0 AND COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id
    HAVING n_caller_modules > 5
    ORDER BY n_caller_modules DESC, s.fan_in DESC LIMIT :lim"""),
(
    "magic-number",
    "Functions with many bare numeric literals (MISRA-C 7.4)",
    "ANSWERS where a function has many magic numbers — bare numeric literals\n"
    "     that are not 0 or 1 and have no named constant. Each is a\n"
    "     maintenance burden.\n"
    "ACT extract magic numbers into named constants or enums.\n"
    "MISLEADS 0 and 1 are excluded by convention. Array indices and bit flags\n"
    "     are sometimes clearer as literals.",
    """SELECT s.name, s.n_magic AS magic_numbers,
        s.n_float_lit AS float_literals,
        s.cyclomatic AS cyclo, s.sloc, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM symbols s JOIN files f ON f.id=s.file_id
    LEFT JOIN modules m ON m.id=s.module_id
    WHERE s.n_magic > 10 AND s.kind='function' AND f.is_test=0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_magic DESC, s.fan_in DESC LIMIT :lim""")
]

_SYMBOL_COLS: set[str] = set()

def _init_symbol_cols() -> None:
    """Which metric keys the schema actually has.

    Checked once so a typo in a metric name is a loud failure at import time
    rather than a column that silently stays zero for the life of the tool.
    """
    base = """n_params n_optional_params n_generic_params n_overloads arity_rank
    is_public is_static is_async is_generator is_abstract is_override
    is_exported is_test is_deprecated is_entrypoint is_generated
    sloc body_bytes n_comment_lines n_doc_lines has_doc
    cyclomatic cognitive max_nesting n_tokens n_operators n_operands
    n_distinct_operators n_distinct_operands halstead_volume maintainability
    n_loops n_branches n_returns n_early_returns n_switch n_cases n_ternary
    n_logical n_try n_catch n_catch_broad n_catch_empty n_finally n_throw
    n_labels n_gotos
    max_loop_depth call_in_loop alloc_in_loop io_in_loop await_in_loop
    lock_in_loop concat_in_loop regex_in_loop query_in_loop branch_in_loop
    n_locals n_assign n_compound_assign n_incdec n_cmp n_bitop n_shift
    n_arith n_string_lit n_regex_lit n_float_lit n_magic n_null_check
    n_subscript n_member_access n_lambda n_closure_capture
    n_calls n_unique_calls n_dynamic_calls n_unresolved_calls
    fan_in fan_out n_callsites is_recursive is_leaf is_root
    n_hazards risk_score""".split()
    _SYMBOL_COLS.update(base)
    _SYMBOL_COLS.update("n_" + c for c in HAZARD_CATEGORIES)
    _SYMBOL_COLS.update(n for n, _ in CAnalyzer.EXTRA_SYMBOL_COLS)

_init_symbol_cols()

# ==========================================================================
# DYN PROJECT QUERY CATALOGUE (dynajs repo only; not upstream)
#
# Numbered after the upstream list, so the upstream numbering (Q1
# untrusted-frontier, Q27 vtable-risk) never moves: run `python3
# bench/codegraph.py . N` with N > 42. Every query takes the same
# :mod / :lim parameters as the upstream catalogue.
# ==========================================================================

_DYN_TAINTED_CTE = """WITH RECURSIVE tainted(id) AS (
        SELECT s.id FROM symbols s
        WHERE s.kind='function' AND s.n_io > 0
        UNION
        SELECT e.callee_id FROM edges e
        JOIN tainted t ON t.id = e.caller_id
    )"""

DYN_QUERIES: list[tuple[str, str, str, str]] = [
(
    "untrusted-reach",
    "Transitive closure: every function attacker bytes can reach",
    "ANSWERS the real attack surface: not just the functions that CALL read/"
    "     recv, but everything downstream of them through resolved edges.\n"
    "ACT this is the ranking to review before shipping a parser -- a bounds\n"
    "     bug in any row here is a reachable memory-safety bug. The fuzz\n"
    "     targets must cover the files named in this list (fuzz_parsers,\n"
    "     fuzz_stdlib exist because of it).\n"
    "MISLEADS fnptr edges are invisible: the aio/reactor dispatch and every\n"
    "     JS_CFUNC-registered native are reached through function pointers,\n"
    "     so this is a FLOOR, not the frontier. Compare against\n"
    "     graph-blindspots before budgeting from it.",
    _DYN_TAINTED_CTE + r"""
    SELECT s.name, s.n_io AS io, s.n_memory AS mem, s.n_alloc AS alloc,
        s.cyclomatic AS cyclo, s.fan_in,
        f.path || ':' || s.line_start AS at
    FROM tainted t JOIN symbols s ON s.id = t.id
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE COALESCE(m.name,'') LIKE :mod
    ORDER BY (s.n_memory + s.n_alloc) DESC, s.n_io DESC LIMIT :lim"""),
(
    "refcount-leak-shape",
    "JSValue refcount drift: more JS_New*/JS_DupValue than JS_FreeValue",
    "ANSWERS the CLAUDE.md law -- 'every value from an API call is owned; "
    "free\n"
    "     once on every path' -- as a measurement, not a habit. Positive drift\n"
    "     with early returns and a non-JSValue return type is the leak shape:\n"
    "     a value created and then abandoned on an error path.\n"
    "ACT a function that RETURNS a JSValue is exempt by design (the caller\n"
    "     owns it now); a void/int function with drift > 0 and returns > 1\n"
    "     needs a free on the paths that do not hand the value out.\n"
    "MISLEADS counts are per function, not per path: a function that creates\n"
    "     two values and frees one on every path can show 0 drift and still\n"
    "     leak. Cross-check each row against its return statements.",
    r"""
    SELECT s.name,
        COALESCE(al.n,0) AS alloc_dup, COALESCE(fr.n,0) AS free_n,
        COALESCE(al.n,0) - COALESCE(fr.n,0) AS drift,
        s.n_returns, s.return_type,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name LIKE 'JS_New%' OR name LIKE 'JS_Dup%'
               GROUP BY caller_id) al ON al.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name LIKE 'JS_FreeValue%' OR name LIKE 'js_free_%'
               GROUP BY caller_id) fr ON fr.caller_id = s.id
    WHERE s.kind='function'
      AND (al.n > 0 OR fr.n > 0)
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY drift DESC, s.n_returns DESC LIMIT :lim"""),
(
    "over-free-shape",
    "More JS_FreeValue than JS_New*/JS_DupValue: double-free risk",
    "ANSWERS the mirror of refcount-leak-shape: a function that frees more\n"
    "     than it demonstrably creates inside itself. Two honest causes: the\n"
    "     value was created OUTSIDE (caller-owned, must not free at all) or\n"
    "     the creator is behind a macro the text pass cannot see.\n"
    "ACT for each row: read the ownership comment. A negative drift on a\n"
    "     function that also creates the value on one path is the double-\n"
    "     free shape.\n"
    "MISLEADS JS_FreeValueRT vs JS_FreeValue are both counted; a function\n"
    "     freeing a borrowed JSValueConst is exactly the bug this exists to\n"
    "     find, so treat every negative row as a review item.",
    r"""
    SELECT s.name,
        COALESCE(al.n,0) AS alloc_dup, COALESCE(fr.n,0) AS free_n,
        COALESCE(al.n,0) - COALESCE(fr.n,0) AS drift,
        s.n_returns, s.return_type,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name LIKE 'JS_New%' OR name LIKE 'JS_Dup%'
               GROUP BY caller_id) al ON al.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name LIKE 'JS_FreeValue%' OR name LIKE 'js_free_%'
               GROUP BY caller_id) fr ON fr.caller_id = s.id
    WHERE s.kind='function'
      AND COALESCE(fr.n,0) > COALESCE(al.n,0)
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY drift ASC LIMIT :lim"""),
(
    "define-vs-set",
    "Every JS_SetProperty*/JS_SetPropertyStr caller, frontier-flagged",
    "ANSWERS where a peer-supplied key can retarget a prototype: JS_SetProperty"
    "\n"
    "     and JS_SetPropertyStr walk the prototype chain, so a key of\n"
    "     '__proto__' re-points the object. The repo rule is DEFINE for every\n"
    "     decoded/mapped key. The on_frontier column comes from the taint\n"
    "     closure; fnptr blindness undercounts it (registered natives are\n"
    "     invisible), so 0 does not clear a row.\n"
    "ACT walk the rows with the most calls first; a Set with a key built\n"
    "     from input bytes (JSON keys, header names, map entries) must be\n"
    "     JS_DefinePropertyValue. Internal fixed keys (error objects, option\n"
    "     structs) are fine with Set.\n"
    "MISLEADS the pass counts names, not arguments: a SetPropertyStr with a\n"
    "     string LITERAL argument is a false positive for the prototype\n"
    "     attack but still worth the walk (a later edit may hoist the key).",
    _DYN_TAINTED_CTE + r"""
    SELECT s.name, ap.n AS set_calls,
        CASE WHEN t.id IS NOT NULL THEN 1 ELSE 0 END AS on_frontier,
        f.path || ':' || s.line_start AS at
    FROM (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
          WHERE name LIKE 'JS_SetProperty%' GROUP BY caller_id) ap
    JOIN symbols s ON s.id = ap.caller_id
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    LEFT JOIN tainted t ON t.id = s.id
    WHERE COALESCE(m.name,'') LIKE :mod
    ORDER BY ap.n DESC, on_frontier DESC LIMIT :lim"""),
(
    "throwing-path-hygiene",
    "JSValue-returning functions with several returns and NO JS_Throw",
    "ANSWERS where an error path returns a non-exception value: a native\n"
    "     method that returns JS_UNDEFINED on failure instead of throwing\n"
    "     reads as success to the caller and silently drops the failure.\n"
    "ACT read the return statements; every failure path should throw a "
    "named\n"
    "     error (the repo's distinct-refusal rule), never return quietly.\n"
    "MISLEADS many honest functions return JS_EXCEPTION after a nested call\n"
    "     already threw, with no JS_Throw of their own -- the count can be\n"
    "     zero while every path is correct.",
    r"""
    SELECT s.name, s.n_returns, s.n_calls, s.return_type,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name LIKE 'JS_Throw%' OR name LIKE 'JS_IsException%'
               GROUP BY caller_id) th ON th.caller_id = s.id
    WHERE s.kind='function' AND s.return_type LIKE 'JSValue%'
      AND s.n_returns > 1 AND COALESCE(th.n,0) = 0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.n_returns DESC, s.n_calls DESC LIMIT :lim"""),
(
    "lazy-static-races",
    "Mutable file-scope state: lazily-initialised statics and shared globals",
    "ANSWERS the data-race class the codebase has hit before: a non-const\n"
    "     static without an initialiser is a lazily-initialised static, and\n"
    "     that is a data race even when every thread writes identical bytes.\n"
    "     A non-static mutable global is shared state with no owner.\n"
    "ACT prefer a compile-time const table; where a runtime probe is "
    "genuinely\n"
    "     required, make the flag atomic.\n"
    "MISLEADS a global only ever touched before thread creation is fine -- "
    "the\n"
    "     scanner cannot see WHEN the write happens.",
    r"""
    SELECT g.name, g.type, g.is_static, g.is_const, g.has_init,
        f.path || ':' || g.line AS at
    FROM globals g
    JOIN files f ON f.id = g.file_id
    LEFT JOIN modules m ON m.id = g.module_id
    WHERE g.is_const = 0 AND (g.is_static = 0 OR g.has_init = 0)
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY g.is_static DESC, g.has_init ASC, f.path LIMIT :lim"""),
(
    "module-cycles",
    "Mutually-reachable module pairs: layering violations",
    "ANSWERS which modules cannot be reasoned about in isolation because\n"
    "     each reaches the other through resolved calls. A module dependency\n"
    "     cycle is the one smell that blocks architectural reasoning "
    "outright.\n"
    "ACT break the most-loaded edge of the cycle (the pair with the highest\n"
    "     out/in counts); a k-cycle of three shows as three rows here.\n"
    "MISLEADS pairs are transitive, not direct: A->B->C->A reports (A,B),\n"
    "     (A,C), (B,C). Each pair still names a real circular dependency.\n"
    "     fnptr edges are invisible, so dispatch-table back-edges do not "
    "show.",
    r"""
    WITH me AS (
      SELECT DISTINCT s1.module_id AS fm, s2.module_id AS tm
      FROM edges e
      JOIN symbols s1 ON s1.id = e.caller_id
      JOIN symbols s2 ON s2.id = e.callee_id
      WHERE s1.module_id IS NOT NULL AND s2.module_id IS NOT NULL
        AND s1.module_id <> s2.module_id
    ),
    tc(fm, tm) AS (
      SELECT fm, tm FROM me
      UNION
      SELECT tc.fm, me.tm FROM tc JOIN me ON tc.tm = me.fm
    )
    SELECT a.name || ' <-> ' || b.name AS cycle,
        co.n_out AS a_calls_out, ci.n_in AS b_calls_in
    FROM tc x
    JOIN modules a ON a.id = x.fm
    JOIN modules b ON b.id = x.tm
    JOIN (SELECT fm, COUNT(*) AS n_out FROM me GROUP BY fm) co ON co.fm = x.fm
    JOIN (SELECT tm, COUNT(*) AS n_in FROM me GROUP BY tm) ci ON ci.tm = x.tm
    WHERE x.fm < x.tm
      AND EXISTS (SELECT 1 FROM tc y WHERE y.fm = x.tm AND y.tm = x.fm)
      AND a.name LIKE :mod
    ORDER BY cycle LIMIT :lim"""),
(
    "feature-envy",
    "Functions whose cross-module calls dominate their own-module calls",
    "ANSWERS misplaced helpers: a function that calls into ONE foreign "
    "module"
    "\n"
    "     far more than into its own is implemented on the wrong side of "
    "the\n"
    "     boundary (the JDeodorant move-method shape).\n"
    "ACT move the function into the envied module, or move the missing "
    "helper\n"
    "     into the envious one; rank by cross_edges.\n"
    "MISLEADS header-only helpers and shared utility modules (dyna-nat, "
    "cutils)\n"
    "     are called by design; a function that envies a utility module is "
    "fine.\n"
    "     Only module pairs that carry real domain logic are findings.",
    r"""
    SELECT s.name, m.name AS own_module, m2.name AS envied_module,
        e.cross AS cross_edges, COALESCE(o.own,0) AS own_edges,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN modules m ON m.id = s.module_id
    JOIN files f ON f.id = s.file_id
    JOIN (SELECT e.caller_id AS cid, s2.module_id AS tmod, COUNT(*) AS cross
          FROM edges e JOIN symbols s2 ON s2.id = e.callee_id
          WHERE s2.module_id IS NOT NULL AND e.same_module = 0
          GROUP BY e.caller_id, s2.module_id) e ON e.cid = s.id
    JOIN modules m2 ON m2.id = e.tmod
    LEFT JOIN (SELECT caller_id, COUNT(*) AS own FROM edges
               WHERE same_module = 1 GROUP BY caller_id) o
           ON o.caller_id = s.id
    WHERE s.kind = 'function' AND e.cross >= 4
      AND e.cross >= 2 * COALESCE(o.own, 0) + 2
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY e.cross DESC LIMIT :lim"""),
(
    "shotgun-surgery",
    "Callees with fan-in spread over the most files",
    "ANSWERS the functions whose signature or contract change ripples into "
    "the"
    "\n"
    "     most translation units: the static proxy for shotgun surgery.\n"
    "ACT wrap in an accessor or push into the owning module BEFORE the next\n"
    "     contract change; each row is a change-amplifier.\n"
    "MISLEADS fan-in breadth is not churn: a stable, never-changing helper\n"
    "     with 20 call sites is a good abstraction, not a smell. Judge by\n"
    "     whether the contract is still evolving (markers nearby).",
    r"""
    SELECT s.name, COUNT(DISTINCT f2.id) AS file_breadth,
        s.fan_in, m.name AS module,
        f.path || ':' || s.line_start AS at
    FROM edges e
    JOIN symbols s ON s.id = e.callee_id
    JOIN symbols sc ON sc.id = e.caller_id
    JOIN files f2 ON f2.id = sc.file_id
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE COALESCE(m.name,'') LIKE :mod
    GROUP BY s.id
    HAVING file_breadth >= 8
    ORDER BY file_breadth DESC, s.fan_in DESC LIMIT :lim"""),
(
    "god-functions",
    "Huge AND complex AND central: the waiver-audit list",
    "ANSWERS which functions are simultaneously large, deep and depended-"
    "upon."
    "\n"
    "     The repo rule: some functions are CORRECTLY enormous (a bytecode\n"
    "     dispatch loop, GC machinery); everything else is an "
    "Extract-Function\n"
    "     target, and the test is whether a sub-pass extracts without an "
    "asm diff.\n"
    "ACT exempt the dispatch loops BY NAME, then rank the rest by "
    "cyclomatic\n"
    "     x sloc and decompose.\n"
    "MISLEADS a 500-line switch is often fine; a 300-line if-chain is not. "
    "The\n"
    "     scanner cannot tell them apart -- n_switch is the discriminator.",
    r"""
    SELECT s.name, s.sloc, s.cyclomatic, s.n_switch, s.fan_in,
        s.n_returns, f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function' AND s.sloc >= 300 AND s.cyclomatic >= 25
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.cyclomatic * s.sloc DESC LIMIT :lim"""),
(
    "middle-men",
    "Exported one-line forwarders called by many",
    "ANSWERS pure pass-throughs on the API surface: short, trivial, "
    "widely"
    "\n"
    "     called, and forwarding to exactly one callee.\n"
    "ACT public ABI wrappers (JS_* forwarding to static internals) are\n"
    "     intentional middle men and exempt BY DESIGN; the finding is "
    "only\n"
    "     actionable when caller and callee share a module -- then the "
    "wrapper\n"
    "     adds a hop without a boundary.\n"
    "MISLEADS a wrapper that adds a check, a conversion or a log line is "
    "not"
    "\n"
    "     a middle man; this list over-includes on purpose.",
    r"""
    SELECT s.name, s.sloc, s.fan_in, s.fan_out, m.name AS module,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function' AND s.sloc <= 10 AND s.fan_out = 1
      AND s.cyclomatic <= 2 AND s.fan_in >= 2 AND s.is_static = 0
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.fan_in DESC LIMIT :lim"""),
(
    "dead-exported-api",
    "Non-static functions nothing in the tree references",
    "ANSWERS the exported surface with no internal caller, no address "
    "taken,"
    "\n"
    "     and no transitive caller: either JS-facing only (registered "
    "through"
    "\n"
    "     JS_CFUNC_DEF lists the scanner cannot see) or genuinely dead.\n"
    "ACT one grep of the registration tables per row settles it; dead rows "
    "are"
    "\n"
    "     free untrusted-surface removal.\n"
    "MISLEADS JS-facing natives are the expected majority -- the query "
    "exists"
    "\n"
    "     to make the few non-registered rows findable.",
    r"""
    SELECT s.name, s.sloc, s.n_calls, r.n_transitive,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    LEFT JOIN addr_taken at ON at.symbol_id = s.id
    LEFT JOIN reach r ON r.symbol_id = s.id
    WHERE s.kind = 'function' AND s.is_static = 0 AND s.fan_in = 0
      AND at.id IS NULL
      AND COALESCE(m.name,'') LIKE :mod
    ORDER BY s.sloc DESC LIMIT :lim"""),
(
    "hidden-coupling",
    "Cross-module function pairs sharing the most callees",
    "ANSWERS implicit coupling: two functions in different modules that "
    "call"
    "\n"
    "     the same set of helpers change together when any of those "
    "helpers"
    "\n"
    "     changes contract -- invisible in the module graph, visible in "
    "the\n"
    "     call graph (the static proxy for logical coupling).\n"
    "ACT pairs that hand-roll the same call sequence (JS_NewObject + "
    "SetProperty"
    "\n"
    "     runs) are both a duplication and a cascade risk: extract the "
    "shared\n"
    "     helper once.\n"
    "MISLEADS shared UTILITY callees (memcpy, JS_FreeValue) create spurious "
    "pairs;"
    "\n"
    "     require shared >= 4 so one common helper does not read as "
    "coupling.",
    r"""
    SELECT s1.name AS f1, s2.name AS f2,
        COUNT(DISTINCT e1.callee_id) AS shared,
        COALESCE(m1.name,'') AS mod1, COALESCE(m2.name,'') AS mod2
    FROM edges e1
    JOIN edges e2 ON e1.callee_id = e2.callee_id
                 AND e1.caller_id < e2.caller_id
    JOIN symbols s1 ON s1.id = e1.caller_id
    JOIN symbols s2 ON s2.id = e2.caller_id
    LEFT JOIN modules m1 ON m1.id = s1.module_id
    LEFT JOIN modules m2 ON m2.id = s2.module_id
    WHERE s1.module_id IS NOT NULL AND s2.module_id IS NOT NULL
      AND s1.module_id <> s2.module_id
      AND (COALESCE(m1.name,'') LIKE :mod OR COALESCE(m2.name,'') LIKE :mod)
    GROUP BY e1.caller_id, e2.caller_id
    HAVING shared >= 4
    ORDER BY shared DESC LIMIT :lim"""),
(
    "instability-distance",
    "Martin's instability I and distance D per module",
    "ANSWERS which modules are stable (I~0: everyone depends on them) and "
    "which"
    "\n"
    "     are unstable (I~1: they depend on everyone). With no abstract "
    "types"
    "\n"
    "     in C, D = |1 - I|: the D~0.5 band is the danger zone -- a module "
    "with"
    "\n"
    "     heavy in- AND out-edges is the change-preventer hub.\n"
    "ACT I=0 with D=1 (the core runtime) means every change to it is "
    "shotgun\n"
    "     surgery; its only relief is an interface boundary (fnptr table). "
    "D~0.5\n"
    "     modules need a split, not more edits.\n"
    "MISLEADS edge counts are static call sites; a module reached only "
    "through"
    "\n"
    "     function pointers reads stabler than it is.",
    r"""
    SELECT m.name,
        COALESCE(ca.n,0) AS afferent,
        COALESCE(ce.n,0) AS efferent,
        CASE WHEN COALESCE(ca.n,0)+COALESCE(ce.n,0)=0 THEN 0
             ELSE CAST(ROUND(100.0*COALESCE(ce.n,0)
                        /(COALESCE(ca.n,0)+COALESCE(ce.n,0)),0) AS INT)
        END AS instability_pct,
        CASE WHEN COALESCE(ca.n,0)+COALESCE(ce.n,0)=0 THEN 0
             ELSE CAST(ROUND(ABS(100.0*COALESCE(ce.n,0)
                       /(COALESCE(ca.n,0)+COALESCE(ce.n,0)) - 100.0),0)
                       AS INT)
        END AS distance_pct
    FROM modules m
    LEFT JOIN (SELECT s.module_id AS mid,
                      COUNT(DISTINCT s2.module_id) AS n
               FROM edges e
               JOIN symbols s ON s.id = e.caller_id
               JOIN symbols s2 ON s2.id = e.callee_id
               WHERE s.module_id IS NOT NULL AND s2.module_id IS NOT NULL
                 AND s.module_id <> s2.module_id
               GROUP BY s.module_id) ce ON ce.mid = m.id
    LEFT JOIN (SELECT s2.module_id AS mid,
                      COUNT(DISTINCT s.module_id) AS n
               FROM edges e
               JOIN symbols s ON s.id = e.caller_id
               JOIN symbols s2 ON s2.id = e.callee_id
               WHERE s.module_id IS NOT NULL AND s2.module_id IS NOT NULL
                 AND s.module_id <> s2.module_id
               GROUP BY s2.module_id) ca ON ca.mid = m.id
    WHERE m.name LIKE :mod
    ORDER BY distance_pct ASC, instability_pct ASC LIMIT :lim"""),
(
    "marker-clusters",
    "TODO/FIXME/HACK density per module: where the rot lives",
    "ANSWERS which modules accumulate the deferred-work markers; density "
    "is"
    "\n"
    "     the honest 'this is not finished' signal, and it concentrates "
    "where\n"
    "     the risky parsers live.\n"
    "ACT rank by markers BEFORE starting new work in a module: the "
    "existing\n"
    "     debt is part of the effort estimate.\n"
    "MISLEADS a stale marker stays forever; density alone does not say how "
    "old"
    "\n"
    "     the debt is.",
    r"""
    SELECT COALESCE(m.name,'(no module)') AS module, COUNT(*) AS markers,
        SUM(mk.kind='TODO') AS todo,
        SUM(mk.kind='FIXME') AS fixme,
        SUM(mk.kind='HACK') AS hack,
        COUNT(DISTINCT f.id) AS files
    FROM markers mk
    JOIN files f ON f.id = mk.file_id
    LEFT JOIN modules m ON m.id = f.module_id
    WHERE COALESCE(m.name,'') LIKE :mod
    GROUP BY m.id
    ORDER BY markers DESC LIMIT :lim"""),
(
    "cstring-leak-risk",
    "JS_ToCString/JS_ToCStringLen without matching JS_FreeCString on exit",
    "ANSWERS where C string conversion buffers allocated by the engine\n"
    "     are leaked on early return paths or missing cleanup. In QuickJS,\n"
    "     every `JS_ToCString*` MUST be paired with `JS_FreeCString`.\n"
    "ACT audit the non-paired return paths; ensure `JS_FreeCString` is\n"
    "     invoked before every return or error exit, or the pointer is\n"
    "     explicitly passed to an owned out-parameter (e.g. `powned`).\n"
    "MISLEADS functions that return `const char *` or hand ownership to an\n"
    "     out-parameter (e.g. `dyn_crypto_data`) show a positive difference\n"
    "     by design. Check whether the pointer escapes.",
    r"""
    SELECT s.name,
        COALESCE(tc.n, 0) AS to_cstring,
        COALESCE(fc.n, 0) AS free_cstring,
        s.n_returns, s.return_type,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
          WHERE name IN ('JS_ToCString', 'JS_ToCStringLen')
          GROUP BY caller_id) tc ON tc.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name = 'JS_FreeCString'
               GROUP BY caller_id) fc ON fc.caller_id = s.id
    WHERE s.kind = 'function'
      AND (fc.n IS NULL OR tc.n > fc.n)
      AND s.return_type NOT LIKE '%const char%'
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY (COALESCE(tc.n,0) - COALESCE(fc.n,0)) DESC, s.n_returns DESC
    LIMIT :lim"""),
(
    "atom-leak-risk",
    "JS_NewAtom* calls with fewer JS_FreeAtom releases",
    "ANSWERS functions that allocate runtime atom table entries without\n"
    "     releasing them. Atoms are reference-counted in QuickJS and\n"
    "     leaked atoms stay permanently in the runtime atom table.\n"
    "ACT ensure every `JS_NewAtom*` has a corresponding `JS_FreeAtom` on\n"
    "     error paths, or is passed to an API function that adopts/defines\n"
    "     the property.\n"
    "MISLEADS atoms passed to property lookup/definition APIs that consume\n"
    "     or store them internally may not need local `JS_FreeAtom`.",
    r"""
    SELECT s.name,
        COALESCE(na.n, 0) AS new_atom,
        COALESCE(fa.n, 0) AS free_atom,
        s.n_returns, s.return_type,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
          WHERE name LIKE 'JS_NewAtom%'
          GROUP BY caller_id) na ON na.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name = 'JS_FreeAtom'
               GROUP BY caller_id) fa ON fa.caller_id = s.id
    WHERE s.kind = 'function'
      AND (fa.n IS NULL OR na.n > fa.n)
      AND s.return_type NOT LIKE '%JSAtom%'
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY (COALESCE(na.n,0) - COALESCE(fa.n,0)) DESC, s.n_returns DESC
    LIMIT :lim"""),
(
    "prop-enum-leak-risk",
    "Property enumeration without JS_FreePropertyEnum cleanup",
    "ANSWERS functions calling `JS_GetPropertyNames` or `JS_GetOwnPropertyNames`\n"
    "     that fail to call `JS_FreePropertyEnum`. Calling raw `js_free` on\n"
    "     the `JSPropertyEnum` array frees the array buffer but LEAKS all\n"
    "     contained `JSAtom` entries.\n"
    "ACT replace manual `free`/`js_free` with `JS_FreePropertyEnum(ctx, tab, ntab)`\n"
    "     on all error and success exit paths.\n"
    "MISLEADS functions that manually loop over `tab[i].atom` with `JS_FreeAtom`\n"
    "     before calling `js_free` will appear here; prefer `JS_FreePropertyEnum`.",
    r"""
    SELECT s.name,
        COALESCE(gp.n, 0) AS get_props,
        COALESCE(fp.n, 0) AS free_props,
        s.n_returns,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
          WHERE name IN ('JS_GetPropertyNames', 'JS_GetOwnPropertyNames', 'JS_GetPropertyEnum')
          GROUP BY caller_id) gp ON gp.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name = 'JS_FreePropertyEnum'
               GROUP BY caller_id) fp ON fp.caller_id = s.id
    WHERE s.kind = 'function'
      AND (fp.n IS NULL OR gp.n > fp.n)
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY gp.n DESC, s.n_returns DESC LIMIT :lim"""),
(
    "unchecked-js-call",
    "JS_Call / JS_Eval invocations without JS_IsException check",
    "ANSWERS C functions executing JavaScript code (`JS_Call`, `JS_Eval`)\n"
    "     that never check if the execution raised a pending JS exception.\n"
    "     Unhandled exceptions stay on the JSContext and corrupt subsequent\n"
    "     operations.\n"
    "ACT check `if (JS_IsException(res))` immediately after the call and\n"
    "     either handle or propagate it to the caller.\n"
    "MISLEADS fire-and-forget logging or cleanup callbacks that explicitly\n"
    "     clear exceptions via other helpers.",
    r"""
    SELECT s.name,
        COALESCE(call.n, 0) AS js_calls,
        s.n_returns,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
          WHERE name IN ('JS_Call', 'JS_CallConstructor', 'JS_Eval', 'JS_EvalThis')
          GROUP BY caller_id) call ON call.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name IN ('JS_IsException', 'JS_GetException')
               GROUP BY caller_id) ex ON ex.caller_id = s.id
    WHERE s.kind = 'function'
      AND ex.n IS NULL
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY call.n DESC, s.n_returns DESC LIMIT :lim"""),
(
    "opaque-unvalidated",
    "JS_GetOpaque callers without error checking or JS_Throw",
    "ANSWERS functions calling `JS_GetOpaque` (which returns NULL without\n"
    "     throwing on class mismatch) and not invoking any `JS_Throw*`.\n"
    "     Dereferencing the returned pointer directly causes a NULL crash.\n"
    "ACT use `JS_GetOpaque2` (which throws a TypeError on mismatch) or\n"
    "     check `if (!ptr) return -1;` before dereferencing struct fields.\n"
    "MISLEADS helper functions whose caller already validated the class ID.",
    r"""
    SELECT s.name,
        COALESCE(op.n, 0) AS get_opaque_calls,
        s.n_returns,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
          WHERE name = 'JS_GetOpaque'
          GROUP BY caller_id) op ON op.caller_id = s.id
    LEFT JOIN (SELECT caller_id, SUM(n) AS n FROM dyn_api_calls
               WHERE name LIKE 'JS_Throw%'
               GROUP BY caller_id) th ON th.caller_id = s.id
    WHERE s.kind = 'function'
      AND th.n IS NULL
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY op.n DESC, s.n_returns DESC LIMIT :lim"""),
(
    "divergent-clones",
    "Near-duplicate functions across different files (structural clones)",
    "ANSWERS pairs of functions across different files with identical\n"
    "     sloc, cyclomatic complexity, parameter count, and branch count:\n"
    "     the static proxy for copy-paste code drift.\n"
    "ACT factor the duplicated logic into a shared helper in `dyna-nat.h`\n"
    "     or `cutils.h`.\n"
    "MISLEADS multi-architecture SIMD kernel variants (SSE4.2 vs NEON vs\n"
    "     AVX2) intentionally share exact structural signatures by design.",
    r"""
    SELECT s1.name AS f1, s2.name AS f2,
        s1.sloc, s1.cyclomatic AS cyclo, s1.n_params AS params,
        f1.path || ':' || s1.line_start AS at1,
        f2.path || ':' || s2.line_start AS at2
    FROM symbols s1
    JOIN symbols s2 ON s1.id < s2.id
      AND s1.file_id <> s2.file_id
      AND s1.kind = 'function' AND s2.kind = 'function'
      AND s1.sloc >= 20 AND s2.sloc >= 20
      AND s1.sloc = s2.sloc
      AND s1.cyclomatic = s2.cyclomatic
      AND s1.n_params = s2.n_params
      AND s1.n_returns = s2.n_returns
      AND s1.n_branches = s2.n_branches
    JOIN files f1 ON f1.id = s1.file_id
    JOIN files f2 ON f2.id = s2.file_id
    LEFT JOIN modules m1 ON m1.id = s1.module_id
    WHERE COALESCE(m1.name, '') LIKE :mod
    ORDER BY s1.sloc DESC, s1.cyclomatic DESC LIMIT :lim"""),
(
    "deeply-nested-control-flow",
    "Functions with deep nesting (depth >= 5) or high branch count (>= 20)",
    "ANSWERS control-flow hotspots that are hard to read and test.\n"
    "ACT apply early returns, guard clauses, or extract inner loop bodies;\n"
    "     for parsing state machines, evaluate table-driven dispatch (BOSCC).\n"
    "MISLEADS complex tokenizer/parser switch cases may legitimately have\n"
    "     many branches; check `n_switch` vs `max_nesting`.",
    r"""
    SELECT s.name, s.max_nesting AS nesting, s.n_branches AS branches,
        s.cyclomatic AS cyclo, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function'
      AND (s.max_nesting >= 5 OR s.n_branches >= 20)
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY s.max_nesting DESC, s.cyclomatic DESC LIMIT :lim"""),
(
    "stack-pressure-risk",
    "Functions with high local variable count (>= 15) and multiple sizeofs",
    "ANSWERS functions with heavy stack footprints from many local variables\n"
    "     and local buffers, risking stack overflow in recursive loops or\n"
    "     constrained thread stacks.\n"
    "ACT move large stack buffers into heap scratch structures or arena\n"
    "     allocators; reduce scope of temporary locals.\n"
    "MISLEADS compiler register allocation can optimize non-aliased scalar\n"
    "     locals into registers; arrays and structs are the real risk.",
    r"""
    SELECT s.name, s.n_locals AS locals, s.n_sizeof AS sizeofs,
        s.max_nesting AS nesting, s.sloc,
        f.path || ':' || s.line_start AS at
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function'
      AND s.n_locals >= 15
      AND s.n_sizeof >= 4
      AND COALESCE(m.name, '') LIKE :mod
    ORDER BY s.n_locals DESC, s.n_sizeof DESC LIMIT :lim"""),
]

DYN_METRICS: list[tuple[str, str, str, str]] = [
(
    "frontier-modules",
    "Per-module untrusted-frontier dashboard",
    "ANSWERS where the attack surface concentrates by module: functions\n"
    "     reachable from io, with memory/alloc hazard counts and the "
    "fnptr\n"
    "     blindness that makes every number a floor.\n"
    "ACT budget security review by frontier_fns, then by mem+alloc; every\n"
    "     parser module should appear here and its fuzz target should "
    "exist.\n"
    "MISLEADS io roots only count RAW descriptor calls in the module's own\n"
    "     functions -- a parser fed by a caller's read is reached through "
    "the\n"
    "     graph, so it IS counted here, but its io column stays 0.",
    _DYN_TAINTED_CTE + r"""
    SELECT COALESCE(m.name,'(no module)') AS module,
        COUNT(*) AS frontier_fns,
        COALESCE(SUM(s.n_io),0) AS io,
        COALESCE(SUM(s.n_memory),0) AS mem,
        COALESCE(SUM(s.n_alloc),0) AS alloc,
        COALESCE(SUM(s.n_fnptr_calls),0) AS fnptr_calls,
        COALESCE(SUM(s.n_unresolved_calls),0) AS unresolved
    FROM tainted t
    JOIN symbols s ON s.id = t.id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE m.name IS NOT NULL AND m.name LIKE :mod
    GROUP BY m.id
    ORDER BY frontier_fns DESC, mem DESC LIMIT :lim"""),
(
    "native-api-density",
    "Per-module JS API call density: who owns the runtime boundary",
    "ANSWERS which modules are the heaviest consumers of the JS runtime "
    "API\n"
    "     (JS_New*/SetProperty/FreeValue family): the modules whose native\n"
    "     methods do the most object plumbing are where the refcount and\n"
    "     prototype-pollution bugs concentrate.\n"
    "ACT cross-reference with refcount-leak-shape and define-vs-set per "
    "module.\n"
    "MISLEADS counts include JS_Throw* (every native method throws); the\n"
    "     ranking still holds because error paths are where the work is.",
    r"""
    SELECT m.name AS module,
        COUNT(DISTINCT s.id) AS fns,
        SUM(CASE WHEN s.is_static=0 THEN 1 ELSE 0 END) AS exported,
        SUM(s.n_fnptr_calls) AS fnptr_calls,
        SUM((SELECT COALESCE(SUM(d.n),0) FROM dyn_api_calls d
             WHERE d.caller_id = s.id)) AS js_api_calls
    FROM symbols s
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function' AND m.name IS NOT NULL AND m.name LIKE :mod
    GROUP BY m.id
    ORDER BY js_api_calls DESC LIMIT :lim"""),
(
    "refactoring-hotspots",
    "Per-module refactoring hotspots (god functions, deep nesting, complexity)",
    "ANSWERS which modules have the highest concentration of complex,\n"
    "     deeply nested, and oversized functions that need decomposition.\n"
    "ACT prioritize refactoring sprints by god_fns count and total_cyclo.\n"
    "MISLEADS core interpreter modules legitimately have high complexity.",
    r"""
    SELECT COALESCE(m.name, '(root)') AS module,
        COUNT(DISTINCT s.id) AS fns,
        SUM(CASE WHEN s.sloc >= 200 AND s.cyclomatic >= 20 THEN 1 ELSE 0 END) AS god_fns,
        SUM(CASE WHEN s.max_nesting >= 5 THEN 1 ELSE 0 END) AS deep_nesting,
        SUM(s.cyclomatic) AS total_cyclo,
        ROUND(AVG(s.cyclomatic), 1) AS avg_cyclo
    FROM symbols s
    JOIN files f ON f.id = s.file_id
    LEFT JOIN modules m ON m.id = s.module_id
    WHERE s.kind = 'function'
      AND COALESCE(m.name, '') LIKE :mod
    GROUP BY m.id
    ORDER BY god_fns DESC, total_cyclo DESC LIMIT :lim"""),
(
    "clone-density",
    "Per-module structural clone density",
    "ANSWERS which modules contain the most duplicate function structures\n"
    "     that could be consolidated into common utility helpers.\n"
    "ACT extract shared patterns into `dyna-nat.h` or module-internal helpers.\n"
    "MISLEADS SIMD architecture variants naturally appear as clone pairs.",
    r"""
    SELECT COALESCE(m1.name, '(root)') AS module,
        COUNT(*) AS clone_pairs,
        COUNT(DISTINCT s1.id) AS cloned_fns
    FROM symbols s1
    JOIN symbols s2 ON s1.id < s2.id
      AND s1.file_id <> s2.file_id
      AND s1.kind = 'function' AND s2.kind = 'function'
      AND s1.sloc >= 20 AND s2.sloc >= 20
      AND s1.sloc = s2.sloc
      AND s1.cyclomatic = s2.cyclomatic
      AND s1.n_params = s2.n_params
      AND s1.n_returns = s2.n_returns
      AND s1.n_branches = s2.n_branches
    LEFT JOIN modules m1 ON m1.id = s1.module_id
    WHERE COALESCE(m1.name, '') LIKE :mod
    GROUP BY m1.id
    ORDER BY clone_pairs DESC LIMIT :lim"""),
]

# ==========================================================================
# END DYN PROJECT QUERY CATALOGUE
# ==========================================================================

CAnalyzer.QUERIES = QUERIES + DYN_QUERIES
CAnalyzer.METRICS = METRICS + DYN_METRICS



ANALYZER = CAnalyzer()


if __name__ == "__main__":
    try:
        sys.exit(main(ANALYZER))
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
