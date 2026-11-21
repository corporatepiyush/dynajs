// tools/js/defects.js: the gate's line-level C defect scanner (D01..D51), the port of tools/defects.py (deleted).
// Usage: ./dynajs --std tools/js/defects.js [root] [--defects-all] [--defects-baseline PATH]
//        [--update-baseline PATH] [--suppress TSV] [--no-suppress] [--emit-tsv] [--gate D04,D28,...] [--baseline-ref REF]
//        exit 1 = an ERROR finding, or a finding absent from the baseline.
//   --gate RULES: any warn finding of RULES outside the TSV baseline fails, and so does a stale row (found < count):
//     the baseline only shrinks, each fix lowers its row in the same patch.
//   --baseline-ref REF (git rev, or file:PATH): a TSV row or count absent from REF's baseline fails, except rows of a
//     rule REF has no row for (the one-time backlog import of a newly gated rule).
// Suppressions: <root>/tools/defects-baseline.tsv (or --suppress), rows file<TAB>rule<TAB>function<TAB>count<TAB>reason;
//   a row hides its findings only while their number is <= count; fewer found = STALE row (shrink it).
//   The header and the per-rule table count unsuppressed findings by their ACTUAL severity.
//   A root with .git scans only files git sees (tracked + untracked, minus .gitignore); --no-git walks everything.
//   --emit-tsv prints unsuppressed findings grouped as TSV rows (reason left empty) to seed a row.
// Output (stdout, exit code, --update-baseline JSON) is byte-identical to the Python original on
// this tree. Deliberate differences, all outside the observable output on real inputs:
//   - regexes use JS (ASCII) \w \b \d \s where Python's are Unicode-aware; a scanned file whose
//     blanked text holds non-ASCII next to an identifier could match differently;
//   - no persistent cache (.obj/codegraph-defects-cache.json): os.stat has millisecond mtimes,
//     so a size+mtime key could serve a stale result; the Python cache is a speed feature only;
//   - single process (no multiprocessing pool); slices count UTF-16 units, not code points.
import * as std from "std";
import * as os from "os";
import { MD5Hex } from "dyna:hash";
import { walk, isfile, isdir, exists, join, basename, abspath, relpath, readText, pyStrip, pyRepr, cmpStr } from "./pyshim.js";

const FUNC_SCAN = /[{}]|\b([A-Za-z_]\w*)\s*\(/g;

export function splitTopArgs(text) {
    const out = [];
    let cur = "", depth = 0;
    for (const ch of text) {
        if (ch === "(" || ch === "[") depth++;
        else if (ch === ")" || ch === "]") depth--;
        if (ch === "," && depth === 0) {
            out.push(pyStrip(cur));
            cur = "";
            continue;
        }
        cur += ch;
    }
    out.push(pyStrip(cur));
    return out;
}

const KEYWORDS = new Set([
    "if", "for", "while", "switch", "return", "sizeof", "case", "do", "else",
    "goto", "typedef", "struct", "enum", "union", "static", "const", "void",
    "int", "char", "unsigned", "signed", "long", "short", "float", "double",
    "volatile", "register", "extern", "inline", "restrict", "_Static_assert",
    "defined", "break", "continue", "default", "_Atomic", "_Bool", "alignas",
    "alignof", "__attribute__", "__typeof__", "typeof", "asm", "__asm__",
    "_Generic", "__inline", "__inline__", "__restrict", "__restrict__",
    "__volatile__", "__extension__", "and", "or", "not",
]);

let nlText = null, nlOffs = null, nlText2 = null, nlOffs2 = null;
function nlIndex(text) {
    if (text === nlText) return nlOffs;
    if (text === nlText2) {
        [nlText, nlOffs, nlText2, nlOffs2] = [nlText2, nlOffs2, nlText, nlOffs];
        return nlOffs;
    }
    const offs = [];
    let i = text.indexOf("\n");
    while (i >= 0) {
        offs.push(i + 1);
        i = text.indexOf("\n", i + 1);
    }
    nlText2 = nlText;
    nlOffs2 = nlOffs;
    nlText = text;
    nlOffs = offs;
    return offs;
}

export function lineOf(text, pos) {
    const offs = nlIndex(text);
    let lo = 0, hi = offs.length;
    while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (offs[mid] <= pos) lo = mid + 1;
        else hi = mid;
    }
    return lo + 1;
}

const BLANK_TOKEN = /\/\*|\/\/|"|'/g;
const NOT_NL = /[^\n]/g;

export function blankC(src) {
    const n = src.length;
    const out = [];
    let prev = 0, pos = 0;
    for (const m of src.matchAll(BLANK_TOKEN)) {
        if (m.index < pos) continue;
        const start = m.index, tok = m[0];
        let end;
        if (tok === "/*") {
            const j = src.indexOf("*/", start + 2);
            end = j < 0 ? n : j + 2;
        } else if (tok === "//") {
            const j = src.indexOf("\n", start);
            end = j < 0 ? n : j;
        } else {
            let j = start + 1;
            while (j < n) {
                const c = src[j];
                if (c === "\\") {
                    j += 2;
                    continue;
                }
                if (c === tok) {
                    j += 1;
                    break;
                }
                if (c === "\n") break;
                j += 1;
            }
            end = j;
        }
        out.push(src.slice(prev, start));
        out.push(src.slice(start, end).replace(NOT_NL, " "));
        prev = end;
        pos = end;
    }
    out.push(src.slice(prev));
    return out.join("");
}

const WS_RUN = /[\s\x1c-\x1f\x85]+/g;
const RE_STATIC = /\bstatic\b/;
const RE_INLINE = /\binline\b/;

export function findFunctions(blank, raw) {
    const n = blank.length, res = [];
    let i = 0, depth = 0;
    for (;;) {
        FUNC_SCAN.lastIndex = i;
        const mm = FUNC_SCAN.exec(blank);
        if (!mm) break;
        i = mm.index;
        const ch = blank[i];
        if (ch === "{") {
            depth++;
            i++;
            continue;
        }
        if (ch === "}") {
            depth = Math.max(0, depth - 1);
            i++;
            continue;
        }
        const mEnd = mm.index + mm[0].length;
        if (depth === 0) {
            const name = mm[1];
            if (!KEYWORDS.has(name)) {
                let pd = 0, j = mEnd - 1;
                while (j < n) {
                    const c = blank[j];
                    if (c === "(") pd++;
                    else if (c === ")") {
                        pd--;
                        if (pd === 0) break;
                    }
                    j++;
                }
                let k = j + 1;
                while (k < n && " \t\r\n".includes(blank[k])) k++;
                if (k < n && blank[k] === "{") {
                    let bd = 0, e = k;
                    while (e < n) {
                        const c = blank[e];
                        if (c === "{") bd++;
                        else if (c === "}") {
                            bd--;
                            if (bd === 0) break;
                        }
                        e++;
                    }
                    const ss = raw.lastIndexOf("\n", i - 1) + 1;
                    const sig = pyStrip(raw.slice(ss, k)).replace(WS_RUN, " ").slice(0, 400);
                    res.push([name, sig, lineOf(blank, i), lineOf(blank, e),
                        RE_STATIC.test(sig) ? 1 : 0, RE_INLINE.test(sig) ? 1 : 0,
                        blank.slice(k, e + 1), k]);
                    i = e + 1;
                    continue;
                }
            }
        }
        i = mEnd;
    }
    return res;
}

const DEFECT_SKIP_DIRS = new Set([".git", ".obj", "node_modules", "third_party", "test262",
    ".swarm", ".prepush", ".agent-work", "pgo-data", "__pycache__",
    "scratch", "tmp_audit", ".commandcode", ".zcode", "docker", ".ws"]);

const ERRCONV_CALL = /\b(JS_To[A-Z]\w*|dyn_path_borrow)\s*\(/g;

class PyKeyError extends Error {
    constructor(key) {
        super("KeyError");
        this.pyRepr = "KeyError(" + pyRepr(key) + ")";
    }
}

class Defect {
    constructor(check, sev, file, line, code, msg) {
        this.check = check;
        this.sev = sev;
        this.file = file;
        this.line = line;
        this.code = code;
        this.msg = msg;
        this.fn = null;
    }
    key() {
        return this.check + "|" + this.file + "|" + MD5Hex(pyStrip(this.code)).slice(0, 10);
    }
    fmt(root) {
        const p = this.file.startsWith(root) ? relpath(this.file, root) : this.file;
        return this.sev.padEnd(5) + " " + this.check.padEnd(38) + " " + p + ":" + this.line + "  " + this.msg;
    }
}

const DEFECT_META = [];
function check(cid, sev, title, fn) {
    DEFECT_META.push([cid, sev, title, fn]);
}

function fnParams(sig) {
    const i = sig.indexOf("(");
    if (i < 0) return new Set();
    const body = sig.slice(i + 1, sig.slice(i).includes(")") ? sig.lastIndexOf(")") : sig.length);
    const names = new Set();
    for (let tok of body.split(",")) {
        tok = pyStrip(tok);
        if (!tok || tok === "void" || tok === "...") continue;
        if (tok.includes("=")) tok = pyStrip(tok.split("=")[0]);
        const parts = tok.replaceAll("*", " * ").split(/[\s\x1c-\x1f\x85]+/).filter((x) => x);
        if (parts.length) names.add(pyStrip(parts[parts.length - 1], "[]0123456789 "));
    }
    return names;
}

const DEFECT_ROOT_DIRS = ["src", "tests", "bench", "examples", "tools"];
const DEFECT_ROOT_FILES = [];

let GIT_VISIBLE;
function gitVisible(root) {
    if (GIT_VISIBLE !== undefined && GIT_VISIBLE.root === root) return GIT_VISIBLE.set;
    let set = null;
    if (!NO_GIT && exists(join(root, ".git"))) {
        const [r, w] = os.pipe();
        const pid = os.exec(["git", "-C", root, "ls-files", "-co", "--exclude-standard", "-z"], { block: false, stdout: w });
        os.close(w);
        const fh = std.fdopen(r, "r");
        const txt = fh.readAsString();
        fh.close();
        const [, st] = os.waitpid(pid, 0);
        if (st === 0) set = new Set(txt.split("\0").filter(Boolean));
    }
    GIT_VISIBLE = { root, set };
    return set;
}
let NO_GIT = false;

function* scanFiles(root) {
    const vis = gitVisible(root);
    for (const p of scanFilesAll(root)) if (!vis || vis.has(p.slice(root.length + 1))) yield p;
}

function* scanFilesAll(root) {
    for (const fn of DEFECT_ROOT_FILES) if (isfile(join(root, fn))) yield join(root, fn);
    for (const top of DEFECT_ROOT_DIRS) {
        for (const [dirpath, dirnames, filenames] of walk(join(root, top))) {
            const keep = dirnames.filter((d) => !DEFECT_SKIP_DIRS.has(d)).sort(cmpStr);
            dirnames.splice(0, dirnames.length, ...keep);
            for (const fn of [...filenames].sort(cmpStr))
                if (fn.endsWith(".c") || fn.endsWith(".h") || fn.endsWith(".inc")) yield join(dirpath, fn);
        }
    }
}

const INCLUDE_RE = /#include\s+"([^"]+)"/g;
const MAX_FILE = 8 * 1024 * 1024;

function fileDefects(path) {
    let raw;
    try {
        raw = readText(path, "utf8-replace");
    } catch (e) {
        return null;
    }
    if (raw.length > MAX_FILE) return null;
    LOCALS_CACHE.clear();
    const blank = blankC(raw);
    const fns = findFunctions(blank, raw);
    const out = [];
    for (const [cid, , , fn] of DEFECT_META) {
        if (cid === "D18-inc-include" || cid === "D19-build-coverage") continue;
        const start = out.length;
        try {
            fn(path, raw, blank, fns, out);
        } catch (ex) {
            out.push(new Defect(cid.split("-")[0], "error", path, 0, "<scanner>", "check crashed: " + (ex.pyRepr || String(ex))));
        }
        runStages(cid.split("-")[0], out, start);
    }
    for (const d of out) {
        if (d.fn) continue;
        const f = fns.find((x) => x[2] <= d.line && d.line <= x[3]);
        d.fn = f ? f[0] : "-";
    }
    return [[...raw.matchAll(INCLUDE_RE)].map((m) => m[1]), out];
}

const STAGES = new Map();
const STAGE_TRACE = !!std.getenv("DEFECT_STAGE_TRACE");
const STAGE_STATS = new Map();
function stage(id, name, drop) {
    if (!STAGES.has(id)) STAGES.set(id, []);
    STAGES.get(id).push([name, drop]);
}
function stageStats(id) {
    let s = STAGE_STATS.get(id);
    if (!s) {
        s = { raw: 0, after: (STAGES.get(id) || []).map(() => 0) };
        STAGE_STATS.set(id, s);
    }
    return s;
}
function runStages(id, out, start) {
    const st = STAGES.get(id);
    if (!st || out.length === start) return;
    let cur = out.splice(start);
    const s = stageStats(id);
    s.raw += cur.length;
    for (let i = 0; i < st.length; i++) {
        const drop = st[i][1];
        cur = cur.filter((d) => {
            const gone = !!(d.ctx && drop(d.ctx));
            if (gone && STAGE_TRACE) std.err.puts("DROP " + id + " " + st[i][0] + " " + d.file + ":" + d.line + "\n");
            return !gone;
        });
        s.after[i] += cur.length;
    }
    for (const d of cur) {
        d.ctx = null;
        out.push(d);
    }
}
function emit(out, d, ctx) {
    d.ctx = ctx;
    out.push(d);
}

function bodyRaw(raw, off, body) {
    return raw.slice(off, off + body.length);
}

function lineAround(raw, s, e) {
    const a = raw.lastIndexOf("\n", s - 1) + 1;
    const b = raw.indexOf("\n", e);
    return raw.slice(a, b < 0 ? raw.length - 1 : b);
}

function closeParen(body, from) {
    let d = 1, j = from;
    while (j < body.length && d) {
        const c = body[j];
        if (c === "(") d++;
        else if (c === ")") d--;
        j++;
    }
    return j;
}

const reCache = new Map();
function R(src, flags = "g") {
    const k = flags + "\u0000" + src;
    let r = reCache.get(k);
    if (!r) {
        r = new RegExp(src, flags);
        reCache.set(k, r);
    }
    r.lastIndex = 0;
    return r;
}
function count(src, s) {
    let n = 0;
    const r = R(src);
    while (r.exec(s)) n++;
    return n;
}

check("D01-c23-octal", "error", "C23 0o octal literal (breaks gcc/clang -std=gnu17; audit C16)", (path, raw, blank, fns, out) => {
    for (const m of blank.matchAll(/\b0[oO][0-7]+\w*/g)) {
        const ln = lineOf(blank, m.index);
        out.push(new Defect("D01", "error", path, ln, lineAround(raw, m.index, m.index + m[0].length),
            "0o literal: Apple clang accepts, gcc/Debian-clang reject in gnu17; use 0-prefixed octal"));
    }
});

const TESTS_DIR = /\/tests?\//;
let SCAN_ROOT = "";
const relp = (p) => (SCAN_ROOT && p.startsWith(SCAN_ROOT + "/") ? p.slice(SCAN_ROOT.length) : p);
check("D02-banned-fn", "error", "unbounded string/temp functions (strcpy, sprintf, gets, tmpnam...)", (path, raw, blank, fns, out) => {
    const sev = TESTS_DIR.test(relp(path)) ? "info" : "error";
    for (const m of blank.matchAll(/\b(strcpy|strcat|sprintf|vsprintf|gets|tmpnam|tempnam)\s*\(/g)) {
        const ln = lineOf(blank, m.index);
        out.push(new Defect("D02", sev, path, ln, lineAround(raw, m.index, m.index + m[0].length),
            m[1] + "(): unbounded; use the bounded form"));
    }
});

const LVAL = "[A-Za-z_]\\w*(?:\\s*(?:->|\\.)\\s*\\w+|\\s*\\[[^\\]\\n]*\\])*";
const LVAL_REALLOC = new RegExp("(?<![\\w>.\\]])(" + LVAL + ")\\s*=\\s*realloc\\s*\\(\\s*(" + LVAL + ")\\s*,", "g");
check("D03-realloc-self", "warn", "p = realloc(p,...) clobbers the only pointer on failure", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        for (const m of body.matchAll(LVAL_REALLOC)) {
            const lv = m[1].replace(/\s+/g, "");
            if (lv !== m[2].replace(/\s+/g, "")) continue;
            const end = body.indexOf(";", m.index + m[0].length);
            emit(out, new Defect("D03", "warn", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, m[0]),
                "realloc into the same variable: on failure the block leaks (allocator shims where the caller keeps its own pointer are the accepted exception)"),
                { lv, params: fnParams(sig), rest: end < 0 ? "" : body.slice(end + 1, end + 1 + 400) });
        }
    }
});
stage("D03", "allocator-shim", (c) => /^[A-Za-z_]\w*$/.test(c.lv) && c.params.has(c.lv));
stage("D03", "fails-hard", (c) => {
    const q = reEscape(c.lv).replace(/->/g, "\\s*->\\s*").replace(/\\\./g, "\\s*\\.\\s*");
    for (const m of c.rest.matchAll(R("\\bif\\s*\\(", "g"))) {
        const ce = closeParen(c.rest, m.index + m[0].length);
        if (R("!\\s*" + q + "(?![\\w\\[.>-])", "").test(c.rest.slice(m.index, ce))
            && /^\s*\{?\s*(?:exit|_exit|abort|\w+_fail|\w*fatal)\s*\(/.test(c.rest.slice(ce))) return true;
    }
    return false;
});

function stmtPrefixBare(body, at) {
    let p = body.slice(Math.max(body.lastIndexOf(";", at - 1), body.lastIndexOf("{", at - 1), body.lastIndexOf("}", at - 1)) + 1, at);
    for (;;) {
        p = p.replace(/^\s+/, "");
        const kw = /^(?:else|do)\b|^(?:case\b[^:]*|default|[A-Za-z_]\w*)\s*:(?!:)/.exec(p);
        if (kw) { p = p.slice(kw[0].length); continue; }
        const ctl = /^(?:if|while|for|switch)\s*\(/.exec(p);
        if (!ctl) break;
        const j = closeParen(p, ctl[0].length);
        if (j > p.length) return false;
        p = p.slice(j);
    }
    return /^(?:\(\s*void\s*\)\s*)?$/.test(p);
}

check("D04-ignored-errconv", "warn", "error-convention call as a bare statement (JS_To* returns -1; audit S11)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(ERRCONV_CALL)) {
            const seg = body.slice(m.index, m.index + 160);
            const head = /^[\w\s(*&,]*\([^;{)]*\)\s*;/.test(seg);
            if (head && stmtPrefixBare(body, m.index)) {
                emit(out, new Defect("D04", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 60)),
                    "return value ignored: -1 leaves a stale out-param under a pending exception"),
                    { fn: m[1], args: splitTopArgs(body.slice(m.index + m[0].length, closeParen(body, m.index + m[0].length) - 1)), head: body.slice(0, m.index), file: blank });
            }
        }
    }
});

function d04Guarded(head, v) {
    const q = reEscape(v);
    const test = "(?:JS_IsNumber\\s*\\(\\s*" + q + "\\s*\\)|JS_IsBool\\s*\\(\\s*" + q + "\\s*\\)|JS_TAG_IS_FLOAT64\\s*\\(\\s*JS_VALUE_GET_(?:NORM_)?TAG\\s*\\(\\s*" + q + "\\s*\\)\\s*\\)|JS_VALUE_GET_(?:NORM_)?TAG\\s*\\(\\s*" + q + "\\s*\\)\\s*==\\s*JS_TAG_(?:INT|FLOAT64|BOOL))";
    const h = head.slice(-800);
    let best = false;
    for (const m of h.matchAll(R("(!\\s*)?" + test, "g"))) {
        const after = h.slice(m.index + m[0].length);
        if (R("\\b" + q + "\\s*=(?!=)", "").test(after)) continue;
        if (m[1]) {
            const cond = h.slice(Math.max(0, h.lastIndexOf("if", m.index)), m.index);
            if (!/\bif\s*\([^;{]*$/.test(cond)) continue;
            const j = closeParen(h, h.indexOf("(", h.lastIndexOf("if", m.index)) + 1);
            best = /^\s*(?:\{[^}]*|[^;{]*)\b(?:return|goto|break|continue|JS_Throw\w*)\b/.test(h.slice(j)) || best;
        } else {
            best = !/\belse\b/.test(after) || best;
        }
    }
    return best;
}
stage("D04", "known-number", (c) => {
    const v = (c.args[c.args.length - 1] || "").trim();
    return /^[\w.\->\[\]]+$/.test(v) && d04Guarded(c.head, v);
});
stage("D04", "closure-data", (c) => {
    const m = /^(?:func_)?data\s*\[\s*(\w+)\s*\]$/.exec((c.args[c.args.length - 1] || "").trim());
    return !!m && R("\\b\\w+\\s*\\[\\s*" + m[1] + "\\s*\\]\\s*=\\s*(?:\\(JSValueConst\\)\\s*)?JS_New(?:Int32|Uint32|Int64|Float64)\\s*\\(", "").test(c.file);
});
stage("D04", "numeric-origin", (c) => {
    const v = (c.args[c.args.length - 1] || "").replace(/\s+/g, "");
    if (/(?:->|\.)u\.(?:value|num\.val)$/.test(v)) return true;
    if (!/^\w+$/.test(v)) return false;
    const as = [...c.head.matchAll(R("\\b" + v + "\\s*=(?!=)\\s*([^;]+);", "g"))];
    return as.length > 0 && /^(?:JS_ToNumeric(?:Free)?|JS_New(?:Int32|Uint32|Int64|Float64|Number))\s*\(/.test(as[as.length - 1][1]);
});

check("D05-snprintf-accum", "warn", "x += snprintf(...): accumulates the UNclamped would-write count (audit: mathx/schema/url)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\b\w+\s*\+=\s*snprintf\s*\(/g)) {
            out.push(new Defect("D05", "warn", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 70)),
                "snprintf returns needed-length, not written-length; a truncating call poisons the accumulator"));
        }
    }
});

check("D06-assign-in-cond", "warn", "assignment inside if(...)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\bif\s*\(/g)) {
            const mEnd = m.index + m[0].length;
            const j = closeParen(body, mEnd);
            const cond = body.slice(mEnd, j - 1);
            for (const a of cond.matchAll(/(?<![=!<>+\-*/%&|^])=(?!=)/g)) {
                const ctx = cond.slice(Math.max(0, a.index - 30), a.index + 30);
                if (/\b(for|sizeof)\b/.test(ctx)) continue;
                const pre = cond.slice(0, a.index);
                if (/\(\(\s*[\w.\->\[\]]+\s*$/.test(pre)) continue;
                out.push(new Defect("D06", "warn", path, lineOf(blank, off + m.index),
                    "if (...) " + pyStrip(ctx), "single = inside a condition (== typo class)"));
            }
        }
    }
});

const FLOAT_LIT = "(?:\\d+\\.\\d(?:[eE][+-]?\\d+)?f?|\\d+[eE][+-]?\\d+f?|\\d+f)";
const FLOAT_EQ = new RegExp("(" + FLOAT_LIT + ")\\s*(==|!=)\\s*|" + FLOAT_LIT + "\\s*(==|!=)", "g");
check("D07-float-eq", "info", "float literal in an ==/!= comparison", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(FLOAT_EQ)) {
            out.push(new Defect("D07", "info", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(Math.max(0, m.index - 20), m.index + 30)),
                "float equality (tolerance compare unless value is exact by construction)"));
        }
    }
});

check("D08-strlen-repeat", "warn", "strlen(s) computed 2+ times in one function (repo-fixed pattern)", (path, raw, blank, fns, out) => {
    for (const [name, , , , , , body, off] of fns) {
        const seen = new Map();
        for (const m of body.matchAll(/strlen\s*\(\s*(\w+)\s*\)/g)) {
            if (!seen.has(m[1])) seen.set(m[1], []);
            seen.get(m[1]).push(m.index);
        }
        for (const [v, poss] of seen) {
            if (poss.length >= 2) {
                out.push(new Defect("D08", "warn", path, lineOf(blank, off + poss[0]),
                    "strlen(" + v + ") x" + poss.length + " in " + name,
                    "hoist the length: every repeat rescans the string"));
            }
        }
    }
});

check("D09-memset-dce", "warn", "memset() on a buffer with no later read (DCE risk; audit S10)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/memset\s*\(\s*&?\s*(\w+)\b/g)) {
            const v = m[1];
            let ce = m.index + m[0].length, dd = 0;
            while (ce < body.length) {
                const c = body[ce];
                if (c === "(") dd++;
                else if (c === ")") {
                    if (dd === 0) break;
                    dd--;
                }
                ce++;
            }
            const rest = body.slice(ce + 1);
            const uses = [];
            const ru = R("\\b" + v + "\\b");
            let u;
            while ((u = ru.exec(rest))) uses.push(u.index);
            const onlyFree = uses.length > 0 && uses.every((p) => /^\s*\)/.test(rest.slice(p)) ||
                R("free\\s*\\(\\s*" + v, "").test(rest.slice(Math.max(0, p - 6), p + 20)));
            if (!uses.length || onlyFree) {
                out.push(new Defect("D09", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                    "memset with no later read: the optimizer may delete it (key material must use explicit_bzero/memset_s)"));
            }
        }
    }
});

check("D10-sizeof-param", "warn", "sizeof(pointer-parameter) as a size argument", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        const params = fnParams(sig);
        if (!params.size) continue;
        for (const m of body.matchAll(/\b(memcpy|memmove|memset|strncpy|snprintf)\s*\(/g)) {
            const j = closeParen(body, m.index + m[0].length);
            const call = body.slice(m.index, j);
            for (const s of call.matchAll(/sizeof\s*\(\s*(\w+)\s*\)/g)) {
                if (params.has(s[1])) {
                    out.push(new Defect("D10", "warn", path, lineOf(blank, off + m.index), call.slice(0, 90),
                        "sizeof(" + s[1] + ") where " + s[1] + " is a parameter: sizeof(ptr)==8, not the buffer size"));
                }
            }
        }
    }
});

const LOCK_PAIRS = [["pthread_mutex_lock", "pthread_mutex_unlock"],
    ["pthread_rwlock_rdlock", "pthread_rwlock_unlock"],
    ["pthread_rwlock_wrlock", "pthread_rwlock_unlock"]];
check("D11-lock-balance", "warn", "pthread lock/unlock counts differ in one function", (path, raw, blank, fns, out) => {
    for (const [name, , l0, , , , body] of fns) {
        for (const [lk, ul] of LOCK_PAIRS) {
            const nl = count("\\b" + lk + "\\s*\\(", body);
            const nu = count("\\b" + ul + "\\s*\\(", body);
            if (nl !== nu) {
                out.push(new Defect("D11", "warn", path, l0, name + ": " + nl + " " + lk + " vs " + nu + " " + ul,
                    "lock/unlock imbalance in one function (early returns need cleanup labels)"));
            }
        }
    }
});

check("D12-js-reentry", "warn", "dyn_res_get/dyn_plain_get BEFORE a JS-invoking call (violates the coerce-then-resolve doctrine)", (path, raw, blank, fns, out) => {
    const jsrun = /\b(JS_GetPropertyStr|JS_GetPropertyUint32|JS_ToCString|JS_Call|JS_GetProperty)\s*\(/g;
    const get = /\b(dyn_res_get|dyn_plain_get|dyn_res_native)\s*\(/;
    for (const [name, , , , , , body, off] of fns) {
        const g = get.exec(body);
        if (!g) continue;
        jsrun.lastIndex = g.index + g[0].length;
        const j = jsrun.exec(body);
        if (j) {
            out.push(new Defect("D12", "warn", path, lineOf(blank, off + j.index),
                bodyRaw(raw, off + j.index, body.slice(j.index, j.index + 60)),
                name + " resolves the native handle then runs user JS; user JS can close()/dispose the resource mid-call -- coerce args FIRST (dyna-nat.h contract)"));
        }
    }
});

check("D13-blocking-io", "warn", "blocking filesystem call inside the reactor core (aio/evloop)", (path, raw, blank, fns, out) => {
    if (!/dyna-(aio|evloop)/.test(path)) return;
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\b(fopen|fsync|fdatasync|opendir|unlink|rename|mkdir|remove)\s*\(/g)) {
            out.push(new Defect("D13", "warn", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                m[1] + ": blocking fs call in the reactor core stalls every connection -- offload to the disk pool"));
        }
    }
});

check("D14-loop-counter-write", "info", "for-loop counter assigned inside the body", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\bfor\s*\(\s*(?:\w+\s+)?(\w+)\s*=/g)) {
            const v = m[1];
            let j = closeParen(body, m.index + m[0].length);
            while (j < body.length && " \t\r\n".includes(body[j])) j++;
            if (j >= body.length || body[j] !== "{") continue;
            let e = j, bd = 0;
            while (e < body.length) {
                const c = body[e];
                if (c === "{") bd++;
                else if (c === "}") {
                    bd--;
                    if (bd === 0) break;
                }
                e++;
            }
            const inner = body.slice(j, e);
            const w = R("\\b" + v + "\\s*(?:\\+\\+|--|=[^=])").exec(inner);
            if (w) {
                out.push(new Defect("D14", "info", path, lineOf(blank, off + w.index),
                    bodyRaw(raw, off + w.index, inner.slice(w.index, w.index + 40)),
                    "loop counter '" + v + "' modified in body"));
            }
        }
    }
});

check("D15-js-value-balance", "info", "JS get/free counts far apart in one function (leak smell)", (path, raw, blank, fns, out) => {
    for (const [name, , l0, , , , body] of fns) {
        const g = count("\\bJS_GetProperty(?:Str|Uint32)?\\s*\\(", body) + count("\\bJS_ToCString(?:Len)?\\s*\\(", body);
        const f = count("\\bJS_FreeValue(?:RT)?\\s*\\(", body) + count("\\bJS_FreeCString\\s*\\(", body);
        if (g - f >= 4) {
            out.push(new Defect("D15", "info", path, l0, name + ": " + g + " gets vs " + f + " frees",
                "owned-JSValue surplus in one function -- early-return paths are the usual leak; verify each goto/err label frees"));
        }
    }
});

check("D16-narrow-cmp", "info", "32-bit cast on one side of a size comparison (64->32 boundary)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\((?:int|int32_t|uint32_t|unsigned)\)\s*\w+\s*(?:<=|>=|<|>)/g)) {
            out.push(new Defect("D16", "info", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                "narrowed operand in a size comparison: values past 2^31 compare wrong (timestamps, offsets)"));
        }
    }
});

check("D17-comment-marker", "warn", "line comment containing block-comment markers", (path, raw, blank, fns, out) => {
    const lines = raw.split("\n");
    for (let i = 0; i < lines.length; i++) {
        const ln = lines[i];
        const k = ln.indexOf("//");
        if (k >= 0) {
            const tail = ln.slice(k + 2);
            if (tail.includes("*/") || tail.includes("/*")) {
                out.push(new Defect("D17", "warn", path, i + 1, pyStrip(ln).slice(0, 90),
                    "block-comment marker inside a // comment -- often a half-converted docstring"));
            }
        }
    }
});

check("D18-inc-include", "error", "*.inc.c must be #included exactly once project-wide", () => {});

const reEsc = (t) => t.replace(/[.*+?^${}()|[\]\\\/-]/g, "\\$&");

const globRe = (g) => R("^" + g.split("*").map(reEsc).join(".*") + "$", "");

function discoveryCoverage(out, mkText, srcFiles, root) {
    const gated = new Set([...mkText.matchAll(/\$\{x\[@\]\/([^/}]+(?:\\\/[^/}]+)*)\/-\}/g)].map((g) => g[1].replace(/\\\//g, "/")));
    const excl = [];
    for (const fl of mkText.split("\n").filter((l) => l.includes("find src -name '*.c'") || /^\s*local x=\(/.test(l))) {
        for (const e of fl.matchAll(/! -(name|path) '?([^'\s)]+)'?/g)) {
            if (gated.has(e[2]) || e[2] === "*.inc.c" || (e[1] === "path" && /^src\/[^*]+\/\*$/.test(e[2]) && !gated.has(e[2]))) continue;
            excl.push({ kind: e[1], re: globRe(e[2]), pat: e[2] });
        }
    }
    for (const sf of srcFiles) {
        const rel = sf.slice(root.length).replace(/^\/+/, "");
        const hit = excl.find((x) => x.re.test(x.kind === "name" ? basename(sf) : rel));
        if (hit && !mkText.includes(rel)) {
            out.push(new Defect("D19", "error", sf, 1, "", "build.sh's source discovery excludes it (" + hit.pat + ") and nothing builds it by path -- it will silently not be built"));
        }
    }
}

function makefileCoverage(out, mkText, srcFiles, root) {
    if (mkText.includes("find src -name '*.c'")) return discoveryCoverage(out, mkText, srcFiles, root);
    const m =/^ENG_CORE_LIBS="([^"]*)"/m.exec(mkText);
    const coreLibs = new Set(m ? m[1].split(/\s+/).filter(Boolean) : []);
    const mm = /^mod_core_libs\(\)\{\n([\s\S]*?)^\}/m.exec(mkText);
    if (mm && mkText.includes('for l in $(mod_core_libs "$1"); do _dir_objs "core/$l"')) {
        for (const lm of mm[1].matchAll(/^\s+[\w-]+\) printf '%s' "([^"]*)" ;;$/gm))
            for (const l of lm[1].split(/\s+/).filter(Boolean)) coreLibs.add(l);
    }
    const discoversModules = mkText.includes('_dir_objs "modules/$1"');
    for (const sf of srcFiles) {
        const rel = sf;
        let dm = /\/src\/core\/([^/]+)\/[^/]+\.c$/.exec(rel) || /\/src\/core\/dyn-([^/]+)\.c$/.exec(rel);
        if (dm && coreLibs.has(dm[1])) continue;
        if (/\/src\/builtins\/[^/]+\.c$/.test(rel) && mkText.includes("_dir_objs builtins")) continue;
        dm = /\/src\/modules\/([^/]+)\/[^/]+\.c$/.exec(rel);
        if (dm && discoversModules && R("\\b" + reEsc(dm[1]) + "\\b", "").test(mkText)) continue;
        const stem = basename(sf).slice(0, -2);
        if (dm || /\/src\/core\/[^/]+\//.test(rel) || !R("\\b" + reEsc(stem) + "\\b", "").test(mkText)) {
            out.push(new Defect("D19", "error", sf, 1, "", "source file absent from build.sh -- it will silently not be built"));
        }
    }
}
check("D19-build-coverage", "error", "src/*.c the build never compiles. A discovering build.sh (find src -name '*.c'): a file its -name/-path exclusions drop that no switch re-admits and no rule builds by path. A listing build.sh: (agent added a file, forgot the build); src/core/<lib>/*.c needs <lib> in ENG_CORE_LIBS or in a mod_core_libs row (module-only lib), src/modules/<m>/*.c needs module <m> known to build.sh", () => {});

check("D20-error-grammar", "info", "JS_Throw literal without the '<module>.<ctx>:' prefix grammar", (path, raw, blank, fns, out) => {
    if (!/dyna-[a-z0-9-]+\.(c|inc\.c)$/.test(path)) return;
    for (const m of raw.matchAll(/JS_Throw\w+\s*\(\s*\w+\s*,\s*"([^"]{12,})"/g)) {
        const s = m[1];
        if (s.includes("%")) continue;
        if (!/^[A-Za-z0-9_ ./<>-]{2,32}:/.test(s)) {
            out.push(new Defect("D20", "info", path, lineOf(blank, m.index), s.slice(0, 80),
                "literal throw without '<module>.<context>:' prefix (docs/ERRORS.md grammar)"));
        }
    }
});

function pyList(arr) {
    return "[" + arr.map(pyRepr).join(", ") + "]";
}

const INC_GUARD = /^\s*(?:#\s*pragma\s+once\b|#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b)/;

function projectChecks(root, out, includesOf, incs) {
    const byBase = new Map();
    for (const [path, incsList] of includesOf) {
        for (const inc of incsList) {
            const b = basename(inc);
            if (!byBase.has(b)) byBase.set(b, []);
            byBase.get(b).push(path);
        }
    }
    for (const [base, ipath] of incs) {
        const users = byBase.get(base) || [];
        if (users.length === 0) {
            out.push(new Defect("D18", "error", ipath, 1, "", ".inc.c fragment included by NOTHING (dead code or a forgotten #include)"));
        } else if (users.length > 1) {
            let nonstatic, guarded = false;
            try {
                const frag = readText(ipath, "utf8-replace");
                const fb = blankC(frag);
                nonstatic = findFunctions(fb, frag).filter((f) => !f[4]).map((f) => f[0]);
                guarded = INC_GUARD.test(fb);
            } catch (e) {
                nonstatic = ["?"];
            }
            if (guarded && !nonstatic.length) continue;
            const sev = nonstatic.length ? "error" : "info";
            out.push(new Defect("D18", sev, ipath, 1, "", ".inc.c fragment included by " + users.length + " files " + pyList(users) +
                " (" + (nonstatic.length ? "non-static symbols: duplicate at link" : "all-static: legal duplication, two copies to keep in sync") + ")"));
        }
    }
    let mk = null;
    for (const mkName of ["build.sh", "Makefile"]) {
        const p = join(root, mkName);
        if (exists(p)) {
            mk = p;
            break;
        }
    }
    if (mk !== null) {
        const mkText = readText(mk, "utf8-replace");
        const srcFiles = [...scanFiles(root)].filter((p) => p.includes("/src/") && p.endsWith(".c") && !p.endsWith(".inc.c"));
        makefileCoverage(out, mkText, srcFiles, root);
    }
    headerStaticAddrTaken(out, [...scanFiles(root)].filter((p) => p.includes("/src/")));
}

const ALLOC_FNS = "js_mallocz?|malloc|calloc";
const ELEM = [["char", 1], ["int8", 1], ["uint8", 1], ["int16", 2], ["uint16", 2],
    ["short", 2], ["int", 4], ["uint32", 4], ["int32", 4], ["float", 4],
    ["long", 8], ["int64", 8], ["uint64", 8], ["double", 8]];
const LOCALS_CACHE = new Map();
const LOC_ARR = /\b(?:static\s+|const\s+|unsigned\s+|struct\s+\w+\s+)*(\w[\w]*)\s+(\w+)\s*\[\s*(\d+)\s*\]/g;
const LOC_SCALAR = /\b(?:static\s+|const\s+|unsigned\s+|register\s+)*(?:void|int|char|long|short|float|double|unsigned|signed|size_t|ssize_t|uint\d+_t|int\d+_t|JSValue|JSValueConst|\w+_t)\s+\*?\s*(\w+)\s*(?:=[^=]|;)/g;
const LOC_SKIP = new Set(["if", "for", "while", "switch", "return", "sizeof"]);

function locals(body) {
    const hit = LOCALS_CACHE.get(body);
    if (hit) return hit;
    const out = new Map();
    for (const m of body.matchAll(LOC_ARR)) {
        const t = m[1], v = m[2], n = parseInt(m[3], 10);
        if (LOC_SKIP.has(t)) continue;
        let es = 0;
        for (const [k, b] of ELEM) {
            if (t.includes(k)) {
                es = b;
                break;
            }
        }
        if (!out.has(v)) out.set(v, { array: n, elem: es });
    }
    for (const m of body.matchAll(LOC_SCALAR)) {
        const v = m[1];
        if (!LOC_SKIP.has(v) && !out.has(v)) out.set(v, { scalar: true });
    }
    if (LOCALS_CACHE.size >= 4096) LOCALS_CACHE.clear();
    LOCALS_CACHE.set(body, out);
    return out;
}

const BARE_BEFORE = /(=|return|if|while|for|&&|\|\||,)\s*$|=\s*$/;
function bareStatement(body, fnameRe) {
    const hits = [];
    for (const m of body.matchAll(fnameRe)) {
        const before = pyStrip(body.slice(Math.max(0, m.index - 40), m.index));
        if (BARE_BEFORE.test(before)) continue;
        const stmt = body.slice(Math.max(body.lastIndexOf(";", m.index), body.lastIndexOf("{", m.index), body.lastIndexOf("}", m.index)) + 1, m.index);
        if (/(?:->|\.)\s*$/.test(stmt) || /(?<![=!<>])=(?!=)/.test(stmt.replace(/\([^()]*\)/g, ""))) continue;
        const mEnd = m.index + m[0].length;
        let j = mEnd - 1;
        if (j >= 0 && body[j] === "(") {
            j = closeParen(body, mEnd);
            let k = j;
            while (k < body.length && " \t\r\n".includes(body[k])) k++;
            if (k < body.length && body[k] === ";") hits.push(m);
        }
    }
    return hits;
}

function headerStaticAddrTaken(out, files) {
    const hdrStatic = new Map();
    for (const p of files) {
        if (!p.endsWith(".h")) continue;
        let hraw;
        try {
            hraw = readText(p, "utf8-replace");
        } catch (e) {
            continue;
        }
        if (!hraw.includes("static")) continue;
        const hblank = blankC(hraw);
        for (const f of findFunctions(hblank, hraw)) if (f[4] && !hdrStatic.has(f[0])) hdrStatic.set(f[0], [p, f[2]]);
    }
    if (!hdrStatic.size) return;
    const names = [...hdrStatic.keys()].sort((a, b) => b.length - a.length).map(reEsc).join("|");
    const quick = new RegExp("\\b(?:" + names + ")\\b");
    const cmpPat = new RegExp("(?:[=!]=\\s*&?\\s*\\b(" + names + ")\\b(?!\\s*\\()|\\b(" + names + ")\\b\\s*[=!]=)", "gd");
    const rmPat = /\b(\w*(?:Remove|remove|Unregister|unregister|Unwatch|unwatch|_off|Off)\w*)\s*\(([^;{}]*)\)/gd;
    const pairs = [["Remove", "Add"], ["remove", "add"], ["Unregister", "Register"], ["unregister", "register"],
        ["Unwatch", "Watch"], ["unwatch", "watch"], ["_off", "_on"], ["Off", "On"]];
    const argPat = new RegExp("(?:^|,)\\s*&?\\s*(" + names + ")\\s*(?=,|$)", "gd");
    const texts = new Map();
    for (const p of files) {
        try {
            texts.set(p, readText(p, "utf8-replace"));
        } catch (e) {}
    }
    const alltext = [...texts.values()].join("\n");
    for (const [p, fraw] of texts) {
        if (!quick.test(fraw)) continue;
        const fblank = blankC(fraw);
        const seen = new Set([...fraw.matchAll(INCLUDE_RE)].map((m) => basename(m[1])));
        seen.add(basename(p));
        const hits = [];
        for (const m of fblank.matchAll(cmpPat)) {
            const g = m[1] !== undefined ? 1 : 2;
            hits.push([m.indices[g][0], m[g]]);
        }
        for (const m of fblank.matchAll(rmPat)) {
            const callee = m[1];
            const adds = new Set();
            for (const [r, a] of pairs) if (callee.includes(r)) adds.add(callee.replace(r, a));
            adds.delete(callee);
            const argOff = m.indices[2][0];
            for (const a of m[2].matchAll(argPat)) {
                const hit = [...adds].some((ad) => new RegExp("\\b" + reEsc(ad) + "\\s*\\([^;{}]*\\b" + reEsc(a[1]) + "\\b").test(alltext));
                if (hit) hits.push([argOff + a.indices[1][0], a[1]]);
            }
        }
        for (const [pos, name] of hits) {
            const [hp, hl] = hdrStatic.get(name);
            if (!seen.has(basename(hp))) continue;
            out.push(new Defect("D46", "error", p, lineOf(fblank, pos), name,
                name + " is compared or removed by address, but it is a static function defined in a header (" + basename(hp) + ":" + hl +
                "): each TU gets its own copy, so a pointer registered in one file never matches one removed or compared in another -- define it once in a .c"));
        }
    }
}
check("D46-header-static-identity", "error", "a static function defined in a header is compared or removed by address: every TU has its own copy, so the identity never matches", () => {});

check("D21-alloc-arith-overflow", "warn", "malloc(a * b): allocation size is unchecked arithmetic (CWE-190)", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        for (const m of body.matchAll(R("\\b(" + ALLOC_FNS + ")\\s*\\("))) {
            const mEnd = m.index + m[0].length;
            const j = closeParen(body, mEnd);
            const arg = body.slice(mEnd, j - 1);
            const a1 = pyStrip(arg.split(",")[0]);
            for (const expr of [a1, arg.includes(",") && m[0].includes("calloc") ? arg : a1]) {
                const es = pyStrip(expr);
                if (/(?<!sizeof\s\()\b\w+\s*\*\s*\w+/.test(expr) && !expr.includes("sizeof") &&
                    !arg.includes("SIZE_MAX") && !arg.includes("size_max") &&
                    !/^sizeof/.test(es) && !/^[\w\s.\->]+\*$/.test(es)) {
                    emit(out, new Defect("D21", "warn", path, lineOf(blank, off + m.index),
                        bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 70)),
                        "allocation size is a product of two values with no overflow guard on the statement"),
                        { expr: d31Norm(expr), cexpr: expr.replace(/\s+/g, ""), head: body.slice(0, m.index), sig });
                    break;
                }
            }
        }
    }
});

function d21Factors(e) {
    const ids = new Set();
    for (const t of e.matchAll(/(?:\b[A-Za-z_]\w*(?:(?:->|\.)\w+)*)/g)) if (!/^(?:sizeof|[A-Z][A-Z0-9_]*)$/.test(t[0])) ids.add(t[0]);
    return [...ids];
}
stage("D21", "constant-size", (c) => d21Factors(c.expr).length === 0);
stage("D21", "bounded-operand", (c) => {
    const h = c.head.replace(/\s+/g, "");
    const fs = d21Factors(c.expr);
    if (!fs.length) return false;
    if (fs.some((v) => R("if\\([^;{]*SIZE_MAX[^;{]*\\b" + reEscape(v) + "\\b|if\\([^;{]*\\b" + reEscape(v) + "\\b[^;{]*SIZE_MAX", "").test(h))) return true;
    return fs.every((v) => {
        const q = reEscape(v);
        return R("if\\([^;{]*(?:\\b" + q + ">=?(?![>=])|<=?" + q + "\\b)[^;{]*\\)(?:\\{)?(?:return|goto|break|continue)", "").test(h) ||
            R("if\\(" + q + ">=?\\w+\\)\\{?" + q + "=\\w+;", "").test(h) ||
            R("(?:size_mul_overflows|dyn_size_mul|__builtin_mul_overflow)\\([^;]*\\b" + q + "\\b", "").test(h);
    });
});
const D21_CAP = /^(?:[A-Z][A-Z0-9_]*|\d+u?)$/;
stage("D21", "constant-assigned", (c) => {
    const h = c.head.replace(/\s+/g, "");
    const fs = d21Factors(c.expr);
    return fs.length > 0 && fs.every((v) => {
        const rhs = [...h.matchAll(R("(?:^|[;{(,]|\\b[A-Za-z_]\\w*\\**)" + reEscape(v) + "=(?!=)([^;,]+)[;,]", "g"))].map((m) => m[1]);
        if (!rhs.length || fnParams(c.sig).has(v)) return false;
        if (R("\\b" + reEscape(v) + "(?:\\+\\+|--|[-+*/|&^]=)|(?:\\+\\+|--|&)" + reEscape(v) + "\\b", "").test(h)) return false;
        return rhs.every((r) => {
            if (D21_CAP.test(r)) return true;
            const t = /^\(?(\w+)(<=?|>=?)(\w+)\)?\?(\w+):(\w+)$/.exec(r);
            if (!t) return false;
            const cap = D21_CAP.test(t[3]) ? t[3] : D21_CAP.test(t[1]) ? t[1] : null;
            return !!cap && (t[4] === cap || t[5] === cap) && (t[4] === cap) === (t[2][0] === (cap === t[3] ? ">" : "<"));
        });
    });
});
const D21_NARROW = "(?:const\\s+)?(?:u?int(?:8|16|32)_t|int|unsigned(?:\\s+(?:int|short))?|short)";
stage("D21", "widened-32bit", (c) => {
    const terms = d31Split(c.cexpr).join("").split(/(?<=.)(?=[-+](?![>]))/);
    const all = [];
    for (let t of terms) {
        t = t.replace(/^[-+]/, "");
        const fac = t.split("*");
        const vars = fac.map((x) => x.replace(/^\((?:size_t|uint64_t)\)/, "")).filter((x) => !D21_CAP.test(x));
        if (fac.length > 2 || (fac.length === 2 && !/^\((?:size_t|uint64_t)\)/.test(t))) return false;
        if (!vars.every((x) => /^\w+$/.test(x))) return false;
        all.push(...vars);
    }
    const decl = c.sig + ";" + c.head;
    return all.length > 0 && terms.length <= 3 && all.every((v) => R("\\b" + D21_NARROW + "\\s+(?:[^;(){}]*,\\s*)?\\**\\s*" + v + "\\s*[,;=)\\[]", "").test(decl));
});

function d22Root(a) {
    let t = a.replace(/\s+/g, " ").trim();
    for (;;) {
        const u = t.replace(/^\((?:const |volatile |struct |unsigned |signed )*\w+(?: \w+)*\s*\**\s*\)\s*/, "").replace(/^&\s*/, "");
        if (u === t) break;
        t = u;
    }
    if (t.startsWith("(")) {
        const k = closeParen(t, 1);
        return d22Root(t.slice(1, k - 1));
    }
    const m = /^[A-Za-z_]\w*(?:\s*(?:\[[^\]]*\]|\((?:[^()]|\([^()]*\))*\)|(?:->|\.)\s*[A-Za-z_]\w*))*/.exec(t);
    if (!m) return null;
    const ids = [...m[0].replace(/\[[^\]]*\]|\((?:[^()]|\([^()]*\))*\)/g, "").matchAll(/[A-Za-z_]\w*/g)];
    return ids.length ? ids[ids.length - 1][0] : null;
}

const D22_HDR = new Map();
function d22Headers(path, text, depth, seen) {
    const dir = path.replace(/[^/]*$/, "");
    for (const m of text.matchAll(/^\s*#\s*include\s+"([^"]+)"/gm)) {
        for (const c of [dir + m[1], "src/" + m[1], "src/core/" + m[1]]) {
            const p = c.replace(/\/\.\//g, "/");
            if (seen.has(p)) break;
            if (!isfile(p)) continue;
            seen.add(p);
            if (!D22_HDR.has(p)) D22_HDR.set(p, readText(p));
            if (depth < 3) d22Headers(p, D22_HDR.get(p), depth + 1, seen);
            break;
        }
    }
    return seen;
}
function d22Scope(path, raw) {
    const seen = d22Headers(path, raw, 0, new Set());
    if (/\.inc\.c$/.test(path)) for (const h of ["src/engine-internal.h", "src/engine-shared.h", "src/quickjs.h"]) if (isfile(h)) d22Headers(h, "#include \"" + h.slice(4) + "\"", 0, seen) && seen.add(h);
    return [blankC(raw), ...[...seen].map((p) => blankC(D22_HDR.get(p) || (D22_HDR.set(p, readText(p)), D22_HDR.get(p))))].join("\n");
}

check("D22-sizeof-mismatch-copy", "warn", "memcpy/memmove size uses sizeof(X) where X is not either operand", (path, raw, blank, fns, out) => {
    let scope = null;
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\b(memcpy|memmove|strcpy)\s*\(/g)) {
            const mEnd = m.index + m[0].length;
            const j = closeParen(body, mEnd);
            const args = splitTopArgs(body.slice(mEnd, j - 1));
            if (args.length < 3) continue;
            const roots = args.slice(0, 2).map(d22Root).filter((r) => r);
            for (const sz of args[2].matchAll(/sizeof\s*\(\s*(\w+)\s*\)/g)) {
                if (!roots.includes(sz[1]) && sz[1] !== "struct") {
                    emit(out, new Defect("D22", "warn", path, lineOf(blank, off + m.index),
                        bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 70)),
                        "size arg is sizeof(" + sz[1] + ") but neither operand is " + sz[1] + " -- copying the wrong object's size"),
                        { T: sz[1], roots, head: body.slice(0, m.index), bases: args.slice(0, 2).map((x) => (/^&\s*(\w+)\s*->\s*\w+\s*\+\s*1$/.exec(x.replace(/\s+/g, " ")) || [])[1]).filter((x) => x), scope: () => scope || (scope = d22Scope(path, raw)) });
                }
            }
        }
    }
});
const D22_DECL = new Map();
function d22Declared(T, r, text) {
    const key = T + " " + r;
    let memo = D22_DECL.get(text);
    if (!memo) D22_DECL.set(text, memo = new Map());
    if (memo.has(key)) return memo.get(key);
    const tre = R("\\b" + T + "(?=[\\s*])", "");
    let hit = false;
    for (const m of text.matchAll(R("\\b" + r + "\\b(?=\\s*[\\[;,=)(])", "g"))) {
        let k = m.index - 1;
        while (k >= 0 && !";{}(".includes(text[k]) && m.index - k < 240) k--;
        const seg = text.slice(k + 1, m.index);
        const piece = k >= 0 && text[k] === "(" ? seg.slice(seg.lastIndexOf(",") + 1) : seg;
        if (!piece.includes("=") && tre.test(piece)) { hit = true; break; }
    }
    memo.set(key, hit);
    return hit;
}
stage("D22", "declared-element-type", (c) => c.roots.some((r) => d22Declared(c.T, r, c.scope())));
stage("D22", "allocated-as-type", (c) => c.roots.some((r) => R("\\b" + r + "\\s*=\\s*(?:\\([^()]*\\)\\s*)?\\w*(?:alloc|ALLOC)\\w*\\s*\\([^;]*\\bsizeof\\s*\\(\\s*" + c.T + "\\s*\\)", "").test(c.head)));
stage("D22", "past-embedded-header", (c) => c.bases.some((x) => d22Declared(c.T, x, c.scope())));

check("D23-use-after-realloc", "warn", "old pointer used after q = realloc(p, ...)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\b(\w+)\s*=\s*realloc\s*\(\s*(\w+)\b/g)) {
            const newv = m[1], oldv = m[2];
            if (newv === oldv) continue;
            const mEnd = m.index + m[0].length;
            const tail = body.slice(mEnd);
            const r = R("\\b" + oldv + "\\b");
            let u;
            while ((u = r.exec(tail))) {
                const ctx = body.slice(mEnd + u.index, mEnd + u.index + 3);
                if (ctx.startsWith(")") || ctx.startsWith(";")) continue;
                const lp = /((?:[A-Za-z_]\w*(?:\s*\[[^\]]*\])*\s*(?:->|\.)\s*)+)$/.exec(body.slice(Math.max(0, m.index - 80), m.index));
                const call = body.indexOf("(", m.index + m[0].length - m[2].length - 1);
                const op = /^\s*([A-Za-z_]\w*(?:\s*(?:->|\.)\s*\w+|\s*\[[^\]]*\])*)/.exec(body.slice(call + 1));
                if (!op) break;
                emit(out, new Defect("D23", "warn", path, lineOf(blank, off + mEnd + u.index),
                    oldv + " used after realloc moved it to " + newv, "the old block may be freed; use the new pointer"),
                { body, newv, newPath: ((lp ? lp[1] : "") + newv).replace(/\s+/g, ""), oldPath: op[1].replace(/\s+/g, ""), callEnd: closeParen(body, call + 1) });
                break;
            }
        }
    }
});

function pathRe(p) {
    return "(?<![\\w>.])" + p.split(/(->|\.|\[|\])/).filter(Boolean).map((t) => (/^\w+$/.test(t) ? t : reEsc(t))).join("\\s*") + "(?![\\w])";
}
function failRanges(body, v) {
    const out = [];
    for (const m of body.matchAll(/\bif\s*\(/g)) {
        const ce = closeParen(body, m.index + m[0].length);
        const cond = body.slice(m.index + m[0].length, ce - 1);
        if (!R("!\\s*\\(?\\s*" + v + "\\b|\\b" + v + "\\s*==\\s*(?:NULL|0)\\b|\\bNULL\\s*==\\s*" + v + "\\b", "").test(cond)) continue;
        let k = ce;
        while (k < body.length && " \t\r\n".includes(body[k])) k++;
        let e;
        if (body[k] === "{") {
            let d = 0;
            for (e = k; e < body.length; e++) {
                if (body[e] === "{") d++;
                else if (body[e] === "}" && --d === 0) break;
            }
        } else e = body.indexOf(";", k);
        out.push([m.index, e < 0 ? body.length : e + 1]);
    }
    return out;
}
function d23Use(c, level) {
    const from = c.callEnd;
    const tail = c.body.slice(from);
    const fails = level >= 3 ? failRanges(c.body, c.newv) : [];
    const inFail = (i) => fails.some(([a, b]) => from + i >= a && from + i < b);
    const re = R(level >= 1 ? pathRe(c.oldPath) : "\\b" + c.oldPath.split(/->|\.|\[/)[0] + "\\b");
    let u;
    while ((u = re.exec(tail))) {
        if (inFail(u.index)) continue;
        const after = tail.slice(u.index + u[0].length);
        if (level >= 2 && /^\s*=(?!=)/.test(after)) return -1;
        return u.index;
    }
    return -1;
}
stage("D23", "same-lvalue", (c) => c.newPath === c.oldPath);
stage("D23", "member-path", (c) => d23Use(c, 1) < 0);
stage("D23", "reassigned-before-use", (c) => d23Use(c, 2) < 0);
stage("D23", "failure-branch", (c) => d23Use(c, 3) < 0);

check("D24-double-dispose", "warn", "free/close/fclose of the same resource twice with no reassignment", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        for (const fn of ["free", "close", "fclose"]) {
            const seen = new Map();
            for (const m of body.matchAll(R("\\b" + fn + "\\s*\\(\\s*(\\w+)\\s*\\)"))) {
                const v = m[1];
                if (seen.has(v)) {
                    const p = seen.get(v);
                    const between = body.slice(p.index + p[0].length, m.index);
                    if (!R("[}]|\\belse\\b|\\bcase\\b|\\bgoto\\b|\\breturn\\b|(?<![^\\n])\\s*\\w+:\\s*(?![^\\n])|\\b" + v + "\\s*=", "").test(between)) {
                        emit(out, new Defect("D24", "warn", path, lineOf(blank, off + m.index),
                            fn + "(" + v + ") again; no reassignment between", "double " + fn), { v, between, sig });
                    }
                }
                seen.set(v, m);
            }
        }
    }
});

stage("D24", "reassigned-via-addr", (c) => R("&\\s*(?:\\(\\s*)?" + c.v + "\\b", "").test(c.between));
stage("D24", "fd-reopened-by-dup2", (c) => R("\\bdup[23]?\\s*\\([^;]*,\\s*" + c.v + "\\s*[,)]", "").test(c.between));

check("D25-free-of-stack", "error", "free()/close() of a local array or &local (stack memory)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        const locs = locals(body);
        for (const m of body.matchAll(/\bfree\s*\(\s*(?:\(\s*\w[\w\s]*\*\s*\)\s*)?(\w+)\s*\)/g)) {
            const v = m[1];
            const info = locs.get(v);
            if (info && "array" in info) {
                if (R("\\b" + v + "\\s*=[^=]", "").test(body)) continue;
                out.push(new Defect("D25", "error", path, lineOf(blank, off + m.index), "free(" + v + ")",
                    v + " is a local array -- freeing stack memory"));
            }
        }
        for (const m of body.matchAll(/\b(?:free|close)\s*\(\s*&\s*(\w+)/g)) {
            out.push(new Defect("D25", "error", path, lineOf(blank, off + m.index), "free/&" + m[1],
                "free() of the ADDRESS of a local -- stack memory"));
        }
    }
});

check("D26-stack-addr-returned", "error", "return &local (dangling pointer)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        const locs = locals(body);
        for (const m of body.matchAll(/\breturn\s*&\s*(\w+)\s*;/g)) {
            if (locs.has(m[1])) {
                out.push(new Defect("D26", "error", path, lineOf(blank, off + m.index), "return &" + m[1],
                    "address of a local outlives the frame"));
            }
        }
    }
});

check("D27-stack-addr-stored", "warn", "address of a local stored into a pointer/global/struct field", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        const locs = locals(body);
        for (const m of body.matchAll(/(\w+(?:\s*->\s*\w+)*|\*\w+)\s*=\s*&\s*(\w+)\s*;/g)) {
            const info = locs.get(m[2]);
            if (info && !locs.has(m[1]) && ("array" in info ? info.array : 1)) {
                emit(out, new Defect("D27", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                    "&" + m[2] + " (a local) escapes into " + m[1]), { body, sig, at: m.index, end: m.index + m[0].length, dst: m[1], v: m[2] });
            }
        }
    }
});

stage("D27", "static-storage", (c) => R("\\bstatic\\b[^;{}()]*\\b" + c.v + "\\s*(?:=|;|\\[|,)", "").test(c.body));
stage("D27", "param-rebind", (c) => /^\w+$/.test(c.dst) && fnParams(c.sig).has(c.dst));
stage("D27", "declarator-init", (c) => /^\*?\w+$/.test(c.dst) && /(?:,|[A-Za-z_]\w*)\s*$/.test(c.body.slice(Math.max(0, c.at - 80), c.at)) &&
    !/\b(?:return|else|do)\s*$/.test(c.body.slice(Math.max(0, c.at - 80), c.at)));
stage("D27", "member-of-local", (c) => {
    const m = /([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*\.\s*$/.exec(c.body.slice(Math.max(0, c.at - 80), c.at));
    return !!m && !fnParams(c.sig).has(m[1]) && R("\\b[A-Za-z_]\\w*\\s+\\**\\s*" + m[1] + "\\s*(?:\\[[^\\]]*\\]\\s*)*(?:;|=|,)", "").test(c.body.slice(0, c.at));
});
stage("D27", "restored-before-exit", (c) => {
    const tail = c.body.slice(c.end);
    const ret = tail.search(/\breturn\b/);
    const scope = ret < 0 ? tail : tail.slice(0, ret);
    const dst = c.dst.replace(/\s+/g, "").split("->").map(reEsc).join("\\s*->\\s*");
    return R("(?<![\\w>.])" + dst + "\\s*=(?!=)\\s*(?!&\\s*" + c.v + "\\b)", "").test(scope);
});

check("D28-deref-before-nullcheck", "warn", "allocation result dereferenced before any NULL test", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(R("\\b(\\w+)\\s*=\\s*(?:" + ALLOC_FNS + ")\\s*\\("))) {
            const v = m[1];
            const mEnd = m.index + m[0].length;
            const rest = body.slice(mEnd, mEnd + 300);
            const deref = R("\\b" + v + "\\s*(?:\\[|->|\\.)", "").exec(rest);
            if (!deref) continue;
            const pre = rest.slice(0, deref.index);
            if (R("\\bif\\s*\\([^)]*\\b" + v + "\\b", "").test(pre) || R("!\\s*" + v + "\\b", "").test(pre) ||
                R("\\b" + v + "\\s*==", "").test(pre)) continue;
            const before = body.slice(Math.max(0, m.index - 80), m.index);
            const lp = /((?:[A-Za-z_]\w*(?:\s*\[[^\]]*\])*\s*(?:->|\.)\s*)+)$/.exec(before);
            emit(out, new Defect("D28", "warn", path, lineOf(blank, off + mEnd + deref.index), v + deref[0],
                v + " is dereferenced before any NULL check"), { v, rest, head: lp ? before.slice(0, lp.index) : before, lval: lp ? lp[1].replace(/\s+/g, "") + v : v });
        }
    }
});

const RESOURCE_LEAK = /\b(\w+)\s*=\s*(malloc|calloc|realloc|js_malloc|js_mallocz|socket|accept4?|fopen|opendir|open)\s*\(/g;
const RELEASE = { malloc: "free", calloc: "free", realloc: "free", js_malloc: "js_free", js_mallocz: "js_free",
    socket: "close", accept: "close", accept4: "close", fopen: "fclose", opendir: "closedir", open: "close" };
export function blankSizeof(s) {
    let o = s;
    for (const m of s.matchAll(/\b(?:sizeof|_Alignof|alignof|__alignof__|offsetof)\b\s*(\()?/g)) {
        let e;
        if (m[1]) e = closeParen(s, m.index + m[0].length);
        else {
            const t = /^\**\s*\w+(?:\s*(?:\[[^\]]*\]|->\s*\w+|\.\s*\w+))*/.exec(s.slice(m.index + m[0].length));
            e = m.index + m[0].length + (t ? t[0].length : 0);
        }
        o = o.slice(0, m.index) + " ".repeat(e - m.index) + o.slice(e);
    }
    return o;
}
function firstDeref(rest, v) {
    return R("\\b" + v + "\\s*(?:\\[|->|\\.)", "").exec(blankSizeof(rest));
}
function d28OldGuard(pre, v) {
    return R("\\bif\\s*\\([^)]*\\b" + v + "\\b", "").test(pre) || R("!\\s*" + v + "\\b", "").test(pre) || R("\\b" + v + "\\s*==", "").test(pre);
}
stage("D28", "sizeof-operand", (c) => {
    const d = firstDeref(c.rest, c.v);
    return !d || d28OldGuard(c.rest.slice(0, d.index), c.v);
});
stage("D28", "assign-in-condition", (c) => /\(\s*$/.test(c.head));
stage("D28", "member-lvalue", (c) => {
    if (c.lval === c.v) return false;
    const p = c.lval.split(/(->|\.|\[|\])/).filter(Boolean).map((t) => (/^\w+$/.test(t) ? t : reEsc(t))).join("\\s*");
    const d = R("(?<![\\w>.])" + p + "\\s*(?:\\[|->|\\.)", "").exec(blankSizeof(c.rest));
    if (!d) return true;
    const pre = c.rest.slice(0, d.index);
    return R("!\\s*\\(?\\s*" + p + "\\b", "").test(pre) || R("\\b" + p + "\\s*[!=]=", "").test(pre) || R("\\bif\\s*\\([^;{]*\\b" + p + "\\b", "").test(pre);
});
stage("D28", "balanced-nullcheck", (c) => {
    const d = firstDeref(c.rest, c.v);
    if (!d) return true;
    const pre = blankSizeof(c.rest.slice(0, d.index));
    if (R("\\bNULL\\s*[!=]=\\s*" + c.v + "\\b|\\b" + c.v + "\\s*!=", "").test(pre)) return true;
    for (const m of pre.matchAll(/\b(?:if|while)\s*\(/g)) {
        const cond = pre.slice(m.index + m[0].length, closeParen(pre, m.index + m[0].length));
        if (R("\\b" + c.v + "\\b(?!\\s*(?:\\[|->|\\.))", "").test(cond)) return true;
    }
    return false;
});

check("D29-resource-no-dispose", "info", "acquired resource never released/returned in its function (leak smell; cleanup labels make this noisy -- review only)", (path, raw, blank, fns, out) => {
    for (const [name, , , , , , body, off] of fns) {
        for (const m of body.matchAll(RESOURCE_LEAK)) {
            const v = m[1], fn = m[2];
            const rel = RELEASE[fn];
            if (R("\\b" + rel + "\\s*\\(\\s*" + v + "\\b", "").test(body)) continue;
            if (R("\\breturn\\s[^;]*\\b" + v + "\\b", "").test(body)) continue;
            if (R("->\\s*\\w+\\s*=\\s*" + v + "\\b", "").test(body)) continue;
            out.push(new Defect("D29", "info", path, lineOf(blank, off + m.index), v + " = " + fn + "(...)",
                "no " + rel + "(" + v + ") and no ownership transfer visible in " + name));
        }
    }
});

function d30Loop(body, j) {
    let i = j;
    while (i < body.length && /\s/.test(body[i])) i++;
    if (body[i] !== "{") {
        const e = body.indexOf(";", i);
        return body.slice(i, e < 0 ? body.length : e + 1);
    }
    let d = 0;
    for (let k = i; k < body.length; k++) {
        if (body[k] === "{") d++;
        else if (body[k] === "}" && --d === 0) return body.slice(i, k + 1);
    }
    return body.slice(i);
}
function d30Indexed(c) {
    return [...c.loop.matchAll(R("(\\w+)\\s*(?:\\([^()]*\\)\\s*)?\\[[^\\]]*\\b" + c.v + "\\b[^\\]]*\\]", "g"))].map((m) => m[1]);
}
const D30_INCL = /(?:mask|max|last|^hi|^top|^om)$|^(?:DYN_)?MAX|_FLOAT64$/i;
check("D30-loop-le-index", "warn", "for (i = 0; i <= bound; ...) with i used as an index (off-by-one; the '<' form is the norm)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\bfor\s*\(\s*\w+\s*=\s*0\s*;\s*(\w+)\s*<=\s*([\w.\->]+)/g)) {
            const v = m[1], bound = m[2];
            const mEnd = m.index + m[0].length;
            const loop = d30Loop(body, closeParen(body, m.index + m[0].indexOf("(") + 1));
            const inner = loop;
            if (R("\\[\\s*" + v + "\\s*\\]", "").test(inner) || R("\\[\\s*" + bound.replaceAll(".", "\\.") + "\\s*\\]", "").test(inner)) {
                emit(out, new Defect("D30", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                    "'<=' over a length-like bound with " + v + " as index -- one past the end"),
                    { v, bound, loop, head: body.slice(0, m.index), tail: body.slice(mEnd, mEnd + 40) });
            }
        }
    }
});

stage("D30", "inclusive-bound", (c) => D30_INCL.test(c.bound.replace(/^.*(?:->|\.)/, "")));
stage("D30", "sentinel-test", (c) => {
    const v = c.v, b = reEscape(c.bound);
    return R("\\b" + v + "\\s*(?:==|>=|<|!=)\\s*" + b + "(?![\\w.>-])|(?<![\\w.>-])" + b + "\\s*(?:==|<=|>|!=)\\s*" + v + "\\b", "").test(c.loop);
});
stage("D30", "window-difference", (c) => /^\s*-\s*[\w(]/.test(c.tail));
stage("D30", "child-array", (c) => {
    const ix = d30Indexed(c);
    return ix.length > 0 && ix.every((a) => /^(?:\w*_)?(?:kid|kids|child|children|cnt)$/.test(a)) && /->n$/.test(c.bound);
});
stage("D30", "literal-in-decl", (c) => {
    if (!/^\d+$/.test(c.bound)) return false;
    const ix = d30Indexed(c);
    return ix.length > 0 && ix.every((a) => {
        const d = R("\\b" + a + "\\s*\\[\\s*(\\d+)\\s*\\]", "").exec(c.head);
        return !!d && +d[1] > +c.bound;
    });
});
stage("D30", "sized-plus-one", (c) => R("(?<![\\w.>-])" + reEscape(c.bound) + "\\s*\\+\\s*1\\b", "").test(c.head));

const D31_CAST = /\((?:const)?(?:unsigned|signed)?(?:size_t|ptrdiff_t|u?int(?:8|16|32|64)?_t|char|int|long|void)\**\)/g;
function d31Norm(a) {
    return a.replace(/\s+/g, "").replace(D31_CAST, "");
}
function d31Split(a) {
    let d = 0;
    for (let i = 0; i < a.length; i++) {
        const ch = a[i];
        if (ch === "(" || ch === "[") d++;
        else if (ch === ")" || ch === "]") d--;
        else if (d === 0 && i > 0 && (ch === "+" || (ch === "-" && a[i + 1] !== ">"))) return [a.slice(0, i), a.slice(i)];
    }
    return [a, ""];
}
function d31Chain(base) {
    const m = /^&?(\w+)/.exec(base);
    if (!m) return [];
    const t = [m[1]];
    let i = m[0].length;
    while (i < base.length) {
        const r = base.slice(i);
        const mm = /^(?:->|\.)(\w+)/.exec(r);
        if (mm) {
            t.push("." + mm[1]);
            i += mm[0].length;
            continue;
        }
        if (r[0] !== "[" && r[0] !== "(") break;
        const close = r[0] === "[" ? "]" : ")";
        let d = 0, j = 0;
        for (; j < r.length; j++) {
            if (r[j] === r[0]) d++;
            else if (r[j] === close && --d === 0) break;
        }
        t.push(r.slice(0, j + 1));
        i += j + 1;
    }
    return t;
}
function d31Diverge(c) {
    const x = d31Chain(c.dstBase), y = d31Chain(c.srcBase);
    for (let i = 0; i < Math.min(x.length, y.length); i++) if (x[i] !== y[i]) return [x[i], y[i], i];
    return null;
}
function d31ConstStep(p, q) {
    const ip = /^\[(.+)\]$/.exec(p), iq = /^\[(.+)\]$/.exec(q);
    if (!ip || !iq) return false;
    const re = (u, v) => R("^" + reEscape(u) + "[-+]\\d+$", "").test(v);
    return re(ip[1], iq[1]) || re(iq[1], ip[1]);
}
function reEscape(t) {
    return t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
function d31Span(off, n, other) {
    if (other !== "") return false;
    if (off === "+" + n) return true;
    const cl = /^\((\w+)<[^?]*\?\1:[^)]*\)\*(\w+)$/.exec(n);
    return !!cl && off === "+" + cl[1] + "*" + cl[2];
}

check("D31-memcpy-self-overlap", "warn", "memcpy between two expressions of the same buffer (needs memmove)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\bmemcpy\s*\(/g)) {
            const mEnd = m.index + m[0].length;
            const j = closeParen(body, mEnd);
            const args = splitTopArgs(body.slice(mEnd, j - 1));
            if (args.length < 3) continue;
            const roots = [];
            for (const a of args.slice(0, 2)) {
                const mm = /\b(\w+)\b/.exec(pyStrip(a));
                if (mm) roots.push(mm[1]);
            }
            if (roots.length === 2 && roots[0] === roots[1]) {
                const [dstBase, dstOff] = d31Split(d31Norm(args[0]));
                const [srcBase, srcOff] = d31Split(d31Norm(args[1]));
                emit(out, new Defect("D31", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 60)),
                    "memcpy with both operands rooted at '" + roots[0] + "': overlapping copy must be memmove"),
                    { dstBase, dstOff, srcBase, srcOff, n: d31Norm(args[2]), head: body.slice(Math.max(0, m.index - 400), m.index) });
            }
        }
    }
});
stage("D31", "distinct-field", (c) => {
    const d = d31Diverge(c);
    return !!d && ((d[0][0] === "." && d[1][0] === ".") || d31ConstStep(d[0], d[1]));
});
stage("D31", "distinct-call", (c) => {
    const d = d31Diverge(c);
    return !!d && d[2] > 0 && d[0][0] === "(" && d[1][0] === "(";
});
stage("D31", "disjoint-span", (c) => {
    if (c.dstBase !== c.srcBase) return false;
    if (d31Span(c.dstOff, c.n, c.srcOff) || d31Span(c.srcOff, c.n, c.dstOff)) return true;
    if (c.dstOff && c.srcOff && c.dstOff.slice(1) + c.srcOff.replace(/^\+/, "-") === c.n) return true;
    const st = /^\+(\w+)\*(.+)$/.exec(c.dstOff);
    return !!st && st[2] === c.n && (c.srcOff === "+(" + st[1] + "-1)*" + c.n || c.srcOff === "+(" + st[1] + "+1)*" + c.n);
});
stage("D31", "overlap-guard", (c) => {
    if (c.dstBase !== c.srcBase) return false;
    const h = c.head.replace(/\s+/g, "");
    const ids = (t) => new Set(t.match(/\b[A-Za-z_]\w*\b/g) || []);
    const a = ids(c.dstOff), b = ids(c.srcOff);
    const da = [...a].filter((v) => !b.has(v)), db = [...b].filter((v) => !a.has(v));
    if (da.length === 1 && db.length === 1 && R("if\\((?:" + da[0] + "==" + db[0] + "|" + db[0] + "==" + da[0] + ")\\)(?:return|continue|break|goto)\\b", "").test(h)) return true;
    const [lo, hi] = c.srcOff.startsWith(c.dstOff) ? [c.dstOff, c.srcOff] : [c.srcOff, c.dstOff];
    if (!hi.startsWith(lo) || !/^[-+]\w+$/.test(hi.slice(lo.length))) return false;
    const dist = hi.slice(lo.length + 1);
    return R("if\\((?:" + dist + ">=" + reEscape(c.n) + "|" + reEscape(c.n) + "<=" + dist + ")\\)\\{?$", "").test(h);
});

check("D32-strlen-in-loop-cond", "warn", "strlen() in a loop condition: O(n^2) rescans", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\bfor\s*\(/g)) {
            const j = closeParen(body, m.index + m[0].length);
            const seg = body.slice(m.index, j);
            if (/strlen\s*\(/.test(seg)) {
                out.push(new Defect("D32", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, seg.slice(0, 70)),
                    "strlen re-evaluated every iteration -- hoist it"));
            }
        }
    }
});

const IO_WARN = /\b(recv|recvfrom|send|sendto|read|write|fread|fwrite|fgets)\s*\(/g;
const IO_INFO = /\b(connect|bind|listen|setsockopt|pthread_create)\s*\(/g;
check("D33-partial-io-ignored", "warn", "IO call as a bare statement: partial read/write/recv goes undetected", (path, raw, blank, fns, out) => {
    const sev = TESTS_DIR.test(relp(path)) ? "info" : "warn";
    for (const [, , , , , , body, off] of fns) {
        for (const m of bareStatement(body, IO_WARN)) {
            out.push(new Defect("D33", sev, path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                "return value dropped: short reads/writes and EOF are silent"));
        }
        for (const m of bareStatement(body, IO_INFO)) {
            out.push(new Defect("D33", "info", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                "failure of this call is ignored (bare statement)"));
        }
    }
});

check("D35-port-no-htons", "warn", "sin_port assigned without htons/htobe16 (endianness trap)", (path, raw, blank, fns, out) => {
    if (TESTS_DIR.test(relp(path))) return;
    for (const m of blank.matchAll(/\bsin_port\s*=\s*([^;]{1,60});/g)) {
        const rhs = m[1];
        if (rhs.includes("htons") || rhs.includes("htobe") || rhs.includes("constant")) continue;
        if (/^(?:0|0x[0-9a-fA-F]+|\d+)$/.test(pyStrip(rhs))) {
            out.push(new Defect("D35", "warn", path, lineOf(blank, m.index), "sin_port = " + pyStrip(rhs),
                "no htons(): network byte order required (a literal 0 is the only safe unconverted value)"));
        }
    }
});

check("D37-signal-handler-unsafe", "warn", "signal handler calling non-async-signal-safe functions", (path, raw, blank, fns, out) => {
    const unsafe = /\b(malloc|calloc|free|printf|fprintf|fopen|pthread_mutex_\w+)\s*\(/g;
    for (const [name, , , , , , body, off] of fns) {
        if (!(name.includes("sig") && name.includes("handler"))) continue;
        for (const m of body.matchAll(unsafe)) {
            out.push(new Defect("D37", "warn", path, lineOf(blank, off + m.index), m[1] + "() inside " + name,
                "not async-signal-safe (man 7 signal-safety)"));
        }
    }
});

const SIMD_CAST = /\(\s*(__m128[a-z]*|__m256[a-z]*|__m512[a-z]*|int8x16_t|int16x8_t|int32x4_t|int64x2_t|float32x4_t)\s*\*\s*\)/g;
check("D38-simd-cast-align", "warn", "SIMD pointer cast + deref: alignment UB unless loadu/storeu/memcpy", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(SIMD_CAST)) {
            const mEnd = m.index + m[0].length;
            const tail = body.slice(mEnd, mEnd + 60);
            if (/^\s*\w+\s*=/.test(tail)) continue;
            const window = body.slice(Math.max(0, m.index - 48), mEnd + 48);
            if (/_mm\d*_\w*(loadu|storeu)\w*|vld1q|memcpy|\bloadu\b|\bstoreu\b/.test(window)) continue;
            if (/^\s*\*\s*\w+/.test(tail) || /^\s*\w+\s*(?:\[|\)|,|;)/.test(tail)) {
                out.push(new Defect("D38", "warn", path, lineOf(blank, off + m.index),
                    bodyRaw(raw, off + m.index, body.slice(m.index, mEnd + 40)),
                    "deref through a SIMD pointer cast: unless the base is declared aligned this is UB -- use the unaligned load/store intrinsics"));
            }
        }
    }
});

const LANE = { 128: 16, 256: 32, 512: 64 };
check("D39-simd-store-oob", "error", "SIMD store into a local array smaller than the vector", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        const locs = locals(body);
        for (const m of body.matchAll(/_mm(\d*)_store[u]?\w*\s*\(\s*&?\s*(\w+)/g)) {
            const key = m[1] || "128";
            const w = key in LANE ? LANE[key] : 16;
            const v = m[2];
            const info = locs.get(v);
            let nb = 0;
            if (info) {
                if (!("array" in info)) throw new PyKeyError("array");
                nb = info.array * (info.elem || 0);
            }
            if (info && info.elem && nb < w) {
                out.push(new Defect("D39", "error", path, lineOf(blank, off + m.index),
                    "_mm" + key + "_store...(" + v + "[" + info.array + "])",
                    w + "-byte vector stored into " + v + "[" + info.array + "] (" + nb + " bytes)"));
            }
        }
    }
});

check("D40-strncpy-unterminated", "warn", "strncpy with no visible NUL termination afterwards", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\bstrncpy\s*\(\s*(\w+)/g)) {
            const v = m[1];
            const after = body.slice(m.index + m[0].length);
            if (R("\\b" + v + "\\s*\\[[^\\]]*\\]\\s*=\\s*0", "").test(after) ||
                R("memset\\s*\\(\\s*" + v + "\\b[^)]*,\\s*0", "").test(body) ||
                R("sizeof\\s*\\(\\s*" + v + "\\s*\\)\\s*-\\s*1", "").test(after)) continue;
            out.push(new Defect("D40", "warn", path, lineOf(blank, off + m.index), "strncpy(" + v + ", ...)",
                "no visible termination of " + v + " after strncpy -- a source longer than the buffer leaves it unterminated"));
        }
    }
});

check("D41-sscanf-unbounded-s", "warn", "sscanf %s without a field width (unbounded write)", (path, raw, blank, fns, out) => {
    for (const m of raw.matchAll(/\bsscanf\s*\([^;]*?"([^"]*)"/g)) {
        const fmt = m[1];
        if (/%\d+s/.test(fmt)) continue;
        if (fmt.includes("%s")) {
            out.push(new Defect("D41", "warn", path, lineOf(blank, m.index), fmt.slice(0, 60),
                "sscanf %%s without a width: unbounded copy into the target buffer (use %63s-style bounds)"));
        }
    }
});

check("D42-atoi-untrusted", "info", "atoi/atol family: no error detection (use strtol)", (path, raw, blank, fns, out) => {
    if (!basename(path).includes("dyna-")) return;
    for (const m of blank.matchAll(/\b(atoi|atol|atoll|atof)\s*\(/g)) {
        out.push(new Defect("D42", "info", path, lineOf(blank, m.index),
            bodyRaw(raw, m.index, blank.slice(m.index, m.index + 30)),
            m[1] + "(): silent garbage on overflow/non-numeric; strtol reports both"));
    }
});

check("D43-strtok-not-r", "warn", "strtok() is not reentrant (use strtok_r)", (path, raw, blank, fns, out) => {
    for (const m of blank.matchAll(/\bstrtok\s*\(/g)) {
        out.push(new Defect("D43", "warn", path, lineOf(blank, m.index), "strtok(...)",
            "hidden global state; strtok_r is the reentrant form"));
    }
});

check("D45-int-cast-of-double", "warn", "(int) cast of a raw floating computation (UB past INT_MAX)", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(/\(\s*(?:int|int32_t|uint32_t)\s*\)\s*\(?\s*(?:sqrt|pow|log|log2|log10|exp|cbrt|hypot|atof)\s*\(/g)) {
            out.push(new Defect("D45", "warn", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 50)),
                "(int) of an unclamped double: out-of-range conversion is UB -- wrap in floor()/lround() with a range test"));
        }
    }
});

const OUT_ALLOC = "\\*\\s*%s\\s*=\\s*(?:\\([^()]*\\)\\s*)?(?:malloc|calloc|realloc|js_malloc|js_mallocz)\\s*\\(";
const RET_TOK = /[{}]|\breturn\b[^;]*;/g;
const ERR_RET = /^return\s*\(?\s*(?:-\s*\d+|[A-Z][A-Z0-9_]*_E_[A-Z0-9_]+|false|NULL|JS_EXCEPTION)\s*\)?\s*;/;

const rfind = (s, ch, end) => (end <= 0 ? -1 : s.lastIndexOf(ch, end - 1));

function outparams(sig) {
    const i = sig.indexOf("(");
    const names = [];
    if (i < 0) return names;
    for (const tok of sig.slice(i + 1, sig.lastIndexOf(")")).split(",")) {
        if ((tok.match(/\*/g) || []).length >= 2) {
            const parts = tok.replaceAll("*", " ").split(/\s+/).filter(Boolean);
            if (parts.length) names.push(pyStrip(parts[parts.length - 1], "[]0123456789 "));
        }
    }
    return names;
}

const TOCSTRING_CALL = /\bJS_ToCString(?:Len2?)?\s*\(/g;
function d48Lvalue(prefix) {
    const m = /(?:^|[;{}(,?:]|\bchar\s*\*|\bconst\s+char\s*\*)\s*(\**\s*[A-Za-z_][\w.]*(?:\s*(?:->|\.)\s*\w+|\s*\[[^\]]*\])*)\s*=\s*(?:[^=;?]*\?\s*)?(?:\(\s*(?:const\s+)?char\s*\*\s*\)\s*)?$/.exec(prefix);
    return m ? m[1].replace(/\s+/g, "") : null;
}
function d48Exits(stmt) {
    return /^\s*(?:\{[^}]*?)?\b(?:return|goto|JS_Throw\w*|abort|exit|\w+_fail|\w*fatal)\b|\bJS_GetException\s*\(|\b(?:bad|err|error|fail|failed|rc|ret|r|status)\s*=\s*(?:-?1|true|TRUE)\s*;/.test(stmt.slice(0, 400));
}
function d48BlockEnd(body, from, levels) {
    let depth = 0;
    for (let i = from; i < body.length; i++) {
        if (body[i] === "{") depth++;
        else if (body[i] === "}" && --depth < 0) {
            if (--levels === 0) return i;
            depth = 0;
        }
    }
    return body.length;
}
check("D48-tocstring-null-pending", "warn", "JS_ToCString result tolerated as NULL (if (s), s && ..., s ? s : ...) or used unchecked: NULL means a pending exception the code then carries", (path, raw, blank, fns, out) => {
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(TOCSTRING_CALL)) {
            const st = Math.max(body.lastIndexOf(";", m.index), body.lastIndexOf("{", m.index), body.lastIndexOf("}", m.index)) + 1;
            const prefix = body.slice(st, m.index);
            if (/^\s*return\b/.test(prefix)) continue;
            const end = closeParen(body, m.index + m[0].length);
            const args = splitTopArgs(body.slice(m.index + m[0].length, end - 1));
            const lv = d48Lvalue(prefix);
            let kind = null;
            if (!lv) {
                kind = /\(\s*$/.test(prefix) && !/\b(?:if|while)\s*\(\s*!?\s*\(?\s*$/.test(prefix) ? "used unchecked" : null;
            } else {
                const q = reEscape(lv).replace(/->/g, "\\s*->\\s*").replace(/\\\./g, "\\s*\\.\\s*").replace(/^(\\\*)+/, (x) => x.replace(/\\\*/g, "\\*\\s*"));
                const condAssign = R("!\\s*\\(\\s*" + q + "\\s*=\\s*$", "").test(prefix.replace(/\(\s*(?:const\s+)?char\s*\*\s*\)\s*$/, ""));
                const restFrom = condAssign ? end : body.indexOf(";", end) + 1;
                const rest = body.slice(restFrom);
                if (condAssign) {
                    const ifStart = body.lastIndexOf("if", m.index);
                    const ce = closeParen(body, body.indexOf("(", ifStart) + 1);
                    if (!d48Exits(body.slice(ce))) kind = "tolerated as NULL";
                } else {
                    const u = R("(?<![\\w>.])" + q + "(?![\\w])", "").exec(rest);
                    if (!u) continue;
                    const before = rest.slice(Math.max(0, u.index - 40), u.index), after = rest.slice(u.index + u[0].length, u.index + u[0].length + 40);
                    if (/\breturn\s*$/.test(before) && /^\s*(?:;|\?[^:;]*:\s*-\s*\d+\s*;)/.test(after)) continue;
                    const nullTest = /!\s*$/.test(before) || /^\s*==\s*(?:NULL|0)\b/.test(after);
                    if (nullTest && /\bif\s*\([^;{]*$/.test(before + " ")) {
                        const ifAt = restFrom + rest.lastIndexOf("if", u.index);
                        const ce = closeParen(body, body.indexOf("(", ifAt) + 1);
                        if (!d48Exits(body.slice(ce))) kind = "tolerated as NULL";
                    } else if (nullTest) {
                        continue;
                    } else if (/\bif\s*\(\s*$/.test(before) || /^\s*(?:&&|\?|!=\s*NULL)/.test(after) || /\|\|\s*$/.test(before)) {
                        kind = "tolerated as NULL";
                    } else if (/^\s*\)/.test(after) && /\bJS_FreeCString\s*\(\s*\w+\s*,\s*$/.test(before)) {
                        continue;
                    } else {
                        kind = "used unchecked";
                    }
                }
            }
            if (!kind) continue;
            emit(out, new Defect("D48", "warn", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 60)),
                kind === "used unchecked" ? "JS_ToCString result used without a NULL check (NULL = pending exception, then a NULL deref)" : "JS_ToCString NULL is tolerated: the function carries on with the exception pending"),
                { q: lv ? reEscape(lv) : null, arg: (args[args.length - 1] || "").trim(), head: body.slice(0, m.index), tail: body.slice(end, d48BlockEnd(body, end, 2)) });
        }
    }
});
stage("D48", "known-string", (c) => {
    if (!/^[\w.\->\[\]]+$/.test(c.arg)) return false;
    const h = c.head.slice(-600), a = reEscape(c.arg);
    if (R("(?<!!\\s*)JS_IsString\\s*\\(\\s*" + a + "\\s*\\)", "").test(h)) return true;
    for (const m of h.matchAll(R("\\bif\\s*\\(\\s*!\\s*JS_IsString\\s*\\(\\s*" + a + "\\s*\\)\\s*\\)", "g")))
        if (d48Exits(h.slice(m.index + m[0].length))) return true;
    return false;
});
stage("D48", "exception-settled", (c) => /\b(?:JS_GetException|JS_HasException|JS_Throw|js_drop_catchable_exception|\w+_take_exc(?:eption)?)\s*\(/.test(c.tail));
stage("D48", "later-exit-test", (c) => {
    if (!c.q) return false;
    for (const m of c.tail.matchAll(R("\\bif\\s*\\(", "g"))) {
        const ce = closeParen(c.tail, m.index + m[0].length);
        if (R("!\\s*" + c.q + "(?![\\w])", "").test(c.tail.slice(m.index, ce)) && d48Exits(c.tail.slice(ce))) return true;
    }
    return false;
});

const GETPROP_CALL = /\bJS_GetProperty(?:Str|Uint32|Int64|Value|Internal|Internal2)?\s*\(/g;
const D49_PROPAGATES = /^(?:JS_To(?:Int32|Int64|Uint32|Index|Float64|Number|Numeric|BigInt64|BigInt|Int32Clamp|Int64Clamp|Int32Sat|Int64Sat|CStringLen2|CStringLen|CString|String|Object|PropertyKey|ArrayLengthFree|LengthFree)(?:Free)?|JS_GetProperty(?:Str|Uint32|Int64|Value|Internal|Internal2)?|JS_IsException|js_get_length(?:32|64)|JS_Throw|string_buffer_concat_value(?:_free)?|wst_slot)$/;
function d49Lvalue(prefix) {
    const m = /(?:^|[;{}(),]|\belse|\b[A-Za-z_]\w*\s)\s*(\**\s*[A-Za-z_][\w.]*(?:\s*(?:->|\.)\s*\w+|\s*\[[^\]]*\])*)\s*=(?!=)\s*(?:[^;]*?[?:]\s*)?$/.exec(prefix);
    return m ? m[1].replace(/\s+/g, "") : null;
}
function d49Callee(text) {
    let d = 0;
    for (let i = text.length - 1; i >= 0; i--) {
        const c = text[i];
        if (c === ")") d++;
        else if (c === "(") {
            if (d === 0) {
                const m = /([A-Za-z_]\w*)\s*$/.exec(text.slice(0, i));
                return m ? m[1] : "";
            }
            d--;
        }
    }
    return null;
}
function d49Kind(callee) {
    if (callee === null) return "used";
    if (D49_PROPAGATES.test(callee)) return null;
    if (callee === "JS_ToBool" || callee === "JS_ToBoolFree") return "JS_ToBool";
    if (/^JS_Is\w+$/.test(callee) || /^JS_VALUE_GET_TAG$/.test(callee)) return "type-test";
    if (callee === "JS_FreeValue" || callee === "JS_FreeValueRT") return "discarded";
    if (/^(?:if|while|switch|return|sizeof)$/.test(callee)) return "used";
    return "passed:" + callee;
}
const D49_MSG = {
    used: "JS_GetProperty* result used before a JS_IsException check: a throwing getter or Proxy trap is ignored and its error stays pending",
    "JS_ToBool": "JS_GetProperty* result fed to JS_ToBool unchecked: the exception turns into true (-1) and stays pending",
    "type-test": "JS_GetProperty* result type-tested before a JS_IsException check: the getter's error is read as a wrong/absent value (or replaced by a new TypeError)",
    discarded: "JS_GetProperty* result freed without a JS_IsException check: the getter's error stays pending",
    passed: "JS_GetProperty* result passed on without a JS_IsException check: the callee receives JS_EXCEPTION as a value",
};
function d49Internal(recv, sig, head) {
    if (/^(?:func_)?data\s*\[\s*\d+\s*\]$/.test(recv)) return true;
    if (recv === "data" && /\b\w+\s+data\s*,\s*int\s+magic\s*,\s*int\s+is_reject\b/.test(sig)) return true;
    if (!/^[A-Za-z_]\w*$/.test(recv)) return false;
    let last = null;
    for (const a of head.matchAll(R("(?<![\\w>.])" + recv + "\\s*=(?!=)\\s*([^;]*);", "g"))) last = a[1];
    return last !== null && /^(?:\([^()]*\)\s*)?JS_NewObjectProto\w*\s*\(\s*ctx\s*,\s*JS_NULL\b/.test(last.trim());
}
check("D49-getprop-exception-unchecked", "warn", "JS_GetProperty* result used, converted or passed on without a JS_IsException check (a throwing getter/Proxy trap is swallowed or replaced)", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        for (const m of body.matchAll(GETPROP_CALL)) {
            const st = Math.max(body.lastIndexOf(";", m.index), body.lastIndexOf("{", m.index), body.lastIndexOf("}", m.index)) + 1;
            const prefix = body.slice(st, m.index);
            if (/\breturn\s*$/.test(prefix) || /^\s*#/.test(prefix)) continue;
            const eol = raw.indexOf("\n", off + m.index);
            if (/\\\s*$/.test(raw.slice(off + m.index, eol < 0 ? raw.length : eol))) continue;
            const end = closeParen(body, m.index + m[0].length);
            const recv = (splitTopArgs(body.slice(m.index + m[0].length, end - 1))[1] || "").trim();
            const lv = d49Lvalue(prefix);
            let kind, q = null, useAt = m.index, callee = null, ustmt = body.slice(st, body.indexOf(";", end) + 1);
            if (!lv) {
                kind = d49Kind(callee = d49Callee(prefix));
            } else {
                q = reEscape(lv).replace(/->/g, "\\s*->\\s*").replace(/\\\./g, "\\s*\\.\\s*").replace(/^(\\\*)+/, (x) => x.replace(/\\\*/g, "\\*\\s*"));
                const restFrom = body.indexOf(";", end) + 1;
                if (!restFrom) continue;
                const rest = body.slice(restFrom);
                const u = R("(?<![\\w>.])" + q + "(?![\\w])", "").exec(rest);
                if (!u) continue;
                useAt = restFrom + u.index;
                const after = rest.slice(u.index + u[0].length);
                if (/^\s*=(?!=)/.test(after)) continue;
                const ust = Math.max(rest.lastIndexOf(";", u.index), rest.lastIndexOf("{", u.index), rest.lastIndexOf("}", u.index)) + 1;
                const upre = rest.slice(ust, u.index);
                if (/\breturn\s*$/.test(upre)) continue;
                const stmtEnd = rest.indexOf(";", u.index), brace = rest.indexOf("{", u.index);
                const stmt = rest.slice(ust, Math.min(stmtEnd < 0 ? rest.length : stmtEnd, brace < 0 ? rest.length : brace));
                if (R("\\bJS_IsException\\s*\\(\\s*" + q + "\\s*\\)", "").test(stmt)) continue;
                ustmt = stmt;
                const lv2 = d49Lvalue(upre);
                if (lv2 && /^\s*[;,)]/.test(after)) kind = "copied";
                else kind = d49Kind(callee = d49Callee(upre));
                if (kind === "copied") continue;
            }
            if (!kind) continue;
            const k = kind.startsWith("passed:") ? "passed" : kind;
            const internal = d49Internal(recv, sig, body.slice(0, m.index));
            emit(out, new Defect("D49", TESTS_DIR.test(relp(path)) || internal ? "info" : "warn", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, end)),
                D49_MSG[k] + (k === "passed" ? " (" + kind.slice(7) + ")" : "") + (internal ? " [internal receiver: OOM-only]" : "")),
                { q, kind, callee, recv, ustmt, head: body.slice(0, m.index), tail: body.slice(useAt, d48BlockEnd(body, useAt, 2)) });
        }
    }
});
function d49NextUse(c) {
    const re = R("(?<![\\w>.])" + c.q + "(?![\\w])", "g");
    let m, n = 0;
    while ((m = re.exec(c.tail))) {
        if (n++ === 0) continue;
        const pre = c.tail.slice(0, m.index);
        const ust = Math.max(pre.lastIndexOf(";"), pre.lastIndexOf("{"), pre.lastIndexOf("}")) + 1;
        const callee = d49Callee(pre.slice(ust));
        if (callee === "JS_IsUndefined" || callee === "JS_IsNull" || callee === "JS_FreeValue") continue;
        if (/^\s*=(?!=)/.test(c.tail.slice(m.index + m[0].length))) continue;
        return callee;
    }
    return null;
}
stage("D49", "absent-gate-then-convert", (c) => {
    if (!c.q || (c.callee !== "JS_IsUndefined" && c.callee !== "JS_IsNull")) return false;
    const n = d49NextUse(c);
    return n !== null && D49_PROPAGATES.test(n);
});
stage("D49", "tobool-sign-tested", (c) => {
    if (c.kind !== "JS_ToBool") return false;
    const m = /^[^;]*?(?:^|[^\w>.])([A-Za-z_]\w*)\s*=\s*JS_ToBool(?:Free)?\s*\(/.exec(c.ustmt || "");
    return !!m && R("\\b" + m[1] + "\\s*<\\s*0\\b", "").test(c.tail);
});
stage("D49", "helper-then-has-exception", (c) => {
    if (c.kind === "type-test" || c.kind === "used") return false;
    const i = c.tail.search(/\bif\s*\(/);
    if (i < 0 || /[{}]/.test(c.tail.slice(0, i))) return false;
    const ce = closeParen(c.tail, c.tail.indexOf("(", i) + 1);
    return /\bJS_HasException\s*\(/.test(c.tail.slice(i, ce));
});

const GETEXC_CALL = /\bJS_GetException\s*\(/g;
const UNCATCHABLE_TEST = /\b(?:JS_HasUncatchableException|js_exception_uncatchable)\s*\(/;
const D50_NOT_FALLIBLE = new Set(["if", "while", "for", "switch", "return", "sizeof", "unlikely", "likely", "defined", "assert",
    "JS_IsException", "JS_IsUndefined", "JS_IsNull", "JS_IsObject", "JS_IsString", "JS_IsNumber", "JS_IsBool", "JS_IsUninitialized",
    "JS_IsError", "JS_IsFunction", "JS_HasException", "JS_FreeValue", "JS_FreeValueRT", "JS_FreeCString", "JS_FreeAtom", "JS_FreeAtomRT",
    "JS_DupValue", "JS_DupValueRT", "js_free", "js_free_rt", "free", "JS_GetRuntime", "JS_GetContextOpaque", "JS_GetOpaque",
    "JS_VALUE_GET_TAG", "JS_VALUE_GET_PTR", "JS_VALUE_GET_OBJ", "JS_VALUE_GET_INT", "JS_VALUE_GET_BOOL", "JS_MKPTR", "JS_MKVAL",
    "strerror", "fprintf", "printf", "snprintf", "vsnprintf", "fputs", "fputc", "fflush", "memcpy", "memmove", "memset", "memcmp",
    "strlen", "strcmp", "strncmp", "strchr", "abort", "exit"]);
const D50_OWN_THROW = /^(?:JS_Throw\w*|js_throw\w*|JS_GetArrayBuffer|JS_GetArrayBufferView|JS_GetTypedArrayBuffer|JS_GetUint8Array|JS_GetOpaque2|JS_GetTypedArrayType)$/;
const D50_ALLOC_ONLY = /^(?:JS_NewObject\w*|JS_NewArray\w*|JS_NewString\w*|JS_NewAtom\w*|JS_NewArrayBuffer\w*|JS_NewUint8Array\w*|JS_NewTypedArray\w*|JS_NewError|JS_NewCFunction\w*|JS_NewBigInt\w*|JS_NewDate|JS_NewSymbol\w*|JS_NewClassObject|JS_NewPlainObject\w*|JS_ConcatStrings|js_new_string\w*|js_malloc\w*|js_mallocz\w*|js_realloc\w*|js_strdup\w*|js_strndup|js_allocate_fast_array|JS_NewInt64|JS_NewFloat64|JS_NewUint32|JS_AtomToString|JS_AtomToValue|JS_NewAtomString|dyn_nat_u8_copy)$/;
function d50Calls(text) {
    const r = [];
    for (const m of text.matchAll(/\b([A-Za-z_]\w*)\s*\(/g)) if (!D50_NOT_FALLIBLE.has(m[1])) r.push(m[1]);
    return r;
}
function d50Encloses(between) {
    const t = between.replace(/^\s+/, "");
    let depth = 0, par = 0;
    if (t[0] === "{") {
        for (const ch of t) {
            if (ch === "{") depth++;
            else if (ch === "}" && --depth === 0) return false;
        }
        return true;
    }
    for (const ch of t) {
        if (ch === "(") par++;
        else if (ch === ")") par--;
        else if (ch === ";" && par === 0) return false;
        else if (ch === "{" || ch === "}") return false;
    }
    return true;
}
function d50Failing(head) {
    const w = head.slice(-700);
    const ifs = [...w.matchAll(/\bif\s*\(/g)];
    for (let i = ifs.length - 1; i >= 0; i--) {
        const ce = closeParen(w, ifs[i].index + ifs[i][0].length);
        if (ce > w.length) continue;
        const between = w.slice(ce);
        if (!d50Encloses(between)) continue;
        const cond = w.slice(ifs[i].index + ifs[i][0].length, ce - 1);
        const calls = d50Calls(cond);
        if (calls.length) return { callee: calls[0], cond };
        const v = /JS_IsException\s*\(\s*([\w.\->]+)\s*\)|!\s*([\w.\->]+)|([\w.\->]+)\s*(?:<\s*0|==\s*(?:NULL|-1|0)\b|!=\s*0)/.exec(cond);
        const name = v && (v[1] || v[2] || v[3]);
        if (!name) break;
        const pre = w.slice(0, ifs[i].index);
        let callee = null;
        for (const a of pre.matchAll(R("(?<![\\w>.])" + reEscape(name) + "\\s*=(?!=)\\s*(?:\\([^()]*\\)\\s*)?([A-Za-z_]\\w*)\\s*\\(", "g"))) callee = a[1];
        if (callee) return { callee, cond };
        break;
    }
    const all = d50Calls(w);
    return { callee: all.length ? all[all.length - 1] : null, cond: "" };
}
check("D50-uncatchable-swallowed", "error", "native takes or clears the pending exception (JS_GetException) without testing JS_HasUncatchableException / js_exception_uncatchable first: an interrupt becomes catchable or vanishes", (path, raw, blank, fns, out) => {
    const p = path.replaceAll("\\", "/");
    if (!/(?:^|\/)src\//.test(p) || /\/src\/fuzz\//.test(p) || /\/src\/dyna-(?:cli|bench)\.c$/.test(p)) return;
    for (const [, , , , , , body, off] of fns) {
        for (const m of body.matchAll(GETEXC_CALL)) {
            const head = body.slice(0, m.index);
            if (UNCATCHABLE_TEST.test(head.slice(-700))) continue;
            const f = d50Failing(head);
            emit(out, new Defect("D50", "error", path, lineOf(blank, off + m.index),
                bodyRaw(raw, off + m.index, body.slice(m.index, m.index + 60)),
                "JS_GetException without an uncatchable test: an uncatchable interrupt becomes a value or is dropped"),
                { callee: f.callee, head });
        }
    }
});
stage("D50", "own-throw", (c) => !!c.callee && D50_OWN_THROW.test(c.callee));
stage("D50", "allocation-only", (c) => !!c.callee && D50_ALLOC_ONLY.test(c.callee));
stage("D50", "known-string", (c) => {
    if (!c.callee || !/^JS_ToCString(?:Len2?)?$/.test(c.callee)) return false;
    const m = [...c.head.matchAll(/\bJS_ToCString(?:Len2?)?\s*\(([^;]*?)\)\s*;?/g)].pop();
    if (!m) return false;
    const args = splitTopArgs(m[1]);
    const a = (args[args.length - 1] || "").trim();
    if (!/^[\w.\->\[\]]+$/.test(a)) return false;
    return R("(?<!!\\s*)JS_IsString\\s*\\(\\s*" + reEscape(a) + "\\s*\\)", "").test(c.head.slice(-900));
});

check("D51-promise-settle-dropped", "error", "JS_PromiseSettle result discarded: -1 leaves an out-of-memory or uncatchable exception pending (check it, or settle through dyn_settle, which drops only a catchable one)", (path, raw, blank, fns, out) => {
    for (const m of blank.matchAll(/(?:^|[;{}:)]|\belse)[ \t\n]*(?:\(\s*void\s*\)\s*)?\bJS_PromiseSettle\s*\(/g)) {
        const at = m.index + m[0].indexOf("JS_PromiseSettle");
        if (m[0][0] === ")" && !/\b(?:if|while|for)\s*\([^;{}]*\)\s*$/.test(blank.slice(Math.max(0, at - 300), at).replace(/\(\s*void\s*\)\s*$/, ""))) continue;
        out.push(new Defect("D51", "error", path, lineOf(blank, at), bodyRaw(raw, at, blank.slice(at, at + 60)),
            "the -1 result is dropped and its pending exception outlives the call"));
    }
});

check("D47-outparam-leak-on-error", "error", "*out = malloc(...) into an out-parameter, then an error return that does not free(*out): the caller sees the failure and never frees the buffer", (path, raw, blank, fns, out) => {
    for (const [, sig, , , , , body, off] of fns) {
        for (const p of outparams(sig)) {
            const e = reEsc(p);
            const freed = new RegExp("\\b(?:free|js_free)\\s*\\(\\s*(?:\\w+\\s*,\\s*)?\\*\\s*" + e + "\\s*\\)");
            const nullchk = new RegExp("!\\s*\\*\\s*" + e + "\\b|\\*\\s*" + e + "\\s*==\\s*NULL");
            for (const m of body.matchAll(new RegExp(OUT_ALLOC.replace("%s", e), "g"))) {
                const mEnd = m.index + m[0].length;
                const after = body.slice(mEnd);
                const opens = [];
                let lastBrace = 0;
                for (const t of after.matchAll(RET_TOK)) {
                    const tok = t[0];
                    const ts = t.index;
                    if (tok === "{") {
                        opens.push(ts);
                        lastBrace = ts + 1;
                        continue;
                    }
                    if (tok === "}") {
                        lastBrace = ts + 1;
                        if (!opens.length) break;
                        opens.pop();
                        continue;
                    }
                    if (!ERR_RET.test(tok)) {
                        if (!opens.length) break;
                        continue;
                    }
                    const seg = after.slice(lastBrace, ts);
                    const start = Math.max(rfind(after, ";", ts) + 1, lastBrace);
                    let guard = after.slice(start, ts);
                    if (!/\bif\s*\(/.test(guard) && opens.length) {
                        const o = opens[opens.length - 1];
                        guard = after.slice(Math.max(rfind(after, ";", o), rfind(after, "}", o), 0), o);
                    }
                    if (freed.test(seg) || nullchk.test(guard)) continue;
                    out.push(new Defect("D47", "error", path, lineOf(blank, off + mEnd + ts),
                        bodyRaw(raw, off + mEnd + ts, tok),
                        "*" + p + " was malloc'd at line " + lineOf(blank, off + m.index) + " and this error return leaves it allocated -- free(*" + p + "); *" + p + " = NULL first"));
                }
            }
        }
    }
});

function jsonPy(v, ind, depth) {
    const pad = "\n" + " ".repeat(ind * (depth + 1)), end = "\n" + " ".repeat(ind * depth);
    if (Array.isArray(v)) return v.length ? "[" + pad + v.map((x) => jsonPy(x, ind, depth + 1)).join("," + pad) + end + "]" : "[]";
    if (v && typeof v === "object") {
        const ks = Object.keys(v).sort(cmpStr);
        return ks.length ? "{" + pad + ks.map((k) => jsonPy(k, ind, depth + 1) + ": " + jsonPy(v[k], ind, depth + 1)).join("," + pad) + end + "}" : "{}";
    }
    if (typeof v === "string") return JSON.stringify(v).replace(/[\u007f-￿]/g, (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"));
    return JSON.stringify(v);
}

const SEV_ORDER = { error: 0, warn: 1, info: 2 };

const SUPPRESS_FILE = "tools/defects-baseline.tsv";
const SUPPRESS_HEAD = "file\trule\tfunction\tcount\treason";

function relKey(root, p) {
    return p.startsWith(root + "/") ? p.slice(root.length + 1) : p;
}

export function parseSuppressions(text) {
    const rows = [];
    const seen = new Set();
    const lines = text.split("\n");
    if (lines[0] !== SUPPRESS_HEAD) throw new Error("line 1 must be the header '" + SUPPRESS_HEAD.replaceAll("\t", "<TAB>") + "'");
    for (let i = 1; i < lines.length; i++) {
        const l = lines[i];
        if (l === "") continue;
        const f = l.split("\t");
        if (f.length !== 5) throw new Error("line " + (i + 1) + ": want 5 tab-separated fields, got " + f.length);
        const [file, rule, fn, cnt, reason] = f;
        const count = Number(cnt);
        if (!/^D\d\d$/.test(rule)) throw new Error("line " + (i + 1) + ": rule '" + rule + "' is not Dnn");
        if (!file || !fn || !Number.isInteger(count) || count < 1) throw new Error("line " + (i + 1) + ": empty file/function or count < 1");
        if (pyStrip(reason).length < 8) throw new Error("line " + (i + 1) + ": a reason of at least 8 characters is required");
        const k = file + "\t" + rule + "\t" + fn;
        if (seen.has(k)) throw new Error("line " + (i + 1) + ": duplicate row for " + k.replaceAll("\t", " "));
        seen.add(k);
        rows.push({ file, rule, fn, count, reason, found: 0 });
    }
    return rows;
}

export function applySuppressions(root, all, rows) {
    const byKey = new Map(rows.map((r) => [r.file + "\t" + r.rule + "\t" + r.fn, r]));
    const groups = new Map();
    for (const d of all) {
        const k = relKey(root, d.file) + "\t" + d.check + "\t" + d.fn;
        if (!byKey.has(k)) continue;
        if (!groups.has(k)) groups.set(k, []);
        groups.get(k).push(d);
    }
    const suppressed = [];
    for (const [k, ds] of groups) {
        const r = byKey.get(k);
        r.found = ds.length;
        if (ds.length > r.count) continue;
        for (const d of ds) {
            d.suppressed = true;
            suppressed.push(d);
        }
    }
    const stale = rows.filter((r) => r.found < r.count);
    return { rows, stale, suppressed };
}

export function runDefects(rootArg, args, o) {
    const root = abspath(rootArg);
    SCAN_ROOT = root;
    NO_GIT = !!args.noGit;
    if (!isdir(root)) {
        std.err.puts("defect scanner: not a directory: " + root + "\n");
        return 2;
    }
    const files = [...scanFiles(root)];
    const live = new Map();
    for (const p of files) {
        const [, err] = os.stat(p);
        if (err) continue;
        const res = fileDefects(p);
        if (res !== null) live.set(p, res);
    }
    const all = [];
    const includesOf = new Map();
    const incs = new Map();
    for (const p of files) {
        const res = live.get(p);
        if (res === undefined) continue;
        const [inc, out] = res;
        includesOf.set(p, inc);
        if (p.endsWith(".inc.c")) incs.set(basename(p), p);
        for (const d of out) all.push(d);
    }
    projectChecks(root, all, includesOf, incs);
    for (const d of all) if (!d.fn) d.fn = "-";

    const supPath = args.noSuppress ? null : (args.suppress || join(root, SUPPRESS_FILE));
    let sup = { rows: [], stale: [], suppressed: [] };
    if (supPath && (args.suppress || exists(supPath))) {
        try {
            sup = applySuppressions(root, all, parseSuppressions(readText(supPath)));
        } catch (ex) {
            std.err.puts("defect scanner: bad suppression file " + supPath + ": " + (ex.message || String(ex)) + "\n");
            return 2;
        }
    }
    const shown = all.filter((d) => !d.suppressed);
    const supBy = new Map();
    for (const d of sup.suppressed) supBy.set(d.check, (supBy.get(d.check) || 0) + 1);

    const stats = new Map();
    const bySev = new Map();
    shown.sort((a, b) => (SEV_ORDER[a.sev] - SEV_ORDER[b.sev]) || cmpStr(a.check, b.check) || cmpStr(a.file, b.file) || (a.line - b.line));
    for (const d of shown) {
        stats.set(d.check, (stats.get(d.check) || 0) + 1);
        if (!bySev.has(d.check)) bySev.set(d.check, { error: 0, warn: 0, info: 0 });
        bySev.get(d.check)[d.sev]++;
    }
    let ne = 0, nw = 0, ni = 0;
    for (const d of shown) {
        if (d.sev === "error") ne++;
        else if (d.sev === "warn") nw++;
        else if (d.sev === "info") ni++;
    }
    o.print("defect scanner: " + shown.length + " findings (" + ne + " error, " + nw + " warn, " + ni + " info); " +
        sup.suppressed.length + " baselined, " + sup.stale.length + " stale baseline rows");
    for (const [cid, , title] of DEFECT_META) {
        const id = cid.split("-")[0];
        const n = stats.get(id) || 0, nb = supBy.get(id) || 0;
        if (!n && !nb) continue;
        const s = bySev.get(id) || { error: 0, warn: 0, info: 0 };
        const split = ["error", "warn", "info"].filter((k) => s[k]).map((k) => k + " " + s[k]).join(" ") || "none 0";
        o.print("  " + cid.padEnd(34) + " " + split.padEnd(18) + " " + ("+" + nb + " baselined").padEnd(14) + "  " + title);
    }
    for (const [cid] of DEFECT_META) {
        const id = cid.split("-")[0], st = STAGES.get(id), ss = STAGE_STATS.get(id);
        if (!st || !ss) continue;
        o.print("  stages " + id + ": raw " + ss.raw + st.map((x, i) => " -> " + x[0] + " " + ss.after[i]).join("") + " -> final " + ss.after[st.length - 1]);
    }
    let gateFail = 0;
    if (args.gate) {
        const gated = new Set(args.gate.split(",").map((x) => x.trim()).filter(Boolean));
        for (const d of shown) if (gated.has(d.check) && d.sev !== "info") { o.print("GATE " + d.fmt(root)); gateFail++; }
        for (const r of sup.stale) { o.print("GATE stale row " + r.file + " " + r.rule + " " + r.fn + ": " + r.count + " baselined, " + r.found + " found -- lower the count"); gateFail++; }
    }
    if (args.baselineRef && supPath) {
        let refText = null;
        if (args.baselineRef.startsWith("file:")) refText = exists(args.baselineRef.slice(5)) ? readText(args.baselineRef.slice(5)) : null;
        else refText = gitShow(root, args.baselineRef + ":" + SUPPRESS_FILE);
        if (refText !== null) {
            let ref;
            try {
                ref = parseSuppressions(refText);
            } catch (ex) {
                ref = [];
            }
            const refBy = new Map(ref.map((r) => [r.file + "\t" + r.rule + "\t" + r.fn, r.count]));
            const refRules = new Set(ref.map((r) => r.rule));
            for (const r of sup.rows) {
                const was = refBy.get(r.file + "\t" + r.rule + "\t" + r.fn);
                if (was === undefined ? refRules.has(r.rule) : r.count > was) {
                    o.print("GATE baseline grew vs " + args.baselineRef + ": " + r.file + " " + r.rule + " " + r.fn + " " + (was === undefined ? "new row" : was + " -> " + r.count));
                    gateFail++;
                }
            }
        }
    }
    if (gateFail) o.print("gate: " + gateFail + " finding(s) outside the shrink-only baseline");
    const show = std.getenv("DEFECT_VERBOSE") || (args.defectsAll ? "1" : "");
    for (const d of shown) if (show === "1" || show === "all" || d.sev === "error") o.print(d.fmt(root));
    for (const r of sup.stale) o.print("STALE " + r.file + "\t" + r.rule + "\t" + r.fn + "\t" + r.count + " baselined, " + r.found + " found: shrink the row");
    if (args.emitTsv) {
        const groups = new Map();
        for (const d of shown) {
            const k = relKey(root, d.file) + "\t" + d.check + "\t" + d.fn;
            groups.set(k, (groups.get(k) || 0) + 1);
        }
        for (const k of [...groups.keys()].sort(cmpStr)) o.print("TSV\t" + k + "\t" + groups.get(k) + "\t");
    }

    if (args.updateBaseline) {
        const keys = [...new Set(shown.map((d) => d.key()))].sort(cmpStr);
        const counts = {};
        for (const [k, n] of stats) counts[k] = n;
        const f = std.open(args.updateBaseline, "w");
        f.puts(jsonPy({ keys, counts }, 1, 0));
        f.close();
        o.print("baseline written: " + keys.length + " keys -> " + args.updateBaseline);
        return 0;
    }
    const bl = args.defectsBaseline;
    if (bl && exists(bl)) {
        let base;
        try {
            base = new Set(JSON.parse(readText(bl)).keys || []);
        } catch (ex) {
            std.err.puts("defect scanner: unreadable baseline " + bl + ": " + String(ex) + "\n");
            return 2;
        }
        const neu = shown.filter((d) => !base.has(d.key()));
        for (const d of neu) o.print("NEW " + d.fmt(root));
        o.print("baseline: " + base.size + " known, " + neu.length + " new");
        if (neu.length) return 1;
    }
    return ne || gateFail ? 1 : 0;
}

function gitShow(root, spec) {
    const [r, w] = os.pipe();
    const pid = os.exec(["git", "-C", root, "show", spec], { block: false, stdout: w, stderr: w });
    os.close(w);
    const fh = std.fdopen(r, "r");
    const txt = fh.readAsString();
    fh.close();
    const [, st] = os.waitpid(pid, 0);
    return st === 0 ? txt : null;
}

function parseArgs(argv) {
    const a = { root: ".", defectsBaseline: null, updateBaseline: null, defectsAll: false, jobs: 0, suppress: null, noSuppress: false, emitTsv: false, gate: null, baselineRef: null };
    let gotRoot = false;
    for (let i = 0; i < argv.length; i++) {
        const t = argv[i];
        const take = () => {
            if (i + 1 >= argv.length) throw new Error("argument " + t + ": expected one argument");
            return argv[++i];
        };
        if (t === "--defects") continue;
        else if (t === "--defects-all") a.defectsAll = true;
        else if (t === "--defects-baseline") a.defectsBaseline = take();
        else if (t.startsWith("--defects-baseline=")) a.defectsBaseline = t.slice(19);
        else if (t === "--update-baseline") a.updateBaseline = take();
        else if (t.startsWith("--update-baseline=")) a.updateBaseline = t.slice(18);
        else if (t === "--jobs") a.jobs = parseInt(take(), 10);
        else if (t === "--suppress") a.suppress = take();
        else if (t === "--no-suppress") a.noSuppress = true;
        else if (t === "--emit-tsv") a.emitTsv = true;
        else if (t === "--no-git") a.noGit = true;
        else if (t === "--gate") a.gate = take();
        else if (t === "--baseline-ref") a.baselineRef = take();
        else if (t.startsWith("-") && t !== "-") throw new Error("unrecognized arguments: " + t);
        else if (!gotRoot) {
            a.root = t;
            gotRoot = true;
        } else throw new Error("unrecognized arguments: " + t);
    }
    return a;
}

let args;
try {
    args = parseArgs(scriptArgs.slice(1));
} catch (e) {
    std.err.puts("usage: defects.js [-h] [--defects-baseline PATH] [--update-baseline PATH] [--defects-all] [--jobs N] [--suppress TSV] [--no-suppress] [--emit-tsv] [--gate RULES] [--baseline-ref REF] [--no-git] [root]\ndefects.js: error: " + e.message + "\n");
    std.exit(2);
}
const o = new (await import("./pyshim.js")).Out();
const rc = runDefects(args.root, args, o);
o.flush();
std.exit(rc);
