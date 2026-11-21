// Python-compatible formatting for the tools ported from Python, so their outputs stay
// byte-identical to what the Python printed: fixed-point with round-half-even on the
// exact binary value (format(x, '.Nf')), compensated float sum (sum() since 3.12),
// json.dumps(indent=N, sort_keys) with ensure_ascii, and repr() of str lists.

export function fixed(x, d) {
    const s = x.toFixed(d);
    if (!Number.isFinite(x) || x === 0) return s;
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, Math.abs(x));
    const hi = dv.getUint32(0), lo = dv.getUint32(4);
    const bexp = (hi >>> 20) & 0x7ff;
    let mant = (BigInt(hi & 0xfffff) << 32n) | BigInt(lo);
    let e;
    if (bexp === 0) e = -1074;
    else { mant |= 1n << 52n; e = bexp - 1075; }
    let num = mant * 2n * 10n ** BigInt(d), den = 1n;
    if (e >= 0) num <<= BigInt(e);
    else den <<= BigInt(-e);
    if (num % den !== 0n) return s;
    const q = num / den;
    if (q % 2n === 0n) return s;
    const down = (q - 1n) / 2n;
    if (down % 2n === 1n) return s;
    const digits = down.toString().padStart(d + 1, "0");
    const body = d ? digits.slice(0, -d) + "." + digits.slice(-d) : digits;
    return (x < 0 ? "-" : "") + body;
}

export function sum(xs) {
    let f = 0, c = 0;
    for (const x of xs) {
        const t = f + x;
        if (Math.abs(f) >= Math.abs(x)) c += (f - t) + x;
        else c += (x - t) + f;
        f = t;
    }
    return c && Number.isFinite(c) ? f + c : f;
}

function jstr(s) {
    let o = '"';
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (ch === '"') o += '\\"';
        else if (ch === "\\") o += "\\\\";
        else if (ch === "\n") o += "\\n";
        else if (ch === "\r") o += "\\r";
        else if (ch === "\t") o += "\\t";
        else if (ch === "\b") o += "\\b";
        else if (ch === "\f") o += "\\f";
        else if (c < 0x20 || (c > 0x7e && c <= 0xffff)) o += "\\u" + c.toString(16).padStart(4, "0");
        else if (c > 0xffff) {
            const v = c - 0x10000;
            o += "\\u" + (0xd800 + (v >> 10)).toString(16) + "\\u" + (0xdc00 + (v & 0x3ff)).toString(16);
        } else o += ch;
    }
    return o + '"';
}

function jnum(x) {
    if (Number.isInteger(x)) return String(x);
    let s = String(x);
    if (/e/.test(s)) {
        const [m, ex] = s.split("e");
        const n = parseInt(ex, 10);
        s = (m.includes(".") ? m : m + ".0") + "e" + (n < 0 ? "-" : "+") + String(Math.abs(n)).padStart(2, "0");
    }
    return s;
}

const byKey = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

export function dumps(v, indent = null, sortKeys = false, level = 0) {
    const nl = indent === null ? "" : "\n" + " ".repeat(indent * (level + 1));
    const end = indent === null ? "" : "\n" + " ".repeat(indent * level);
    const sep = indent === null ? ", " : ",";
    if (v === null || v === undefined) return "null";
    if (typeof v === "boolean") return v ? "true" : "false";
    if (typeof v === "number") return jnum(v);
    if (typeof v === "string") return jstr(v);
    if (v instanceof Set) v = [...v];
    if (Array.isArray(v)) {
        if (!v.length) return "[]";
        return "[" + nl + v.map((x) => dumps(x, indent, sortKeys, level + 1)).join(sep + nl) + end + "]";
    }
    let ent = v instanceof Map ? [...v] : Object.entries(v);
    if (!ent.length) return "{}";
    if (sortKeys) ent = ent.sort((a, b) => byKey(a[0], b[0]));
    return "{" + nl + ent.map(([k, x]) => jstr(String(k)) + ": " + dumps(x, indent, sortKeys, level + 1)).join(sep + nl) + end + "}";
}

export function repr(s) {
    if (Array.isArray(s)) return "[" + s.map(repr).join(", ") + "]";
    const q = s.includes("'") && !s.includes('"') ? '"' : "'";
    let o = q;
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (ch === "\\") o += "\\\\";
        else if (ch === q) o += "\\" + q;
        else if (ch === "\n") o += "\\n";
        else if (ch === "\r") o += "\\r";
        else if (ch === "\t") o += "\\t";
        else if (c < 0x20 || c === 0x7f) o += "\\x" + c.toString(16).padStart(2, "0");
        else o += ch;
    }
    return o + q;
}
