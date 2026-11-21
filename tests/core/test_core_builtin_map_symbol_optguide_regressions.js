// covers: c:src/builtins/builtin-map-symbol.c
// Split from tests/test_optguide_regressions.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_map_symbol_optguide_regressions");
export {};

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}

__kitS.section("len", () => {
    const wpad = (b, n) => { let s = b; while (s.length < n) s += "一二三四五六七八"; return s.slice(0, n); };
    const npad = (b, n) => { let s = b; while (s.length < n) s += "abcdefghijklmnop"; return s.slice(0, n); };
    const LENS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 128];
    for (const n of LENS) {
        for (const [kind, pad] of [["narrow", npad], ["wide", wpad]]) {
            const a = pad("k" + n + "_", n);
            const b = (a + "!").slice(0, n);
            ok(a === b, kind + " len " + n + ": equal strings compare equal");
            ok(!(a < b) && !(a > b), kind + " len " + n + ": equal strings do not order");
            if (n > 0) {
                const last = a.slice(0, n - 1) + (kind === "wide" ? "鿿" : "~");
                ok(a !== last, kind + " len " + n + ": differing LAST unit is unequal");
                ok(a < last, kind + " len " + n + ": ordering by the last unit");
                const first = (kind === "wide" ? "鿿" : "~") + a.slice(1);
                ok(a !== first, kind + " len " + n + ": differing FIRST unit is unequal");
                ok(a < first, kind + " len " + n + ": ordering by the first unit");
            }
        }
        if (n > 0) {
            const nar = npad("m_", n - 1) + "b";
            const wid = npad("m_", n - 1) + "一";
            eq(nar.length, wid.length, "mixed len " + n + ": equal lengths");
            ok(nar !== wid, "mixed len " + n + ": a wide unit never equals a byte");
            ok(nar < wid, "mixed len " + n + ": ordering across widths");
        }
    }
    for (const [kind, pad] of [["narrow", npad], ["wide", wpad]]) {
        const m = new Map(), keys = [];
        for (let i = 0; i < 500; i++) { const k = pad("key" + i + "_", 12); keys.push(k); m.set(k, i); }
        let hits = 0;
        for (let i = 0; i < 500; i++) if (m.get((keys[i] + "#").slice(0, 12)) === i) hits++;
        eq(hits, 500, kind + " Map keys: every distinct-object lookup hits");
        ok(!m.has(pad("absent_", 12) + "zz"), kind + " Map: an absent key misses");
    }
    const ws = ["二", "一", "三z", "三a", "鿿"];
    eqj(ws.slice().sort(), ["一", "三a", "三z", "二", "鿿"],
        "wide sort matches UTF-16 code-unit order (4E00 4E09 4E8C 9FFF)");
});
__kitS.end();
print("test_core_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
