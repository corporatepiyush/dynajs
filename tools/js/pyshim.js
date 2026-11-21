import * as os from "os";
import * as std from "std";
import { latin1ToUtf8 } from "dyna:bytes";

const S_IFMT = os.S_IFMT, S_IFDIR = os.S_IFDIR, S_IFREG = os.S_IFREG, S_IFLNK = os.S_IFLNK;

export function listdir(p) {
    const [names, err] = os.readdir(p === "" ? "." : p);
    if (err) return null;
    const out = [];
    for (const n of names) if (n !== "." && n !== "..") out.push(n);
    return out;
}

export function isdir(p) {
    const [st, err] = os.stat(p);
    return !err && (st.mode & S_IFMT) === S_IFDIR;
}

export function isfile(p) {
    const [st, err] = os.stat(p);
    return !err && (st.mode & S_IFMT) === S_IFREG;
}

export function islink(p) {
    const [st, err] = os.lstat(p);
    return !err && (st.mode & S_IFMT) === S_IFLNK;
}

export function exists(p) {
    const [, err] = os.stat(p);
    return !err;
}

export function getmtime(p) {
    const [st, err] = os.stat(p);
    if (err) throw new Error("stat failed: " + p);
    return st.mtime / 1000;
}

export function join(a, ...rest) {
    let p = a;
    for (const b of rest) {
        if (b.startsWith("/")) p = b;
        else if (p === "" || p.endsWith("/")) p += b;
        else p += "/" + b;
    }
    return p;
}

export function* walk(top) {
    const names = listdir(top);
    if (names === null) return;
    const dirs = [], files = [];
    for (const n of names) {
        if (isdir(join(top, n))) dirs.push(n);
        else files.push(n);
    }
    yield [top, dirs, files];
    for (const d of dirs) {
        const np = join(top, d);
        if (!islink(np)) yield* walk(np);
    }
}

export function basename(p) {
    const i = p.lastIndexOf("/");
    return i < 0 ? p : p.slice(i + 1);
}

export function dirname(p) {
    const i = p.lastIndexOf("/");
    if (i < 0) return "";
    let head = p.slice(0, i + 1);
    if (head && head !== "/".repeat(head.length)) head = head.replace(/\/+$/, "");
    return head;
}

export function normpath(p) {
    if (p === "") return ".";
    const initial = p.startsWith("/") ? (p.startsWith("//") && !p.startsWith("///") ? 2 : 1) : 0;
    const comps = p.split("/");
    const out = [];
    for (const c of comps) {
        if (c === "" || c === ".") continue;
        if (c !== ".." || (!initial && !out.length) || (out.length && out[out.length - 1] === "..")) out.push(c);
        else if (out.length) out.pop();
    }
    let r = out.join("/");
    if (initial) r = "/".repeat(initial) + r;
    return r || ".";
}

let cwdCache = null;
export function abspath(p) {
    if (!p.startsWith("/")) {
        if (cwdCache === null) cwdCache = os.getcwd()[0];
        p = join(cwdCache, p);
    }
    return normpath(p);
}

export function relpath(path, start = ".") {
    const s = abspath(start).split("/").filter(Boolean);
    const t = abspath(path).split("/").filter(Boolean);
    let i = 0;
    while (i < s.length && i < t.length && s[i] === t[i]) i++;
    const rel = [];
    for (let k = i; k < s.length; k++) rel.push("..");
    for (let k = i; k < t.length; k++) rel.push(t[k]);
    return rel.length ? rel.join("/") : ".";
}

export function readBytes(p) {
    const f = std.open(p, "rb");
    if (!f) throw new Error("cannot open " + p);
    f.seek(0, std.SEEK_END);
    const n = f.tell();
    f.seek(0, std.SEEK_SET);
    const b = f.readBytes(n);
    f.close();
    return b;
}

const utf8Dec = new TextDecoder("utf-8");

export function readText(p, mode = "utf8") {
    if (mode === "latin1") return utf8Dec.decode(latin1ToUtf8(readBytes(p)));
    const s = std.loadFile(p);
    if (s === null) throw new Error("cannot open " + p);
    if (mode === "utf8-ignore") return s.indexOf("�") < 0 ? s : utf8Dec.decode(readBytes(p)).replaceAll("�", "");
    if (mode === "utf8-replace") return s.indexOf("�") < 0 && s.isWellFormed() ? s : utf8Dec.decode(readBytes(p));
    return s;
}

export function writeText(p, s) {
    const f = std.open(p, "w");
    if (!f) throw new Error("cannot open " + p);
    f.puts(s);
    f.close();
}

const MAGIC = /[*?[]/;
export function hasMagic(s) {
    return MAGIC.test(s);
}

export function fnmatchRegex(pat) {
    let i = 0, res = "";
    const n = pat.length;
    while (i < n) {
        const c = pat[i++];
        if (c === "*") {
            if (!res.endsWith("[\\s\\S]*")) res += "[\\s\\S]*";
        } else if (c === "?") res += "[\\s\\S]";
        else if (c === "[") {
            let j = i;
            if (j < n && pat[j] === "!") j++;
            if (j < n && pat[j] === "]") j++;
            while (j < n && pat[j] !== "]") j++;
            if (j >= n) res += "\\[";
            else {
                let stuff = pat.slice(i, j).replaceAll("\\", "\\\\");
                i = j + 1;
                if (stuff[0] === "!") stuff = "^" + stuff.slice(1);
                else if (stuff[0] === "^") stuff = "\\" + stuff;
                res += "[" + stuff + "]";
            }
        } else res += c.replace(/[.*+?^${}()|[\]\\\/]/g, "\\$&");
    }
    return new RegExp("^" + res + "$");
}

function glob1(dir, pat, dironly) {
    const names = listdir(dir || ".");
    if (names === null) return [];
    const re = fnmatchRegex(pat);
    const hidden = pat[0] === ".";
    const out = [];
    for (const n of names) {
        if (!hidden && n[0] === ".") continue;
        if (dironly && !isdir(join(dir || ".", n))) continue;
        if (re.test(n)) out.push(n);
    }
    return out;
}

function rlistdir(dir, dironly) {
    const names = listdir(dir || ".");
    if (names === null) return [];
    const out = [];
    for (const n of names) {
        if (n[0] === ".") continue;
        const p = dir ? join(dir, n) : n;
        const d = isdir(p);
        if (dironly && !d) continue;
        out.push(n);
        if (d && !islink(p)) for (const y of rlistdir(p, dironly)) out.push(join(n, y));
    }
    return out;
}

function glob2(dir, pat, dironly) {
    const out = [""];
    for (const x of rlistdir(dir, dironly)) out.push(x);
    return out;
}

export function glob(pattern, recursive = false) {
    return iglob(pattern, recursive, false);
}

function iglob(pathname, recursive, dironly) {
    const dn = dirname(pathname), bn = basename(pathname);
    if (!hasMagic(pathname)) {
        if (bn ? exists(pathname) : isdir(dn)) return [pathname];
        return [];
    }
    if (!dn) {
        if (recursive && bn === "**") return glob2(dn, bn, dironly);
        return glob1(dn, bn, dironly);
    }
    let dirs;
    if (dn !== pathname && hasMagic(dn)) dirs = iglob(dn, recursive, true);
    else dirs = [dn];
    const out = [];
    for (const d of dirs) {
        let names;
        if (hasMagic(bn)) names = recursive && bn === "**" ? glob2(d, bn, dironly) : glob1(d, bn, dironly);
        else names = (bn ? exists(join(d, bn)) : isdir(d)) ? [bn] : [];
        for (const n of names) out.push(n === "" ? join(d, "") : join(d, n));
    }
    return out;
}

export function pyRepr(s) {
    let q = "'";
    if (s.includes("'") && !s.includes('"')) q = '"';
    let out = q;
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (ch === q || ch === "\\") out += "\\" + ch;
        else if (ch === "\n") out += "\\n";
        else if (ch === "\r") out += "\\r";
        else if (ch === "\t") out += "\\t";
        else if (c < 0x20 || c === 0x7f) out += "\\x" + c.toString(16).padStart(2, "0");
        else if (c >= 0x80 && c < 0xa0) out += "\\x" + c.toString(16).padStart(2, "0");
        else out += ch;
    }
    return out + q;
}

export class Out {
    constructor() {
        this.parts = [];
    }
    print(...a) {
        this.parts.push(a.join(" ") + "\n");
    }
    flush(f = std.out) {
        f.puts(this.parts.join(""));
        f.flush();
        this.parts = [];
    }
}

export function pyStrip(s, chars) {
    if (chars === undefined) return s.replace(/^[\s\x1c-\x1f\x85]+|[\s\x1c-\x1f\x85]+$/g, "");
    let a = 0, b = s.length;
    while (a < b && chars.includes(s[a])) a++;
    while (b > a && chars.includes(s[b - 1])) b--;
    return s.slice(a, b);
}

export function pySplitlines(s) {
    const out = s.split(/\r\n|[\n\r\v\f\x1c\x1d\x1e\x85\u2028\u2029]/);
    if (out.length && out[out.length - 1] === "") out.pop();
    return out;
}

export function cmpStr(a, b) {
    return a < b ? -1 : a > b ? 1 : 0;
}

export function cmpTuple(a, b) {
    const n = Math.min(a.length, b.length);
    for (let i = 0; i < n; i++) {
        const x = a[i], y = b[i];
        if (x < y) return -1;
        if (x > y) return 1;
    }
    return a.length - b.length;
}
