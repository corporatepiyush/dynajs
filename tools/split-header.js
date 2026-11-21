// flags: --std
// split-header.js -- split a public C header into one header per component, keeping the old path as
// an umbrella that includes them all, so no consumer has to change.
//
// usage: ./dynajs --std tools/split-header.js SPEC.json [--dry]
//
// SPEC:
//   {
//     "header":  "src/core/dyn-ds.h",       the header to split (rewritten as the umbrella)
//     "outdir":  "src/core/ds",
//     "umbrella_prefix": "ds/",              how the umbrella names the parts, relative to itself
//     "common":  "ds-common.h",              declarations no rule claims
//     "rules":   [["^(dyn|DYN)_(bitset|BITSET)_", "ds-bitset.h"], ...]   first match on the declared name
//   }
//
// A top-level unit is a preprocessor line, or a declaration running to the `;` (or the closing `}` of an
// inline function) at brace depth 0. Its declared name is the macro name, the identifier before the first
// `(` of a function, or the last identifier of a typedef. An #if block without #else is distributed: each
// unit keeps the condition in the header it lands in. Each part includes the parts whose names it uses.
// Port of tools/split-header.py; output is byte-identical to it (checked on dyn-codec.h, dyn-ds.h and
// dyn-mathx.h with prefix rules before the .py was removed).
import * as std from "std";
import * as os from "os";

const die = (m) => { std.err.puts("split-header: " + m + "\n"); std.exit(1); };
const argv = scriptArgs.slice(1);
if (!argv[0]) die("usage: split-header.js SPEC.json [--dry]");
const read = (p) => { const t = std.loadFile(p); if (t === null) die("cannot read " + p); return t; };
const write = (p, t) => { const f = std.open(p, "w"); if (!f) die("cannot write " + p); f.puts(t); f.close(); };

const spec = JSON.parse(read(argv[0]));
const dry = argv.includes("--dry");
const src = spec.header, outdir = spec.outdir, common = spec.common;
const rules = spec.rules.map(([r, f]) => [new RegExp(r), f]);
let lines = read(src).split("\n");

let i0 = 0;
while (i0 < lines.length && !lines[i0].trim()) i0++;
if (!(lines[i0].startsWith("#ifndef") && lines[i0 + 1].startsWith("#define"))) die("no include guard");
let iEnd = lines.length - 1;
while (!lines[iEnd].trim()) iEnd--;
if (lines[iEnd].trim() !== "#endif") die("guard does not close at the end");
let text = lines.slice(i0 + 2, iEnd).join("\n");
text = text.replace(/#ifdef __cplusplus\nextern "C" \{\n#endif\n?/g, "").replace(/#ifdef __cplusplus\n\}\n#endif\n?/g, "");
const body = text.split("\n");

const units = [], conds = [];
for (let i = 0; i < body.length;) {
    const s = body[i].trim();
    if (!s) { i++; continue; }
    if (s.startsWith("#")) {
        let j = i;
        while (body[j].trimEnd().endsWith("\\")) j++;
        const txt = body.slice(i, j + 1).join("\n");
        const word = /^#\s*(\w+)/.exec(s)[1];
        if (word === "if" || word === "ifdef" || word === "ifndef") conds.push(txt);
        else if (word === "endif") { if (!conds.length) die("unbalanced #endif"); conds.pop(); }
        else if (word === "else" || word === "elif") die("#else inside the header is not handled");
        else units.push({ text: txt, conds: conds.slice(), pp: word });
        i = j + 1;
        continue;
    }
    let depth = 0, j = i;
    const buf = [];
    while (j < body.length) {
        const cur = body[j];
        buf.push(cur);
        depth += cur.split("{").length - cur.split("}").length;
        const st = cur.trim();
        if (depth === 0 && (st.endsWith(";") || st === "}")) break;
        j++;
    }
    units.push({ text: buf.join("\n"), conds: conds.slice(), pp: null });
    i = j + 1;
}
if (conds.length) die("unbalanced #if");

const IDENT = /\b[A-Za-z_]\w*\b/g;
const idents = (t) => t.match(IDENT) || [];
const KEYWORDS = new Set(["typedef", "struct", "union", "enum", "const", "static", "inline", "void", "int", "unsigned",
    "char", "double", "float", "long", "short", "signed", "extern", "volatile", "return", "if", "else",
    "for", "while", "sizeof", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int64_t",
    "int32_t", "uintptr_t", "do", "break", "continue", "switch", "case", "default"]);

function declared(u) {
    const t = u.text;
    if (u.pp === "define" || u.pp === "undef") { const m = /^\s*#\s*\w+\s+(\w+)/.exec(t); return m ? [m[1]] : []; }
    if (u.pp) return [];
    const flat = t.trimStart().startsWith("typedef") ? t.replace(/\{[\s\S]*\}/, "{}") : t;
    if (flat.trimStart().startsWith("typedef")) {
        let m = /\(\s*\*\s*(\w+)\s*\)/.exec(flat);
        if (m) return [m[1]];
        const ids = idents(flat), names = [ids[ids.length - 1]];
        m = /^\s*typedef\s+(?:struct|union|enum)\s+(\w+)/.exec(flat);
        if (m) names.push(m[1]);
        return names;
    }
    const m = /^\s*(?:struct|union|enum)\s+(\w+)\s*\{/.exec(t);
    if (m) return [m[1]];
    const ids = idents(t.split("(")[0]);
    return ids.length ? [ids[ids.length - 1]] : [];
}
const ruleOf = (name) => { for (const [r, f] of rules) if (r.test(name)) return f; return null; };

for (const u of units) {
    u.names = declared(u);
    u.out = null;
    for (const n of u.names) { const f = ruleOf(n); if (f) { u.out = f; break; } }
    if (u.out === null && u.pp === "include") u.out = "@all";
    if (u.out === null) u.out = common;
}
const definedIn = new Map();
for (const u of units) {
    for (const n of u.names) if (!definedIn.has(n)) definedIn.set(n, u.out);
    for (const m of u.text.matchAll(/\bstruct\s+(\w+)\s*\{/g)) if (!definedIn.has(m[1])) definedIn.set(m[1], u.out);
}

let outs = [];
for (const u of units) if (u.out !== "@all" && !outs.includes(u.out)) outs.push(u.out);
outs = [common, ...outs.filter((o) => o !== common)];

const deps = new Map(outs.map((o) => [o, new Set()]));
for (const u of units) {
    if (u.out === "@all") continue;
    const own = new Set(u.names);
    for (const idn of new Set(idents(u.text))) {
        if (own.has(idn) || KEYWORDS.has(idn)) continue;
        const d = definedIn.get(idn);
        if (d && d !== u.out && d !== "@all") deps.get(u.out).add(d);
    }
}
for (const o of outs) if (o !== common) deps.get(o).add(common);

const order = [], state = new Map();
function visit(o, path) {
    if (state.get(o) === 1) die("include cycle: " + [...path, o].join(" -> "));
    if (state.get(o) === 2) return;
    state.set(o, 1);
    for (const d of [...deps.get(o)].sort()) visit(d, [...path, o]);
    state.set(o, 2);
    order.push(o);
}
for (const o of outs) visit(o, []);

const sysIncs = units.filter((u) => u.out === "@all").map((u) => u.text.trim());
const guardOf = (name) => name.replace(/[^A-Za-z0-9]/g, "_").toUpperCase();
const sameConds = (a, b) => a.length === b.length && a.every((c, k) => c === b[k]);

function render(o) {
    const groups = [];
    for (const u of units.filter((x) => x.out === o)) {
        const g = groups[groups.length - 1];
        if (g && sameConds(g[0], u.conds)) g[1].push(u);
        else groups.push([u.conds, [u]]);
    }
    const chunks = groups.map(([c, us]) => {
        let blk = "", prevPp = false;
        for (const u of us) {
            const pp = !!u.pp && !u.text.includes("\n");
            if (blk) blk += pp && prevPp ? "\n" : "\n\n";
            blk += u.text.trimEnd();
            prevPp = pp;
        }
        for (const cond of [...c].reverse()) blk = cond + "\n" + blk + "\n#endif";
        return blk;
    });
    const g = guardOf(o);
    const h = ["#ifndef " + g, "#define " + g, "", ...sysIncs];
    for (const d of order) if (deps.get(o).has(d)) h.push(`#include "${d}"`);
    h.push("", "#ifdef __cplusplus", 'extern "C" {', "#endif", "", chunks.join("\n\n"),
        "", "#ifdef __cplusplus", "}", "#endif", "", "#endif", "");
    return h.join("\n");
}

const files = new Map(outs.map((o) => [o, render(o)]));
const pre = spec.umbrella_prefix ?? "";
const ug = guardOf(src.split("/").pop());
const umb = ["#ifndef " + ug, "#define " + ug, "", ...order.map((o) => `#include "${pre}${o}"`), "", "#endif", ""];

for (const o of order) print(String(files.get(o).split("\n").length - 1).padStart(5) + "  " + o);
if (dry) std.exit(0);
os.mkdir(outdir, 0o755);
for (const [o, t] of files) write(outdir + "/" + o, t);
write(src, umb.join("\n"));
