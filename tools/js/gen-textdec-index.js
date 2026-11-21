// flags: --std
// tools/js/gen-textdec-index.js -- regenerate src/core/dyn-textdec-index.h, the WHATWG Encoding Standard label
// table and decoder indexes used by TextDecoder's legacy decoders (src/core/dyn-textdec.c).
//
//     ./dynajs --std tools/js/gen-textdec-index.js > src/core/dyn-textdec-index.h
//
// Sources (data, not code; the build never reads them -- the header is committed and
// tools/js/check-generated.js re-derives it when the sources are present):
//   WPT_DIR (default: the main checkout's third_party/wpt, commit 6c8a4923), copies of the WHATWG indexes.json:
//     encoding/resources/encodings.js                        names and labels (encodings.json)
//     encoding/resources/single-byte-decoder.js              every single-byte index
//     encoding/legacy-mb-japanese/euc-jp/jis0208_index.js    index jis0208
//     encoding/legacy-mb-japanese/euc-jp/jis0212_index.js    index jis0212
//     encoding/legacy-mb-korean/euc-kr/euckr_index.js        index EUC-KR
//     encoding/legacy-mb-tchinese/big5/big5_index.js         index Big5
//     encoding/legacy-mb-schinese/gb18030/resources/ranges.js  index gb18030 ranges (GB18030-2022 revision)
//   GO_XTEXT_DIR (default: the newest ~/go/pkg/mod/golang.org/x/text@*): encoding/simplifiedchinese/tables.go,
//     whose `decode` table is index-gb18030 as generated from encoding.spec.whatwg.org before the GB18030-2022
//     revision; the 18 two-byte pointers that revision changed are taken from WPT's gb18030-decoder.any.js
//     ("GB18030-2022 1".."18") and laid over it.
//   the PUA entries x/text leaves out come from `iconv -f GB18030` (see puaFill), after WPT gbk-decoder.any.js pins.
import * as std from "std";
import * as os from "os";
import * as sysmod from "dyna:sys";

const WPT = (std.getenv("WPT_DIR") || "/Users/piyush/ai/dynascript/third_party/wpt") + "/encoding/";

function src(path) {
    const t = std.loadFile(path);
    if (t === null) throw new Error("textdec index sources missing: " + path + " (set WPT_DIR / GO_XTEXT_DIR)");
    return t;
}

function jsArray(text, name) {
    const m = new RegExp("var\\s+" + name + "\\s*=\\s*\\[").exec(text);
    if (!m) throw new Error("no array " + name);
    const a = m.index + m[0].length - 1;
    const b = text.indexOf("]", a);
    return JSON.parse(text.slice(a, b + 1));
}

function goDir() {
    const env = std.getenv("GO_XTEXT_DIR");
    if (env) return env;
    const base = (std.getenv("HOME") || "") + "/go/pkg/mod/golang.org/x";
    const [names] = os.readdir(base);
    const vs = (names || []).filter((n) => /^text@v\d+\.\d+\.\d+$/.test(n))
        .sort((x, y) => { const p = (s) => s.slice(6).split(".").map(Number); const [a, b] = [p(x), p(y)]; return a[0] - b[0] || a[1] - b[1] || a[2] - b[2]; });
    if (!vs.length) throw new Error("textdec index sources missing: no golang.org/x/text module (set GO_XTEXT_DIR)");
    return base + "/" + vs[vs.length - 1];
}

function gbIndex() {
    const t = src(goDir() + "/encoding/simplifiedchinese/tables.go");
    const a = t.indexOf("\nvar decode = [...]uint16{");
    const b = t.indexOf("\n}", a);
    if (a < 0 || b < 0) throw new Error("tables.go: no decode table");
    const idx = [];
    for (const m of t.slice(a, b).matchAll(/(\d+):\s*0x([0-9a-fA-F]+)/g)) idx[+m[1]] = parseInt(m[2], 16);
    const test = src(WPT + "legacy-mb-schinese/gb18030/gb18030-decoder.any.js");
    let n = 0;
    for (const m of test.matchAll(/decode\(\[0x([0-9A-F]{2}), 0x([0-9A-F]{2})\], "\\u([0-9A-F]{4})", "GB18030-2022 \d+"\)/gi)) {
        const lead = parseInt(m[1], 16), byte = parseInt(m[2], 16);
        idx[(lead - 0x81) * 190 + byte - (byte < 0x7f ? 0x40 : 0x41)] = parseInt(m[3], 16);
        n++;
    }
    if (n !== 18) throw new Error("gb18030-decoder.any.js: expected 18 GB18030-2022 two-byte cases, found " + n);
    const gbk = src(WPT + "legacy-mb-schinese/gbk/gbk-decoder.any.js");
    const ptrs = JSON.parse("[" + /const gbkPointers = \[([^\]]*)\]/.exec(gbk)[1].replace(/,\s*$/, "") + "]");
    const cps = /const codePoints = \[([^\]]*)\]/.exec(gbk)[1].match(/0x[0-9a-fA-F]+/g).map(Number);
    if (ptrs.length !== cps.length) throw new Error("gbk-decoder.any.js: pointer/code point lists differ");
    ptrs.forEach((p, i) => { idx[p] = cps[i]; });
    const missing = [];
    for (let i = 0; i < 23940; i++) if (!idx[i]) missing.push(i);
    const pua = puaFill(missing);
    missing.forEach((p, i) => { idx[p] = pua[i]; });
    const out = [];
    for (let i = 0; i < 23940; i++) out.push(idx[i] || 0);
    if (out.some((v) => !v)) throw new Error("gb18030 index still has unmapped pointers");
    return out;
}

// x/text leaves out every private-use mapping of index-gb18030 (the user-defined blocks and the 0xE766.. run).
// They follow GB18030's own table, which the system iconv implements: each missing pointer is decoded with
// `iconv -f GB18030` and must land in the PUA. Pointers WPT pins (gbk-decoder.any.js) were laid over first.
function puaFill(pointers) {
    const bytes = new Uint8Array(pointers.length * 2);
    pointers.forEach((p, i) => {
        const lead = 0x81 + Math.floor(p / 190), o = p % 190;
        bytes[2 * i] = lead;
        bytes[2 * i + 1] = o + (o < 0x3f ? 0x40 : 0x41);
    });
    const r = sysmod.Exec("iconv", ["-f", "GB18030", "-t", "UTF-32BE"], { input: bytes, encoding: "bytes" });
    if (r.code !== 0 || r.stdout.length !== pointers.length * 4)
        throw new Error("textdec index sources missing: iconv -f GB18030 failed (" + r.code + ")");
    const out = [];
    for (let i = 0; i < pointers.length; i++) {
        const c = (r.stdout[4 * i] << 24) | (r.stdout[4 * i + 1] << 16) | (r.stdout[4 * i + 2] << 8) | r.stdout[4 * i + 3];
        if (c < 0xe000 || c > 0xf8ff) throw new Error("iconv maps gb18030 pointer " + pointers[i] + " outside the PUA: " + c.toString(16));
        out.push(c);
    }
    return out;
}

function gbRanges() {
    const t = src(WPT + "legacy-mb-schinese/gb18030/resources/ranges.js");
    const out = [];
    for (const m of t.matchAll(/\[(\d+), "\\u([0-9A-Fa-f]{4})"\]/g)) out.push([+m[1], parseInt(m[2], 16)]);
    if (out.length < 200) throw new Error("ranges.js: only " + out.length + " ranges");
    return out;
}

function table() {
    const t = src(WPT + "resources/encodings.js");
    const a = t.indexOf("[");
    const b = t.lastIndexOf("]");
    return JSON.parse(t.slice(a, b + 1));
}

function singleByte() {
    const t = src(WPT + "resources/single-byte-decoder.js");
    const a = t.indexOf("singleByteIndexes = {");
    const b = t.indexOf("\n}", a);
    return JSON.parse(t.slice(a + "singleByteIndexes = ".length, b + 2));
}

const ident = (name) => "DYN_TD_" + name.toUpperCase().replace(/[^A-Z0-9]/g, "_");

function rows(values, per, fmt) {
    const out = [];
    for (let i = 0; i < values.length; i += per) out.push("    " + values.slice(i, i + per).map(fmt).join(", ") + ",");
    return out;
}
const h4 = (v) => "0x" + v.toString(16).toUpperCase().padStart(4, "0");
const h5 = (v) => "0x" + v.toString(16).toUpperCase().padStart(5, "0");

export function generate() {
    const groups = table();
    const sb = singleByte();
    const encs = [];
    const labels = [];
    for (const g of groups)
        for (const e of g.encodings) {
            const id = encs.length;
            encs.push({ name: e.name, single: g.heading === "Legacy single-byte encodings" });
            for (const l of e.labels) labels.push([l, id]);
        }
    labels.sort((x, y) => (x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0));
    const out = [];
    const w = (s) => out.push(s);
    w("#ifndef DYN_TEXTDEC_INDEX_H");
    w("#define DYN_TEXTDEC_INDEX_H");
    w("");
    w("#include <stdint.h>");
    w("");
    encs.forEach((e, i) => w(`#define ${ident(e.name)} ${i}`));
    w(`#define DYN_TD_COUNT ${encs.length}`);
    w(`#define DYN_TD_LABEL_COUNT ${labels.length}`);
    w("");
    w("static const char* const dyn_td_names[DYN_TD_COUNT] = {");
    for (const e of encs) w(`    "${e.name.toLowerCase()}",`);
    w("};");
    w("");
    w("static const struct {");
    w("    const char* label;");
    w("    uint8_t enc;");
    w("} dyn_td_labels[DYN_TD_LABEL_COUNT] = {");
    for (const [l, id] of labels) w(`    { "${l}", ${ident(encs[id].name)} },`);
    w("};");
    w("");
    const sbList = encs.map((e, i) => [e, i]).filter(([e]) => e.single);
    w("static const int8_t dyn_td_sb_slot[DYN_TD_COUNT] = {");
    let k = 0;
    w("    " + encs.map((e) => (e.single ? k++ : -1)).join(", ") + ",");
    w("};");
    w("");
    w(`static const uint16_t dyn_td_sb[${sbList.length}][128] = {`);
    for (const [e] of sbList) {
        const idx = sb[e.name === "ISO-8859-8-I" ? "ISO-8859-8" : e.name];
        if (!idx || idx.length !== 128) throw new Error("single-byte index " + e.name + " missing");
        w("    {");
        for (const r of rows(idx.map((v) => v ?? 0), 8, h4)) w("    " + r);
        w("    },");
    }
    w("};");
    w("");
    const mb = [
        ["jis0208", jsArray(src(WPT + "legacy-mb-japanese/euc-jp/jis0208_index.js"), "jis0208"), 16],
        ["jis0212", jsArray(src(WPT + "legacy-mb-japanese/euc-jp/jis0212_index.js"), "jis0212"), 16],
        ["euckr", jsArray(src(WPT + "legacy-mb-korean/euc-kr/euckr_index.js"), "euckr"), 16],
        ["gb18030", gbIndex(), 16],
    ];
    for (const [name, arr, per] of mb) {
        if (arr.some((v) => v !== null && v > 0xffff)) throw new Error(name + " has a non-BMP entry");
        w(`#define DYN_TD_${name.toUpperCase()}_N ${arr.length}`);
        w(`static const uint16_t dyn_td_${name}[${arr.length}] = {`);
        for (const r of rows(arr.map((v) => v ?? 0), per, h4)) w(r);
        w("};");
        w("");
    }
    const big5 = jsArray(src(WPT + "legacy-mb-tchinese/big5/big5_index.js"), "big5");
    w(`#define DYN_TD_BIG5_N ${big5.length}`);
    w(`static const uint32_t dyn_td_big5[${big5.length}] = {`);
    for (const r of rows(big5.map((v) => v ?? 0), 12, h5)) w(r);
    w("};");
    w("");
    const ranges = gbRanges();
    w(`#define DYN_TD_GB_RANGES_N ${ranges.length}`);
    w(`static const uint32_t dyn_td_gb_ranges[${ranges.length}][2] = {`);
    for (const [p, c] of ranges) w(`    { ${p}, ${h4(c)} },`);
    w("};");
    w("");
    w("#endif");
    return out.join("\n") + "\n";
}

if (scriptArgs[0] && scriptArgs[0].endsWith("gen-textdec-index.js")) std.out.puts(generate());
