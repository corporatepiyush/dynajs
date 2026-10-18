// tests/lib/dtsparse.js -- parse dynajs.d.ts into callable signatures: { module: { functions, classes } }.
// Runs under dynajs (and node, for development). Types are kept as text and
// classified later; aliases declared anywhere in the file are resolved.

export function stripComments(s) {
    let out = "", i = 0;
    while (i < s.length) {
        const c = s[i], d = s[i + 1];
        if (c === "/" && d === "*") { const e = s.indexOf("*/", i + 2); i = e < 0 ? s.length : e + 2; continue; }
        if (c === "/" && d === "/") { const e = s.indexOf("\n", i); i = e < 0 ? s.length : e; continue; }
        if (c === '"' || c === "'" || c === "`") {
            let j = i + 1;
            while (j < s.length && s[j] !== c) j += s[j] === "\\" ? 2 : 1;
            out += s.slice(i, j + 1); i = j + 1; continue;
        }
        out += c; i++;
    }
    return out;
}

function matchClose(s, i) {
    const open = s[i], close = { "{": "}", "(": ")", "[": "]", "<": ">" }[open];
    let depth = 0;
    for (let j = i; j < s.length; j++) {
        const c = s[j];
        if (c === '"' || c === "'") { const q = c; j++; while (j < s.length && s[j] !== q) j += s[j] === "\\" ? 2 : 1; continue; }
        if (c === "{" || c === "(" || c === "[" || (c === "<" && open === "<")) depth++;
        else if (c === "}" || c === ")" || c === "]" || (c === ">" && open === "<" && s[j - 1] !== "=")) { depth--; if (depth === 0) return j; }
    }
    return -1;
}

export function splitTop(s, sep) {
    const out = []; let depth = 0, cur = "";
    for (let i = 0; i < s.length; i++) {
        const c = s[i];
        if (c === '"' || c === "'") { let j = i + 1; while (j < s.length && s[j] !== c) j += s[j] === "\\" ? 2 : 1; cur += s.slice(i, j + 1); i = j; continue; }
        if (c === "(" || c === "{" || c === "[" || c === "<") depth++;
        else if (c === ")" || c === "}" || c === "]" || (c === ">" && s[i - 1] !== "=")) depth--;
        if (c === sep && depth === 0) { out.push(cur); cur = ""; continue; }
        cur += c;
    }
    if (cur.trim()) out.push(cur);
    return out.map((x) => x.trim()).filter(Boolean);
}

function parseParams(text) {
    return splitTop(text, ",").map((p) => {
        const rest = p.startsWith("...");
        if (rest) p = p.slice(3);
        const m = /^([A-Za-z_$][\w$]*)(\??)\s*(?::\s*([\s\S]*))?$/.exec(p);
        if (!m) return { name: "?", type: "any", optional: true, rest };
        let type = (m[3] || "any").trim();
        let optional = m[2] === "?";
        // a default is a top-level "=", never the one in an arrow type's "=>"
        const eq = splitTop(type.replace(/=>/g, "\u0001"), "=").map((x) => x.replace(/\u0001/g, "=>"));
        if (eq.length > 1) { type = eq[0].trim(); optional = true; }
        return { name: m[1], type, optional, rest };
    });
}

// members of a class/interface/namespace body: methods, ctors, properties
function parseMembers(body) {
    const out = [];
    let i = 0;
    while (i < body.length) {
        while (i < body.length && /[\s;,]/.test(body[i])) i++;
        if (i >= body.length) break;
        const rest = body.slice(i);
        const m = /^((?:static|readonly|public|private|protected|get|set|async)\s+)*([A-Za-z_$][\w$]*|\[[^\]]+\]|"[^"]*")\s*(\??)\s*(<)?/.exec(rest);
        if (!m) { const e = body.indexOf(";", i); i = e < 0 ? body.length : e + 1; continue; }
        const isStatic = /\bstatic\b/.test(m[1] || "");
        const name = m[2];
        let j = i + m[0].length;
        if (m[4]) { j = matchClose(body, j - 1) + 1; while (/\s/.test(body[j])) j++; }
        if (body[j] === "(") {
            const k = matchClose(body, j);
            const params = parseParams(body.slice(j + 1, k));
            let e = k + 1, depth = 0;
            for (; e < body.length; e++) {
                const c = body[e];
                if (c === "(" || c === "{" || c === "[" || c === "<") depth++;
                else if (c === ")" || c === "}" || c === "]" || (c === ">" && body[e - 1] !== "=")) depth--;
                else if ((c === ";" || c === "\n") && depth <= 0) break;
            }
            out.push({ kind: "method", name, isStatic, params, ret: body.slice(k + 1, e).replace(/^\s*:\s*/, "").trim() });
            i = e + 1;
        } else {
            let e = j, depth = 0;
            for (; e < body.length; e++) {
                const c = body[e];
                if (c === "(" || c === "{" || c === "[" || c === "<") depth++;
                else if (c === ")" || c === "}" || c === "]" || (c === ">" && body[e - 1] !== "=")) depth--;
                else if ((c === ";" || c === "\n") && depth <= 0) break;
            }
            out.push({ kind: "prop", name, isStatic, optional: /^\s*\?/.test(body.slice(j, e)), type: body.slice(j, e).replace(/^\s*\??\s*:\s*/, "").trim() });
            i = e + 1;
        }
    }
    return out;
}

export function parseDts(text) {
    const src = stripComments(text);
    const aliases = {}, interfaces = {}, modules = {};
    const reAlias = /\btype\s+([A-Za-z_$][\w$]*)\s*(<[^=]*>)?\s*=\s*/g;
    let m;
    while ((m = reAlias.exec(src))) {
        let e = m.index + m[0].length, depth = 0;
        for (; e < src.length; e++) {
            const c = src[e];
            if (c === "(" || c === "{" || c === "[" || c === "<") depth++;
            else if (c === ")" || c === "}" || c === "]" || (c === ">" && src[e - 1] !== "=")) depth--;
            else if (c === ";" && depth <= 0) break;
        }
        aliases[m[1]] = src.slice(m.index + m[0].length, e).trim();
    }
    const reIface = /\binterface\s+([A-Za-z_$][\w$]*)[^{]*\{/g;
    while ((m = reIface.exec(src))) {
        const k = matchClose(src, m.index + m[0].length - 1);
        interfaces[m[1]] = parseMembers(src.slice(m.index + m[0].length, k));
    }
    const reMod = /declare\s+module\s+"dyna:([\w-]+)"\s*\{/g;
    while ((m = reMod.exec(src))) {
        const start = m.index + m[0].length, end = matchClose(src, start - 1);
        const body = src.slice(start, end);
        const mod = modules[m[1]] = { functions: {}, classes: {}, objects: {} };
        let i = 0;
        while (i < body.length) {
            const rest = body.slice(i);
            let k;
            if ((k = /^\s*(?:export\s+)?function\s+([A-Za-z_$][\w$]*)\s*(<[^(]*>)?\s*\(/.exec(rest))) {
                const p0 = i + k[0].length - 1, p1 = matchClose(body, p0);
                let e = p1 + 1, depth = 0;
                for (; e < body.length; e++) {
                    const c = body[e];
                    if (c === "(" || c === "{" || c === "[" || c === "<") depth++;
                    else if (c === ")" || c === "}" || c === "]" || (c === ">" && body[e - 1] !== "=")) depth--;
                    else if (c === ";" && depth <= 0) break;
                }
                (mod.functions[k[1]] ||= []).push({ params: parseParams(body.slice(p0 + 1, p1)), ret: body.slice(p1 + 1, e).replace(/^\s*:\s*/, "").trim() });
                i = e + 1; continue;
            }
            if ((k = /^\s*(?:export\s+)?(?:abstract\s+)?class\s+([A-Za-z_$][\w$]*)(?:<[^{]*>)?(?:\s+extends\s+([A-Za-z_$][\w$.]*)[^{]*)?(?:\s+implements[^{]*)?\s*\{/.exec(rest))) {
                const b0 = i + k[0].length - 1, b1 = matchClose(body, b0);
                const members = parseMembers(body.slice(b0 + 1, b1));
                mod.classes[k[1]] = { extends: k[2] || null, members };
                i = b1 + 1; continue;
            }
            if ((k = /^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*:\s*\{/.exec(rest))) {
                const b0 = i + k[0].length - 1, b1 = matchClose(body, b0);
                mod.objects[k[1]] = parseMembers(body.slice(b0 + 1, b1));
                i = b1 + 1; continue;
            }
            if ((k = /^\s*(?:export\s+)?namespace\s+([A-Za-z_$][\w$]*)\s*\{/.exec(rest))) {
                const b0 = i + k[0].length - 1, b1 = matchClose(body, b0);
                const inner = body.slice(b0 + 1, b1);
                const fns = [];
                const rf = /function\s+([A-Za-z_$][\w$]*)\s*(?:<[^(]*>)?\s*\(/g;
                let f;
                while ((f = rf.exec(inner))) {
                    const p0 = f.index + f[0].length - 1, p1 = matchClose(inner, p0);
                    fns.push({ kind: "method", name: f[1], isStatic: true, params: parseParams(inner.slice(p0 + 1, p1)), ret: "" });
                }
                mod.objects[k[1]] = fns;
                i = b1 + 1; continue;
            }
            const nl = body.indexOf("\n", i);
            i = nl < 0 ? body.length : nl + 1;
        }
    }
    return { aliases, interfaces, modules };
}
