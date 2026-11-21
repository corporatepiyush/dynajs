// flags: --std
// tools/js/numeric-audit.js: the port of tools/numeric-audit.py (deleted); its output was proven identical, apart from naming itself
// tools/js/numeric-audit.js -- scan C sources for integer/float/string misuse patterns.
//
// High-signal detectors, each mapping to a real bug class this project has met
// or that CERT/CWE name:
//
//   alloc-mul      malloc/calloc/realloc size argument multiplies two variables;
//                  needs an explicit bound check nearby or it is CVE-shaped
//   narrow-len     (int)/(uint32_t)/(int32_t) cast applied to an expression that
//                  mentions len/count/size/n -- silent truncation at 2^31
//   f2i-cast       (int)(...) around a floating expression: UB when out of int
//                  range, silent truncation otherwise
//   float-eq       == / != between float-ish operands (not vs 0/NULL literal)
//   nan-filter     !(x > t) / !(x < t) style negated compares on doubles: NaN
//                  passes BOTH arms of a split filter and writes OOB (met here)
//   char-index     table[<char var>] indexing without an (unsigned char) cast:
//                  signed-char UB / negative index for bytes >= 0x80
//
// Usage: ./dynajs --std tools/js/numeric-audit.js [root]   (default src/)
// Exit is always 0: this is a review list, not a gate.
import { walk, join, readText, pyStrip, Out } from "./pyshim.js";
import { pySlice } from "./pystr.js";

const ROOT = scriptArgs.length > 1 ? scriptArgs[1] : "src";

const ALLOC = /\b(malloc|calloc|realloc)\s*\(/;
const MUL_IN_CALL = /[\w.\)\]]+\s*\*\s*[\w.\(]+/;
const NARROW = /\((?:uint32_t|int32_t|int|unsigned(?:\s+int)?)\)\s*\(?[^)]*\b(?:len|count|size|total|nrows|rowcount)\w*/i;
const F2I = /\(int(?:32_t)?\)\s*\(?\s*(?:[a-z_]*\((?:float|double)|[a-z0-9_]*(?:floor|ceil|round|sqrt|pow|log|exp)\b|[a-z_]*(?:_f|_d)\b)/;
const FLOAT_EQ = /[a-zA-Z_][a-zA-Z0-9_]*(?:f|d)\b\s*(==|!=)\s*(?!0\b|NULL\b)[0-9]/;
const NAN_FILTER = /!\s*\(\s*[A-Za-z_]\w*\s*[<>]=?\s*[A-Za-z0-9_.]+\s*\)/;
const CHAR_DECL = /\b(?:signed\s+)?char\s+([a-z]\w*)\s*(?:=|;|\[)/gi;
const CTX_OK = /MAX|LIMIT|cap\b|overflow|SIZE_MAX|UINT32_MAX|too large|exceed/i;
const UNSIGNED_CHAR = /\bunsigned\s+char\b/;
const LT_GT = />|</;
const idxCache = new Map();

function idxRe(cv) {
    let r = idxCache.get(cv);
    if (!r) idxCache.set(cv, (r = new RegExp("\\[\\s*" + cv + "\\s*\\]")));
    return r;
}

function scan(path, text) {
    const hits = [];
    const lines = text.split("\n");
    const charvars = new Set();
    for (const l of lines) {
        for (const m of l.matchAll(CHAR_DECL)) {
            if (UNSIGNED_CHAR.test(l)) continue;
            charvars.add(m[1]);
        }
    }
    for (let i = 1; i <= lines.length; i++) {
        const l = lines[i - 1];
        const t = pyStrip(l);
        if (!t || t.startsWith("*") || t.startsWith("/*") || t.startsWith("//") || t.startsWith("#")) continue;
        const snip = pySlice(t, 110);
        if (ALLOC.test(l) && MUL_IN_CALL.test(l)) {
            const ctx = lines.slice(Math.max(0, i - 3), i + 2).join("\n");
            if (!CTX_OK.test(ctx)) hits.push([i, "alloc-mul", snip]);
        }
        if (NARROW.test(l)) hits.push([i, "narrow-len", snip]);
        if (F2I.test(l)) hits.push([i, "f2i-cast", snip]);
        if (FLOAT_EQ.test(l) && !l.includes("const")) hits.push([i, "float-eq", snip]);
        if (NAN_FILTER.test(l) && LT_GT.test(l)) hits.push([i, "nan-filter", snip]);
        for (const cv of charvars) {
            if (idxRe(cv).test(l) && !l.includes("(unsigned char)") && !l.includes("(uint8_t)") && !l.includes("unsigned")) {
                hits.push([i, "char-index", snip]);
                break;
            }
        }
    }
    return hits;
}

function main() {
    const o = new Out();
    const total = new Map();
    for (const [root, , files] of walk(ROOT)) {
        files.sort();
        for (const f of files) {
            if (!(f.endsWith(".c") || f.endsWith(".h"))) continue;
            const p = join(root, f);
            let text;
            try {
                text = readText(p, "utf8-ignore");
            } catch (e) {
                continue;
            }
            for (const [ln, kind, snip] of scan(p, text)) {
                let a = total.get(kind);
                if (!a) total.set(kind, (a = []));
                a.push(`${p}:${ln}: ${snip}`);
            }
        }
    }
    let grand = 0;
    for (const kind of [...total.keys()].sort()) {
        const rows = total.get(kind);
        grand += rows.length;
        o.print(`\n=== ${kind}: ${rows.length} ===`);
        for (const r of rows.slice(0, 40)) o.print("  " + r);
        if (rows.length > 40) o.print(`  ... and ${rows.length - 40} more`);
    }
    o.print(`\ntotal findings: ${grand} (review list, not a gate)`);
    o.flush();
}
main();
