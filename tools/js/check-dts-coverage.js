import * as std from "std";
import { Exec } from "dyna:sys";
import { walk, join, readText, isdir, pyRepr, pyStrip, pySplitlines, Out } from "./pyshim.js";

const DYNAJS = std.getenv("DTS_DYNAJS") || "./dynajs";
const FILE_MODULES = { "*/src/pool.js": "./src/pool.js" };
const EXCEPTIONS = {
    "dyna:sdl": "built only with CONFIG_SDL3=y; the sdl gate stage re-runs this check against that binary (DTS_DYNAJS=.obj-sdl/dynajs), where the block is verified.",
    "dyna:ui": "built only with CONFIG_UI=y; the ui gate stage re-runs this check against that binary (DTS_DYNAJS=.obj-ui/dynajs).",
    "dyna:raylib": "built only with CONFIG_RAYLIB=y; the raylib gate stage re-runs this check against that binary (DTS_DYNAJS=.obj-raylib/dynajs).",
    "dyna:uring": "io_uring is Linux-only; a darwin binary cannot load it to probe (its d.ts block documents the gating). Linux CI verifies this block through the same direction.",
};
const RUNTIME_DECL = /^ {4}(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(?:class|function|const|var|enum|namespace)\s+([A-Za-z_$][\w$]*)/gm;
const PY_SPECIAL = /[()[\]{}?*+\-|^$\\.&~# \t\n\r\v\f]/g;
const pyEscape = (s) => s.replace(PY_SPECIAL, "\\$&");
const stripComments = (t) => t.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");

function declaredRuntimeNames(block) {
    const text = stripComments(block);
    return [...new Set([...text.matchAll(RUNTIME_DECL)].map((m) => m[1]))].sort();
}

function braceBlocks(src) {
    const text = stripComments(src);
    const out = new Map();
    for (const m of text.matchAll(/\ndeclare module "([^"]+)"\s*\{/g)) {
        let depth = 1, i = m.index + m[0].length;
        const st = i;
        while (depth && i < text.length) {
            const c = text[i];
            if (c === "{") depth++;
            else if (c === "}") depth--;
            i++;
        }
        out.set(m[1], (out.get(m[1]) || "") + "\n" + text.slice(st, i - 1));
    }
    return out;
}
const PROBE = "\nimport(m).then(ns => {\n  const out = { names: [] };\n  const seen = new Set();\n  function collect(prefix, obj) {\n    for (const k of Object.getOwnPropertyNames(obj)) {\n      if (k === \"prototype\" || k === \"constructor\" || k === \"length\" || k === \"name\") continue;\n      out.names.push(prefix + k);\n    }\n    const p = obj.prototype;\n    if (p && !seen.has(p)) {\n      seen.add(p);\n      for (const k of Object.getOwnPropertyNames(p)) {\n        if (k === \"constructor\") continue;\n        out.names.push(prefix + \"prototype.\" + k);\n      }\n    }\n  }\n  for (const k of Object.getOwnPropertyNames(ns)) {\n    if (k === \"default\") continue;\n    const v = ns[k];\n    out.names.push(k);\n    if (typeof v === \"function\" && v.prototype && Object.getOwnPropertyNames(v.prototype).length > 1)\n      collect(k + \".\", v);\n  }\n  print(JSON.stringify(out));\n}).catch(e => print(JSON.stringify({ error: e.message })));\n";

function moduleNames() {
    const mods = new Set();
    for (const [root, , files] of walk("src")) {
        for (const f of files) {
            if (!(f.endsWith(".c") || f.endsWith(".h"))) continue;
            let text;
            try {
                text = readText(join(root, f), "utf8-ignore");
            } catch (e) {
                continue;
            }
            for (const m of text.matchAll(/"dyna:[a-z0-9]+"/g)) mods.add(m[0].slice(1, -1));
        }
    }
    return [...mods].sort();
}

function probe(module) {
    let r;
    try {
        r = Exec(DYNAJS, ["-e", `const m = ${pyRepr(module)};\n${PROBE}`], { timeoutMs: 120000, env: std.getenviron() });
    } catch (e) {
        return [[], "probe failed: " + e.message];
    }
    if (r.code !== 0) return [[], `probe exited ${r.code}: ${r.stderr.slice(-200)}`];
    const out = pySplitlines(pyStrip(r.stdout));
    if (!out.length) return [[], "no output"];
    let d;
    try {
        d = JSON.parse(out[out.length - 1]);
    } catch (e) {
        return [[], "unparsable output: " + out[out.length - 1].slice(0, 200)];
    }
    if ("error" in d) return [[], "module refused: " + d.error];
    return [[...new Set(d.names)].sort(), null];
}

const IN_STANDARD_LIB = new Set([
    "String.prototype.substr", "String.prototype.trimRight", "String.prototype.trimLeft",
    "String.prototype.anchor", "String.prototype.big", "String.prototype.blink",
    "String.prototype.bold", "String.prototype.fixed", "String.prototype.fontcolor",
    "String.prototype.fontsize", "String.prototype.italics", "String.prototype.link",
    "String.prototype.small", "String.prototype.strike", "String.prototype.sub",
    "String.prototype.sup",
    "Number.NaN", "Number.EPSILON", "Number.MAX_SAFE_INTEGER", "Number.MIN_SAFE_INTEGER",
    "RegExp.prototype.compile",
]);

function main() {
    const o = new Out();
    const src = readText("dynajs.d.ts");
    const blocks = src.split(/\ndeclare module "((?:dyna:|ext:)[a-z0-9]+)" \{/);
    const modsText = new Map();
    for (let i = 1; i < blocks.length; i += 2) modsText.set(blocks[i], (modsText.get(blocks[i]) || "") + "\n" + blocks[i + 1]);
    const fblocks = src.split(/\ndeclare module "([^"]*\*[^"]*)" \{/);
    for (let i = 1; i < fblocks.length; i += 2) modsText.set(fblocks[i], fblocks[i + 1]);

    const declared = new Set([...modsText.keys()].filter((m) => m.startsWith("dyna:")));
    const loadable = moduleNames();
    const probed = [], refused = [];
    for (const m of loadable) {
        const [names, err] = probe(m);
        if (err) {
            refused.push([m, err]);
            continue;
        }
        probed.push([m, names]);
    }
    const missingMods = probed.filter(([m]) => !declared.has(m)).map(([m]) => m);
    const missingSyms = [];
    let total = 0;
    for (const [m, names] of probed) {
        const text = modsText.get(m) || "";
        for (const s of names) {
            total++;
            const base = s.split(".")[0];
            if (!new RegExp("\\b(class|function|const|var|namespace|interface|type)\\s+" + pyEscape(base) + "\\b").test(text)) missingSyms.push(`${m} -> ${s}`);
        }
    }
    o.print(`modules loadable: ${probed.length}`);
    o.print(`skipped (not loadable on this platform): ${refused.length}`);
    for (const [m, err] of refused) o.print(`  ${m}: ${err}`);
    o.print(`symbols exported: ${total}`);
    o.print(`MISSING module blocks: ${missingMods.length}`);
    for (const m of missingMods) o.print("  " + m);
    o.print(`MISSING symbols (base undeclared): ${missingSyms.length}`);
    for (const x of missingSyms.slice(0, 60)) o.print("  " + x);
    if (missingSyms.length > 60) o.print(`  ... and ${missingSyms.length - 60} more`);

    const declaredNotLive = [], exceptionSkips = [];
    const exact = braceBlocks(src);
    for (const m of [...exact.keys()].sort()) {
        if (!(m.startsWith("dyna:") || m.includes("*"))) continue;
        const declNames = declaredRuntimeNames(exact.get(m));
        if (!declNames.length) continue;
        let liveNames;
        if (m.includes("*")) {
            if (!(m in FILE_MODULES)) {
                declaredNotLive.push(`${m} -> <wildcard module with no FILE_MODULES entry>`);
                continue;
            }
            const [names, err] = probe(FILE_MODULES[m]);
            if (err) {
                declaredNotLive.push(`${m} -> <probe failed: ${err}>`);
                continue;
            }
            liveNames = new Set(names.map((n) => n.split(".")[0]));
        } else {
            const live = probed.find(([mm]) => mm === m);
            if (!live) {
                if (m in EXCEPTIONS) {
                    exceptionSkips.push(m);
                    continue;
                }
                declaredNotLive.push(`${m} -> <declared, not loadable here>`);
                continue;
            }
            liveNames = new Set(live[1].map((n) => n.split(".")[0]));
        }
        for (const n of declNames) if (!liveNames.has(n)) declaredNotLive.push(`${m} -> ${n}`);
    }
    let nblocks = 0;
    for (const [m, t] of exact) if ((m.startsWith("dyna:") || m.includes("*")) && declaredRuntimeNames(t).length) nblocks++;
    o.print(`d.ts->live: runtime decls checked in ${nblocks} module blocks`);
    o.print(`DECLARED NOT LIVE (d.ts -> runtime): ${declaredNotLive.length}`);
    for (const x of declaredNotLive.slice(0, 60)) o.print("  " + x);
    if (declaredNotLive.length > 60) o.print(`  ... and ${declaredNotLive.length - 60} more`);
    if (exceptionSkips.length) {
        o.print(`d.ts->live skipped via EXCEPTIONS: ${exceptionSkips.length}`);
        for (const m of exceptionSkips) o.print(`  ${m}: ${EXCEPTIONS[m]}`);
    }
    const haveT262 = isdir("test262/test/built-ins");
    if (!haveT262) o.print("ext top-level coverage: SKIPPED -- test262/ absent (standard-vs-extension classification needs it)");
    const inv = JSON.parse(Exec(DYNAJS, ["--std", "tools/api-inventory.js", "--json"], { env: std.getenviron() }).stdout);
    const topLevel = src.replace(/declare module "dyna:[a-z0-9]+" \{[\s\S]*?\n\}/g, "");
    const extMissing = [];
    let extTotal = 0;
    for (const e of inv.entries) {
        if (!e.module.startsWith("ext:") || e.standard) continue;
        if (!haveT262) continue;
        extTotal++;
        const s = e.symbol;
        if (IN_STANDARD_LIB.has(s)) continue;
        const parts = s.split(".");
        const base = parts[0], meth = parts[parts.length - 1];
        if (!new RegExp("interface " + base + "(?:Constructor)?\\b[\\s\\S]{0,20000}?" + pyEscape(meth) + "(?![A-Za-z0-9_$])").test(topLevel)) extMissing.push(s);
    }
    o.print(`non-standard ext symbols: ${extTotal}`);
    o.print(`MISSING from top-level interfaces: ${extMissing.length}`);
    for (const x of extMissing.slice(0, 60)) o.print("  " + x);
    o.flush();
    const bad = missingMods.length || missingSyms.length || extMissing.length || declaredNotLive.length;
    std.exit(bad ? 1 : 0);
}
main();
