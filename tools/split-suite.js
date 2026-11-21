// flags: --std
// Split a multi-symbol suite into one-symbol suites, moving code VERBATIM.
//
// A suite here is: a prelude (imports, helpers, shared fixtures), a run of
// top-level units -- `{ ... }` blocks, `test("..", ...)` / `section(...)` calls,
// or `(async () => {...})()` style IIFEs -- and a trailer (the summary print).
// Each unit is classified by the module symbols it calls; units are grouped by
// their primary symbol (the one called most in the unit, ties to the first
// called), and each group becomes
//
//     tests/<area>/test_<module>_<symbol>[_<aspect>].js
//     line 1..2: the original's // flags: / // timeout: headers
//     line 1..3: // covers: <module>.<Symbol>
//     the prelude, the group's units in their original order, the trailer
//
// so every assertion is moved, none rewritten (tools/check-assertions-preserved.js
// proves it). <aspect> is the original file's suffix after test_<module>_ (e.g.
// test_semver_ref.js -> _ref), so fragments of one symbol from several suites sit
// side by side under one name prefix instead of being merged across preludes that
// may not compose.
//
//   split-suite.js plan FILE [--module dyna:x]           units, symbols, grouping
//   split-suite.js header FILE CATEGORY "REASON" [--module dyna:x]
//       a suite that is ONE unit by design (a reference-vector table, an input
//       harness, a cross-module sweep) keeps its body and gets
//       // covers: CATEGORY <every module symbol it uses> -- REASON
//       (one symbol: a plain header); CATEGORY is integration|blackbox|example|contract
//   split-suite.js apply FILE [--module dyna:x] [--by-member CLASS] [--assign UNIT=SYMBOL ...] [--inherit] [--dry-run]
//       --by-member CLASS groups by the CLASS method each unit calls most
//       (a module that is one class: covers dyna:random.Random.nextInt)
//       --inherit gives a unit that calls no module symbol its predecessor's
//       writes the new files, deletes FILE and prints them (tests/suites.sh discovers suites by name,
//       so no list needs rewriting; SPLIT_LOG=PATH appends "FILE<TAB>NEW..." for a caller that keeps one)
//
// Top-level declarations between units travel with the unit that follows them
// (a fixture table declared just before the assertions that read it); if a later
// unit of ANOTHER group also reads it, that split file fails to run -- so every
// written file is run once by `apply` and the split is refused if one fails.
// Refuses (exit 1) when a unit cannot be placed (no symbol, no --assign and no
// --inherit), or when the trailer asserts on a running count.
import * as std from "std";
import * as os from "os";
import { Exec } from "dyna:sys";
import * as tcm from "./js/dts-model.js";

const ROOT = tcm.ROOT;
const out = (s) => std.out.puts(s + "\n");
const byKey = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const reEsc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const basename = (p) => p.slice(p.lastIndexOf("/") + 1);
const dirname = (p) => (p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : "");
const join = (a, b) => (a ? a + "/" + b : b);
const exists = (p) => os.stat(p)[1] === 0;
const word = (nm) => new RegExp("(?<![\\p{L}\\p{N}_$.])" + reEsc(nm) + "(?![\\p{L}\\p{N}_$])", "u");
const IDCH = /[\p{L}\p{N}_$]/u;

function readText(p) {
    const t = std.loadFile(p);
    if (t === null) throw new Error("cannot read " + p);
    return t;
}

function writeText(p, s) {
    const f = std.open(p, "w");
    f.puts(s);
    f.close();
}

let BY_MEMBER = null;
let INHERIT = false;
export function setInherit(v) { INHERIT = v; }

export function scanUnits(src) {
    let i = 0, depth = 0, stmtStart = null, lastSig = "";
    const n = src.length, units = [];
    const skipString = (j, q) => {
        j++;
        while (j < n && src[j] !== q) {
            if (src[j] === "\\") j++;
            else if (q === "`" && src.startsWith("${", j)) {
                let d = 1;
                j += 2;
                while (j < n && d) {
                    if (src[j] === "{") d++;
                    else if (src[j] === "}") d--;
                    else if ("\"'`".includes(src[j])) j = skipString(j, src[j]);
                    j++;
                }
                continue;
            }
            j++;
        }
        return j;
    };
    while (i < n) {
        const c = src[i];
        if (" \t\r\n".includes(c)) { i++; continue; }
        if (src.startsWith("//", i)) {
            const e = src.indexOf("\n", i);
            i = e === -1 ? n : e + 1;
            continue;
        }
        if (src.startsWith("/*", i)) {
            const e = src.indexOf("*/", i + 2);
            i = e === -1 ? n : e + 2;
            continue;
        }
        if (depth === 0 && stmtStart === null) stmtStart = i;
        if ("\"'`".includes(c)) {
            i = skipString(i, c) + 1;
            lastSig = "x";
            continue;
        }
        if (c === "/" && ["", "(", ",", "=", ":", "[", "!", "&", "|", "?", "{", "}", ";", "\n"].includes(lastSig)) {
            let j = i + 1, cls = false;
            while (j < n && (src[j] !== "/" || cls) && src[j] !== "\n") {
                if (src[j] === "\\") j++;
                else if (src[j] === "[") cls = true;
                else if (src[j] === "]") cls = false;
                j++;
            }
            i = j + 1;
            lastSig = "x";
            continue;
        }
        if ("({[".includes(c)) depth++;
        else if (")}]".includes(c)) {
            depth--;
            if (depth === 0 && c === "}") {
                let k = i + 1;
                while (k < n && " \t".includes(src[k])) k++;
                if (k >= n || "\r\n".includes(src[k]) || src[k] === ";") {
                    const rest = src.slice(k, k + 200).replace(/^[ \t\r\n;]+/, "");
                    const cont = /^(\.|\)|,|\?|:|else\b|catch\b|finally\b|while\b|\+|-|\*|&&|\|\|)/.test(rest) && !(k < n && src[k] === ";");
                    if (!cont) {
                        const end = k < n && src[k] === ";" ? k + 1 : k;
                        units.push([stmtStart, end]);
                        stmtStart = null;
                        i = end;
                        lastSig = ";";
                        continue;
                    }
                }
            }
        } else if (c === ";" && depth === 0) {
            units.push([stmtStart, i + 1]);
            stmtStart = null;
            i++;
            lastSig = ";";
            continue;
        }
        lastSig = c;
        i++;
    }
    if (stmtStart !== null) units.push([stmtStart, n]);
    return units;
}

export function declNames(text) {
    let m = /^(?:export\s+)?(?:async\s+)?(?:function\s*\*?|class)\s*([A-Za-z_$][\w$]*)/.exec(text);
    if (m) return new Set([m[1]]);
    m = /^(?:export\s+)?(?:const|let|var)\s+/.exec(text);
    if (!m) return new Set();
    const names = new Set(), n = text.length;
    let depth = 0, j = m[0].length, cur = "", expectName = true;
    while (j < n) {
        const ch = text[j];
        if ("([{".includes(ch)) {
            if (depth === 0 && ch === "(") break;
            depth++;
        } else if (")]}".includes(ch)) depth--;
        else if (ch === "=" && depth === 0) {
            if (cur && expectName) names.add(cur);
            cur = "";
            expectName = false;
            let d = 0;
            j++;
            while (j < n && !(d === 0 && ",;".includes(text[j]))) {
                if ("([{".includes(text[j])) d++;
                else if (")]}".includes(text[j])) d--;
                else if ("\"'`".includes(text[j])) {
                    const q = text[j];
                    j++;
                    while (j < n && text[j] !== q) j += text[j] === "\\" ? 2 : 1;
                }
                j++;
            }
            if (j < n && text[j] === ",") expectName = true;
            else break;
        } else if (ch === ":" && depth > 0) cur = "";
        if (IDCH.test(ch)) cur += ch;
        else {
            if (cur && expectName && !/^\p{Nd}/u.test(cur)) names.add(cur);
            cur = "";
        }
        j++;
    }
    if (cur && expectName) names.add(cur);
    return names;
}

const add = (m, k, v) => m.set(k, (m.get(k) || 0) + v);

export function classify(src, path, module) {
    const model = tcm.dtsModel();
    const spans = scanUnits(src);
    const kinds = spans.map(([s, e]) => {
        const text = src.slice(s, e);
        if (/^(import\b|export\b|function\b|async\s+function\b|class\b|const\b|let\b|var\b)/.test(text)) return "decl";
        if (/^\{/.test(text) || /^(test|section|group|describe|it|check_group)\s*\(/.test(text) ||
            /^\(\s*(async\s*)?(function|\()/.test(text) || /^await\s+\(/.test(text)) return "unit";
        return /\b(assert\w*|check\w*|eq\w*|expect\w*|throws\w*|ok|same\w*)\s*\(/.test(text) ? "unit" : "stmt";
    });
    const first = kinds.indexOf("unit");
    const last = kinds.lastIndexOf("unit");
    if (first === -1) return null;
    const named = new Map();
    let nss = [];
    for (const rx of [tcm.IMPORT_FROM, tcm.DYN_IMPORT]) {
        for (const m of src.matchAll(rx)) {
            if (m[2] !== module) continue;
            const [a, b] = tcm.parseBindings(m[1]);
            for (const [k, v] of a) named.set(k, v);
            nss = nss.concat(b);
        }
    }
    const exports = model.get(module) || new Map();
    const count = (text) => {
        let counts = new Map(), order = [];
        for (const [local, exp] of named) {
            for (const m of text.matchAll(new RegExp("(?<![\\p{L}\\p{N}_$.])" + reEsc(local) + "(?![\\p{L}\\p{N}_$])", "gu"))) {
                add(counts, exp, 1);
                order.push([m.index, exp]);
            }
        }
        for (const ns of nss) {
            for (const m of text.matchAll(new RegExp(reEsc(ns) + "\\s*\\.\\s*([A-Za-z_$][\\w$]*)", "g"))) {
                if (exports.has(m[1])) {
                    add(counts, m[1], 1);
                    order.push([m.index, m[1]]);
                }
            }
        }
        if (BY_MEMBER) {
            const [cls, members] = BY_MEMBER;
            let mc = new Map(), mo = [];
            for (const m of text.matchAll(/\.\s*([A-Za-z_$][\w$]*)\s*\(/g)) {
                if (members.has(m[1])) {
                    const key = `${cls}.${m[1]}`;
                    add(mc, key, 1);
                    mo.push([m.index, key]);
                }
            }
            const conv = new Set(["toString", "valueOf", "toJSON"].map((x) => `${cls}.${x}`));
            if ([...mc.keys()].some((k) => !conv.has(k))) {
                mc = new Map([...mc].filter(([k]) => !conv.has(k)));
                mo = mo.filter(([, k]) => !conv.has(k));
            }
            if (mc.size) { counts = mc; order = mo; }
        }
        return [counts, order];
    };
    let helpers = new Map();
    const htext = new Map();
    spans.forEach(([s, e], k) => {
        if (kinds[k] === "decl" && !/^(import|export)\b/.test(src.slice(s, e))) {
            const t = tcm.stripComments(src.slice(s, e));
            for (const nm of declNames(src.slice(s, e))) {
                htext.set(nm, t);
                helpers.set(nm, count(t));
            }
        }
    });
    for (let r = 0; r < 3; r++) {
        for (const [nm, t] of htext) {
            if (helpers.get(nm)[0].size) continue;
            const acc = new Map();
            for (const [other, [hc]] of [...helpers]) {
                if (other !== nm && hc.size && word(other).test(t)) for (const [x, c] of hc) add(acc, x, c);
            }
            if (acc.size) helpers.set(nm, [acc, []]);
        }
    }
    helpers = new Map([...helpers].filter(([, v]) => v[0].size));
    const units = [], decls = [];
    let pending = null;
    for (let k = first; k <= last; k++) {
        let [s, e] = spans[k];
        if (kinds[k] === "decl") {
            const s0 = pending === null ? s : pending;
            pending = null;
            decls.push({ start: s0, end: e, names: declNames(src.slice(s, e)), next: units.length });
            continue;
        }
        if (kinds[k] !== "unit") {
            if (pending === null) pending = s;
            continue;
        }
        if (pending !== null) { s = pending; pending = null; }
        const text = tcm.stripComments(src.slice(s, e));
        const [counts, order] = count(text);
        if (!counts.size) {
            for (const [nm, [hc]] of helpers) {
                for (const m of text.matchAll(new RegExp("(?<![\\p{L}\\p{N}_$.])" + reEsc(nm) + "(?![\\p{L}\\p{N}_$])", "gu"))) {
                    for (const [x, c] of hc) {
                        add(counts, x, c);
                        order.push([m.index, x]);
                    }
                }
            }
        }
        order.sort((a, b) => (a[0] - b[0]) || byKey(a[1], b[1]));
        const firsts = [];
        for (const [, x] of order) if (!firsts.includes(x)) firsts.push(x);
        let prim = null;
        for (const x of firsts) if (prim === null || counts.get(x) > counts.get(prim)) prim = x;
        let line = 1;
        for (let p = src.indexOf("\n"); p !== -1 && p < s; p = src.indexOf("\n", p + 1)) line++;
        units.push({ idx: k, start: s, end: e, kind: kinds[k], syms: counts, primary: prim, line });
    }
    return { spans, kinds, first, last, units, decls, prelude_end: spans[first][0], trailer_start: spans[last][1] };
}

export function guessModule(path) {
    const model = tcm.dtsModel();
    const m = /^(?:test|bb)_([a-z0-9]+)/.exec(basename(path));
    const seg = path.split("/")[1];
    for (const cand of [...(m ? [m[1]] : []), seg]) if (model.has("dyna:" + cand)) return "dyna:" + cand;
    return "dyna:" + (m ? m[1] : seg);
}

export function snake(name) {
    const s = name.replace(/([a-z0-9])([A-Z])/g, "$1_$2");
    return s.replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2").toLowerCase();
}

export function plan(path, module, assign) {
    const src = readText(join(ROOT, path));
    const c = classify(src, path, module);
    const problems = [];
    if (c === null) return [src, null, ["no top-level test units found (nothing to split)"]];
    for (const u of c.units) if (assign.has(String(u.line))) u.primary = assign.get(String(u.line));
    const us = c.units;
    if (INHERIT) {
        us.forEach((u, k) => {
            if (u.primary) return;
            let prev = null;
            for (let j = k - 1; j >= 0 && !prev; j--) prev = us[j].primary || null;
            let nxt = null;
            for (let j = k + 1; j < us.length && !nxt; j++) nxt = us[j].primary || null;
            u.primary = prev || nxt;
        });
    }
    for (const u of us) if (!u.primary) problems.push(`line ${u.line}: unit uses no ${module} symbol -- --assign ${u.line}=<symbol> or --inherit`);
    const trailer = src.slice(c.trailer_start);
    if (/\b(assert\w*|check\w*|expect\w*|eq\w*)\s*\(/.test(tcm.stripComments(trailer)))
        problems.push("the trailer asserts (a running count?) -- it cannot be copied into every split");
    return [src, c, problems];
}

export function buildFiles(path, module, src, c) {
    const base = basename(path).slice(0, -3);
    const area = dirname(path);
    const mshort = module.split(":").pop();
    const aspect = base.replace(new RegExp("^(test|bb)_(" + reEsc(mshort) + "[a-z0-9]*(?:_|$))?"), "");
    const lines = src.slice(0, c.prelude_end).split("\n");
    const hdr = [];
    while (lines.length && /^\/\/\s*(flags|timeout|covers):/.test(lines[0])) {
        const line = lines.shift();
        if (!line.startsWith("// covers:")) hdr.push(line);
    }
    const prelude = lines.join("\n");
    const trailer = src.slice(c.trailer_start);
    const groups = new Map();
    c.units.forEach((u, k) => {
        u.pos = k;
        if (!groups.has(u.primary)) groups.set(u.primary, []);
        groups.get(u.primary).push(u);
    });
    const decls = c.decls || [];
    const res = new Map();
    for (const [sym, us] of groups) {
        const parts = !sym.startsWith("c:") ? sym.split(".").map(snake)
            : [basename(sym.slice(2)).replace(/(?<!^)\.[^.]*$/, "").toLowerCase().replace(/[^a-z0-9]+/g, "_")];
        const have = new Set(parts.join("_").split("_"));
        const at = aspect ? aspect.split("_") : [];
        while (at.length && have.has(at[0])) at.shift();
        const asp = at.join("_");
        const name = `test_${mshort}_` + parts.join("_") + (asp ? `_${asp}` : "") + ".js";
        const mine = new Set(us.map((u) => u.pos));
        const textOf = [...us.map((u) => tcm.stripComments(src.slice(u.start, u.end))), tcm.stripComments(trailer)];
        const take = decls.map((d) => mine.has(d.next));
        let changed = true;
        while (changed) {
            changed = false;
            const hay = [...textOf, ...decls.filter((d, k) => take[k]).map((d) => tcm.stripComments(src.slice(d.start, d.end)))];
            decls.forEach((d, k) => {
                if (!take[k] && [...d.names].some((nm) => { const rx = word(nm); return hay.some((h) => rx.test(h)); })) {
                    take[k] = true;
                    changed = true;
                }
            });
        }
        const chunks = [...us.map((u) => [u.start, u.end]), ...decls.filter((d, k) => take[k]).map((d) => [d.start, d.end])]
            .sort((a, b) => (a[0] - b[0]) || (a[1] - b[1]));
        const body = chunks.map(([a, b]) => src.slice(a, b)).join("\n\n");
        const cov = `// covers: ${sym.startsWith("c:") ? sym : module + "." + sym}`;
        res.set(join(area, name), [...hdr, cov].join("\n") + "\n" + prelude.replace(/\n+$/, "") + "\n\n" + body + trailer);
    }
    return res;
}

function runOk(f) {
    const dyn = std.getenv("DYNAJS") || "./dynajs";
    let flags = [];
    for (const l of readText(join(ROOT, f)).split("\n").slice(0, 3)) {
        const m = /^\/\/\s*flags:(.*)$/.exec(l);
        if (m) flags = m[1].split(/\s+/).filter(Boolean);
    }
    const r = Exec(dyn, [...flags, f], { cwd: ROOT, timeoutMs: 600000, maxBuffer: 1 << 28 });
    const all = String(r.stdout) + String(r.stderr);
    const failed = r.code !== 0 || /^\s*FAIL([: ]|$)/m.test(all);
    if (failed) out(`  ${f}: exit ${r.code}\n` + all.trim().split("\n").slice(-6).map((l) => "    " + l).join("\n"));
    return !failed;
}

function addHeader(path, category, reason, module) {
    const model = tcm.dtsModel();
    const [used] = tcm.fileSymbols(path, model);
    let syms = [...new Set([...used].filter((u) => u.startsWith(module + ".") && !u.endsWith(".*")).map((u) => u.split("#")[0]))].sort(byKey);
    if (!syms.length) syms = [module + ".*"];
    const lines = readText(join(ROOT, path)).split("\n");
    if (lines.slice(0, 3).some((l) => /^\/\/\s*covers:/.test(l))) {
        out(`  ${path}: has a covers header already`);
        return 1;
    }
    let k = 0;
    while (k < lines.length && k < 2 && /^\/\/\s*(flags|timeout):/.test(lines[k])) k++;
    const single = syms.length === 1 && !syms[0].endsWith(".*");
    const hdr = single ? `// covers: ${syms[0]}` : `// covers: ${category} ` + syms.join(" ") + ` -- ${reason}`;
    lines.splice(k, 0, hdr);
    let target = path;
    if (single && basename(path).startsWith("test_")) {
        const mshort = module.split(":").pop();
        const want = `test_${mshort}_${snake(syms[0].slice(syms[0].indexOf(".") + 1))}`;
        const base = basename(path).slice(0, -3);
        if (base !== want && !base.startsWith(want + "_")) {
            const aspect = base.replace(new RegExp("^test_(" + reEsc(mshort) + "_?)?"), "");
            target = join(dirname(path), want + (aspect ? `_${aspect}` : "") + ".js");
            if (exists(join(ROOT, target))) {
                out(`  REFUSED: ${target} exists`);
                return 1;
            }
        }
    }
    writeText(join(ROOT, target), lines.join("\n"));
    if (target !== path) {
        os.remove(join(ROOT, path));
        rewriteBuild(path, [target]);
    }
    out(`  ${target}: ${[...hdr].slice(0, 120).join("")}`);
    return 0;
}

function logSplit(old, nw) {
    const log = std.getenv("SPLIT_LOG");
    const sorted = [...nw].sort(byKey).join(" ");
    if (log) {
        const f = std.open(log, "a");
        f.puts(old + "\t" + sorted + "\n");
        f.close();
    }
}

export function selftest() {
    let bad = 0;
    const fail = (msg) => { out("selftest: " + msg); bad++; };
    const split = (path, module, src, by = null) => {
        BY_MEMBER = by;
        try {
            const c = classify(src, path, module);
            return [c, buildFiles(path, module, src, c)];
        } finally {
            BY_MEMBER = null;
        }
    };
    const keys = (m) => [...m.keys()];
    const prims = (c) => c.units.map((u) => u.primary);
    const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
    const occ = (t, s) => t.split(s).length - 1;
    const pre = 'import { Parse, Stringify } from "dyna:yaml";\nlet n = 0;\nfunction ok(c) { n++; if (!c) throw 1; }\n';
    let src = pre + '{ ok(Parse("b: 2")); }\nconst T = "a: 1";\n{ ok(Parse(T)); }\n{ ok(Stringify(Parse(T))); ok(Stringify(1)); }\nprint(n);\n';
    let [c, fs] = split("tests/yaml/test_yaml_x.js", "dyna:yaml", src);
    if (!same(keys(fs).sort(byKey), ["tests/yaml/test_yaml_parse_x.js", "tests/yaml/test_yaml_stringify_x.js"]) ||
        [...fs.values()].some((t) => occ(t, "const T = ") !== 1))
        fail(`a declaration two groups read was not copied into each exactly once: ${JSON.stringify(keys(fs).sort(byKey))}`);
    src = pre + '{ ok(Parse("b: 2")); }\nlet block = "";\nblock += "k: 1";\nconst spill = Parse(block);\n{ ok(spill); }\n{ ok(Stringify(spill)); ok(Stringify(2)); }\nprint(n);\n';
    [c, fs] = split("tests/yaml/test_yaml_y.js", "dyna:yaml", src);
    if ([...fs.values()].some((t) => occ(t, "const spill") !== 1 || occ(t, "block += ") !== 1))
        fail("statements before a declaration were duplicated or lost");
    src = pre + 'const R = (x) => Parse(x);\n{ ok(R("a: 1")); }\nprint(n);\n';
    [c, fs] = split("tests/yaml/test_yaml_z.js", "dyna:yaml", src);
    if (!same(prims(c), ["Parse"])) fail(`a unit calling only a helper did not inherit the helper's symbol: ${JSON.stringify(prims(c))}`);
    src = 'import { ParseStream } from "dyna:yaml";\n{ ParseStream("a"); }\n';
    [c, fs] = split("tests/yaml/test_yaml_stream.js", "dyna:yaml", src);
    if (!same(keys(fs), ["tests/yaml/test_yaml_parse_stream.js"])) fail(`an aspect repeating the symbol was kept: ${JSON.stringify(keys(fs))}`);
    src = "// covers: dyna:yaml.Parse\n" + pre + '{ ok(Parse("a")); }\nprint(n);\n';
    [c, fs] = split("tests/yaml/test_yaml_parse_w.js", "dyna:yaml", src);
    if ([...fs.values()].some((t) => occ(t, "// covers:") !== 1)) fail("a re-split kept the old covers header beside the new one");
    src = 'import { Decimal } from "dyna:decimal";\n{ const d = new Decimal("1"); if (d.div(d).toString() !== d.toString()) throw 1; }\n{ if (new Decimal("2").toString() !== "2") throw 1; }\n';
    [c] = split("tests/decimal/test_decimal_q.js", "dyna:decimal", src, ["Decimal", new Set(["div", "toString"])]);
    if (!same(prims(c), ["Decimal.div", "Decimal.toString"])) fail(`toString carried the comparison but took the unit: ${JSON.stringify(prims(c))}`);
    const tmpRel = ".selftest-split.js";
    writeText(join(ROOT, tmpRel), pre + '{ ok(Parse("a: 1")); }\n{ ok(n === 1); }\n');
    let pr, pr2;
    try {
        INHERIT = true;
        pr = plan(tmpRel, "dyna:yaml", new Map());
        INHERIT = false;
        pr2 = plan(tmpRel, "dyna:yaml", new Map());
    } finally {
        INHERIT = false;
        os.remove(join(ROOT, tmpRel));
    }
    if (!same(prims(pr[1]), ["Parse", "Parse"]) || pr[2].length)
        fail(`--inherit did not give a symbol-less unit its predecessor's symbol: ${JSON.stringify(prims(pr[1]))} ${JSON.stringify(pr[2])}`);
    if (!same(pr2[2], ["line 5: unit uses no dyna:yaml symbol -- --assign 5=<symbol> or --inherit"]))
        fail(`a symbol-less unit without --inherit/--assign was not refused: ${JSON.stringify(pr2[2])}`);
    c = classify(pre + "{ ok(1); }\n", "tests/yaml/test_yaml_v.js", "dyna:yaml");
    c.units[0].primary = "c:src/builtins/builtin-string.c";
    fs = buildFiles("tests/yaml/test_yaml_v.js", "dyna:yaml", pre + "{ ok(1); }\n", c);
    if (!same(keys(fs), ["tests/yaml/test_yaml_builtin_string_v.js"]) || ![...fs.values()][0].includes("// covers: c:src/builtins/builtin-string.c\n"))
        fail(`a c:<src> group was not named and headed by its source: ${JSON.stringify(keys(fs))}`);
    out("split-suite selftest: " + (!bad ? "ok" : `${bad} problem(s)`));
    return bad ? 1 : 0;
}

const DOC = readText(import.meta.url.replace(/^file:\/\//, "")).split("\n").slice(1)
    .filter((l) => l.startsWith("//")).map((l) => l.replace(/^\/\/ ?/, "")).join("\n");

export function main(argv) {
    if (argv[0] === "selftest") return selftest();
    if (argv.length >= 4 && argv[0] === "header") {
        os.chdir(ROOT);
        let mod = guessModule(argv[1]);
        if (argv.includes("--module")) mod = argv[argv.indexOf("--module") + 1];
        return addHeader(argv[1], argv[2], argv[3], mod);
    }
    if (argv.length < 2 || !["plan", "apply"].includes(argv[0])) {
        out(DOC);
        return 2;
    }
    os.chdir(ROOT);
    const [cmd, path] = argv;
    let module = guessModule(path), dry = false;
    const assign = new Map();
    for (let i = 2; i < argv.length; i++) {
        if (argv[i] === "--module") module = argv[++i];
        else if (argv[i] === "--assign") {
            const kv = argv[++i], e = kv.indexOf("=");
            assign.set(kv.slice(0, e), kv.slice(e + 1));
        } else if (argv[i] === "--by-member") {
            const cls = argv[++i];
            BY_MEMBER = [cls, new Set(tcm.dtsModel().get(module).get(cls).members)];
        } else if (argv[i] === "--inherit") INHERIT = true;
        else if (argv[i] === "--dry-run") dry = true;
    }
    const [src, c, problems] = plan(path, module, assign);
    if (c) {
        for (const u of c.units) {
            const syms = [...u.syms].map((kv, i) => [kv, i]).sort((a, b) => (b[0][1] - a[0][1]) || (a[1] - b[1]))
                .map(([[k, v]]) => `${k}x${v}`).join(", ");
            out(`  line ${String(u.line).padStart(5)} ${u.kind.padEnd(5)} -> ${(u.primary || "?").padEnd(24)} [${syms}]`);
        }
    }
    for (const p of problems) out("  PROBLEM: " + p);
    if (cmd === "plan" || problems.length) return problems.length ? 1 : 0;
    const files = buildFiles(path, module, src, c);
    for (const f of files.keys()) {
        if (exists(join(ROOT, f)) && f !== path) {
            out(`  REFUSED: ${f} exists already -- pass --module or rename the aspect`);
            return 1;
        }
    }
    if (dry) {
        for (const [f, t] of files) out(`  would write ${f} (${t.split("\n").length - 1} lines)`);
        return 0;
    }
    for (const [f, t] of files) writeText(join(ROOT, f), t);
    if (!files.has(path)) os.remove(join(ROOT, path));
    const bad = keys(files).sort(byKey).filter((f) => !runOk(f));
    if (bad.length) {
        for (const f of files.keys()) os.remove(join(ROOT, f));
        writeText(join(ROOT, path), src);
        for (const f of bad) out(`  REFUSED: ${f} does not pass on its own (a shared fixture went to another group?) -- rolled back`);
        return 1;
    }
    logSplit(path, keys(files));
    for (const f of keys(files).sort(byKey)) out(`  wrote ${f}`);
    out(`split-suite: ${path} -> ${files.size} suite(s)`);
    return 0;
}

function keys(m) { return [...m.keys()]; }

if (typeof scriptArgs !== "undefined" && /split-suite\.js$/.test(scriptArgs[0] || ""))
    std.exit(main(scriptArgs.slice(1)));
