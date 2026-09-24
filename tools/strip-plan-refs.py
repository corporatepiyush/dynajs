#!/usr/bin/env python3
"""
strip-plan-refs.py -- find and remove plan/issue/ticket number references
from COMMENTS and DOC PROSE.

Targets (the noise this exists to kill):
    CL-2  DT-13  CC-6  RE-2..RE-14  HS-2..4  CY-2..4/6..8  RD-*
    E6-T1  T-UDP-SELF-CLOSE-UAF  ticket 16  work22  SS18  §36.6  §I.1
    plan item 3  (CC-6):  (RE-9).  (BY-4 -- read this ...)  (DT-8, resolved)

Never touches: code, string literals, template literal text, comment
delimiters, fenced code blocks, inline code spans, link destinations,
technical tokens (UTF-8, SHA-256, HMAC-SHA-256, AES-256, ISO-4217, P-256,
X-509, (--std), (-1), ES2025, RFC 9106, ...).

SAFETY MODEL (why --apply is safe):
  1. Only comment interiors and markdown prose are mutable. Every region the
     lexer marks immutable (code + strings + comment delimiters) is protected
     by a byte-level check: after editing, the file is re-lexed and the
     concatenation of all immutable spans must be identical to the original.
     A file that fails is left untouched and reported as an error. This
     catches pathological deletions such as text that merges '*' + '/' into a
     comment terminator, and any edit whose surroundings get reclassified.
  2. Patterns cannot cross lines (they use [ \t], never \\s with newlines) and
     cannot match comment delimiters (//, /*, */, #).
  3. Tidy-up runs only on lines that were edited, and never rewrites a
     comment line's leader (the '*' prefix of block-comment lines).
  4. REPORT is the default mode. --apply writes; --check is the gate form.

PROCESS-LEDGER FILES are skipped by default (they are the source of these
numbers, not product surface): AGENT.md, CLAUDE.md, profile-audit.md,
API_UPGRADE_PLAN.md, SECURITY_COMPAT_PLAN.md, CHANGELOG*, Changelog,
findings-ml.md, progress.log, LAUNCH_RECORD.md, *.patch, *.log, plus
.agent-work/, scratch/, audit/, tmp_audit/, third_party/, test262/ trees.
Use --include-process to scan them too.

REVIEW-ONLY PATTERNS (reported, never removed): ambiguous shapes that are
also ordinary language -- "phase 2" (a compiler term here too), "x1"
(a variable), "L1"/"M1" (cache/chip), "item 0" (array indexing), lane
codenames. A human decides on those.

Usage:
    tools/strip-plan-refs.py                 # report (default, no writes)
    tools/strip-plan-refs.py --check         # gate: exit 1 if removable text exists
    tools/strip-plan-refs.py --apply         # remove (per-file safety check)
    tools/strip-plan-refs.py --selftest      # prove the patterns and the guard rails
    tools/strip-plan-refs.py --include-process --md-code paths...   # wider sweeps

KNOWN LIMITATION: text inside JS template-literal interpolations ${...} is
treated as opaque (comments in there are not scanned). Completeness comes
back through --check on the final tree; safety is never traded for it.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Callable, Iterable, List, Optional, Sequence, Tuple

Region = Tuple[str, int, int]  # (kind, start, end); kind: code | string | mutable

# --------------------------------------------------------------------------
# pattern vocabulary
# --------------------------------------------------------------------------

VOCAB = (
    "CN", "DT", "CC", "RE", "NS", "BY", "EC", "HS", "CY", "CP", "RD", "UD",
    "CV", "DC", "TM", "SD", "NT", "HP", "UL", "HL", "SP", "SY", "FL", "CL",
    "XM", "YA", "JS", "ST", "DF", "ML", "SC", "CG", "LG", "MT", "MX", "SV",
    "SZ", "VL", "OA", "UI", "GB",
)
VOC = "(?:" + "|".join(VOCAB) + ")"
NUM = r"\d{1,2}"
RANGE = rf"(?:\.\.(?:{VOC}-)?{NUM})"
SEG = rf"(?:[/,][ \t]*(?:{VOC}-)?{NUM}(?:{RANGE})*|{RANGE})*"
ITEM_EXPR = (
    rf"(?<![\w-]){VOC}-{NUM}(?:{RANGE})*{SEG}(?![\w-])"
    rf"|(?<![\w-]){VOC}-\*(?![\w-])"
)
E6T = r"(?<![\w-])E\d+-T\d+(?![\w-])"
TICKET_T = r"(?<![\w-])T-(?:[0-9]+|[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*)(?![\w-])"
WORK = r"(?<![\w-])work\d{1,3}(?![\w-])"
SECTION = (
    r"§\d+(?:\.\d+)*(?:[ \t]*(?:items?[ \t]+\d+[a-z]?(?:[ \t]*(?:,|and|&|-)[ \t]*\d+[a-z]?)*)|[-–]\d+[a-z]?)?"
    r"|§[IVXLC]+(?:\.\d+)*"
    r"|(?<![\w-])SS\d+(?:\.\d+)*(?![\w-])"
)
CTX_WORD = r"(?:ticket|issue|defect|finding|bug|checklist)"
CTX = rf"(?<![\w-]){CTX_WORD}s?[ \t]*#?[ \t]*(?:{E6T}|{TICKET_T}|\d+[a-z]?)(?![\w-])"
CTX2 = rf"(?<![\w-])(?:plan|audit|queue|roadmap)[ \t]+items?[ \t]+(?:{ITEM_EXPR}|\d+[a-z]?(?:[-–]\d+[a-z]?)?)"

STRONG_PATTERNS: Sequence[re.Pattern] = tuple(
    re.compile(p) for p in (
        SECTION,   # longest first: "§36.6 items 1-7" before bare numbers
        CTX2,
        CTX,
        ITEM_EXPR,
        E6T,
        TICKET_T,
        WORK,
    )
)

# a parenthetical that contains ONLY item references -> drop the whole group
PURE_PAREN = re.compile(
    rf"[ \t]?\(\s*(?:{ITEM_EXPR})(?:[ \t]*[,;][ \t]*(?:{ITEM_EXPR}))*[ \t]*\)"
)

# never remove these even if a pattern would match inside them
WHITELIST = re.compile(
    r"(?:HMAC-)?(?:SHA-?\d{0,3}|SHA3-\d{2,3}|SHAKE\d{2,3}|AES-\d{2,3}"
    r"|UTF-\d{1,2}|UCS-\d{1,2}|ISO-[\d-]+|RSA-\d{2,4}|P-\d{3}|X-?509"
    r"|ARC4-\d{1,2}|SEED-\d{0,2}|GOST-?\d*|3?DES-\d{0,3}|ChaCha\d*|Poly\d{3})"
)

# ambiguous shapes: report for a human, never touch
REVIEW_PATTERNS: Sequence[Tuple[str, re.Pattern]] = tuple(
    (name, re.compile(p))
    for name, p in (
        ("phase-n", r"(?<![\w-])[Pp]hase[ \t]+\d+"),
        ("letter-digit-id", r"(?<![\w-])[FCNRMHDTSGPE]\d{1,2}(?![\w-])"),
        ("x-finding", r"(?<![\w-])[xX][1-9](?![\w-])"),
        ("bare-item", r"(?<![\w-])(?:item|step|bullet|point|entry)[ \t]+\d+[a-z]?(?![\w-])"),
        ("lane-codename", r"(?<![\w-])(?:p0-dts|a\d-[a-z0-9][a-z0-9-]*|ns\d-stream"
                          r"|b\d-[a-z0-9][a-z0-9-]*|wc\d?-?[a-z0-9][a-z0-9-]*"
                          r"|d\d-[a-z0-9][a-z0-9-]*|e\d-[a-z0-9][a-z0-9-]*"
                          r"|fix-[a-z0-9][a-z0-9-]*|make-testmod)(?![\w-])"),
    )
)

# --------------------------------------------------------------------------
# tidy rules -- applied ONLY to lines whose text was edited
# --------------------------------------------------------------------------

TIDY_RULES: Sequence[Tuple[re.Pattern, str]] = (
    (re.compile(r"\([ \t]*\)"), ""),                       # empty parens
    (re.compile(r"\([ \t]*[,;:][ \t]*"), "("),            # (, resolved) -> (resolved)
    (re.compile(r"\([ \t]*(?:--|[-–—])[ \t]+"), "("),   # ( -- read this -> (read this
    (re.compile(r"\([ \t]+"), "("),                       # ( BEHAVIOR -> (BEHAVIOR
    (re.compile(r"[ \t]*[,;:][ \t]*\)"), ")"),
    (re.compile(r"[ \t]+(?:--|[-–—])[ \t]*\)"), ")"),
    (re.compile(r"^[ \t]*(?:--|—)[ \t]+"), ""),          # body-leading gloss dash; a single "-" is a list bullet and stays
    (re.compile(r"^[ \t]*[,;:][ \t]+"), ""),            # label-punct residue: "(SD-5): the..." -> "the..." at body start
    (re.compile(r"[ \t]{2,}"), " "),                      # double spaces
    (re.compile(r"[ \t]+([,;:])"), r"\1"),
)

BLOCK_LEADER = re.compile(r"^([ \t]*\*[ \t]*)(.*)$")

# --------------------------------------------------------------------------
# lexers: split text into code | string | mutable (comment/prose) regions
# --------------------------------------------------------------------------


def _merge_code(regions: List[Region], kind: str, start: int, end: int) -> None:
    if end <= start:
        return
    if regions and regions[-1][0] == kind and kind == "code" and regions[-1][2] == start:
        regions[-1] = (kind, regions[-1][1], end)
    else:
        regions.append((kind, start, end))


def scan_c_family(
    text: str,
    line_comment: str = "//",
    block_comment: Optional[Tuple[str, str]] = ("/*", "*/"),
    quotes: str = "\"'",
    template: bool = False,
) -> List[Region]:
    regions: List[Region] = []
    n = len(text)
    i = 0
    while i < n:
        ch = text[i]
        if line_comment and text.startswith(line_comment, i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            regions.append(("code", i, i + len(line_comment)))
            regions.append(("mutable", i + len(line_comment), j))
            i = j
            continue
        if block_comment and text.startswith(block_comment[0], i):
            close = text.find(block_comment[1], i + len(block_comment[0]))
            close_end = n if close < 0 else close + len(block_comment[1])
            regions.append(("code", i, i + len(block_comment[0])))
            regions.append(("mutable", i + len(block_comment[0]), close_end - len(block_comment[1])))
            i = close_end
            continue
        if ch in quotes:
            q = ch
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == q:
                    j += 1
                    break
                j += 1
            regions.append(("string", i, min(j, n)))
            i = min(j, n)
            continue
        if template and ch == "`":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "`":
                    j += 1
                    break
                j += 1
            regions.append(("string", i, min(j, n)))
            i = min(j, n)
            continue
        # plain code char (coalesced at the end)
        j = i
        while j < n:
            c = text[j]
            if c in quotes or (template and c == "`"):
                break
            if line_comment and text.startswith(line_comment, j):
                break
            if block_comment and text.startswith(block_comment[0], j):
                break
            j += 1
        _merge_code(regions, "code", i, j)
        i = j
    return _coalesce(regions, text)


def scan_hash_comments(text: str, word_start: bool = False, recipe_tabs: bool = False) -> List[Region]:
    """'#'-comment languages (python/sh/make/yaml/toml). Strings are opaque."""
    regions: List[Region] = []
    n = len(text)
    i = 0
    line_start = 0
    while i < n:
        ch = text[i]
        if ch == "\n":
            i += 1
            line_start = i
            continue
        if recipe_tabs and ch == "\t" and i == line_start:
            j = text.find("\n", i)
            j = n if j < 0 else j
            regions.append(("code", i, j))  # make recipe line: opaque shell
            i = j
            continue
        if ch == "#" and (
            not word_start
            or i == line_start
            or text[i - 1] in " \t;&|("
        ) and not (i > 0 and text[i - 1] == "\\"):
            j = text.find("\n", i)
            j = n if j < 0 else j
            regions.append(("code", i, i + 1))
            regions.append(("mutable", i + 1, j))
            i = j
            continue
        if ch in "\"'":
            q = ch
            if text.startswith(q * 3, i):
                close = text.find(q * 3, i + 3)
                end = n if close < 0 else close + 3
                regions.append(("string", i, end))
                i = end
                continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == q or text[j] == "\n":
                    j += 1
                    break
                j += 1
            regions.append(("string", i, min(j, n)))
            i = min(j, n)
            continue
        j = i
        while j < n:
            c = text[j]
            if c == "\n" or c in "\"'":
                break
            if c == "#" and (
                not word_start
                or j == line_start
                or text[j - 1] in " \t;&|("
            ) and not (text[j - 1] == "\\"):
                break
            j += 1
        _merge_code(regions, "code", i, j)
        i = j
    return _coalesce(regions, text)


_FENCE = re.compile(r"^[ \t]{0,3}(```+|~~~+)", re.M)
_INLINE_CODE = re.compile(r"(`+)[^`\n]*?\1")
_LINK_DEST = re.compile(r"\]\([^)\n]*\)|<https?://[^>\s]*>")
_HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)


def scan_markdown(text: str, include_code: bool = False) -> List[Region]:
    """Markdown: prose is mutable; fenced blocks / inline code / link
    destinations are immutable (API.md examples are executed by the gates)."""
    # fenced code blocks (a closing fence is the next fence-marker line)
    cands: List[Tuple[int, int, str]] = []  # (start, end, kind)
    pos = 0
    while True:
        m = _FENCE.search(text, pos)
        if not m:
            break
        close = _FENCE.search(text, m.end())
        end = close.end() if close else len(text)
        cands.append((m.start(), end, "mutable" if include_code else "string"))
        pos = end

    if not include_code:
        for m in _INLINE_CODE.finditer(text):
            cands.append((m.start(), m.end(), "string"))
    for m in _LINK_DEST.finditer(text):
        cands.append((m.start(), m.end(), "string"))

    # resolve overlaps linearly: spans sorted by start that are pairwise
    # disjoint mean only the last accepted span can overlap the next candidate
    spans: List[Tuple[int, int, str]] = []
    for s, e, kind in sorted(cands, key=lambda c: (c[0], -(c[1] - c[0]))):
        if spans and s < spans[-1][1]:
            continue  # overlaps the previously accepted span
        spans.append((s, e, kind))
    spans.sort()
    regions: List[Region] = []
    pos = 0
    for s, e, kind in spans:
        if s > pos:
            regions.append(("mutable", pos, s))
        regions.append((kind, s, e))
        pos = e
    if pos < len(text):
        regions.append(("mutable", pos, len(text)))
    return _coalesce(regions, text)


def _coalesce(regions: List[Region], text: str) -> List[Region]:
    """Merge adjacent same-kind regions and verify full, ordered coverage."""
    out: List[Region] = []
    for kind, s, e in regions:
        if e <= s and kind != "string":
            continue
        if out and out[-1][0] == kind and out[-1][2] == s:
            out[-1] = (kind, out[-1][1], e)
        else:
            out.append((kind, s, e))
    # fill any gaps as code (defensive: a lexer hole must never become mutable)
    filled: List[Region] = []
    pos = 0
    for kind, s, e in out:
        if s > pos:
            filled.append(("code", pos, s))
        filled.append((kind, max(s, pos), e))
        pos = max(pos, e)
    if pos < len(text):
        filled.append(("code", pos, len(text)))
    return filled


LEXERS: Sequence[Tuple[str, Callable[[str], List[Region]]]] = (
    (".c", lambda t: scan_c_family(t)),
    (".h", lambda t: scan_c_family(t)),
    (".inc", lambda t: scan_c_family(t)),
    (".js", lambda t: scan_c_family(t, template=True)),
    (".mjs", lambda t: scan_c_family(t, template=True)),
    (".cjs", lambda t: scan_c_family(t, template=True)),
    (".ts", lambda t: scan_c_family(t, template=True)),
    (".tsx", lambda t: scan_c_family(t, template=True)),
    (".py", lambda t: scan_hash_comments(t)),
    (".sh", lambda t: scan_hash_comments(t, word_start=True)),
    (".bash", lambda t: scan_hash_comments(t, word_start=True)),
    (".mk", lambda t: scan_hash_comments(t, recipe_tabs=True)),
    (".yml", lambda t: scan_hash_comments(t, word_start=True)),
    (".yaml", lambda t: scan_hash_comments(t, word_start=True)),
    (".toml", lambda t: scan_hash_comments(t, word_start=True)),
    (".md", lambda t: scan_markdown(t)),
    (".txt", lambda t: [("mutable", 0, len(t))]),  # prose
)

NAMED_LEXERS = {
    "Makefile": lambda t: scan_hash_comments(t, recipe_tabs=True),
    "makefile": lambda t: scan_hash_comments(t, recipe_tabs=True),
    "GNUmakefile": lambda t: scan_hash_comments(t, recipe_tabs=True),
    "Dockerfile": lambda t: scan_hash_comments(t, word_start=True),
}

SKIP_DIRS = {
    ".git", ".agent-work", ".obj", ".swarm", "tmp_audit", "third_party",
    "test262", "pgo-data", "node_modules", "__pycache__", "scratch", "audit",
    "core",  # crash dump
}
SKIP_DIR_SUFFIXES = (".dSYM",)
SKIP_FILES = {
    "AGENT.md", "CLAUDE.md", "profile-audit.md", "API_UPGRADE_PLAN.md",
    "SECURITY_COMPAT_PLAN.md", "Changelog", "findings-ml.md", "progress.log",
    "LAUNCH_RECORD.md", "heartbeat",
    # the stripper's own docs/selftest quote target patterns as EXAMPLES;
    # stripping them would gut the tool's documentation
    "strip-plan-refs.py",
}
SKIP_FILE_PREFIXES = ("CHANGELOG",)
SKIP_FILE_SUFFIXES = (".patch", ".log", ".profdata", ".a", ".dylib", ".so")
PROCESS_DIR_PARTS = (os.path.join(".agent-work"),)

SCAN_SUFFIXES = {ext for ext, _ in LEXERS}
TEXT_NAMES = set(NAMED_LEXERS)


# --------------------------------------------------------------------------
# per-line cleaning of one mutable region
# --------------------------------------------------------------------------


class Hit:
    def __init__(self, path: str, line: int, category: str, matched: str,
                 before: str, after: str):
        self.path, self.line, self.category = path, line, category
        self.matched, self.before, self.after = matched, before, after


def _strong_spans(body: str) -> List[Tuple[int, int, str]]:
    """Spans to delete, whitelist-protected, deduped, ordered."""
    found: List[Tuple[int, int, str]] = []
    for rx in STRONG_PATTERNS:
        for m in rx.finditer(body):
            found.append((m.start(), m.end(), m.group(0)))
    # whole-paren pure-id groups win over inner token deletes
    for m in PURE_PAREN.finditer(body):
        found.append((m.start(), m.end(), m.group(0)))
    found.sort()
    # drop spans contained in a longer span (the paren groups / section phrases)
    kept: List[Tuple[int, int, str]] = []
    for s, e, g in found:
        if any(a <= s and e <= b for a, b, _ in kept):
            continue
        kept.append((s, e, g))
    # whitelist + overlap resolution left-to-right
    out: List[Tuple[int, int, str]] = []
    for s, e, g in kept:
        if any(a < e and s < b for a, b, _ in out):
            continue
        win_start = max(0, s - 24)
        win = body[win_start:e + 24]
        blocked = False
        wm = WHITELIST.search(win)
        while wm:
            if wm.start() + win_start < e and s < wm.end() + win_start:
                blocked = True
                break
            wm = WHITELIST.search(win, wm.end())
        if blocked:
            continue
        out.append((s, e, g))
    return out


def clean_body(body: str, hits: List[Tuple[str, str, str]]) -> str:
    """body is one line of comment/prose text (no line terminator).
    hits: mutable list collecting (category, matched, before)."""
    spans = _strong_spans(body)
    review_seen = set()
    for name, rx in REVIEW_PATTERNS:
        for m in rx.finditer(body):
            if any(s <= m.start() and m.end() <= e for s, e, _ in spans):
                continue
            key = (name, m.group(0))
            if key not in review_seen:
                review_seen.add(key)
                hits.append(("review:" + name, m.group(0), body))

    if not spans:
        return body

    # sentence-final rule: "(RE-9)." after text that already ends in .!? ->
    # the trailing period belonged to the parenthetical; drop it too.
    drop: List[Tuple[int, int]] = [(s, e) for s, e, _ in spans]
    for s, e, g in spans:
        if g.lstrip().startswith("(") and e < len(body) and body[e] == ".":
            before = body[:s].rstrip()
            if before and before[-1] in ".!?":
                drop.append((e, e + 1))

    out = []
    pos = 0
    for s, e in sorted(drop):
        if s < pos:
            continue
        out.append(body[pos:s])
        pos = e
    out.append(body[pos:])
    new_body = "".join(out)

    if new_body != body:
        for s, e, g in spans:
            hits.append(("remove", g, body))
        # whitespace residue from the deletions themselves (only when the
        # line did not begin/end in whitespace before)
        if body[:1] not in (" ", "\t") and new_body[:1] in (" ", "\t"):
            new_body = new_body.lstrip(" \t")
        if body[-1:] not in (" ", "\t") and new_body[-1:] in (" ", "\t"):
            new_body = new_body.rstrip(" \t")
        for rx, repl in TIDY_RULES:
            new_body = rx.sub(repl, new_body)
    return new_body


def clean_region(text: str, start: int, end: int, block: bool,
                 path: str, first_line: int, hits: List[Hit]) -> Tuple[str, int]:
    """Clean one mutable region. For block comments, per-line '*' leaders are
    preserved byte-for-byte. Returns (new_text, edit_count)."""
    seg = text[start:end]
    parts = seg.split("\n")
    out_parts = []
    edits = 0
    for idx, raw in enumerate(parts):
        has_cr = raw.endswith("\r")
        line = raw[:-1] if has_cr else raw
        line_no = first_line + idx
        leader, body = "", line
        if block and idx > 0:
            m = BLOCK_LEADER.match(line)
            if m:
                leader, body = m.group(1), m.group(2)
        local: List[Tuple[str, str, str]] = []
        new_body = clean_body(body, local)
        if new_body != body:
            edits += 1
            for category, matched, before in local:
                hits.append(Hit(path, line_no, category, matched, before, new_body))
        out_parts.append(leader + new_body + ("\r" if has_cr else ""))
    return "\n".join(out_parts), edits


# --------------------------------------------------------------------------
# immutable-skeleton safety check
# --------------------------------------------------------------------------


def skeleton(text: str, regions: Sequence[Region]) -> bytes:
    return "".join(text[s:e] for kind, s, e in regions if kind != "mutable").encode("utf-8")


def structure(regions: Sequence[Region]) -> Tuple:
    return tuple(kind for kind, _, _ in regions)


def process_text(path: str, text: str, lexer: Callable[[str], List[Region]],
                 hits: List[Hit]) -> Optional[str]:
    """Return edited text, or None when nothing changed or validation failed."""
    regions = lexer(text)
    skel_before = skeleton(text, regions)

    pieces: List[str] = []
    pos = 0
    total_edits = 0
    line_no = 1
    scan_from = 0
    for kind, s, e in regions:
        line_no += text.count("\n", scan_from, s)  # incremental: O(n) per file
        scan_from = e
        pieces.append(text[pos:s])
        if kind == "mutable":
            block = text[s:e].find("*") >= 0 and ("\n" in text[s:e])
            new_seg, edits = clean_region(text, s, e, block, path, line_no, hits)
            pieces.append(new_seg)
            total_edits += edits
        else:
            pieces.append(text[s:e])
        line_no += text.count("\n", s, e)
        pos = e
    pieces.append(text[pos:])
    new_text = "".join(pieces)

    if new_text == text:
        return None

    # safety: re-lex and demand a byte-identical immutable skeleton (code +
    # strings + comment delimiters). This catches a deletion that forges a
    # comment terminator, or any edit whose text gets reclassified as
    # string/code. Region-structure equality is deliberately NOT required:
    # deleting a whole prose gap merely merges adjacent protected spans.
    regions2 = lexer(new_text)
    if skeleton(new_text, regions2) != skel_before:
        hits.append(Hit(path, 0, "ERROR", "skeleton mismatch -- file left untouched",
                        "", ""))
        return None
    return new_text


# --------------------------------------------------------------------------
# file discovery
# --------------------------------------------------------------------------


def want_file(path: str, include_process: bool) -> bool:
    name = os.path.basename(path)
    if name in TEXT_NAMES:
        return True
    if not include_process and (
        name in SKIP_FILES
        or name.startswith(SKIP_FILE_PREFIXES)
        or name.endswith(SKIP_FILE_SUFFIXES)
    ):
        return False
    _, ext = os.path.splitext(name)
    return ext in SCAN_SUFFIXES


def iter_files(root: str, include_process: bool) -> Iterable[str]:
    if os.path.isfile(root):
        if want_file(root, include_process):
            yield root
        return
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(
            d for d in dirnames
            if d not in SKIP_DIRS
            and not d.endswith(SKIP_DIR_SUFFIXES)
            and not d.startswith(".")
        )
        for name in sorted(filenames):
            if name.startswith("."):
                continue
            path = os.path.join(dirpath, name)
            if not include_process and (
                name in SKIP_FILES or name.startswith(SKIP_FILE_PREFIXES)
                or name.endswith(SKIP_FILE_SUFFIXES) or name.endswith(".patch")
            ):
                continue
            if not want_file(path, include_process):
                continue
            yield path


def lexer_for(path: str) -> Optional[Callable[[str], List[Region]]]:
    name = os.path.basename(path)
    if name in NAMED_LEXERS:
        return NAMED_LEXERS[name]
    _, ext = os.path.splitext(name)
    for lex_ext, fn in LEXERS:
        if ext == lex_ext:
            return fn
    return None


def read_text(path: str) -> Optional[str]:
    try:
        with open(path, "rb") as fh:
            head = fh.read(8192)
        if b"\x00" in head:
            return None  # binary
        with open(path, "r", encoding="utf-8", newline="") as fh:
            return fh.read()
    except (UnicodeDecodeError, OSError):
        return None


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------


def run(paths: Sequence[str], apply: bool, include_process: bool,
        md_code: bool, quiet: bool) -> int:
    all_hits: List[Hit] = []
    files_scanned = 0
    files_edited = 0
    removable = 0
    review = 0
    errors = 0

    if md_code:
        LEXERS_MD = lambda t: scan_markdown(t, include_code=True)  # noqa: E731
    else:
        LEXERS_MD = None

    for root in paths:
        for path in iter_files(root, include_process):
            lexer = lexer_for(path)
            if lexer is None:
                continue
            if md_code and path.endswith(".md"):
                lexer = LEXERS_MD
            text = read_text(path)
            if text is None:
                continue
            files_scanned += 1
            hits: List[Hit] = []
            new_text = process_text(path, text, lexer, hits)
            all_hits.extend(hits)
            if new_text is not None and apply:
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(new_text)
                files_edited += 1

    for h in all_hits:
        if h.category == "ERROR":
            errors += 1
            print(f"ERROR {h.path}: {h.matched}")
        elif h.category == "remove":
            removable += 1
            if not quiet:
                print(f"{h.path}:{h.line}: REMOVE  '{h.matched}'")
                print(f"    before: {h.before}")
                print(f"    after : {h.after}")
        else:
            review += 1
            if not quiet:
                print(f"{h.path}:{h.line}: REVIEW  [{h.category}] '{h.matched}'")

    print(
        f"\nsummary: {files_scanned} files scanned, {files_edited} edited, "
        f"{removable} removable refs, {review} review-only, {errors} errors"
    )
    if errors:
        return 2
    return 1 if removable and not apply else 0


# --------------------------------------------------------------------------
# self-test: prove the patterns AND the guard rails
# --------------------------------------------------------------------------


def selftest() -> int:
    failures = []

    def check(name: str, got, want):
        if got != want:
            failures.append(f"{name!r}: got {got!r} want {want!r}")

    def clean(line: str) -> str:
        hits: List[Tuple[str, str, str]] = []
        return clean_body(line, hits)

    # --- removals -----------------------------------------------------------
    check("item-basic", clean("DT-12 parseDuration returns ns"), "parseDuration returns ns")
    check("item-leading-dash", clean("(BY-4 -- read this before hunting)"),
          "(read this before hunting)")
    check("pure-paren", clean("STRICT (CC-6): unknown keys throw"),
          "STRICT: unknown keys throw")
    check("pure-paren-sentence", clean("call. (RE-9)."), "call.")
    check("pure-paren-mid", clean("chainable (BY-3 BEHAVIOR CHANGE: returns handle)"),
          "chainable (BEHAVIOR CHANGE: returns handle)")
    check("mixed-paren", clean("getter note (DT-8, resolved): the member"),
          "getter note (resolved): the member")
    check("range", clean("see CC-1..4/6 now"), "see now")
    check("range-dotted", clean("assertions for DT-1..DT-17 land"),
          "assertions for land")
    check("range-double", clean("fix CY-2..4/6..8 fast"), "fix fast")
    check("wildcard", clean("RD-* and CG-* done"), "and done")
    check("slash-list", clean("wire GB-1/3/4 into the gate"), "wire into the gate")
    check("section", clean("open §36.6 items 1-7 today"), "open today")
    check("section-roman", clean("see §I.1 for recipes"), "see for recipes")
    check("ss-ref", clean("DISCARDED per SS18 already"), "DISCARDED per already")
    check("lane-work", clean("work22 review verdict"), "review verdict")
    check("ticket-e6", clean("E6-T1 detached text"), "detached text")
    check("ticket-cap", clean("T-UDP-SELF-CLOSE-UAF ticketed"), "ticketed")
    check("ctx-ticket", clean("the ticket 16 flaky assert"), "the flaky assert")
    check("ctx-plan-item", clean("plan item CL-2 lands"), "lands")
    check("double-space-collapse", clean("RE-2..RE-14  docs"), "docs")
    check("empty-paren", clean("done (NS-1) now"), "done now")

    # --- protections (must NOT change) --------------------------------------
    for keep in (
        "UTF-8 encoding",
        "SHA-256 and HMAC-SHA-256 and SHA3-256",
        "AES-256-GCM with ISO-4217 amounts",
        "curve P-256 / P-521 / X-509 certs",
        "pass (--std) and (-1) and (-o out)",
        "L1 cache and M1 Pro and p50 latency",
        "ES2025 and RFC 9106 and SP 800-38D s8",
        "phase 1 parser temp is gone",
        "let x1 = 1;",
        "node v22.23.1 rejects entirely",
        "HTTP/1.1 and TLS 1.3",
        "item 0 and item 1 of the array",
    ):
        check(f"keep:{keep[:24]}", clean(keep), keep)

    # a single "-" line marker is a list bullet and must survive tidying
    check("md-bullet-keeps-dash", clean("- DT-1 note"), "- note")
    # label-punct residue cases seen on real data
    check("paren-label-residue", clean("(SD-5): the kernels read"), "the kernels read")
    check("paren-label-mid", clean("reinterpretation (SD-5): the kernels"),
          "reinterpretation: the kernels")
    check("keep-colon-param", clean(":param CL-1 was here"), ":param was here")

    # --- review-only (reported, unchanged) ----------------------------------
    hits: List[Tuple[str, str, str]] = []
    got = clean_body("phase 2 of the pipeline", hits)
    check("review-untouched", got, "phase 2 of the pipeline")
    check("review-reported", any(h[0] == "review:phase-n" for h in hits), True)

    # --- lexer safety -------------------------------------------------------
    src = 'int x; // CL-1 fix\nchar *s = "CL-2";\n/* BY-3 */\n'
    regions = scan_c_family(src)
    edited = process_text("t.c", src, lambda t: scan_c_family(t), [])
    check("c-edit", edited, 'int x; // fix\nchar *s = "CL-2";\n/* */\n')
    check("c-string-protected", '"CL-2"' in (edited or ""), True)

    js = 'const t = `keep ${/* CL-1 */ a} BY-3`;\n'
    got = process_text("t.js", js, lambda t: scan_c_family(t, template=True), [])
    check("js-template-opaque", got, None)

    sh = 'echo a#b  # CL-1 drop\n'
    got = process_text("t.sh", sh, lambda t: scan_hash_comments(t, word_start=True), [])
    check("sh-word-start", got, "echo a#b  # drop\n")

    mk = 'all:\n\t# CL-1 recipe-comment stays\n# CL-2 top goes\n'
    got = process_text("Makefile", mk, lambda t: scan_hash_comments(t, recipe_tabs=True), [])
    check("make-recipe-opaque", got, "all:\n\t# CL-1 recipe-comment stays\n# top goes\n")

    md = 'Prose CL-1 gone.\n\n```js\n// CL-2 stays in fence\n```\nInline `CL-3` stays.\n'
    got = process_text("t.md", md, lambda t: scan_markdown(t), [])
    check("md-prose-only", got, "Prose gone.\n\n```js\n// CL-2 stays in fence\n```\nInline `CL-3` stays.\n")

    md2 = 'Prose CL-1 gone.\n\n```js\n// CL-2 goes too\n```\n'
    got = process_text("t.md", md2, lambda t: scan_markdown(t, include_code=True), [])
    check("md-code-flag", got, "Prose gone.\n\n```js\n// goes too\n```\n")

    # --- the guard rail: a deletion that would forge a comment terminator ----
    # "HS-1" sits exactly between '*' and '/', so removing it would merge
    # them into '*/' and close the comment early -- the validator must refuse
    evil = "/* keep A*HS-1/B tail */\nint ok;\n"
    hits = []
    got = process_text("evil.c", evil, lambda t: scan_c_family(t), hits)
    check("forge-terminator-blocked", got, None)
    check("forge-terminator-reported",
          any(h.category == "ERROR" for h in hits), True)
    check("forge-terminator-error-named",
          [h.path for h in hits if h.category == "ERROR"], ["evil.c"])

    # --- block-comment leader preservation ----------------------------------
    blk = "/*\n *  DT-8 name\n *  keep me\n */\n"
    got = process_text("t.c", blk, lambda t: scan_c_family(t), [])
    check("leader-preserved", got, "/*\n *  name\n *  keep me\n */\n")

    if failures:
        print("SELFTEST FAILURES:")
        for f in failures:
            print("  " + f)
        return 1
    print("selftest: all checks passed")
    return 0


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("paths", nargs="*", default=["."])
    ap.add_argument("--apply", action="store_true", help="write removals (default: report)")
    ap.add_argument("--check", action="store_true", help="gate mode: exit 1 if removable refs exist")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--include-process", action="store_true",
                    help="also scan the process/ledger docs that are skipped by default")
    ap.add_argument("--md-code", action="store_true",
                    help="also edit fenced code blocks / inline code in markdown")
    ap.add_argument("--quiet", action="store_true", help="summary only")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    rc = run(args.paths, args.apply, args.include_process, args.md_code, args.quiet)
    if args.check and rc == 0:
        return 0
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
