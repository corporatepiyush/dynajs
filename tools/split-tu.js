// flags: --std
// split-tu.js -- split one C translation unit into several, driven by clang's AST.
//
// usage: ./dynajs --std tools/split-tu.js SPEC.json [--dry]
//
// SPEC:
//   {
//     "source":  "src/dyna-structures.c",          the TU to split
//     "inline":  ["src/dyna-structures-db.inc.c"],  its private .inc.c files, inlined where included
//                                                    (or [spelling-in-the-#include, path] pairs)
//     "outdir":  "src/modules/structures",
//     "header":  "structures-internal.h",            shared types, macros, static inline, prototypes
//     "default": "structures-module.c",              anything no rule and no affinity places
//     "flags":   "<compile flags without -c/-o/source>",
//     "rules":   [["^dsw_bitset_", "structures-bitset.c"], ...],   first match on the decl name wins
//     "origin_rules": [["builtins/array", "engine-array.c"], ...],    then: the file a definition came from
//     "header_names": ["^helper_that_must_be_inline$"],             optional: force into the header
//     "include_rewrite": {"dyn-ds.h": "../dyn-ds.h"},                optional: quoted includes, as seen from outdir
//     "inline_lines": 12                                              optional: a static function shared across
//   }                                                                   outputs and at most this long becomes
//                                                                       static inline in the header (keeps inlining);
//                                                                       only from the files in "inline_from"
//                                                                       (default: the default file -- shared helpers)
//   also: "raw_file"/"raw_guard", "core_tu_macro", "alias_headers", "keep_fragments", "localise".
//
// What moves where:
//   - top-level #include/#define/#undef, types (struct/union/enum/typedef), static inline functions and
//     file-scope #if blocks holding only those go to the header, in their original order;
//   - every other function/variable goes to the file its name's rule names; a declaration with no rule
//     goes to the one file every reference to it comes from (affinity, to a fixed point), else to default;
//   - a static symbol referenced from another output file (or from the header) loses `static` and gets a
//     prototype / extern in the header; one referenced only from its own file stays static;
//   - an outer `#if CONFIG...` guard spanning the whole file is replicated into every output, and so is
//     any #if block without #else whose declarations go to several outputs.
// Nothing is rewritten inside a declaration except a leading `static`.
// Port of tools/split-tu.py: clang reports byte offsets, which are mapped to string indices once, so the
// rest works on the decoded text. Output, stdout and report.json were byte-identical to the .py on the
// 99 specs under .agent-work/split before the .py was removed.
import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";
import { indexOf as bytesIndexOf } from "dyna:bytes";

const die = (m) => { std.err.puts("split-tu: " + m + "\n"); std.exit(1); };
const argv = scriptArgs.slice(1);
if (!argv[0]) die("usage: split-tu.js SPEC.json [--dry]");
const read = (p) => { const t = std.loadFile(p); if (t === null) die("cannot read " + p); return t; };
const write = (p, t) => { const f = std.open(p, "w"); if (!f) die("cannot write " + p); f.puts(t); f.close(); };
const mkdirs = (p) => { let acc = p.startsWith("/") ? "" : "."; for (const s of p.split("/").filter(Boolean)) { acc += "/" + s; os.mkdir(acc, 0o755); } };
const basename = (p) => p.replace(/\/+$/, "").split("/").pop();
const absPath = (p) => {
    const base = p.startsWith("/") ? [] : os.getcwd()[0].split("/").filter(Boolean);
    for (const s of p.split("/")) { if (!s || s === ".") continue; if (s === "..") base.pop(); else base.push(s); }
    return "/" + base.join("/");
};
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const WS = "[ \\t\\n\\r\\f\\v]";
const isWs = (c) => c === " " || c === "\t" || c === "\n" || c === "\r" || c === "\f" || c === "\v";
const rstrip = (s, set) => { let e = s.length; while (e > 0 && (set ? set.includes(s[e - 1]) : isWs(s[e - 1]))) e--; return s.slice(0, e); };
const lstrip = (s) => { let b = 0; while (b < s.length && isWs(s[b])) b++; return s.slice(b); };
const strip = (s, set) => { if (set) { let b = 0; while (b < s.length && set.includes(s[b])) b++; return rstrip(s.slice(b), set); } return rstrip(lstrip(s)); };
const count = (s, sub) => s.split(sub).length - 1;
const utf8len = (s) => { let n = 0; for (const ch of s) { const c = ch.codePointAt(0); n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; } return n; };
const pyCmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const sorted = (it) => [...it].sort(pyCmp);
const union = (...ss) => { const r = new Set(); for (const s of ss) for (const v of s) r.add(v); return r; };
const IDENT_G = /\b[A-Za-z_]\w*\b/g;
const findall = (re, t) => t.match(re) || [];

const spec = JSON.parse(read(argv[0]));
const dry = argv.includes("--dry");
const src = spec.source, outdir = spec.outdir, hdrName = spec.header, dflt = spec.default;
const rules = spec.rules.map(([r, f]) => [new RegExp(r), f]);
const hdrForce = (spec.header_names || []).map((r) => new RegExp(r));
const work = ".agent-work/split/" + basename(outdir);
mkdirs(work);

let text = read(src);
const rewrites = Object.entries(spec.include_rewrite || {});
for (const [o, n] of rewrites) text = text.split(`#include "${o}"`).join(`#include "${n}"`);
const inlined = [];
for (let inc of spec.inline || []) {
    let spelled;
    if (Array.isArray(inc)) [spelled, inc] = inc; else spelled = basename(inc);
    const line = `#include "${spelled}"\n`;
    if (count(text, line) !== 1) die(`${inc} is not included exactly once by ${src}`);
    let body = read(inc);
    if (inc.endsWith(".h")) {
        const g = /^(\s*)#ifndef\s+(\w+)\s*\n#define\s+\2\s*\n/.exec(body);
        if (g) {
            const tail = rstrip(body);
            if (!tail.endsWith("#endif")) die(`${inc}: include guard does not close at the end`);
            body = body.slice(g[0].length, tail.length - "#endif".length);
        }
    }
    if (!body.endsWith("\n")) body += "\n";
    for (const [o, n] of rewrites) body = body.split(`#include "${o}"`).join(`#include "${n}"`);
    text = text.split(line).join(body);
    inlined.push([inc, body]);
}
const origins = [];
for (const [inc, body] of inlined) {
    const at = text.indexOf(body);
    if (at < 0 || text.indexOf(body, at + 1) >= 0) die(`cannot place ${inc} in the combined text`);
    origins.push([at, at + body.length, inc]);
}
const originRules = (spec.origin_rules || []).map(([r, f]) => [new RegExp(r), f]);
const combined = absPath(work + "/combined.c");
write(combined, text);
const tb = text;

const b2c = (() => {
    const m = new Int32Array(utf8len(tb) + 1);
    let b = 0, c = 0;
    for (const ch of tb) {
        const n = utf8len(ch);
        for (let k = 0; k < n; k++) m[b + k] = c;
        b += n; c += ch.length;
    }
    m[b] = c;
    return m;
})();
const toC = (byteOff) => (byteOff >= b2c.length ? tb.length + (byteOff - (b2c.length - 1)) : b2c[byteOff]);

const astPath = work + "/ast.json";
const r0 = Exec("sh", ["-c", `clang ${spec.flags} -fsyntax-only -Xclang -ast-dump=json ${combined} > ${astPath}`], { encoding: "utf8", maxBuffer: 64 << 20 });
if (r0.code !== 0) die("clang failed:\n" + String(r0.stderr).slice(-4000));
function* astInner(path) {
    const f = std.open(path, "rb");
    if (!f) die("cannot read " + path);
    const dec = new TextDecoder(), enc = (s) => new TextEncoder().encode(s);
    const OPEN = enc('\n  "inner": [\n'), CLOSE = enc("\n    }"), INNER = enc('\n      "inner": ['), REFD = enc('"referencedDecl": {');
    let buf = new Uint8Array(0), pos = 0, started = false, eof = false;
    const more = () => {
        const c = f.readBytes(64 << 20);
        if (!c || !c.length) { eof = true; return false; }
        const nb = new Uint8Array(buf.length - pos + c.length);
        nb.set(buf.subarray(pos));
        nb.set(c, buf.length - pos);
        buf = nb; pos = 0;
        return true;
    };
    while (!started) {
        const at = bytesIndexOf(buf, OPEN, pos);
        if (at >= 0) { pos = at + OPEN.length; started = true; } else if (!more()) { f.close(); return; }
    }
    for (;;) {
        let end = -1;
        for (let from = pos; ;) {
            const at = bytesIndexOf(buf, CLOSE, from);
            if (at >= 0 && at + CLOSE.length < buf.length) {
                const nx = buf[at + CLOSE.length];
                if (nx === 0x2c || nx === 0x0a) { end = at + CLOSE.length; break; }
                from = at + 1;
                continue;
            }
            if (eof) break;
            if (!more()) break;
            from = pos;
        }
        if (end < 0) break;
        if (end - pos > (256 << 20)) {
            const at = bytesIndexOf(buf.subarray(pos, end), INNER);
            if (at < 0) die("an AST element of " + (end - pos) + " bytes has no inner list to cut");
            const head = dec.decode(buf.subarray(pos, pos + at)).replace(/,\s*$/, "") + "\n    }";
            const node = { ...JSON.parse(head), truncated: end - pos, refNames: [], fnRefNames: [] };
            const body = buf.subarray(pos + at, end);
            for (let k = bytesIndexOf(body, REFD); k >= 0; k = bytesIndexOf(body, REFD, k + REFD.length)) {
                const m = /"kind": "(\w+)",\s*"name": "([^"]*)"/.exec(dec.decode(body.subarray(k, k + 4096)));
                if (!m) die("cannot read a referencedDecl inside a " + (end - pos) + "-byte AST element");
                node.refNames.push(m[2]);
                if (m[1] === "FunctionDecl") node.fnRefNames.push(m[2]);
            }
            yield node;
        } else yield JSON.parse(dec.decode(buf.subarray(pos, end)));
        pos = end + 1;
        if (buf[end] === 0x0a) break;
    }
    f.close();
}

let cur = null;
function seeLoc(l) {
    if (!l || typeof l !== "object" || Array.isArray(l)) return;
    for (const k of ["spellingLoc", "expansionLoc"]) if (k in l) seeLoc(l[k]);
    if ("file" in l) cur = l.file;
}
const expLoc = (l) => ("expansionLoc" in l ? l.expansionLoc : l);
function walkLocs(n) {
    seeLoc(n.loc);
    const r = n.range;
    if (r) { seeLoc(r.begin); seeLoc(r.end); }
    for (const c of n.inner || []) walkLocs(c);
}
function collectRefs(n, out) {
    const rd = n.referencedDecl;
    if (rd && "name" in rd) out.add(rd.name);
    for (const c of n.inner || []) collectRefs(c, out);
}
function collectVrefs(n, out, callee) {
    const k = n.kind;
    if (k === "DeclRefExpr" && !callee) {
        const rd = n.referencedDecl;
        if (rd && rd.kind === "FunctionDecl" && "name" in rd) out.add(rd.name);
    }
    (n.inner || []).forEach((c, i) => collectVrefs(c, out, (k === "CallExpr" && i === 0) || (callee && (k === "ImplicitCastExpr" || k === "ParenExpr"))));
}
function bodyBegin(n) {
    for (const c of n.inner || []) if (c.kind === "CompoundStmt") return toC(expLoc(c.range.begin).offset);
    return null;
}

const decls = [];
for (const n of astInner(astPath)) {
    seeLoc(n.loc);
    const r = n.range || {};
    seeLoc(r.begin);
    seeLoc(r.end);
    for (const c of n.inner || []) walkLocs(c);
    if (n.isImplicit || !n.range || !Object.keys(n.range).length) continue;
    const b = expLoc(r.begin), e = expLoc(r.end);
    if (!("offset" in b) || !("offset" in e)) continue;
    if ("includedFrom" in b || ["<built-in>", "<scratch space>", "<command line>"].includes(b.file || "")) continue;
    const kind = n.kind, name = n.name || "";
    const beg = toC(b.offset);
    let end = toC(e.offset + (e.tokLen || 0));
    if ("includedFrom" in e || end < beg) {
        let depth = 0;
        for (let j = beg; j < tb.length;) {
            const ch = tb[j];
            if (ch === "\n" && tb[j + 1] === "#") { const k = tb.indexOf("\n", j + 1); j = k >= 0 ? k : tb.length; continue; }
            if (ch === "{" || ch === "(") depth++;
            else if (ch === "}" || ch === ")") depth--;
            else if (ch === ";" && depth === 0) { end = j; break; }
            j++;
        }
    }
    const fromMacro = "expansionLoc" in r.end;
    if (fromMacro) {
        let j = end;
        while (j < tb.length && (tb[j] === " " || tb[j] === "\t" || tb[j] === "\n")) j++;
        if (tb[j] === "(") {
            let depth = 0;
            for (; j < tb.length; j++) {
                const ch = tb[j];
                if (ch === "(") depth++;
                else if (ch === ")") { depth--; if (depth === 0) { end = j + 1; break; } }
            }
        }
    }
    if (name && !fromMacro && !tb.slice(beg, end).includes(name)) {
        if (kind === "FunctionDecl" || kind === "VarDecl") die(`location tracking lost sync at ${name} (${kind})`);
        continue;
    }
    const refs = new Set(), vrefs = new Set();
    collectRefs(n, refs);
    collectVrefs(n, vrefs, false);
    if (n.truncated) {
        if (kind !== "VarDecl") die(`an AST element of ${n.truncated} bytes (${kind} ${name}) is too large to load and is not a variable`);
        for (const r of n.refNames) refs.add(r);
        for (const r of n.fnRefNames) vrefs.add(r);
    }
    const params = (n.inner || []).filter((c) => c.kind === "ParmVarDecl").map((c) => c.name || "");
    decls.push({ kind, name, beg, end, storage: n.storageClass || "", vrefs, params, inline: !!n.inline, refs,
        qual: (n.type || {}).qualType || "", body: kind === "FunctionDecl" ? bodyBegin(n) : null, init: n.init ?? null });
}
os.remove(astPath);
decls.sort((a, b) => a.beg - b.beg || a.end - b.end);

const varNames = new Set(decls.filter((d) => d.kind === "VarDecl" && d.name).map((d) => d.name));
for (const d of decls)
    if (d.kind === "VarDecl" || d.kind === "RecordDecl" || d.kind === "TypedefDecl")
        for (const w of findall(IDENT_G, tb.slice(d.beg, d.end))) if (varNames.has(w) && w !== d.name) d.refs.add(w);

const skipWs = (i) => { while (i < tb.length && (tb[i] === " " || tb[i] === "\t" || tb[i] === "\n" || tb[i] === "\r")) i++; return i; };
const items = [];
for (const d of decls) {
    const j = skipWs(d.end);
    if (j < tb.length && tb[j] === ";") d.end = j + 1;
    const last = items[items.length - 1];
    if (last && d.beg < last.end) { last.end = Math.max(last.end, d.end); last.decls.push(d); }
    else items.push({ beg: d.beg, end: d.end, decls: [d], kind: "decl" });
}
const covered = items.map((it) => [it.beg, it.end]);
function inDecl(pos) {
    let lo = 0, hi = covered.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (covered[mid][1] <= pos) lo = mid + 1; else hi = mid; }
    return lo < covered.length && covered[lo][0] <= pos && pos < covered[lo][1];
}

const directives = [];
{
    const lines = tb.split("\n");
    let off = 0;
    for (let i = 0; i < lines.length;) {
        const ln = lines[i], start = off, ls = lstrip(ln);
        if (ls.startsWith("#") && !inDecl(start + (ln.length - ls.length))) {
            let j = i, endoff = off + ln.length;
            while (rstrip(lines[j]).endsWith("\\") && j + 1 < lines.length) { j++; endoff += 1 + lines[j].length; }
            const txt = tb.slice(start, endoff);
            const m = new RegExp("^" + WS + "*#" + WS + "*(\\w+)").exec(txt);
            directives.push({ beg: start, end: endoff + 1, word: m ? m[1] : "", text: txt });
            for (let k = i; k <= j; k++) off += lines[k].length + 1;
            i = j + 1;
            continue;
        }
        off += ln.length + 1;
        i++;
    }
}
const seq = [...items.map((it) => ({ ...it, kind: "decl" })), ...directives.map((d) => ({ kind: "pp", ...d }))];
seq.sort((a, b) => a.beg - b.beg);

function nest(sq) {
    const root = [], stack = [root], blocks = [];
    for (const x of sq) {
        if (x.kind === "pp" && ["if", "ifdef", "ifndef"].includes(x.word)) {
            const blk = { kind: "block", beg: x.beg, end: null, parts: [x], body: [] };
            stack[stack.length - 1].push(blk);
            stack.push(blk.body);
            blocks.push(blk);
        } else if (x.kind === "pp" && (x.word === "elif" || x.word === "else")) stack[stack.length - 1].push(x);
        else if (x.kind === "pp" && x.word === "endif") {
            stack[stack.length - 1].push(x);
            stack.pop();
            blocks.pop().end = x.end;
        } else stack[stack.length - 1].push(x);
    }
    if (blocks.length) die("unbalanced #if");
    return root;
}
let tree = nest(seq);

const pyRepr = (s) => "'" + s.replace(/\\/g, "\\\\").replace(/'/g, "\\'").replace(/\n/g, "\\n").replace(/\t/g, "\\t") + "'";
function checkGaps(nodes, lo, hi) {
    let last = lo;
    for (const x of nodes) {
        const gap = tb.slice(last, x.beg);
        if (strip(gap)) die(`uncovered text at offset ${last}: b${pyRepr(strip(gap).slice(0, 120))}`);
        last = Math.max(last, x.end);
    }
    if (strip(tb.slice(last, hi))) die(`uncovered text at offset ${last}: b${pyRepr(strip(tb.slice(last, hi)).slice(0, 120))}`);
}
checkGaps(tree, 0, tb.length);

let guard = null, guardElse = null;
let outsideIds = new Set();
const blocksTop = tree.filter((x) => x.kind === "block");
if (blocksTop.length) {
    let big = blocksTop[0];
    for (const b of blocksTop) if (b.end - b.beg > big.end - big.beg) big = b;
    const outside = tree.filter((x) => x !== big);
    if (outside.every((x) => x.kind === "pp" && x.word === "include") && big.body.some((y) => y.kind === "decl")) {
        const cond = big.parts[0].text, body = big.body;
        const els = [];
        body.forEach((y, i) => { if (y.kind === "pp" && (y.word === "elif" || y.word === "else")) els.push(i); });
        if (els.some((i) => body[i].word === "elif") || els.length > 1) die("outer guard has an #elif");
        guard = cond;
        outsideIds = new Set(outside);
        if (els.length) {
            guardElse = rstrip(tb.slice(body[els[0]].beg, body[body.length - 1].beg));
            tree = [...outside, ...body.slice(0, els[0])];
        } else tree = [...outside, ...body.slice(0, -1)];
    }
}

function flatDecls(x) {
    if (x.kind === "decl") return x.decls;
    if (x.kind === "block") return x.body.flatMap(flatDecls);
    return [];
}
function isHeaderDecl(d) {
    if (["RecordDecl", "TypedefDecl", "EnumDecl", "StaticAssertDecl"].includes(d.kind)) return true;
    if (d.kind === "FunctionDecl" && d.inline && d.storage === "static") return true;
    if (d.kind === "FunctionDecl" && d.name && hdrForce.some((r) => r.test(d.name))) return true;
    return false;
}

const fnDefs = new Map(), varDefs = new Map();
for (const d of decls) {
    if (d.kind === "FunctionDecl" && d.body !== null) fnDefs.set(d.name, d);
    else if (d.kind === "VarDecl") {
        if (!varDefs.has(d.name) || d.init !== null || tb.slice(d.beg, d.end).includes("= ")) {
            if (!varDefs.has(d.name) || !varDefs.get(d.name)._def) {
                varDefs.set(d.name, d);
                d._def = d.init !== null || tb.slice(d.beg, d.end).includes("=");
            }
        }
    }
}
const symbols = union(fnDefs.keys(), varDefs.keys());
const defOf = (n) => fnDefs.get(n) || varDefs.get(n);

function originOf(off) {
    for (const [a0, a1, inc] of origins) if (a0 <= off && off < a1) return inc;
    return src;
}
function ruleFile(name) {
    for (const [r, f] of rules) if (r.test(name)) return f;
    const d = defOf(name);
    if (d !== undefined && originRules.length) {
        const o = originOf(d.beg);
        for (const [r, f] of originRules) if (r.test(o)) return f;
    }
    return null;
}

const assign = new Map();
for (const name of symbols) {
    const d = defOf(name);
    if (isHeaderDecl(d)) { assign.set(name, "H"); continue; }
    const f = ruleFile(name);
    if (f) assign.set(name, f);
}
const userOf = new Map([...symbols].map((n) => [n, new Set()]));
for (const d of decls) {
    const owner = symbols.has(d.name) ? d.name : null;
    for (const r of d.refs) if (symbols.has(r) && r !== d.name) userOf.get(r).add(owner || "@" + d.beg);
}
for (let changed = true; changed;) {
    changed = false;
    for (const name of sorted(symbols)) {
        if (assign.has(name)) continue;
        const fs = new Set();
        let unknown = false;
        for (const u of userOf.get(name)) {
            if (u.startsWith("@")) fs.add("H");
            else if (assign.has(u)) fs.add(assign.get(u));
            else unknown = true;
        }
        if (!unknown && fs.size === 1 && !fs.has("H")) { assign.set(name, [...fs][0]); changed = true; }
    }
}
for (const name of symbols) if (!assign.has(name)) assign.set(name, dflt);

function unifyElseBlocks(nodes) {
    for (const x of nodes) {
        if (x.kind !== "block") continue;
        if (x.body.some((y) => y.kind === "pp" && (y.word === "elif" || y.word === "else"))) {
            const names = flatDecls(x).map((d) => d.name).filter((n) => symbols.has(n) && assign.get(n) !== "H");
            const files = names.map((n) => assign.get(n));
            const fset = new Set(files);
            if (fset.size > 1) {
                const key = (f) => [files.filter((g) => g === f).length, f === dflt ? 1 : 0];
                let win = null;
                for (const f of fset) {
                    if (win === null) { win = f; continue; }
                    const a = key(f), b = key(win);
                    if (a[0] > b[0] || (a[0] === b[0] && a[1] > b[1])) win = f;
                }
                for (const n of names) assign.set(n, win);
            }
        } else unifyElseBlocks(x.body);
    }
}
unifyElseBlocks(tree);

const rawFile = spec.raw_file, rawGuard = spec.raw_guard;
const rawPs = new Set();
function* allPp(nodes) {
    for (const x of nodes) {
        if (x.kind === "pp") yield x;
        else if (x.kind === "block") yield* allPp(x.body);
    }
}
const stripPp = (before) => before.replace(/^[ \t]*#(?:[^\n]*\\\n)*[^\n]*/gm, "");
if (rawFile) {
    for (const x of allPp(tree)) {
        const mdef = x.word === "define" ? new RegExp("^" + WS + "*#" + WS + "*define" + WS + "+(\\w+)").exec(x.text) : null;
        if (!mdef) continue;
        const nm = mdef[1], before = tb.slice(0, x.beg), code = stripPp(before);
        if (!new RegExp("\\b" + esc(nm) + "\\b").test(code) || new RegExp("#" + WS + "*define" + WS + "+" + esc(nm) + "\\b").test(before)) continue;
        rawPs.add(x);
        const pat = new RegExp("\\b" + esc(nm) + "\\b");
        for (const d of decls)
            if (d.end <= x.beg && symbols.has(d.name) && assign.get(d.name) !== "H" && pat.test(tb.slice(d.beg, d.end))) assign.set(d.name, rawFile);
    }
}
function originTarget(off) {
    const o = originOf(off);
    for (const [r, f] of originRules) if (r.test(o)) return f;
    return dflt;
}
function ppMacro(x, word) {
    if (x.kind !== "pp" || x.word !== word) return null;
    const m = new RegExp("^" + WS + "*#" + WS + "*\\w+" + WS + "+(\\w+)").exec(x.text);
    return m ? m[1] : null;
}
function positionSensitive(x) {
    const name = ppMacro(x, "define");
    if (!name) return false;
    const before = tb.slice(0, x.beg), code = stripPp(before);
    return new RegExp("\\b" + esc(name) + "\\b").test(code) && !new RegExp("#" + WS + "*define" + WS + "+" + esc(name) + "\\b").test(before);
}
function pragmaOnlyBlock(x) {
    if (x.kind !== "block") return false;
    const inner = x.body.slice(1, -1);
    return inner.length > 0 && inner.every((y) => y.kind === "pp" && y.word === "pragma");
}
const CODE_RE = /^[A-Za-z_][^;#\n]*\)[ \t\n\r\f\v]*\n?\{|^static[ \t\n\r\f\v]+(?!inline)[^(;\n]*[;=]/m;
const IDEF_RE = /^((?:static[ \t\n\r\f\v]+)?(?:inline[ \t\n\r\f\v]+)?[A-Za-z_][\w \t*]*?\b(\w+)[ \t\n\r\f\v]*\([^;{}]*\))[ \t\n\r\f\v]*\n?\{/gm;

function itemTargets(x) {
    const ds = flatDecls(x);
    if (pragmaOnlyBlock(x)) return new Set([originTarget(x.beg)]);
    if (x.kind === "block" && !ds.length && CODE_RE.test(tb.slice(x.beg, x.end))) return new Set(["@near"]);
    if (x.kind === "pp") {
        if (rawPs.has(x)) return new Set(["H"]);
        if (positionSensitive(x)) return new Set([originTarget(x.beg)]);
        return new Set(["H"]);
    }
    const tg = new Map();
    const add = (k, t) => tg.set(k + "\u0000" + t, [k, t]);
    for (const d of ds) {
        if (symbols.has(d.name) && (d.kind === "FunctionDecl" || d.kind === "VarDecl")) {
            if (d.kind === "FunctionDecl" && d.body === null) add("P", d.storage === "static" ? assign.get(d.name) : "H");
            else add("D", assign.get(d.name));
        } else if (d.kind === "FunctionDecl" && d.body === null) add("P", "H");
        else if ((d.kind === "RecordDecl" || d.kind === "EnumDecl") && !d.name && ds.length > 1) continue;
        else add("D", "H");
    }
    const vals = [...tg.values()];
    let files = new Set(vals.filter(([k]) => k === "D").map(([, t]) => t));
    if (!files.size) files = new Set(vals.filter(([k]) => k === "P").map(([, t]) => t));
    return files.size ? files : new Set(["H"]);
}
function itemTarget(x) {
    const files = itemTargets(x);
    if (files.size === 2 && files.has("H") && x.kind === "block") return [...files].find((f) => f !== "H");
    if (files.size > 1) {
        const names = flatDecls(x).map((d) => d.name);
        die(`one item spans several outputs [${sorted(files).map(pyRepr).join(", ")}]: [${names.slice(0, 12).map(pyRepr).join(", ")}]`);
    }
    return [...files][0];
}
function inElse(x, name) {
    const els = x.body.filter((y) => y.kind === "pp" && (y.word === "elif" || y.word === "else"));
    if (!els.length) return false;
    return new RegExp("\\b" + esc(name) + "\\b").test(tb.slice(els[0].beg, x.end));
}
const hasElse = (x) => x.body.some((y) => y.kind === "pp" && (y.word === "elif" || y.word === "else"));
function flatten(nodes, conds) {
    const res = [];
    for (const x of nodes) {
        if (x.kind === "block" && itemTargets(x).size > 1 && !hasElse(x)) res.push(...flatten(x.body.slice(0, -1), [...conds, strip(x.parts[0].text)]));
        else res.push([x, conds.slice()]);
    }
    return res;
}
function pragmaKind(x) {
    if (x.kind !== "pp" || x.word !== "pragma") return null;
    const m = new RegExp("^" + WS + "*#" + WS + "*pragma" + WS + "+(?:GCC|clang)" + WS + "+diagnostic" + WS + "+(push|pop)\\b").exec(x.text);
    return m ? m[1] : null;
}

const pragmaWrap = new Map();
function localPragmas(flat, targets) {
    pragmaWrap.clear();
    const stack = [];
    for (let i = 0; i < flat.length; i++) {
        const k = pragmaKind(flat[i][0]);
        if (k === "push") stack.push(i);
        else if (k === "pop") {
            if (!stack.length) die("unbalanced #pragma diagnostic pop");
            const a = stack.pop();
            const inner = new Set();
            for (let j = a + 1; j < i; j++) if (flat[j][0].kind !== "pp") inner.add(targets[j]);
            if (inner.size > 1) {
                const pre = [];
                for (let j = a; j < i; j++) { if (flat[j][0].kind !== "pp") break; pre.push(strip(flat[j][0].text)); }
                let other = false;
                for (let j = a; j < i; j++) if (flat[j][0].kind === "pp" && j >= a + pre.length) other = true;
                if (!pre.every((p) => p.startsWith("#pragma")) || other)
                    die(`a #pragma diagnostic region spans outputs [${sorted(inner).map(pyRepr).join(", ")}] and holds other directives`);
                const post = strip(flat[i][0].text);
                for (let j = a; j <= i; j++) {
                    if (flat[j][0].kind === "pp") targets[j] = "@drop";
                    else pragmaWrap.set(j, [pre.join("\n"), post]);
                }
                continue;
            }
            if (inner.size) {
                const t = [...inner][0];
                for (let j = a; j <= i; j++) if (flat[j][0].kind === "pp") targets[j] = t;
            }
        }
    }
    if (stack.length) die("unbalanced #pragma diagnostic push");
}
function localMacros(flat, targets) {
    for (let i = 0; i < flat.length; i++) {
        const name = ppMacro(flat[i][0], "define");
        if (!name) continue;
        for (let k = i + 1; k < flat.length; k++) {
            if (ppMacro(flat[k][0], "undef") !== name) continue;
            const inner = new Set();
            for (let j = i + 1; j < k; j++) if (flat[j][0].kind !== "pp") inner.add(targets[j]);
            if (inner.size === 1) {
                const t = [...inner][0];
                targets[i] = t; targets[k] = t;
                for (let j = i + 1; j < k; j++) if (flat[j][0].kind === "pp") targets[j] = t;
            } else if (inner.size > 1 && targets[i] === "H") {
                for (let q = k + 1; q < flat.length; q++)
                    if (ppMacro(flat[q][0], "define") === name) die(`macro ${name} is #undef'd between uses in several outputs and defined again`);
                targets[k] = "@drop";
            }
            break;
        }
    }
}

const inlineMark = new Set();
const inlineLines = parseInt(spec.inline_lines || 0, 10);
const fnLines = (d) => count(tb.slice(d.beg, d.end), "\n") + 1;
let flat, targets, exported, userFiles;
function plan() {
    flat = flatten(tree, []);
    userFiles = new Map([...symbols].map((n) => [n, new Set()]));
    targets = flat.map(([x]) => itemTarget(x));
    for (let i = 0; i < targets.length; i++) {
        if (targets[i] !== "@near") continue;
        const skip = (t) => t !== "H" && t !== "@near" && t !== "@drop";
        let prev = null, nxt = null;
        for (let j = i - 1; j >= 0; j--) if (skip(targets[j])) { prev = targets[j]; break; }
        for (let j = i + 1; j < targets.length; j++) if (skip(targets[j])) { nxt = targets[j]; break; }
        targets[i] = prev ?? nxt ?? dflt;
    }
    flat.forEach(([x], i) => {
        const t = targets[i];
        for (const d of flatDecls(x)) for (const r of d.refs) if (symbols.has(r)) userFiles.get(r).add(t);
        if (x.kind === "block" || t === "H")
            for (const w of new Set(findall(IDENT_G, tb.slice(x.beg, x.end)))) if (symbols.has(w)) userFiles.get(w).add(t);
    });
    localMacros(flat, targets);
    localPragmas(flat, targets);
    exported = new Set();
    for (const name of symbols) for (const f of userFiles.get(name)) if (f !== assign.get(name)) exported.add(name);
}

const topSingle = new Set();
for (const x of tree) if (x.kind === "decl" && x.decls.length === 1) topSingle.add(x.decls[0].name);
plan();
const addrTaken = new Set();
for (const d of decls) for (const v of d.vrefs) addrTaken.add(v);
const inlineFrom = spec.inline_from || [dflt];
while (inlineLines) {
    const cand = [...exported].filter((n) => {
        const d = fnDefs.get(n);
        return d && d.storage === "static" && assign.get(n) !== "H" && !addrTaken.has(n)
            && !/\bno_?inline\b/.test(tb.slice(d.beg, d.body ?? 0))
            && topSingle.has(n) && fnLines(d) <= inlineLines && inlineFrom.includes(assign.get(n))
            && d.body !== null && d.body > d.beg;
    });
    if (!cand.length) break;
    for (const n of cand) { assign.set(n, "H"); inlineMark.add(n); }
    plan();
}

const thunkMacro = spec.core_tu_macro;
const thunked = new Set();
if (thunkMacro) {
    const vrefElsewhere = new Set(), inBlock = new Set(), coreUsed = new Set();
    flat.forEach(([x], i) => {
        const t = targets[i];
        for (const d of flatDecls(x)) {
            if (t !== dflt) for (const v of d.vrefs) vrefElsewhere.add(v);
            else if (d.body !== null || d.kind === "VarDecl") for (const r of d.refs) if (r !== d.name) coreUsed.add(r);
            if (x.kind === "block") inBlock.add(d.name);
        }
    });
    for (const n of exported) {
        const d = fnDefs.get(n);
        if (d === undefined || assign.get(n) !== dflt || d.storage !== "static" || d.inline
            || d.qual.includes("...") || (!d.params.length && !d.qual.endsWith("(void)"))
            || d.params.some((p) => !p) || vrefElsewhere.has(n) || inBlock.has(n) || !coreUsed.has(n) || d.body === null) continue;
        thunked.add(n);
    }
}

const srcOf = (a, b) => tb.slice(a, b);
function stripStatic(s) {
    const m = new RegExp("^(" + WS + "*(?:__attribute__\\(\\([\\s\\S]*?\\)\\)" + WS + "*)*)static" + WS + "+").exec(s);
    if (!m) return s;
    return m[1] + s.slice(m[0].length);
}
function thunkExtHead(d) {
    let head = stripStatic(rstrip(srcOf(d.beg, d.body)));
    head = head.replace(new RegExp("\\b(no_inline|__attribute__\\(\\(noinline\\)\\))" + WS + "+", "g"), "");
    const re = new RegExp("\\b" + esc(d.name) + "(" + WS + "*\\()");
    if (!re.test(head)) die("cannot name the thunk of " + d.name);
    return head.replace(re, (_, p) => d.name + "__ext" + p);
}
const thunkHeader = (d) => `#ifdef ${thunkMacro}\n${rstrip(srcOf(d.beg, d.body)) + ";"}\n#else\n#define ${d.name} ${d.name}__ext\n#endif\n${thunkExtHead(d)};`;
function thunkDef(d) {
    const isVoid = d.qual.startsWith("void (");
    return `${thunkExtHead(d)}\n{\n    ${isVoid ? "" : "return "}${d.name}(${d.params.join(", ")});\n}`;
}
const makeInline = (txt) => txt.replace(new RegExp("^(" + WS + "*)static" + WS + "+(?!inline)"), (_, a) => a + "static inline ");
function joinPlain(parts) {
    let res = "", prevPp = false;
    for (let s of parts) {
        s = rstrip(s);
        const pp = lstrip(s).startsWith("#") && !s.includes("\n");
        if (res) res += pp && prevPp ? "\n" : "\n\n";
        res += s;
        prevPp = pp;
    }
    return res;
}
const sameConds = (a, b) => a.length === b.length && a.every((c, k) => c === b[k]);
function joinParts(parts) {
    const groups = [];
    for (const [conds, s] of parts) {
        const g = groups[groups.length - 1];
        if (g && sameConds(g[0], conds)) g[1].push(s); else groups.push([conds, [s]]);
    }
    const chunks = groups.map(([conds, ss]) => {
        let body = joinPlain(ss);
        for (const c of [...conds].reverse()) body = c + "\n" + body + "\n#endif";
        return body;
    });
    return joinPlain(chunks);
}
const protoOf = (d) => stripStatic(rstrip(srcOf(d.beg, d.body))) + ";";
function tlsOf(d) {
    const m = /\b(_Thread_local|__thread|thread_local)\b/.exec(tb.slice(d.beg, d.end));
    return m ? m[1] + " " : "";
}
function anonSplit(d) {
    if (d.kind !== "VarDecl" || !d.qual.includes("(unnamed")) return null;
    const t = srcOf(d.beg, d.end);
    const m = new RegExp("^" + WS + "*(static" + WS + "+)?((?:(?:const|volatile|_Thread_local)" + WS + "+)*)(struct|union)" + WS + "*\\{").exec(t);
    if (!m) return null;
    let i = m[0].length, depth = 1;
    while (depth && i < t.length) { depth += t[i] === "{" ? 1 : t[i] === "}" ? -1 : 0; i++; }
    const body = t.slice(m[0].length, i - 1), rest = t.slice(i), tag = d.name + "_s";
    const declPart = rstrip(rest.split(/[=;]/)[0]);
    return [`${m[3]} ${tag} {${body}};`, `extern ${m[2]}${m[3]} ${tag}${declPart};`, `${m[1] || ""}${m[2]}${m[3]} ${tag}${rest}`];
}
const anonSdef = (a) => a[0], anonExt = (a) => a[1], anonNamed = (a) => a[2];
function externOf(d) {
    const a = anonSplit(d);
    if (a) return anonExt(a);
    const q = d.qual;
    const noAtomic = q.replace(/_Atomic\([^()]*\)/g, "");
    let m = new RegExp("\\(\\*" + WS + "*((?:const|volatile|restrict)" + WS + "*)*((?:\\[\\d*\\])*)\\)").exec(q);
    if (m && noAtomic.includes("(")) {
        const quals = strip(m[1] || "");
        const inner = `(*${quals ? quals + " " : ""}${d.name}${m[2] || ""})`;
        return `extern ${q.slice(0, m.index) + inner + q.slice(m.index + m[0].length)};`;
    }
    if (noAtomic.includes("(")) die(`cannot write an extern for ${d.name} of type ${q}`);
    m = new RegExp("^([\\s\\S]*?)" + WS + "*((?:\\[\\d*\\])+)$").exec(q);
    if (m) return `extern ${tlsOf(d)}${m[1]} ${d.name}${m[2]};`;
    return `extern ${tlsOf(d)}${q}${q.endsWith("*") ? "" : " "}${d.name};`;
}
function renderDeclItem(x, tgt) {
    const ds = x.decls;
    let txt = srcOf(x.beg, x.end);
    for (const d of ds) {
        if (exported.has(d.name) && anonSplit(d)) {
            const a = anonSplit(d), vt = srcOf(d.beg, d.end);
            const at = txt.indexOf(vt);
            txt = at >= 0 ? txt.slice(0, at) + anonNamed(a) + txt.slice(at + vt.length) : anonNamed(a);
        }
    }
    const expHere = ds.filter((d) => exported.has(d.name) && !thunked.has(d.name) && (d.kind === "FunctionDecl" || d.kind === "VarDecl"));
    if (tgt !== "H" && expHere.length) {
        if (ds.every((d) => d.kind === "FunctionDecl" && d.body === null)) return null;
        txt = stripStatic(txt);
    }
    return txt;
}

const inactiveExp = new Map();
const texts = flat.map(([x], i) => [tb.slice(x.beg, x.end), targets[i]]);
flat.forEach(([x], idx) => {
    const t = targets[idx];
    if (x.kind !== "block" || t === "H" || t === "@drop") return;
    const bt = tb.slice(x.beg, x.end);
    for (const m of bt.matchAll(IDEF_RE)) {
        const nm = m[2];
        if (fnDefs.has(nm) || ["if", "while", "for", "switch", "return", "sizeof"].includes(nm)) continue;
        const pat = new RegExp("\\b" + esc(nm) + "\\b");
        if (texts.some(([tx, tt]) => tt !== t && pat.test(tx))) {
            if (!inactiveExp.has(idx)) inactiveExp.set(idx, []);
            inactiveExp.get(idx).push([nm, m.index, m[1]]);
        }
    }
});

const out = new Map([[hdrName, []]]);
const outOrg = new Map();
let curOrigin = null, curWrap = null;
function put(f, conds, txt) {
    if (!outOrg.has(f)) outOrg.set(f, []);
    outOrg.get(f).push(curOrigin);
    if (curWrap) txt = curWrap[0] + "\n" + strip(txt, "\n") + "\n" + curWrap[1];
    if (!out.has(f)) out.set(f, []);
    out.get(f).push([conds, txt]);
}
function putExtern(conds, d) {
    const a = anonSplit(d);
    if (a) put(hdrName, conds, anonSdef(a));
    put(hdrName, conds, externOf(d));
}
function inactiveHeader(x, conds, defs) {
    const seps = x.body.filter((y) => y.kind === "pp" && (y.word === "elif" || y.word === "else"));
    const branches = Array.from({ length: seps.length + 1 }, () => []);
    for (const [, off, head] of defs) {
        const k = seps.filter((y) => y.beg < x.beg + off).length;
        branches[k].push(strip(stripStatic(head)) + ";");
    }
    const lines = [strip(x.parts[0].text), ...branches[0]];
    seps.forEach((y, k) => lines.push(strip(y.text), ...branches[k + 1]));
    lines.push("#endif");
    put(hdrName, conds, lines.join("\n"));
}
const outsideTxt = new Set();
for (const x of tree) if (outsideIds.has(x)) { outsideTxt.add(rstrip(srcOf(x.beg, x.end), "\n")); outsideTxt.add(srcOf(x.beg, x.end)); }
const seenProto = new Set();

flat.forEach(([x, conds], idx) => {
    const tgt = targets[idx];
    curOrigin = originOf(x.beg);
    curWrap = pragmaWrap.get(idx) || null;
    if (tgt === "@drop") return;
    if (x.kind === "decl") {
        const ds = x.decls;
        if (ds.every((d) => d.kind === "FunctionDecl" && d.body === null)) {
            for (const d of ds) {
                const nm = d.name;
                if (thunked.has(nm)) {
                    if (!seenProto.has(nm)) { put(hdrName, conds, thunkHeader(fnDefs.get(nm))); seenProto.add(nm); }
                    put(assign.get(nm), conds, srcOf(x.beg, x.end));
                    continue;
                }
                if (exported.has(nm) || assign.get(nm) === "H" || !symbols.has(nm)) {
                    if (!seenProto.has(nm)) {
                        let txt = srcOf(x.beg, x.end);
                        if (inlineMark.has(nm)) txt = makeInline(txt);
                        else if (!(assign.get(nm) === "H" && fnDefs.get(nm)?.inline)) txt = stripStatic(txt);
                        put(hdrName, conds, txt);
                        seenProto.add(nm);
                    }
                } else put(assign.get(nm), conds, srcOf(x.beg, x.end));
            }
            return;
        }
        for (const d of ds) {
            if (thunked.has(d.name) && !seenProto.has(d.name)) { put(hdrName, conds, thunkHeader(fnDefs.get(d.name))); seenProto.add(d.name); }
            else if (exported.has(d.name) && tgt !== "H" && !seenProto.has(d.name)) {
                if (d.kind === "FunctionDecl" && d.body !== null) put(hdrName, conds, protoOf(d));
                else if (d.kind === "VarDecl") putExtern(conds, d);
                seenProto.add(d.name);
            }
        }
        let txt = renderDeclItem(x, tgt);
        if (txt !== null) {
            if (tgt === "H" && ds.some((d) => inlineMark.has(d.name))) txt = makeInline(txt);
            put(tgt !== "H" ? tgt : hdrName, conds, txt);
            for (const d of ds) if (thunked.has(d.name) && d.body !== null) put(tgt, conds, thunkDef(d));
        }
        return;
    }
    let txt = rstrip(srcOf(x.beg, x.end), "\n");
    if (rawPs.has(x) && tgt === "H") txt = `#ifndef ${rawGuard}\n${txt}\n#endif`;
    if (tgt === "H") {
        for (const d of flatDecls(x)) if (exported.has(d.name) && !seenProto.has(d.name)) seenProto.add(d.name);
        put(hdrName, conds, txt);
        return;
    }
    for (const d of flatDecls(x)) {
        if (!exported.has(d.name) || seenProto.has(d.name)) continue;
        const bc = x.kind === "block" && !inElse(x, d.name) ? [...conds, strip(x.parts[0].text)] : conds;
        if (d.kind === "FunctionDecl" && d.body !== null) put(hdrName, bc, protoOf(d));
        else if (d.kind === "FunctionDecl" && fnDefs.has(d.name)) put(hdrName, bc, protoOf(fnDefs.get(d.name)));
        else if (d.kind === "VarDecl") putExtern(bc, d);
        seenProto.add(d.name);
    }
    for (const d of flatDecls(x)) {
        if (exported.has(d.name) && anonSplit(d)) {
            const vt = srcOf(d.beg, d.end), at = txt.indexOf(vt);
            if (at >= 0) txt = txt.slice(0, at) + anonNamed(anonSplit(d)) + txt.slice(at + vt.length);
        }
    }
    for (const nm of sorted(exported))
        if (assign.has(nm) && assign.get(nm) === tgt)
            txt = txt.replace(new RegExp("^static" + WS + "+((?:[\\w*]+" + WS + "+\\**)+?" + esc(nm) + WS + "*[(\\[=;,])", "gm"), "$1");
    if (inactiveExp.has(idx)) {
        for (const [nm] of inactiveExp.get(idx))
            txt = txt.replace(new RegExp("^static" + WS + "+((?:[\\w*]+" + WS + "+\\**)+?" + esc(nm) + WS + "*\\()", "gm"), "$1");
        inactiveHeader(x, conds, inactiveExp.get(idx));
    }
    put(tgt, conds, txt);
});

function partNames(t) {
    const st = lstrip(t);
    if (st.startsWith("#")) {
        const m = new RegExp("^#" + WS + "*define" + WS + "+(\\w+)").exec(st);
        return m ? new Set([m[1]]) : new Set();
    }
    const names = new Set();
    const flatT = t.replace(/\{[\s\S]*\}/, "{}");
    let m = new RegExp("^" + WS + "*(?:typedef" + WS + "+)?(?:struct|union|enum)" + WS + "+(\\w+)").exec(flatT);
    if (m) names.add(m[1]);
    if (lstrip(flatT).startsWith("typedef")) {
        const m2 = new RegExp("\\(" + WS + "*\\*" + WS + "*(\\w+)" + WS + "*\\)").exec(flatT);
        if (m2) names.add(m2[1]);
        else { const ids = findall(IDENT_G, flatT); if (ids.length) names.add(ids[ids.length - 1]); }
    } else if (flatT.includes("(") && !["struct", "union", "enum", "_Static_assert", "extern"].some((p) => lstrip(flatT).startsWith(p))) {
        const ids = findall(IDENT_G, flatT.split("(")[0]);
        if (ids.length) names.add(ids[ids.length - 1]);
    }
    if (new RegExp("^" + WS + "*enum\\b").test(t))
        for (const mm of t.matchAll(new RegExp("\\b([A-Z][A-Z0-9_]+)" + WS + "*(?:=|,|\\})", "g"))) names.add(mm[1]);
    return names;
}

function globSorted(pattern) {
    const r = Exec("sh", ["-c", `for f in ${pattern}; do [ -e "$f" ] && printf '%s\\n' "$f"; done`], { encoding: "utf8" });
    return String(r.stdout).split("\n").filter(Boolean).sort(pyCmp);
}
const TYPEDEF_ALIAS = new RegExp("typedef" + WS + "+(?:struct|union|enum)" + WS + "+(\\w+)" + WS + "+(\\w+)" + WS + "*;", "g");
const alias = new Map();
for (const p of (spec.alias_headers || []).flatMap(globSorted))
    for (const m of read(p).matchAll(TYPEDEF_ALIAS)) { if (!alias.has(m[1])) alias.set(m[1], new Set()); alias.get(m[1]).add(m[2]); }

function localise() {
    const hparts = out.get(hdrName);
    const fileIds = new Map();
    for (const [f, ps] of out) if (f !== hdrName) fileIds.set(f, new Set(findall(IDENT_G, ps.map(([, t]) => t).join("\n"))));
    const cand = new Map();
    hparts.forEach(([, t], idx) => {
        const st = lstrip(t);
        if (st.startsWith("#") && !new RegExp("^#" + WS + "*define\\b").test(st)) return;
        if (outsideTxt.has(t) || new RegExp("^" + WS + "*(extern\\b|_Static_assert)").test(t)) return;
        if (!new RegExp("^" + WS + "*(#|typedef|struct|union|enum|static inline|static __inline)").test(t)) return;
        const names = partNames(t);
        for (const n of [...names]) for (const a of alias.get(n) || []) names.add(a);
        for (const m of t.matchAll(TYPEDEF_ALIAS)) { names.add(m[1]); names.add(m[2]); }
        if (names.size) cand.set(idx, names);
    });
    const partIds = hparts.map(([, t]) => new Set(findall(IDENT_G, t)));
    const meets = (a, b) => { for (const v of a) if (b.has(v)) return true; return false; };
    const cons = new Map();
    for (const [idx, names] of cand) cons.set(idx, new Set([...fileIds].filter(([, ids]) => meets(names, ids)).map(([f]) => f)));
    const pinned = new Set();
    for (const [idx, names] of cand) partIds.forEach((ids, j2) => { if (j2 !== idx && !cand.has(j2) && meets(names, ids)) pinned.add(idx); });
    const subset = (a, b) => [...a].every((v) => b.has(v));
    for (let changed = true; changed;) {
        changed = false;
        for (const [idx, names] of cand) {
            for (const j2 of cand.keys()) {
                if (j2 !== idx && meets(names, partIds[j2]) && !subset(cons.get(j2), cons.get(idx))) {
                    for (const v of cons.get(j2)) cons.get(idx).add(v);
                    changed = true;
                }
                if (j2 !== idx && meets(names, partIds[j2]) && pinned.has(j2) && !pinned.has(idx)) { pinned.add(idx); changed = true; }
            }
        }
    }
    const moving = new Set([...cand.keys()].filter((idx) => cons.get(idx).size === 1 && !pinned.has(idx)));
    for (let changed = true; changed;) {
        changed = false;
        partIds.forEach((ids, j2) => {
            if (moving.has(j2)) return;
            for (const idx of [...moving]) if (idx !== j2 && meets(cand.get(idx), ids)) { moving.delete(idx); changed = true; }
        });
    }
    const mv = [...moving].sort((a, b) => a - b);
    const perFile = new Map();
    for (const idx of mv) {
        const f = [...cons.get(idx)][0];
        if (!perFile.has(f)) perFile.set(f, []);
        perFile.get(f).push(hparts[idx]);
    }
    for (const [f, ps] of perFile) {
        out.get(f).unshift(...ps);
        if (!outOrg.has(f)) outOrg.set(f, []);
        outOrg.get(f).unshift(...ps.map(() => null));
    }
    for (const idx of mv.reverse()) hparts.splice(idx, 1);
    if (std.getenv("SPLIT_DEBUG")) std.err.puts(`localised ${mv.length} header parts\n`);
}
if (spec.localise ?? true) localise();

const guardId = hdrName.replace(/[^A-Za-z0-9]/g, "_").toUpperCase();
let hl = ["#ifndef " + guardId, "#define " + guardId, ""];
if (guard) {
    const hp = out.get(hdrName);
    hl.push(...hp.filter(([, s]) => outsideTxt.has(s)).map(([, s]) => strip(s)));
    hl.push("", strip(guard), "", joinParts(hp.filter(([, s]) => !outsideTxt.has(s))), "", "#endif", "");
} else hl.push(joinParts(out.get(hdrName)), "");
hl.push("#endif");
const seenInc = new Set(), hl2 = [];
for (const ln of hl.join("\n").split("\n")) {
    if (new RegExp("^" + WS + "*#" + WS + "*include" + WS + "+\"").test(ln)) {
        if (seenInc.has(strip(ln))) continue;
        seenInc.add(strip(ln));
    }
    hl2.push(ln);
}
const files = new Map([[hdrName, hl2.join("\n") + "\n"]]);
const keepFrag = spec.keep_fragments ? new RegExp(spec.keep_fragments) : null;
const spelledOf = new Map((spec.inline || []).map((i) => (Array.isArray(i) ? [i[1], i[0]] : [i, basename(i)])));
const fragFiles = new Map();
function fragBody(f, parts) {
    const orgs = outOrg.get(f) || parts.map(() => null);
    if (keepFrag === null || orgs.length !== parts.length) return joinParts(parts);
    const runs = [];
    parts.forEach((part, k) => {
        const o = orgs[k], key = o && keepFrag.test(o) ? o : null;
        const last = runs[runs.length - 1];
        if (last && last[0] === key) last[1].push(part); else runs.push([key, [part]]);
    });
    const chunks = [];
    for (const [key, ps] of runs) {
        if (key === null) { chunks.push(joinParts(ps)); continue; }
        if (fragFiles.has(key)) die(`fragment ${key} is not contiguous in ${f}`);
        fragFiles.set(key, joinParts(ps) + "\n");
        chunks.push(`#include "${spelledOf.get(key) ?? key}"`);
    }
    return joinPlain(chunks);
}
for (const [f, parts] of out) {
    if (f === hdrName) continue;
    const body = fragBody(f, parts);
    let t = `#include "${hdrName}"\n`;
    if (thunkMacro && f === dflt) t = `#define ${thunkMacro}\n${t}`;
    if (rawFile && f === rawFile) t = `#define ${rawGuard}\n${t}`;
    if (guard) t += "\n" + strip(guard) + "\n\n" + body + (guardElse && f === dflt ? "\n\n" + guardElse : "") + "\n\n#endif\n";
    else t += "\n" + body + "\n";
    files.set(f, t);
}

const pyLines = (s) => { const a = s.split(/\r\n|[\n\r\v\f\x1c\x1d\x1e\x85\u2028\u2029]/); if (a[a.length - 1] === "") a.pop(); return a.length; };
const pyJson = (v, ind) => {
    const pad = (n) => "\n" + " ".repeat(n);
    const go = (x, d) => {
        if (Array.isArray(x)) return x.length ? "[" + x.map((y) => pad(d + ind) + go(y, d + ind)).join(",") + pad(d) + "]" : "[]";
        if (x && typeof x === "object") { const ks = Object.keys(x); return ks.length ? "{" + ks.map((k) => pad(d + ind) + pyStr(k) + ": " + go(x[k], d + ind)).join(",") + pad(d) + "}" : "{}"; }
        return typeof x === "string" ? pyStr(x) : JSON.stringify(x);
    };
    return go(v, 0);
};
const pyStr = (s) => JSON.stringify(s).replace(/[\u0080-\uffff]/g, (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"));
const report = {
    exported: sorted(exported),
    files: Object.fromEntries([...files].map(([f, v]) => [f, pyLines(v)])),
    assign: Object.fromEntries(sorted(assign.keys()).map((k) => [k, assign.get(k)])),
};
write(work + "/report.json", pyJson(report, 1));
for (const f of sorted(Object.keys(report.files))) print(String(report.files[f]).padStart(6) + "  " + f);
print("exported: " + exported.size);
if (dry) std.exit(0);
mkdirs(outdir);
for (const [f, t] of files) write(outdir + "/" + f, t);
for (const [f, t] of fragFiles) write(f, t);
