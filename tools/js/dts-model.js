// flags: --std
// dynajs.d.ts and import-clause parsing shared by tools (tools/split-suite.js):
// dtsModel (module -> declared names and members), stripComments, parseBindings, IMPORT_FROM, DYN_IMPORT.
import * as std from "std";
import * as os from "os";

const HERE = (() => {
    const u = import.meta.url.replace(/^file:\/\//, "");
    const d = u.slice(0, u.lastIndexOf("/"));
    const [abs] = os.realpath(d);
    return abs || d;
})();
export const ROOT = HERE.slice(0, HERE.lastIndexOf("/tools"));
const DTS = ROOT + "/dynajs.d.ts";
const byKey = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const sortStr = (a) => [...a].sort(byKey);

export function stripComments(src) {
    src = src.replace(/\/\*[\s\S]*?\*\//g, (m) => "\n".repeat((m.match(/\n/g) || []).length));
    return src.replace(/\/\/[^\n]*/g, "");
}

const TOP_DECL = /^ {4}(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?(class|function|const|let|var|enum|namespace)\s+([A-Za-z_$][\w$]*)/gm;
const MEMBER = /^ {8}(?:static\s+)?(?:readonly\s+)?(?:get\s+|set\s+)?(?:async\s+)?\*?([A-Za-z_$][\w$]*)\s*[(<?:]/gm;

export function dtsModel(path = DTS) {
    const text = stripComments(std.loadFile(path));
    const model = new Map();
    for (const m of text.matchAll(/\ndeclare module "([^"]+)"\s*\{/g)) {
        let depth = 1, i = m.index + m[0].length;
        while (depth && i < text.length) {
            const c = text[i];
            if (c === "{") depth++;
            else if (c === "}") depth--;
            i++;
        }
        const start = m.index + m[0].length;
        const body = text.slice(start, i - 1);
        if (!model.has(m[1])) model.set(m[1], new Map());
        const mod = model.get(m[1]);
        for (const d of body.matchAll(TOP_DECL)) {
            const kind = d[1], name = d[2];
            if (!mod.has(name)) mod.set(name, { kind, members: new Set() });
            const ent = mod.get(name);
            const dend = d.index + d[0].length;
            const obj = kind === "const" && /^\s*:\s*\{/.test(body.slice(dend));
            if (kind === "class" || kind === "namespace" || obj) {
                const j = body.indexOf("{", dend);
                const nl = body.indexOf("\n", dend);
                const lead = j === -1 ? "" : body.slice(dend, j).trim();
                if (j !== -1 && (nl === -1 || j < nl || lead.startsWith("extends") || lead.startsWith("implements") || lead.startsWith("<"))) {
                    let dep = 1, k = j + 1;
                    while (dep && k < body.length) {
                        if (body[k] === "{") dep++;
                        else if (body[k] === "}") dep--;
                        k++;
                    }
                    const inner = body.slice(j + 1, k - 1);
                    for (const mm of inner.matchAll(MEMBER)) if (mm[1] !== "constructor" && mm[1] !== "new") ent.members.add(mm[1]);
                    if (kind === "namespace")
                        for (const nd of inner.matchAll(/^ {8}(?:export\s+)?(?:function|const|class|let|var)\s+([A-Za-z_$][\w$]*)/gm)) ent.members.add(nd[1]);
                }
            }
        }
    }
    for (const mod of model.values()) for (const ent of mod.values()) ent.members = sortStr(ent.members);
    return model;
}

export const IMPORT_FROM = /import\s+([\s\S]*?)\s+from\s*["']([^"']+)["']/g;
export const DYN_IMPORT = /(?:const|let|var)\s+(\{[^}]*\}|[A-Za-z_$][\w$]*)\s*=\s*(?:await\s+)?import\(\s*["']([^"']+)["']\s*\)/g;
const IDENT = /^[A-Za-z_$][\w$]*$/;

export function parseBindings(clause) {
    const named = new Map(), nss = [];
    clause = clause.trim();
    const m = /\*\s+as\s+([A-Za-z_$][\w$]*)/.exec(clause);
    if (m) nss.push(m[1]);
    const b = /\{([^}]*)\}/.exec(clause);
    if (b) {
        for (let part of b[1].split(",")) {
            part = part.trim();
            if (!part) continue;
            const mm = /^([A-Za-z_$][\w$]*)(?:\s+as\s+|\s*:\s*)([A-Za-z_$][\w$]*)$/.exec(part);
            if (mm) named.set(mm[2], mm[1]);
            else if (IDENT.test(part)) named.set(part, part);
        }
    }
    let rest = clause.replace(/\{[^}]*\}/g, "");
    rest = rest.replace(/\*\s+as\s+[A-Za-z_$][\w$]*/g, "").trim().replace(/^,+|,+$/g, "").trim();
    if (IDENT.test(rest)) nss.push(rest);
    return [named, nss];
}
