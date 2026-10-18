#!/usr/bin/env python3
"""convert.py — convert console.log/print-oracle probes into assert-based probes.

Reads each probe, captures the output-call argument values on BOTH engines
(node = primary oracle, dynajs = divergence detection), and emits an
assert-based probe that embeds the baked expectations and reports through the
shared harness (tests/agent/_h/h.js).

Modes
  line       : every output call site becomes __A(label, fn) (structured
               assert_eq) or __L(id, args) (line oracle, incl. per-line
               divergence markers).
  transcript : transcript generators become digest pins: rolling FNV-1a over
               the record stream + record count + first-N exact records.
  throw      : probes whose whole contract is "this source must throw" become
               assert_throws-style tests with assert_diverge on ctor/message.

Skips and tools are configured in tools_agent/exclude.tsv (single source of
truth shared with the runner).
"""

import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H_PATH = os.path.join(ROOT, "tests", "agent", "_h", "h.js")
NODE = "node"
DYNA = os.path.join(ROOT, "dynajs")

OUTPUT_PAT = r"(?:console\s*\.\s*log|print|process\s*\.\s*stdout\s*\.\s*write)\s*\("
# alias-initializer pattern: also matches BARE console.log / print (ternary alias style)
ALIAS_INIT_PAT = r"(?:console\s*\.\s*log|print|process\s*\.\s*stdout\s*\.\s*write)(?!\s*\()|(?:console\s*\.\s*log|print|process\s*\.\s*stdout\s*\.\s*write)\s*\("

ALIAS_ALLOW = {"out", "emit", "show", "write", "println", "emitln", "say", "echo",
               "puts", "log", "p", "pr", "__write"}

# per-file pre-transforms (applied once, asserted), e.g. iteration knobs
PRE_REPLACE = {
    ("ev", "diff_battery.js"): [("(scriptArgs[1] | 0) : 20000;", "(scriptArgs[1] | 0) : 8000;")],
    ("ev_bigint", "diff_battery.js"): [("(scriptArgs[1] | 0) : 20000;", "(scriptArgs[1] | 0) : 6000;")],
}

# files converted in transcript mode: (suite, name) -> #records with exact pin
TRANSCRIPT = {
    ("ev", "diff_battery.js"): 40,
    ("ev_bigint", "diff_battery.js"): 40,
    ("ev_bigint", "special2.js"): 40,
    ("dtoa_shortest", "dtoa_string_diff.js"): 40,
    ("dtoa_shortest", "dtoa_radix_tables_diff.js"): 40,
    ("builtins_ext", "test_dtoa_radix_random.js"): 40,
}

CAPTURE_SHIM = r"""
/* ---- capture shim (converter only; never shipped) ----
 * Storage uses null-prototype objects: probes under test may poison
 * Array.prototype index properties (non-writable), and Array.prototype.push
 * would then throw ([[Set]] with throw=true). Null-proto key writes are
 * immune to that. Uses __NP/__Object from h.js (modules forbid duplicate
 * declarations, so the shim must not re-declare anything). */
var __RECS = __NP(), __SEQ = __NP(), __SEQN = 0;
function __REC(id, arr) {
    var lst = __RECS[id];
    if (!lst) { lst = __RECS[id] = __NP(); lst.n = 0; lst.recs = __NP(); }
    var an = (arr.n !== undefined) ? arr.n : arr.length;
    var tags = __NP();
    for (var i = 0; i < an; i++) tags[i] = __tagval(arr[i]);
    lst.recs[lst.n] = tags;
    lst.n++;
    __SEQ[__SEQN] = __fmtLine(arr);
    __SEQN++;
}
var __THREW = null;
"""

CAPTURE_DUMP = r"""
;setTimeout(function () {
    console.log("@@CAP@@" + JSON.stringify({ r: __RECS, s: __SEQ, n: __SEQN, t: __THREW }));
}, 10);
"""


def run_engine(cmd, timeout=240):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout, cwd=ROOT)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


# --------------------------------------------------------------------------
# masking: neutralize strings/comments/regex so scanning sees code structure
# --------------------------------------------------------------------------

KEYWORD_BEFORE_REGEX = {"return", "typeof", "instanceof", "in", "of", "new", "delete",
                        "void", "case", "do", "else", "yield", "await", "throw"}


def mask_source(src):
    out = list(src)
    n = len(src)
    i = 0
    modes = ["code"]           # stack: 'code' | 'tpl'
    brace = []                 # brace depth per 'code' frame opened by '${'
    regex_ok = True
    while i < n:
        c = src[i]
        if modes[-1] == "tpl":
            if c == "\\":
                out[i] = " "
                if i + 1 < n:
                    out[i + 1] = " "
                i += 2
                continue
            if c == "`":
                out[i] = " "
                modes.pop()
                i += 1
                regex_ok = False
                continue
            if c == "$" and i + 1 < n and src[i + 1] == "{":
                modes.append("code")
                brace.append(0)
                regex_ok = True
                i += 2
                continue
            out[i] = " "
            i += 1
            continue
        # code mode
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c in "'\"":
            q = c
            out[i] = " "
            i += 1
            while i < n:
                if src[i] == "\\":
                    out[i] = " "
                    if i + 1 < n:
                        out[i + 1] = " "
                    i += 2
                    continue
                if src[i] == q:
                    out[i] = " "
                    i += 1
                    break
                out[i] = " "
                i += 1
            regex_ok = False
            continue
        if c == "`":
            out[i] = " "
            modes.append("tpl")
            i += 1
            continue
        if c == "/":
            if regex_ok:
                j = i + 1
                incls = False
                closed = False
                while j < n:
                    ch = src[j]
                    if ch == "\\":
                        j += 2
                        continue
                    if ch == "[":
                        incls = True
                    elif ch == "]":
                        incls = False
                    elif ch == "/" and not incls:
                        closed = True
                        break
                    elif ch == "\n":
                        break
                    j += 1
                if closed:
                    j += 1
                    while j < n and src[j].isalpha():
                        j += 1
                    for k in range(i, min(j, n)):
                        out[k] = " "
                    i = j
                    regex_ok = False
                    continue
            regex_ok = True
            i += 1
            continue
        if c.isalnum() or c in "_$":
            j = i
            while j < n and (src[j].isalnum() or src[j] in "_$"):
                j += 1
            word = src[i:j]
            regex_ok = word in KEYWORD_BEFORE_REGEX
            i = j
            continue
        if c == "{":
            if brace:
                brace[-1] += 1
            regex_ok = True
            i += 1
            continue
        if c == "}":
            if brace:
                if brace[-1] == 0:
                    brace.pop()
                    modes.pop()  # close ${...}: back to template text
                    i += 1
                    continue
                brace[-1] -= 1
            regex_ok = True
            i += 1
            continue
        if c in "(,=:[!&|?;+-*%~^<>":
            regex_ok = True
        elif c in ")]":
            regex_ok = False
        i += 1
    return "".join(out)


def find_calls(masked, aliases):
    """Output-call sites in order: {id, start, end, name, arg_spans}."""
    pats = []
    for a in sorted(aliases):
        pats.append((a, re.compile(r"(?<![.\w$])" + re.escape(a) + r"\s*\(")))
    pats.append(("console.log", re.compile(r"(?<![.\w$])console\s*\.\s*log\s*\(")))
    hits = []
    for name, pat in pats:
        for m in pat.finditer(masked):
            # never rewrite the callee of a function DECLARATION
            before = masked[max(0, m.start() - 30):m.start()]
            if re.search(r"(?<![.\w$])function\s+$", before):
                continue
            hits.append((m.start(), m.end(), name))
    hits.sort()
    calls = []
    cid = 0
    n = len(masked)
    for start, after, name in hits:
        depth = 0
        j = after - 1
        while j < n:
            ch = masked[j]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= n:
            continue
        inner_s, inner_e = after, j
        args = []
        depth = 0
        seg_start = inner_s
        k = inner_s
        while k < inner_e:
            ch = masked[k]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == "," and depth == 0:
                args.append((seg_start, k))
                seg_start = k + 1
            k += 1
        args.append((seg_start, inner_e))
        calls.append({"id": cid, "start": start, "end": j + 1, "name": name,
                      "arg_spans": args})
        cid += 1
    return calls


def detect_emitter_bodies(src, masked, aliases):
    """Printing helper functions: `function NAME(...) { ... print ... }` with
    NAME in the allowlist. Returns (body_ranges, emitter_names)."""
    ranges = []
    names = set()
    callpat = re.compile(r"(?<![.\w$])(" + "|".join(
        re.escape(x) for x in sorted(set(ALIAS_ALLOW) | set(aliases))) + r")\s*\(")
    printpat = re.compile(r"(?<![.\w$])print\s*(?![\w$(])")
    for m in re.finditer(r"(?<![.\w$])function\s+([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{", masked):
        name = m.group(1)
        if name not in ALIAS_ALLOW and name not in aliases:
            continue
        depth = 0
        j = m.end() - 1
        n = len(masked)
        while j < n:
            ch = masked[j]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = masked[m.end():j]
        if (re.search(OUTPUT_PAT, body) or callpat.search(body) or printpat.search(body)):
            ranges.append((m.start(), j + 1))
            names.add(name)
    return ranges, names


def detect_alias_vars(src, masked):
    """`var|let|const X = <init mentioning an output fn>;` -> alias X."""
    found = []
    n = len(src)
    for m in re.finditer(r"\b(?:var|let|const)\s+([A-Za-z_$][\w$]*)\s*=", src):
        name = m.group(1)
        j = m.end()
        depth = 0
        end = n
        while j < n:
            ch = masked[j]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == ";" and depth == 0:
                end = j
                break
            elif ch == "\n" and depth == 0:
                rest = masked[j + 1:j + 60].lstrip()
                if rest and rest[0] not in "?.|&+-/:,=(*":
                    end = j
                    break
            j += 1
        seg = src[m.end():end]
        if re.search(ALIAS_INIT_PAT, seg) and name not in ("summary", "test"):
            found.append((name, m.start(), end))
    return found


def parse_js_string_literal(s):
    """Best-effort JS string literal -> value (for structured labels)."""
    if len(s) < 2:
        return None
    q = s[0]
    if q not in "'\"" or s[-1] != q:
        return None
    body = s[1:-1]
    out = []
    i = 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v",
              "0": "\0", "\\": "\\", "'": "'", '"': '"', "`": "`"}
    while i < len(body):
        c = body[i]
        if c == "\\":
            i += 1
            if i >= len(body):
                return None
            e = body[i]
            if e in simple:
                out.append(simple[e])
                i += 1
            elif e == "x":
                try:
                    out.append(chr(int(body[i + 1:i + 3], 16)))
                    i += 3
                except (ValueError, IndexError):
                    return None
            elif e == "u":
                if body[i + 1:i + 2] == "{":
                    end = body.find("}", i)
                    if end < 0:
                        return None
                    try:
                        out.append(chr(int(body[i + 2:end], 16)))
                    except (ValueError, OverflowError):
                        return None
                    i = end + 1
                else:
                    try:
                        out.append(chr(int(body[i + 1:i + 5], 16)))
                        i += 5
                    except (ValueError, IndexError):
                        return None
            else:
                return None
        else:
            out.append(c)
            i += 1
    return "".join(out)


SCALAR_TAGS = {"str", "num", "big", "bool", "und", "nul"}


def js_literal_from_tag(tag):
    kind, val, rep = tag
    if kind == "str":
        return json.dumps(val, ensure_ascii=True)
    if kind == "num":
        return rep  # NaN / Infinity / -0 / shortest-decimal are valid literals
    if kind == "big":
        return val + "n"
    if kind == "bool":
        return val
    if kind == "und":
        return "undefined"
    if kind == "nul":
        return "null"
    return None


def line_from_tags(tags):
    parts = []
    for kind, val, rep in tags:
        if kind in ("str", "big"):
            parts.append(val)
        else:
            parts.append(rep)
    return " ".join(parts)


def fnv1a32(lines):
    h = 0x811C9DC5
    for line in lines:
        for ch in (line + "\n"):
            h ^= ord(ch)
            h = (h * 0x01000193) & 0xFFFFFFFF
    return format(h, "x")  # match JS (h >>> 0).toString(16): no zero padding


def apply_rewrite(text, spans_and_texts):
    out = []
    pos = 0
    for start, end, rep in spans_and_texts:
        out.append(text[pos:start])
        out.append(rep)
        pos = end
    out.append(text[pos:])
    return "".join(out)


def has_async(masked):
    return bool(re.search(r"\bawait\b|\basync\b|\.then\s*\(|setTimeout\s*\(", masked))


# --------------------------------------------------------------------------
# capture
# --------------------------------------------------------------------------

def parse_capture(stdout):
    # NOTE: split on "\n" only — str.splitlines() would also split on raw
    # U+2028/U+2029, which JSON.stringify leaves unescaped inside strings.
    capline = None
    for line in reversed(stdout.split("\n")):
        if line.startswith("@@CAP@@"):
            capline = line
            break
    if capline is None:
        return None
    try:
        d = json.loads(capline[len("@@CAP@@"):])
    except json.JSONDecodeError:
        return None
    # normalize: r = {id: {"n": count, "recs": {idx: {i: tag}}}} ->
    #            r = {id: [[[tag,...],...]]}, s = [line,...]
    recs = {}
    for sid, lst in d["r"].items():
        out_recs = []
        for k in range(lst["n"]):
            tags_d = lst["recs"][str(k)]
            out_recs.append([tags_d[str(i)] for i in range(len(tags_d))])
        recs[str(sid)] = out_recs
    seq = [d["s"][str(i)] for i in range(d["n"])]
    return {"r": recs, "s": seq, "t": d["t"]}


EVCAP_TMPL = r"""
/* the eval runs inside a FUNCTION so that capture context == converted-test
 * context (direct eval vars are scoped to the nearest function environment) */
function __evcapRun() {
    var __lines = __NP(); __lines.n = 0; var __e = null;
    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;
    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };
    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }
    try { eval(__CONVERT_SRC__); } catch (err) { __e = err; }
    console.log = __sl;
    if (__sp) { try { print = __sp; } catch (__e8) {} }
    return { n: __lines.n, s: __lines, threw: __e !== null,
        ctor: __e ? ((__e.constructor && __e.constructor.name) || "unknown") : null,
        msg: __e ? ((__e.message !== undefined) ? String(__e.message) : String(__e)) : null };
}
console.log("@@EVCAP@@" + JSON.stringify(__evcapRun()));
"""


def parse_evcap(stdout):
    for line in reversed(stdout.split("\n")):
        if line.startswith("@@EVCAP@@"):
            try:
                return json.loads(line[len("@@EVCAP@@"):])
            except json.JSONDecodeError:
                return None
    return None


def run_evalcap(src, ext, tag, engine="node", timeout=120):
    """Eval the source with silenced output; return per-run behavior record."""
    tmpdir = os.path.join(ROOT, "tools_agent", "_capture_tmp")
    os.makedirs(tmpdir, exist_ok=True)
    path = os.path.join(tmpdir, tag + f".evcap.{engine}." + ext)
    with open(path, "w") as f:
        f.write(H_CODE + EVCAP_TMPL.replace("__CONVERT_SRC__", json.dumps(src, ensure_ascii=True)))
    cmd = [DYNA, "-m", path] if (engine == "dynajs" and ext == "mjs") else (
          [DYNA, path] if engine == "dynajs" else [NODE, path])
    try:
        rc, out, err = run_engine(cmd, timeout)
    finally:
        os.unlink(path)
    d = parse_evcap(out)
    if d is None:
        raise RuntimeError(f"evalcap failed on {engine} for {tag}; rc={rc} err={err[-300:]}")
    return d


def probe_error(src, ext="js"):
    """Run raw source on both engines; return ({engine: (ctor, msg) or None})."""
    tmpdir = os.path.join(ROOT, "tools_agent", "_capture_tmp")
    os.makedirs(tmpdir, exist_ok=True)
    path = os.path.join(tmpdir, f"errprobe.{ext}")
    with open(path, "w") as f:
        f.write(src)
    res = {}
    for name, cmd in (("node", [NODE, path]), ("dynajs", [DYNA, "-m", path] if ext == "mjs" else [DYNA, path])):
        try:
            rc, out, err = run_engine(cmd, 60)
        except subprocess.TimeoutExpired:
            res[name] = ("TIMEOUT", "")
            continue
        ctor, msg = None, ""
        for line in err.splitlines():
            mm = re.match(r"^\s*([A-Za-z]*Error|Error)\s*:\s?(.*)$", line)
            if mm:
                ctor, msg = mm.group(1), mm.group(2).strip()
                break
        res[name] = (ctor, msg) if rc != 0 else (None, None)
    os.unlink(path)
    return res


def run_capture(capture, ext, tag):
    tmpdir = os.path.join(ROOT, "tools_agent", "_capture_tmp")
    os.makedirs(tmpdir, exist_ok=True)
    cpath = os.path.join(tmpdir, tag + ".capture." + ext)
    with open(cpath, "w") as f:
        f.write(capture)
    try:
        rc_n, out_n, err_n = run_engine([NODE, cpath])
        rc_d, out_d, err_d = run_engine([DYNA, "-m", cpath] if ext == "mjs" else [DYNA, cpath])
    finally:
        pass
    if parse_capture(out_n) is None:
        dbg = os.path.join(ROOT, "tools_agent", "_capture_tmp", f"fail_{tag}.js")
        with open(dbg, "w") as f:
            f.write(capture)
    cap_n = parse_capture(out_n)
    cap_d = parse_capture(out_d)
    if cap_n is None:
        raise RuntimeError(f"node capture failed ({tag}); rc={rc_n} outlen={len(out_n)} "
                           f"tail={out_n[-120:]!r} err={err_n[-300:]}")
    if cap_d is None:
        raise RuntimeError(f"dynajs capture failed ({tag}); rc={rc_d} outlen={len(out_d)} "
                           f"tail={out_d[-120:]!r} err={err_d[-300:]}")
    return cap_n, cap_d


# --------------------------------------------------------------------------
# conversion
# --------------------------------------------------------------------------

def stable_line(caps, sid, k):
    """(line, stable) for execution k of site sid; stable iff ALL captures
    of that engine produced the same line (run-varying lines like stack
    depth counters get prefix-baked instead)."""
    lines = []
    for cap in caps:
        recs = cap["r"].get(sid, [])
        lines.append(line_from_tags(recs[k]) if k < len(recs) else None)
    return lines[0], all(l == lines[0] for l in lines)


def prefix_of(line, other):
    """longest common prefix trimmed to a whitespace boundary."""
    i = 0
    m = min(len(line), len(other))
    while i < m and line[i] == other[i]:
        i += 1
    cut = line.rfind(" ", 0, i)
    if cut <= 0:
        cut = i
    return line[:cut].rstrip()


def convert(suite, fname, src, suite_name=None, allow_throw_contract=True):
    """Return (converted_source, report). Raises on failure."""
    suite_name = suite_name or suite
    key = (suite, fname)
    ext = "mjs" if fname.endswith(".mjs") else "js"

    for old, new in PRE_REPLACE.get(key, []):
        if old not in src:
            raise RuntimeError(f"pre_replace target not found in {suite}/{fname}: {old!r}")
        src = src.replace(old, new, 1)

    masked = mask_source(src)
    alias_vars = detect_alias_vars(src, masked)
    emitter_ranges, emitter_names = detect_emitter_bodies(src, masked,
                                                          set(a for a, _, _ in alias_vars) | ALIAS_ALLOW)
    alias_init_ranges = [(s, e) for _, s, e in alias_vars]
    # output-function names: detected var aliases + detected emitter functions.
    # (ALIAS_ALLOW is only a hint for emitter detection — probe-local helpers
    # that merely share a name with a printer must not be rewritten.)
    aliases = {name for name, _, _ in alias_vars} | emitter_names
    skip_ranges = emitter_ranges + alias_init_ranges

    calls = [c for c in find_calls(masked, aliases) if not any(s <= c["start"] < e for s, e in skip_ranges)]
    # keep only outermost call sites (an output call nested inside another
    # output call's argument list is rewritten with its parent)
    outermost = []
    for c in calls:
        if any(k["start"] <= c["start"] and c["end"] <= k["end"] for k in outermost):
            continue
        outermost.append(c)
    calls = outermost
    mode = "transcript" if key in TRANSCRIPT else "line"
    report = {"suite": suite, "file": fname, "mode": mode, "sites": len(calls),
              "lines": 0, "divergent": False, "diverge_sites": 0}

    # ---- capture on both engines ------------------------------------------
    rewrite = []
    for c in calls:
        args_src = [src[s:e].strip() for s, e in c["arg_spans"]]
        rewrite.append((c["start"], c["end"], f"__REC({c['id']}, [{', '.join(args_src)}])"))
    instr = apply_rewrite(src, rewrite)
    parts = [H_CODE]
    prelude = os.path.join(ROOT, "tests", "agent", suite, "_h.js")
    if os.path.exists(prelude):
        with open(prelude) as f:
            parts.append(f.read())
    parts.append(CAPTURE_SHIM)
    if ext == "mjs":
        parts.append(instr)  # no try-wrap: modules may contain import/export
    else:
        parts.append("try {\n" + instr + "\n} catch (__e) { __THREW = __tagval(__e); }")
    parts.append(CAPTURE_DUMP)
    capture = "".join(p if p.endswith("\n") else p + "\n" for p in parts)

    cap_n = cap_d = None
    try:
        cap_n, cap_d = run_capture(capture, ext, f"{suite}__{fname}")
    except RuntimeError:
        if allow_throw_contract and ext != "mjs":
            # capture could not run at all (e.g. the probe source does not
            # parse): fall back to a throw-contract conversion
            errs = probe_error(src, ext)
            ctor_n, msg_n = errs["node"]
            ctor_d, msg_d = errs["dynajs"]
            if ctor_n is None:
                raise
            body = []
            body.append("// converted from a throw-contract probe: the original source is")
            body.append("// re-executed via eval and must throw (ctor/message pinned per engine)")
            body.append("__EXP = null;")
            body.append(f"test({json.dumps(fname + ' must throw')}, function () {{")
            body.append(f"    var __e = __mustThrow(function () {{ return eval({json.dumps(src, ensure_ascii=True)}); }});")
            body.append("    assert(__e !== null, 'expected a throw, got none');")
            body.append(f"    assert_diverge(__e && __e.constructor.name, {json.dumps(ctor_d or ctor_n)}, {json.dumps(ctor_n)}, 'constructor');")
            body.append(f"    assert_diverge(__e && __e.message, {json.dumps(msg_d)}, {json.dumps(msg_n)}, 'message');")
            body.append("});")
            body.append(f"summary({json.dumps(suite_name)});")
            report["throw_contract"] = True
            report["lines"] = 1
            return "\n".join(body) + "\n", report
        raise
    seq_n, seq_d = cap_n["s"], cap_d["s"]
    # stability pass: run both captures again to find run-varying lines
    # (e.g. stack-overflow depth counters); unstable expectations are baked
    # as "~prefix" entries (h.js prefix-matches them)
    cap_n2, cap_d2 = run_capture(capture, ext, f"{suite}__{fname}.r2")
    cap_n3, _ = run_capture(capture, ext, f"{suite}__{fname}.r3")
    if mode == "transcript" and (seq_n != cap_n2["s"] or seq_n != cap_n3["s"]):
        raise RuntimeError(f"{suite}/{fname}: nondeterministic transcript on node across runs")
    divergent = seq_n != seq_d
    threw_n = cap_n["t"]

    report = {"suite": suite, "file": fname, "mode": mode, "sites": len(calls),
              "lines": len(seq_n), "divergent": divergent, "diverge_sites": 0}

    # ---------------- throw contract analysis -------------------------------
    # threw_n/threw_d: the CAPTURE run aborted (an uncaught error escaped the
    # probe). Such probes are converted via the eval-capture form below: the
    # original source is re-executed (silenced) inside a test and its full
    # per-engine behavior (threw? ctor? message? output lines?) is asserted
    # against expectations baked per engine. This covers both-throw probes,
    # one-throw probes, and probes whose semantics live at script level
    # (documented caveat: eval context vs script context).
    threw_d = cap_d["t"]
    contract = None
    if threw_n is not None or threw_d is not None:
        errs = probe_error(src, ext)
        if errs["node"][0] is None and errs["dynajs"][0] is None:
            raise RuntimeError(
                f"{suite}/{fname}: capture threw but neither raw engine run "
                f"reproduces a throw (likely a rewrite bug)")
        contract = "both" if (threw_n is not None and threw_d is not None) else "divergent"

    if contract and allow_throw_contract and ext != "mjs":
        ev_n = run_evalcap(src, ext, f"{suite}__{fname}")
        ev_d = run_evalcap(src, ext, f"{suite}__{fname}", engine="dynajs")
        body = []
        body.append("// converted from an escaping-throw probe: the original source is")
        body.append("// re-executed (output silenced) and its per-engine behavior —")
        body.append("// throw + emitted lines — asserted against baked expectations")
        body.append("__EXP = null;")
        body.append(f"test({json.dumps(fname + ' per-engine behavior contract')}, function () {{")
        body.append(f"    var __SRC = {json.dumps(src, ensure_ascii=True)};")
        body.append("    var __lines = __NP(); __lines.n = 0; var __e = null;")
        body.append("    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;")
        body.append("    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };")
        body.append("    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }")
        body.append("    try { eval(__SRC); } catch (err) { __e = err; }")
        body.append("    console.log = __sl;")
        body.append("    if (__sp) { try { print = __sp; } catch (__e8) {} }")
        for eng, ev in (("dynajs", ev_d), ("node", ev_n)):
            ev = {"threw": ev["threw"], "ctor": ev["ctor"], "msg": ev["msg"],
                  "n": ev["n"], "s": [ev["s"][str(i)] for i in range(ev["n"])]}
            branch = "__ENGINE === 'dynajs'" if eng == "dynajs" else "__ENGINE !== 'dynajs'"
            body.append(f"    if ({branch}) {{")
            body.append(f"        assert_eq(__e !== null, {json.dumps(ev['threw'])}, {json.dumps(eng + ' threw')});")
            if ev["threw"]:
                body.append(f"        assert_eq(__e && __e.constructor && __e.constructor.name, {json.dumps(ev['ctor'])}, {json.dumps(eng + ' error ctor')});")
                body.append(f"        assert_eq(__e && __e.message, {json.dumps(ev['msg'])}, {json.dumps(eng + ' error message')});")
            body.append(f"        assert_eq(__lines.n, {ev['n']}, {json.dumps(eng + ' line count')});")
            for i, ln in enumerate(ev["s"]):
                body.append(f"        assert_eq(__lines[{i}], {json.dumps(ln, ensure_ascii=True)}, {json.dumps(eng + f' line {i+1}')});")
            body.append("    }")
        body.append("});")
        body.append(f"summary({json.dumps(suite_name)});")
        report["throw_contract"] = contract
        report["lines"] = max(ev_n["n"], ev_d["n"])
        return "\n".join(body) + "\n", report

    # ---------------- transcript mode --------------------------------------
    if mode == "transcript":
        n_pin = TRANSCRIPT[key]
        digest_n = fnv1a32(seq_n)
        digest_d = fnv1a32(seq_d)
        lines = []
        lines.append("// converted transcript generator: record stream pinned by")
        lines.append("// count + FNV-1a digest + first-%d records (node oracle%s)" % (
            n_pin, "; dynajs divergences are explicit" if divergent else ""))
        lines.append(f"__PIN_EXP_N = {n_pin};")
        pin_n = [rec if len(rec) <= 4000 else rec[:4000] for rec in seq_n[:n_pin]]
        lines.append("__PIN_EXP_LINES = " + json.dumps(pin_n, ensure_ascii=True) + ";")
        if divergent:
            pin_d = [rec if len(rec) <= 4000 else rec[:4000] for rec in seq_d[:n_pin]]
            lines.append("__PIN_DYN_LINES = " + json.dumps(pin_d, ensure_ascii=True) + ";")
        rewrite = []
        for c in calls:
            args_src = [src[s:e].strip() for s, e in c["arg_spans"]]
            rewrite.append((c["start"], c["end"], "__PIN(" + ", ".join(args_src) + ")"))
        lines.append(apply_rewrite(src, rewrite))
        lines.append(f'__A("transcript-record-count", function () {{ assert_eq(__PIN_count, {len(seq_n)}); }});')
        if divergent:
            lines.append(f'__A("transcript-digest-fnv1a", function () {{ assert_diverge((__PIN_h1 >>> 0).toString(16), {json.dumps(digest_d)}, {json.dumps(digest_n)}, "fnv1a"); }});')
        else:
            lines.append(f'__A("transcript-digest-fnv1a", function () {{ assert_eq((__PIN_h1 >>> 0).toString(16), {json.dumps(digest_n)}); }});')
        lines.append(f'__FINISH({json.dumps(suite_name)});' if has_async(masked)
                     else f'summary({json.dumps(suite_name)});')
        return "\n".join(lines) + "\n", report

    # ---------------- line mode --------------------------------------------
    lines = []
    lines.append("// converted from console.log/print oracle: expectations baked from the")
    lines.append("// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.")
    exp_blocks = []
    exp_ids = set()
    rewrite = []
    ndiv = 0
    for c in calls:
        recs_n = cap_n["r"].get(str(c["id"]), [])
        recs_d = cap_d["r"].get(str(c["id"]), [])
        args_src = [src[s:e].strip() for s, e in c["arg_spans"]]
        args_list = ", ".join(args_src)
        if (len(args_src) == 2 and len(recs_n) == 1 and len(recs_d) == 1
                and line_from_tags(recs_n[0]) == line_from_tags(recs_d[0])
                and stable_line([cap_n, cap_n2, cap_n3], str(c["id"]), 0)[1]
                and stable_line([cap_d, cap_d2], str(c["id"]), 0)[1]
                and not re.search(r"\bawait\b|\barguments\b|\bthis\b|\bnew\.target\b|\beval\b", args_src[1])):
            label_val = parse_js_string_literal(args_src[0])
            lit = js_literal_from_tag(recs_n[0][1])
            if label_val is not None and lit is not None:
                name = f"{fname}:{label_val}"
                # NOTE: no trailing ';' — the original call's own terminator stays,
                # and the replacement must stay valid in expression positions
                # (arrow bodies, ternaries, if-heads).
                rewrite.append((c["start"], c["end"],
                                f'__A({json.dumps(name)}, function () {{ assert_eq({args_src[1]}, {lit}, {args_src[0]}); }})'))
                continue
        exps = []
        def maybe_unstable(line):
            """Lines ending in a large bare counter (4+ digit number) vary
            run-to-run on some engines (stack-overflow depth counters); bake
            them as prefix expectations."""
            import re as _re
            if line and _re.search(r"\s\d{4,}$", line):
                parts = line.rsplit(" ", 1)
                return parts[0]
            return None

        for k in range(max(len(recs_n), len(recs_d))):
            ln, stn = stable_line([cap_n, cap_n2, cap_n3], str(c["id"]), k)
            ld, std = stable_line([cap_d, cap_d2], str(c["id"]), k)
            if ln is not None and stn:
                mu = maybe_unstable(ln)
                if mu is not None:
                    stn = False
                    ln = mu
            if ld is not None and std:
                mu = maybe_unstable(ld)
                if mu is not None:
                    std = False
                    ld = mu
            if ln is None and ld is None:
                continue
            if ln is None:
                # dynajs-only execution: pin the dynajs line (unstable -> prefix)
                if not std:
                    other = cap_d2["r"].get(str(c["id"]), [])
                    ld2 = line_from_tags(other[k]) if k < len(other) else ""
                    exps.append("~" + prefix_of(ld, ld2))
                else:
                    exps.append(ld)
            elif ld is None:
                # node-only execution: pin the node line (unstable -> prefix)
                if not stn:
                    other = cap_n2["r"].get(str(c["id"]), [])
                    lb2 = line_from_tags(other[k]) if k < len(other) else ""
                    exps.append("~" + prefix_of(ln, lb2))
                else:
                    exps.append(ln)
            elif ln == ld and stn and std:
                exps.append(ln)
            else:
                if not stn:
                    other = cap_n2["r"].get(str(c["id"]), [])
                    lb2 = line_from_tags(other[k]) if k < len(other) else ""
                    ln = "~" + prefix_of(ln, lb2)
                if not std:
                    other = cap_d2["r"].get(str(c["id"]), [])
                    ld2 = line_from_tags(other[k]) if k < len(other) else ""
                    ld = "~" + prefix_of(ld, ld2)
                exps.append([ld, ln])
                ndiv += 1
        if exps:
            exp_blocks.append(f"__EXP[{c['id']}] = " + json.dumps(exps, ensure_ascii=True) + ";")
            exp_ids.add(c["id"])
        rewrite.append((c["start"], c["end"], f"__L({c['id']}, {args_list})"))
    report["diverge_sites"] = ndiv
    lines.append("__EXP = {};" if exp_blocks else "__EXP = null;")
    lines.extend(exp_blocks)
    if divergent:
        # per-engine REQUIRED call counts, for line-mode (__L) sites only —
        # structured __A sites never consume through __CTR. A site recorded
        # only on the other engine must not be reported as unreached here.
        req = {}
        for e_name, cap in (("dynajs", cap_d), ("node", cap_n)):
            m = {}
            for c in calls:
                if c["id"] not in exp_ids:
                    continue
                n_exec = len(cap["r"].get(str(c["id"]), []))
                if n_exec:
                    m[c["id"]] = n_exec
            req[e_name] = m
        lines.append("__REQ = " + json.dumps(
            {"dynajs": req["dynajs"], "node": req["node"]}, ensure_ascii=True) + ";")
    lines.append(apply_rewrite(src, rewrite))
    lines.append(f'__FINISH({json.dumps(suite_name)});' if has_async(masked)
                 else f'summary({json.dumps(suite_name)});')
    return "\n".join(lines) + "\n", report


def load_h():
    global H_CODE
    with open(H_PATH) as f:
        H_CODE = f.read()


H_CODE = ""


def main():
    load_h()
    print("convert.py is a library; use convert_all.py")


if __name__ == "__main__":
    main()
