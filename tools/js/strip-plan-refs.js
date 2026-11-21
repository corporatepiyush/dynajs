import * as std from "std";
import { listdir, isdir, isfile, join, basename, readBytes, writeText, pyRepr, cmpTuple, Out } from "./pyshim.js";

const VOCAB = [
    "CN", "DT", "CC", "RE", "NS", "BY", "EC", "HS", "CY", "CP", "RD", "UD",
    "CV", "DC", "TM", "SD", "NT", "HP", "UL", "HL", "SP", "SY", "FL", "CL",
    "XM", "YA", "JS", "ST", "DF", "ML", "SC", "CG", "LG", "MT", "MX", "SV",
    "SZ", "VL", "OA", "UI", "GB",
];
const VOC = "(?:" + VOCAB.join("|") + ")";
const NUM = "\\d{1,2}";
const RANGE = `(?:\\.\\.(?:${VOC}-)?${NUM})`;
const SEG = `(?:[/,][ \\t]*(?:${VOC}-)?${NUM}(?:${RANGE})*|${RANGE})*`;
const ITEM_EXPR = `(?<![\\w-])${VOC}-${NUM}(?:${RANGE})*${SEG}(?![\\w-])` + `|(?<![\\w-])${VOC}-\\*(?![\\w-])`;
const E6T = "(?<![\\w-])E\\d+-T\\d+(?![\\w-])";
const TICKET_T = "(?<![\\w-])T-(?:[0-9]+|[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)*)(?![\\w-])";
const WORK = "(?<![\\w-])work\\d{1,3}(?![\\w-])";
const SECTION =
    "§\\d+(?:\\.\\d+)*(?:[ \\t]*(?:items?[ \\t]+\\d+[a-z]?(?:[ \\t]*(?:,|and|&|-)[ \\t]*\\d+[a-z]?)*)|[-–]\\d+[a-z]?)?" +
    "|§[IVXLC]+(?:\\.\\d+)*" +
    "|(?<![\\w-])SS\\d+(?:\\.\\d+)*(?![\\w-])";
const CTX_WORD = "(?:ticket|issue|defect|finding|bug|checklist)";
const CTX = `(?<![\\w-])${CTX_WORD}s?[ \\t]*#?[ \\t]*(?:${E6T}|${TICKET_T}|\\d+[a-z]?)(?![\\w-])`;
const CTX2 = `(?<![\\w-])(?:plan|audit|queue|roadmap)[ \\t]+items?[ \\t]+(?:${ITEM_EXPR}|\\d+[a-z]?(?:[-–]\\d+[a-z]?)?)`;

const STRONG_PATTERNS = [SECTION, CTX2, CTX, ITEM_EXPR, E6T, TICKET_T, WORK].map((p) => new RegExp(p, "g"));
const PURE_PAREN = new RegExp(`[ \\t]?\\(\\s*(?:${ITEM_EXPR})(?:[ \\t]*[,;][ \\t]*(?:${ITEM_EXPR}))*[ \\t]*\\)`, "g");
const WHITELIST = new RegExp(
    "(?:HMAC-)?(?:SHA-?\\d{0,3}|SHA3-\\d{2,3}|SHAKE\\d{2,3}|AES-\\d{2,3}" +
        "|UTF-\\d{1,2}|UCS-\\d{1,2}|ISO-[\\d-]+|RSA-\\d{2,4}|P-\\d{3}|X-?509" +
        "|ARC4-\\d{1,2}|SEED-\\d{0,2}|GOST-?\\d*|3?DES-\\d{0,3}|ChaCha\\d*|Poly\\d{3})",
    "g",
);
const REVIEW_PATTERNS = [
    ["phase-n", "(?<![\\w-])[Pp]hase[ \\t]+\\d+"],
    ["letter-digit-id", "(?<![\\w-])[FCNRMHDTSGPE]\\d{1,2}(?![\\w-])"],
    ["x-finding", "(?<![\\w-])[xX][1-9](?![\\w-])"],
    ["bare-item", "(?<![\\w-])(?:item|step|bullet|point|entry)[ \\t]+\\d+[a-z]?(?![\\w-])"],
    [
        "lane-codename",
        "(?<![\\w-])(?:p0-dts|a\\d-[a-z0-9][a-z0-9-]*|ns\\d-stream" +
            "|b\\d-[a-z0-9][a-z0-9-]*|wc\\d?-?[a-z0-9][a-z0-9-]*" +
            "|d\\d-[a-z0-9][a-z0-9-]*|e\\d-[a-z0-9][a-z0-9-]*" +
            "|fix-[a-z0-9][a-z0-9-]*|make-testmod)(?![\\w-])",
    ],
].map(([n, p]) => [n, new RegExp(p, "g")]);

const TIDY_RULES = [
    [/\([ \t]*\)/g, ""],
    [/\([ \t]*[,;:][ \t]*/g, "("],
    [/\([ \t]*(?:--|[-–—])[ \t]+/g, "("],
    [/\([ \t]+/g, "("],
    [/[ \t]*[,;:][ \t]*\)/g, ")"],
    [/[ \t]+(?:--|[-–—])[ \t]*\)/g, ")"],
    [/^[ \t]*(?:--|—)[ \t]+/g, ""],
    [/^[ \t]*[,;:][ \t]+/g, ""],
    [/[ \t]{2,}/g, " "],
    [/[ \t]+([,;:])/g, "$1"],
];
const BLOCK_LEADER = /^([ \t]*\*[ \t]*)([^\n]*)$/;

function mergeCode(regions, kind, start, end) {
    if (end <= start) return;
    const last = regions[regions.length - 1];
    if (last && last[0] === kind && kind === "code" && last[2] === start) last[2] = end;
    else regions.push([kind, start, end]);
}

function scanCFamily(text, template = false) {
    const regions = [];
    const n = text.length;
    let i = 0;
    while (i < n) {
        const ch = text[i];
        if (text.startsWith("//", i)) {
            let j = text.indexOf("\n", i);
            j = j < 0 ? n : j;
            regions.push(["code", i, i + 2]);
            regions.push(["mutable", i + 2, j]);
            i = j;
            continue;
        }
        if (text.startsWith("/*", i)) {
            const close = text.indexOf("*/", i + 2);
            const closeEnd = close < 0 ? n : close + 2;
            regions.push(["code", i, i + 2]);
            regions.push(["mutable", i + 2, closeEnd - 2]);
            i = closeEnd;
            continue;
        }
        if (ch === '"' || ch === "'" || (template && ch === "`")) {
            const q = ch;
            let j = i + 1;
            while (j < n) {
                if (text[j] === "\\") {
                    j += 2;
                    continue;
                }
                if (text[j] === q) {
                    j++;
                    break;
                }
                j++;
            }
            regions.push(["string", i, Math.min(j, n)]);
            i = Math.min(j, n);
            continue;
        }
        let j = i;
        while (j < n) {
            const c = text[j];
            if (c === '"' || c === "'" || (template && c === "`")) break;
            if (text.startsWith("//", j)) break;
            if (text.startsWith("/*", j)) break;
            j++;
        }
        mergeCode(regions, "code", i, j);
        i = j;
    }
    return coalesce(regions, text);
}

function isWordStartPrev(c) {
    return c === " " || c === "\t" || c === ";" || c === "&" || c === "|" || c === "(";
}

function scanHashComments(text, wordStart = false, recipeTabs = false) {
    const regions = [];
    const n = text.length;
    let i = 0, lineStart = 0;
    while (i < n) {
        const ch = text[i];
        if (ch === "\n") {
            i++;
            lineStart = i;
            continue;
        }
        if (recipeTabs && ch === "\t" && i === lineStart) {
            let j = text.indexOf("\n", i);
            j = j < 0 ? n : j;
            regions.push(["code", i, j]);
            i = j;
            continue;
        }
        if (ch === "#" && (!wordStart || i === lineStart || isWordStartPrev(text[i - 1])) && !(i > 0 && text[i - 1] === "\\")) {
            let j = text.indexOf("\n", i);
            j = j < 0 ? n : j;
            regions.push(["code", i, i + 1]);
            regions.push(["mutable", i + 1, j]);
            i = j;
            continue;
        }
        if (ch === '"' || ch === "'") {
            const q = ch;
            const q3 = q + q + q;
            if (text.startsWith(q3, i)) {
                const close = text.indexOf(q3, i + 3);
                const end = close < 0 ? n : close + 3;
                regions.push(["string", i, end]);
                i = end;
                continue;
            }
            let j = i + 1;
            while (j < n) {
                if (text[j] === "\\") {
                    j += 2;
                    continue;
                }
                if (text[j] === q || text[j] === "\n") {
                    j++;
                    break;
                }
                j++;
            }
            regions.push(["string", i, Math.min(j, n)]);
            i = Math.min(j, n);
            continue;
        }
        let j = i;
        while (j < n) {
            const c = text[j];
            if (c === "\n" || c === '"' || c === "'") break;
            if (c === "#" && (!wordStart || j === lineStart || isWordStartPrev(text[j - 1])) && !(text[j - 1] === "\\")) break;
            j++;
        }
        mergeCode(regions, "code", i, j);
        i = j;
    }
    return coalesce(regions, text);
}

const FENCE = /^[ \t]{0,3}(```+|~~~+)/gm;
const INLINE_CODE = /(`+)[^`\n]*?\1/g;
const LINK_DEST = /\]\([^)\n]*\)|<https?:\/\/[^>\s]*>/g;

function scanMarkdown(text, includeCode = false) {
    const cands = [];
    let pos = 0;
    for (;;) {
        FENCE.lastIndex = pos;
        const m = FENCE.exec(text);
        if (!m) break;
        const mEnd = m.index + m[0].length;
        FENCE.lastIndex = mEnd;
        const close = FENCE.exec(text);
        const end = close ? close.index + close[0].length : text.length;
        cands.push([m.index, end, includeCode ? "mutable" : "string"]);
        pos = end;
    }
    if (!includeCode) for (const m of text.matchAll(INLINE_CODE)) cands.push([m.index, m.index + m[0].length, "string"]);
    for (const m of text.matchAll(LINK_DEST)) cands.push([m.index, m.index + m[0].length, "string"]);
    cands.sort((a, b) => a[0] - b[0] || b[1] - b[0] - (a[1] - a[0]));
    const spans = [];
    for (const c of cands) {
        if (spans.length && c[0] < spans[spans.length - 1][1]) continue;
        spans.push(c);
    }
    spans.sort(cmpTuple);
    const regions = [];
    pos = 0;
    for (const [s, e, kind] of spans) {
        if (s > pos) regions.push(["mutable", pos, s]);
        regions.push([kind, s, e]);
        pos = e;
    }
    if (pos < text.length) regions.push(["mutable", pos, text.length]);
    return coalesce(regions, text);
}

function coalesce(regions, text) {
    const out = [];
    for (const [kind, s, e] of regions) {
        if (e <= s && kind !== "string") continue;
        const last = out[out.length - 1];
        if (last && last[0] === kind && last[2] === s) last[2] = e;
        else out.push([kind, s, e]);
    }
    const filled = [];
    let pos = 0;
    for (const [kind, s, e] of out) {
        if (s > pos) filled.push(["code", pos, s]);
        filled.push([kind, Math.max(s, pos), e]);
        pos = Math.max(pos, e);
    }
    if (pos < text.length) filled.push(["code", pos, text.length]);
    return filled;
}

const LEXERS = [
    [".c", (t) => scanCFamily(t)],
    [".h", (t) => scanCFamily(t)],
    [".inc", (t) => scanCFamily(t)],
    [".js", (t) => scanCFamily(t, true)],
    [".mjs", (t) => scanCFamily(t, true)],
    [".cjs", (t) => scanCFamily(t, true)],
    [".ts", (t) => scanCFamily(t, true)],
    [".tsx", (t) => scanCFamily(t, true)],
    [".py", (t) => scanHashComments(t)],
    [".sh", (t) => scanHashComments(t, true)],
    [".bash", (t) => scanHashComments(t, true)],
    [".mk", (t) => scanHashComments(t, false, true)],
    [".yml", (t) => scanHashComments(t, true)],
    [".yaml", (t) => scanHashComments(t, true)],
    [".toml", (t) => scanHashComments(t, true)],
    [".md", (t) => scanMarkdown(t)],
    [".txt", (t) => [["mutable", 0, t.length]]],
];
const LEXER_MAP = new Map(LEXERS);
const NAMED_LEXERS = new Map([
    ["Makefile", (t) => scanHashComments(t, false, true)],
    ["makefile", (t) => scanHashComments(t, false, true)],
    ["GNUmakefile", (t) => scanHashComments(t, false, true)],
    ["Dockerfile", (t) => scanHashComments(t, true)],
]);
const SKIP_DIRS = new Set([
    ".git", ".agent-work", ".obj", ".swarm", "tmp_audit", "third_party",
    "test262", "pgo-data", "node_modules", "__pycache__", "scratch", "audit", "core",
]);
const SKIP_FILES = new Set([
    "AGENT.md", "CLAUDE.md", "profile-audit.md", "API_UPGRADE_PLAN.md",
    "SECURITY_COMPAT_PLAN.md", "Changelog", "findings-ml.md", "progress.log",
    "LAUNCH_RECORD.md", "heartbeat", "strip-plan-refs.py",
]);
const SKIP_FILE_SUFFIXES = [".patch", ".log", ".profdata", ".a", ".dylib", ".so"];

function splitext(name) {
    let i = name.lastIndexOf(".");
    if (i <= 0) return [name, ""];
    let k = 0;
    while (k < i && name[k] === ".") k++;
    if (k === i) return [name, ""];
    return [name.slice(0, i), name.slice(i)];
}

const endsWithAny = (s, list) => list.some((x) => s.endsWith(x));

function skippedProcess(name) {
    return SKIP_FILES.has(name) || name.startsWith("CHANGELOG") || endsWithAny(name, SKIP_FILE_SUFFIXES);
}

function wantFile(path, includeProcess) {
    const name = basename(path);
    if (NAMED_LEXERS.has(name)) return true;
    if (!includeProcess && skippedProcess(name)) return false;
    return LEXER_MAP.has(splitext(name)[1]);
}

function* walkPruned(top) {
    const names = listdir(top);
    if (names === null) return;
    let dirs = [];
    const files = [];
    for (const n of names) (isdir(join(top, n)) ? dirs : files).push(n);
    dirs = dirs.filter((d) => !SKIP_DIRS.has(d) && !d.endsWith(".dSYM") && !d.startsWith(".")).sort();
    yield [top, files];
    for (const d of dirs) yield* walkPruned(join(top, d));
}

function* iterFiles(root, includeProcess) {
    if (isfile(root)) {
        if (wantFile(root, includeProcess)) yield root;
        return;
    }
    for (const [dirpath, filenames] of walkPruned(root)) {
        for (const name of filenames.sort()) {
            if (name.startsWith(".")) continue;
            const path = join(dirpath, name);
            if (!includeProcess && (skippedProcess(name) || name.endsWith(".patch"))) continue;
            if (!wantFile(path, includeProcess)) continue;
            yield path;
        }
    }
}

function lexerFor(path) {
    const name = basename(path);
    if (NAMED_LEXERS.has(name)) return NAMED_LEXERS.get(name);
    return LEXER_MAP.get(splitext(name)[1]) || null;
}

const strictDec = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
function readTextStrict(path) {
    let b;
    try {
        b = readBytes(path);
    } catch (e) {
        return null;
    }
    const head = b.length > 8192 ? b.subarray(0, 8192) : b;
    if (head.indexOf(0) >= 0) return null;
    try {
        return strictDec.decode(b);
    } catch (e) {
        return null;
    }
}

function findAll(rx, s, out) {
    rx.lastIndex = 0;
    let m;
    while ((m = rx.exec(s)) !== null) {
        out.push([m.index, m.index + m[0].length, m[0]]);
        if (m[0].length === 0) rx.lastIndex++;
    }
}

function strongSpans(body) {
    const found = [];
    for (const rx of STRONG_PATTERNS) findAll(rx, body, found);
    findAll(PURE_PAREN, body, found);
    found.sort(cmpTuple);
    const kept = [];
    for (const f of found) {
        const [s, e] = f;
        if (kept.some(([a, b]) => a <= s && e <= b)) continue;
        kept.push(f);
    }
    const out = [];
    for (const f of kept) {
        const [s, e] = f;
        if (out.some(([a, b]) => a < e && s < b)) continue;
        const winStart = Math.max(0, s - 24);
        const win = body.slice(winStart, e + 24);
        let blocked = false;
        WHITELIST.lastIndex = 0;
        let wm = WHITELIST.exec(win);
        while (wm) {
            const wEnd = wm.index + wm[0].length;
            if (wm.index + winStart < e && s < wEnd + winStart) {
                blocked = true;
                break;
            }
            WHITELIST.lastIndex = wEnd;
            wm = WHITELIST.exec(win);
            if (wm && wm[0].length === 0 && wm.index === wEnd) {
            }
        }
        if (blocked) continue;
        out.push(f);
    }
    return out;
}

const lstripWs = (s) => s.replace(/^[ \t]+/, "");
const rstripWs = (s) => s.replace(/[ \t]+$/, "");
const rstripAll = (s) => s.replace(/[\s\x1c-\x1f\x85]+$/, "");
const lstripAll = (s) => s.replace(/^[\s\x1c-\x1f\x85]+/, "");

export function cleanBody(body, hits) {
    const spans = strongSpans(body);
    const reviewSeen = new Set();
    for (const [name, rx] of REVIEW_PATTERNS) {
        rx.lastIndex = 0;
        let m;
        while ((m = rx.exec(body)) !== null) {
            const ms = m.index, me = ms + m[0].length;
            if (m[0].length === 0) rx.lastIndex++;
            if (spans.some(([s, e]) => s <= ms && me <= e)) continue;
            const key = name + "\0" + m[0];
            if (!reviewSeen.has(key)) {
                reviewSeen.add(key);
                hits.push(["review:" + name, m[0], body]);
            }
        }
    }
    if (!spans.length) return body;
    const drop = spans.map(([s, e]) => [s, e]);
    for (const [s, e, g] of spans) {
        if (lstripAll(g).startsWith("(") && e < body.length && body[e] === ".") {
            const before = rstripAll(body.slice(0, s));
            if (before && ".!?".includes(before[before.length - 1])) drop.push([e, e + 1]);
        }
    }
    drop.sort(cmpTuple);
    const out = [];
    let pos = 0;
    for (const [s, e] of drop) {
        if (s < pos) continue;
        out.push(body.slice(pos, s));
        pos = e;
    }
    out.push(body.slice(pos));
    let nb = out.join("");
    if (nb !== body) {
        for (const [, , g] of spans) hits.push(["remove", g, body]);
        const isWs = (c) => c === " " || c === "\t";
        if (!isWs(body.slice(0, 1)) && isWs(nb.slice(0, 1))) nb = lstripWs(nb);
        if (!isWs(body.slice(-1)) && isWs(nb.slice(-1))) nb = rstripWs(nb);
        for (const [rx, repl] of TIDY_RULES) nb = nb.replace(rx, repl);
    }
    return nb;
}

class Hit {
    constructor(path, line, category, matched, before, after) {
        this.path = path;
        this.line = line;
        this.category = category;
        this.matched = matched;
        this.before = before;
        this.after = after;
    }
}

function cleanRegion(text, start, end, block, path, firstLine, hits) {
    const parts = text.slice(start, end).split("\n");
    const outParts = [];
    let edits = 0;
    for (let idx = 0; idx < parts.length; idx++) {
        const raw = parts[idx];
        const hasCr = raw.endsWith("\r");
        const line = hasCr ? raw.slice(0, -1) : raw;
        let leader = "", body = line;
        if (block && idx > 0) {
            const m = BLOCK_LEADER.exec(line);
            if (m) {
                leader = m[1];
                body = m[2];
            }
        }
        const local = [];
        const nb = cleanBody(body, local);
        if (nb !== body) {
            edits++;
            for (const [category, matched, before] of local) hits.push(new Hit(path, firstLine + idx, category, matched, before, nb));
        }
        outParts.push(leader + nb + (hasCr ? "\r" : ""));
    }
    return [outParts.join("\n"), edits];
}

function skeleton(text, regions) {
    let s = "";
    for (const [kind, a, b] of regions) if (kind !== "mutable") s += text.slice(a, b);
    return s;
}

function countNl(text, a, b) {
    let n = 0;
    for (let k = text.indexOf("\n", a); k >= 0 && k < b; k = text.indexOf("\n", k + 1)) n++;
    return n;
}

export function processText(path, text, lexer, hits) {
    const regions = lexer(text);
    const skelBefore = skeleton(text, regions);
    const pieces = [];
    let pos = 0, lineNo = 1, scanFrom = 0;
    for (const [kind, s, e] of regions) {
        lineNo += countNl(text, scanFrom, s);
        scanFrom = e;
        pieces.push(text.slice(pos, s));
        if (kind === "mutable") {
            const seg = text.slice(s, e);
            const block = seg.indexOf("*") >= 0 && seg.includes("\n");
            const [ns] = cleanRegion(text, s, e, block, path, lineNo, hits);
            pieces.push(ns);
        } else pieces.push(text.slice(s, e));
        lineNo += countNl(text, s, e);
        pos = e;
    }
    pieces.push(text.slice(pos));
    const nt = pieces.join("");
    if (nt === text) return null;
    if (skeleton(nt, lexer(nt)) !== skelBefore) {
        hits.push(new Hit(path, 0, "ERROR", "skeleton mismatch -- file left untouched", "", ""));
        return null;
    }
    return nt;
}

function run(paths, apply, includeProcess, mdCode, quiet, o) {
    const allHits = [];
    let filesScanned = 0, filesEdited = 0, removable = 0, review = 0, errors = 0;
    for (const root of paths) {
        for (const path of iterFiles(root, includeProcess)) {
            let lexer = lexerFor(path);
            if (lexer === null) continue;
            if (mdCode && path.endsWith(".md")) lexer = (t) => scanMarkdown(t, true);
            const text = readTextStrict(path);
            if (text === null) continue;
            filesScanned++;
            const hits = [];
            const nt = processText(path, text, lexer, hits);
            for (const h of hits) allHits.push(h);
            if (nt !== null && apply) {
                writeText(path, nt);
                filesEdited++;
            }
        }
    }
    for (const h of allHits) {
        if (h.category === "ERROR") {
            errors++;
            o.print(`ERROR ${h.path}: ${h.matched}`);
        } else if (h.category === "remove") {
            removable++;
            if (!quiet) {
                o.print(`${h.path}:${h.line}: REMOVE  '${h.matched}'`);
                o.print(`    before: ${h.before}`);
                o.print(`    after : ${h.after}`);
            }
        } else {
            review++;
            if (!quiet) o.print(`${h.path}:${h.line}: REVIEW  [${h.category}] '${h.matched}'`);
        }
    }
    o.print(`\nsummary: ${filesScanned} files scanned, ${filesEdited} edited, ${removable} removable refs, ${review} review-only, ${errors} errors`);
    if (errors) return 2;
    return removable && !apply ? 1 : 0;
}

function reprAny(v) {
    if (v === null || v === undefined) return "None";
    if (v === true) return "True";
    if (v === false) return "False";
    if (typeof v === "string") return pyRepr(v);
    if (Array.isArray(v)) return "[" + v.map(reprAny).join(", ") + "]";
    return String(v);
}

function eq(a, b) {
    if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => eq(x, b[i]));
    return (a ?? null) === (b ?? null);
}

function selftest(o) {
    const failures = [];
    const check = (name, got, want) => {
        if (!eq(got, want)) failures.push(`${pyRepr(name)}: got ${reprAny(got)} want ${reprAny(want)}`);
    };
    const clean = (line) => cleanBody(line, []);
    check("item-basic", clean("DT-12 parseDuration returns ns"), "parseDuration returns ns");
    check("item-leading-dash", clean("(BY-4 -- read this before hunting)"), "(read this before hunting)");
    check("pure-paren", clean("STRICT (CC-6): unknown keys throw"), "STRICT: unknown keys throw");
    check("pure-paren-sentence", clean("call. (RE-9)."), "call.");
    check("pure-paren-mid", clean("chainable (BY-3 BEHAVIOR CHANGE: returns handle)"), "chainable (BEHAVIOR CHANGE: returns handle)");
    check("mixed-paren", clean("getter note (DT-8, resolved): the member"), "getter note (resolved): the member");
    check("range", clean("see CC-1..4/6 now"), "see now");
    check("range-dotted", clean("assertions for DT-1..DT-17 land"), "assertions for land");
    check("range-double", clean("fix CY-2..4/6..8 fast"), "fix fast");
    check("wildcard", clean("RD-* and CG-* done"), "and done");
    check("slash-list", clean("wire GB-1/3/4 into the gate"), "wire into the gate");
    check("section", clean("open §36.6 items 1-7 today"), "open today");
    check("section-roman", clean("see §I.1 for recipes"), "see for recipes");
    check("ss-ref", clean("DISCARDED per SS18 already"), "DISCARDED per already");
    check("lane-work", clean("work22 review verdict"), "review verdict");
    check("ticket-e6", clean("E6-T1 detached text"), "detached text");
    check("ticket-cap", clean("T-UDP-SELF-CLOSE-UAF ticketed"), "ticketed");
    check("ctx-ticket", clean("the ticket 16 flaky assert"), "the flaky assert");
    check("ctx-plan-item", clean("plan item CL-2 lands"), "lands");
    check("double-space-collapse", clean("RE-2..RE-14  docs"), "docs");
    check("empty-paren", clean("done (NS-1) now"), "done now");
    for (const keep of [
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
    ])
        check(`keep:${keep.slice(0, 24)}`, clean(keep), keep);
    check("md-bullet-keeps-dash", clean("- DT-1 note"), "- note");
    check("paren-label-residue", clean("(SD-5): the kernels read"), "the kernels read");
    check("paren-label-mid", clean("reinterpretation (SD-5): the kernels"), "reinterpretation: the kernels");
    check("keep-colon-param", clean(":param CL-1 was here"), ":param was here");
    let hits = [];
    const got0 = cleanBody("phase 2 of the pipeline", hits);
    check("review-untouched", got0, "phase 2 of the pipeline");
    check("review-reported", hits.some((h) => h[0] === "review:phase-n"), true);
    const src = 'int x; // CL-1 fix\nchar *s = "CL-2";\n/* BY-3 */\n';
    const edited = processText("t.c", src, (t) => scanCFamily(t), []);
    check("c-edit", edited, 'int x; // fix\nchar *s = "CL-2";\n/* */\n');
    check("c-string-protected", (edited || "").includes('"CL-2"'), true);
    const js = "const t = `keep ${/* CL-1 */ a} BY-3`;\n";
    check("js-template-opaque", processText("t.js", js, (t) => scanCFamily(t, true), []), null);
    const sh = "echo a#b  # CL-1 drop\n";
    check("sh-word-start", processText("t.sh", sh, (t) => scanHashComments(t, true), []), "echo a#b  # drop\n");
    const mk = "all:\n\t# CL-1 recipe-comment stays\n# CL-2 top goes\n";
    check("make-recipe-opaque", processText("Makefile", mk, (t) => scanHashComments(t, false, true), []), "all:\n\t# CL-1 recipe-comment stays\n# top goes\n");
    const md = "Prose CL-1 gone.\n\n```js\n// CL-2 stays in fence\n```\nInline `CL-3` stays.\n";
    check("md-prose-only", processText("t.md", md, (t) => scanMarkdown(t), []), "Prose gone.\n\n```js\n// CL-2 stays in fence\n```\nInline `CL-3` stays.\n");
    const md2 = "Prose CL-1 gone.\n\n```js\n// CL-2 goes too\n```\n";
    check("md-code-flag", processText("t.md", md2, (t) => scanMarkdown(t, true), []), "Prose gone.\n\n```js\n// goes too\n```\n");
    const evil = "/* keep A*HS-1/B tail */\nint ok;\n";
    hits = [];
    check("forge-terminator-blocked", processText("evil.c", evil, (t) => scanCFamily(t), hits), null);
    check("forge-terminator-reported", hits.some((h) => h.category === "ERROR"), true);
    check("forge-terminator-error-named", hits.filter((h) => h.category === "ERROR").map((h) => h.path), ["evil.c"]);
    const blk = "/*\n *  DT-8 name\n *  keep me\n */\n";
    check("leader-preserved", processText("t.c", blk, (t) => scanCFamily(t), []), "/*\n *  name\n *  keep me\n */\n");
    if (failures.length) {
        o.print("SELFTEST FAILURES:");
        for (const f of failures) o.print("  " + f);
        return 1;
    }
    o.print("selftest: all checks passed");
    return 0;
}

function main(argv) {
    const o = new Out();
    const flags = { apply: false, check: false, selftest: false, includeProcess: false, mdCode: false, quiet: false };
    const names = { "--apply": "apply", "--check": "check", "--selftest": "selftest", "--include-process": "includeProcess", "--md-code": "mdCode", "--quiet": "quiet" };
    const paths = [];
    for (const a of argv) {
        if (names[a]) flags[names[a]] = true;
        else if (a.startsWith("-") && a !== "-") {
            std.err.puts(`usage: strip-plan-refs.js [-h] [--apply] [--check] [--selftest] [--include-process] [--md-code] [--quiet] [paths ...]\nstrip-plan-refs.js: error: unrecognized arguments: ${a}\n`);
            return 2;
        } else paths.push(a);
    }
    let rc;
    if (flags.selftest) rc = selftest(o);
    else {
        rc = run(paths.length ? paths : ["."], flags.apply, flags.includeProcess, flags.mdCode, flags.quiet, o);
        if (flags.check && rc === 0) rc = 0;
    }
    o.flush();
    return rc;
}

std.exit(main(scriptArgs.slice(1)));
