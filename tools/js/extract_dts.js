import { readText, writeText, pyStrip, pySplitlines, Out } from "./pyshim.js";

const rawSrc = readText(scriptArgs.length > 1 ? scriptArgs[1] : "dynajs.d.ts");
let src = rawSrc.replace(/\/\*[\s\S]*?\*\//g, "");
src = src.replace(/\/\/[^\n]*/g, "");

const TLS_TOKENS = /\bthe\s+([A-Za-z0-9_*\/]+(?:\/[A-Za-z0-9_*\/]+)*)\s+(functions|classes|namespaces)/gi;
const WIN32_CONST = /\/\*\*[^*]*Windows only[^*]*\*\/\s*\n\s*const\s+([A-Za-z_$][\w$]*)/g;

function gatedPlatform(raw, mod) {
    const i = raw.indexOf(`declare module "${mod}"`);
    if (i < 0) return false;
    const lines = pySplitlines(raw.slice(0, i));
    for (let k = lines.length - 1; k >= 0; k--) {
        const s = pyStrip(lines[k]);
        if (s.startsWith("//")) return s.includes("does not exist otherwise");
        if (s) return false;
    }
    return false;
}

function maskNonMembers(cbody) {
    const out = cbody.split("");
    let paren = 0, brace = 0;
    for (let i = 0; i < cbody.length; i++) {
        const ch = cbody[i];
        if (ch === "(") paren++;
        else if (ch === ")") paren--;
        else if (ch === "{") brace++;
        else if (ch === "}") brace--;
        if (paren === 0 && brace === 0) continue;
        if (ch !== "\n" && ch !== "(") out[i] = " ";
    }
    return out.join("");
}

function tlsGatedTokens(raw, mod, kind) {
    const i = raw.indexOf(`declare module "${mod}"`);
    if (i < 0) return [];
    const comment = [];
    const lines = pySplitlines(raw.slice(0, i));
    for (let k = lines.length - 1; k >= 0; k--) {
        const s = pyStrip(lines[k]);
        if (s.startsWith("//")) comment.push(pyStrip(s.slice(2)));
        else if (s === "" && !comment.length) continue;
        else break;
    }
    const text = comment.reverse().join(" ");
    if (!text.includes("CONFIG_TLS=y builds")) return [];
    const out = [];
    for (const m of text.matchAll(TLS_TOKENS)) {
        if (m[2] !== kind) continue;
        out.push(...m[1].split("/"));
    }
    return out;
}

function matchBrace(s, i) {
    let depth = 1;
    while (depth && i < s.length) {
        const c = s[i];
        if (c === "{") depth++;
        else if (c === "}") depth--;
        i++;
    }
    return i;
}

const pushUniq = (a, v) => {
    if (!a.includes(v)) a.push(v);
};

const modules = new Map();
for (const m of src.matchAll(/declare module "([^"]+)"\s*\{/g)) {
    const name = m[1];
    if (name.includes("*")) continue;
    if (gatedPlatform(rawSrc, name)) continue;
    const start = m.index + m[0].length;
    const i = matchBrace(src, start);
    modules.set(name, (modules.get(name) || "") + "\n" + src.slice(start, i - 1));
}

const surface = {};
const win32Names = new Set([...rawSrc.matchAll(WIN32_CONST)].map((m) => m[1]));
for (const [mod, body] of modules) {
    const entry = { functions: [], consts: [], classes: {}, enums: [], namespaces: {} };
    for (const fn of body.matchAll(/^\s{4}function\s+([A-Za-z_$][\w$]*)/gm)) pushUniq(entry.functions, fn[1]);
    for (const c of body.matchAll(/^\s{4}const\s+([A-Za-z_$][\w$]*)/gm)) pushUniq(entry.consts, c[1]);
    const aliasLocals = new Set();
    for (const al of body.matchAll(/^\s{4}export\s*\{([^}]*)\}/gm)) {
        for (let part of al[1].split(",")) {
            part = pyStrip(part);
            if (!part) continue;
            const k = part.indexOf(" as ");
            if (k >= 0) aliasLocals.add(pyStrip(part.slice(0, k)));
            const nm = k >= 0 ? pyStrip(part.slice(k + 4)) : part;
            pushUniq(entry.consts, nm);
        }
    }
    entry.consts = entry.consts.filter((c) => !aliasLocals.has(c));
    for (const ns of body.matchAll(/^\s{4}namespace\s+([A-Za-z_$][\w$]*)\s*\{/gm)) {
        const st = ns.index + ns[0].length;
        const i = matchBrace(body, st);
        const nbody = body.slice(st, i - 1);
        const members = [];
        for (const f2 of nbody.matchAll(/^\s{8}function\s+([A-Za-z_$][\w$]*)/gm)) pushUniq(members, f2[1]);
        for (const v2 of nbody.matchAll(/^\s{8}(?:const|readonly)\s+([A-Za-z_$][\w$]*)/gm)) pushUniq(members, v2[1]);
        entry.namespaces[ns[1]] = members;
    }
    for (const e of body.matchAll(/^\s{4}(?:export\s+)?enum\s+([A-Za-z_$][\w$]*)/gm)) entry.enums.push(e[1]);
    for (const cl of body.matchAll(/^\s{4}(?:export\s+)?(?:abstract\s+)?class\s+([A-Za-z_$][\w$]*)[^\n{]*\{/gm)) {
        const cname = cl[1];
        const end = cl.index + cl[0].length;
        const inherits = cl[0].includes("DynResource");
        const i = matchBrace(body, end);
        const cbody = maskNonMembers(body.slice(end, i - 1));
        const members = [];
        if (inherits) members.push("close", "dispose", "closed", "[Symbol.dispose]");
        for (const mm of cbody.matchAll(/^\s+(?:static\s+)?(?:get\s+|readonly\s+)?([A-Za-z_$][\w$]*)\s*[(:<=]/gm)) {
            const n = mm[1];
            if (n !== "constructor" && !members.includes(n)) members.push(n);
        }
        for (const mm of cbody.matchAll(/^\s+get\s+([A-Za-z_$][\w$]*)/gm)) pushUniq(members, mm[1]);
        entry.classes[cname] = members;
    }
    for (const [key, kind] of [["tls_functions", "functions"], ["tls_classes", "classes"], ["tls_namespaces", "namespaces"]]) {
        const bucket = [];
        for (const tok of tlsGatedTokens(rawSrc, mod, kind)) {
            const star = tok.endsWith("*");
            const pref = star ? tok.slice(0, -1) : tok;
            const pool = Array.isArray(entry[kind]) ? [...entry[kind]] : Object.keys(entry[kind]);
            for (const n of pool) {
                if (n === pref || (star && n.startsWith(pref))) {
                    bucket.push(n);
                    if (kind === "classes" || kind === "namespaces") delete entry[kind][n];
                    else entry[kind].splice(entry[kind].indexOf(n), 1);
                }
            }
        }
        if (bucket.length) entry[key] = bucket;
    }
    const moved = entry.consts.filter((c) => win32Names.has(c));
    if (moved.length) {
        entry.consts = entry.consts.filter((c) => !moved.includes(c));
        entry.win32_consts = moved;
    }
    surface[mod] = entry;
}

writeText(scriptArgs.length > 2 ? scriptArgs[2] : "surface.json", JSON.stringify(surface, null, 1));
const vals = Object.values(surface);
const nmods = vals.length;
const nfns = vals.reduce((a, e) => a + e.functions.length, 0);
const ncls = vals.reduce((a, e) => a + Object.keys(e.classes).length, 0);
const nmembers = vals.reduce((a, e) => a + Object.values(e.classes).reduce((b, v) => b + v.length, 0), 0);
const nconsts = vals.reduce((a, e) => a + e.consts.length, 0);
const o = new Out();
o.print(`${nmods} modules, ${nfns} functions, ${ncls} classes, ${nmembers} class members, ${nconsts} consts`);
o.flush();
