// flags: --std
// tools/js/gen-charsets.js -- regenerate src/dyna-charsets.h from the Unicode single-byte mappings
// (read from Python's codec tables: data, not code; PY_ENCODINGS_DIR overrides the search).
// Committed generator + committed output: the build never reaches the network. It emits exactly
// the committed header (comment-free, clang-formatted); tools/js/check-generated.js re-derives and
// compares, so a hand edit of the header shows up as drift.
//
//     ./dynajs --std tools/js/gen-charsets.js > src/dyna-charsets.h
import * as std from "std";
import { readText, glob, isdir } from "./pyshim.js";

const CHARSETS = [
    ["windows-1252", "cp1252"], ["windows-1251", "cp1251"],
    ["windows-1250", "cp1250"], ["windows-1253", "cp1253"],
    ["windows-1254", "cp1254"], ["windows-1255", "cp1255"],
    ["windows-1256", "cp1256"], ["windows-1257", "cp1257"],
    ["windows-1258", "cp1258"],
    ["iso-8859-2", "iso8859_2"], ["iso-8859-3", "iso8859_3"],
    ["iso-8859-4", "iso8859_4"], ["iso-8859-5", "iso8859_5"],
    ["iso-8859-6", "iso8859_6"], ["iso-8859-7", "iso8859_7"],
    ["iso-8859-8", "iso8859_8"], ["iso-8859-9", "iso8859_9"],
    ["iso-8859-10", "iso8859_10"], ["iso-8859-13", "iso8859_13"],
    ["iso-8859-14", "iso8859_14"], ["iso-8859-15", "iso8859_15"],
    ["iso-8859-16", "iso8859_16"],
    ["koi8-r", "koi8_r"], ["koi8-u", "koi8_u"],
    ["macintosh", "mac_roman"], ["ibm866", "cp866"],
];

function encodingsDir() {
    const env = std.getenv("PY_ENCODINGS_DIR");
    if (env) return env;
    const cands = [
        ...glob("/opt/homebrew/opt/python@3*/Frameworks/Python.framework/Versions/3*/lib/python3*/encodings"),
        ...glob("/usr/local/opt/python@3*/Frameworks/Python.framework/Versions/3*/lib/python3*/encodings"),
        ...glob("/usr/lib/python3*/encodings"),
    ].sort();
    for (let i = cands.length - 1; i >= 0; i--) if (isdir(cands[i])) return cands[i];
    throw new Error("no Python encodings directory found; set PY_ENCODINGS_DIR");
}

const ESC = /\\(x[0-9a-fA-F]{2}|u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[\\'"nrtabfv0])/g;
function unescape(s) {
    return s.replace(ESC, (m, e) => {
        const c = e[0];
        if (c === "x" || c === "u" || c === "U") return String.fromCodePoint(parseInt(e.slice(1), 16));
        return { "\\": "\\", "'": "'", '"': '"', n: "\n", r: "\r", t: "\t", a: "\x07", b: "\b", f: "\f", v: "\v", 0: "\0" }[c];
    });
}

function decodingTable(dir, pyname) {
    const src = readText(dir + "/" + pyname + ".py");
    const a = src.indexOf("\ndecoding_table = (");
    const b = src.indexOf("\n)", a);
    if (a < 0 || b < 0) throw new Error(pyname + ": no decoding_table");
    const table = [];
    for (const line of src.slice(a, b).split("\n")) {
        const m = /^\s*(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)")/.exec(line);
        if (m) table.push(unescape(m[1] ?? m[2]));
    }
    if (table.length !== 256) throw new Error(pyname + ": decoding_table has " + table.length + " entries");
    return table;
}

const hex4 = (c) => "0x" + c.toString(16).toUpperCase().padStart(4, "0");
const hex2 = (c) => "0x" + c.toString(16).toUpperCase().padStart(2, "0");

export function generate() {
    const dir = encodingsDir();
    const out = [];
    const w = (s) => out.push(s);
    const names = [], fwd = {};
    for (const [label, pyname] of CHARSETS) {
        const ident = label.replaceAll("-", "_");
        const t = decodingTable(dir, pyname);
        const row = [];
        for (let b = 0x80; b < 0x100; b++) {
            const ch = t[b];
            let cp = ch === "\ufffe" || ch.length === 0 ? 0xffff : ch.codePointAt(0);
            if (cp > 0xffff) cp = 0xffff;
            row.push(cp);
        }
        w(`static const uint16_t dyn_cs_${ident}[128] = {\n`);
        for (const c of row) w("    " + hex4(c) + ",\n");
        w("};\n\n");
        fwd[ident] = row;
        names.push([label, ident]);
    }
    for (const [, ident] of names) {
        const row = [];
        for (let b = 0x80; b < 0x100; b++) {
            const cp = fwd[ident][b - 0x80];
            if (cp !== 0xffff) row.push([cp, b]);
        }
        row.sort((x, y) => x[0] - y[0] || x[1] - y[1]);
        w(`static const uint16_t dyn_rev_${ident}[${row.length}][2] = {\n`);
        for (const [c, b] of row) w(`    { ${hex4(c)}, ${hex2(b)} },\n`);
        w("};\n\n");
    }
    w("typedef struct {\n    const char* label;\n    const uint16_t* hi;\n    const uint16_t (*rev)[2];\n    unsigned n_rev;\n} dyn_charset_t;\n\n");
    w("static const dyn_charset_t DYN_CHARSETS[] = {\n");
    for (const [label, ident] of names)
        w(`    { "${label}", dyn_cs_${ident}, dyn_rev_${ident}, (unsigned)(sizeof dyn_rev_${ident} / sizeof dyn_rev_${ident}[0]) },\n`);
    w('    { "iso-8859-1", NULL, NULL, 0 },\n');
    w('    { "us-ascii", NULL, NULL, 0 },\n');
    w("};\n");
    return out.join("");
}

if (/gen-charsets\.js$/.test(scriptArgs[0] || "")) std.out.puts(generate());
