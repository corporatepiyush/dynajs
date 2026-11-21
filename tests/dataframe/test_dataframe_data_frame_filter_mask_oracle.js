// covers: dyna:dataframe.DataFrame
// FILTER builds its row list with a branchless compaction (store the row id, advance by mask[i] != 0), so the
// row list must hold exactly the rows whose mask byte is nonzero, in order, for any byte value and density.
// The oracle selects the rows in JS.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_filter_mask_oracle");
import { DataFrame } from "dyna:dataframe";

let checks = 0, fails = 0;
function eqArr(got, want, what) {
    checks++;
    let ok = got.length === want.length;
    for (let i = 0; ok && i < want.length; i++) ok = Object.is(got[i], want[i]);
    if (!ok) { fails++; print("FAIL: " + what + ": got [" + Array.from(got).slice(0, 12) + "] (" + got.length + "), want [" + want.slice(0, 12) + "] (" + want.length + ")"); }
}
let seed = 0x1234567;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
function check(n, density, what) {
    const v = new Float64Array(n), id = new Int32Array(n), m = new Uint8Array(n);
    for (let i = 0; i < n; i++) { v[i] = i * 1.5 - 7; id[i] = i; m[i] = rnd() < density ? 1 + ((rnd() * 255) | 0) : 0; }
    const want = [], wantV = [];
    for (let i = 0; i < n; i++) if (m[i] !== 0) { want.push(i); wantV.push(v[i]); }
    const out = new DataFrame({ v, id }).FILTER(m).TO_COLUMNS();
    eqArr(out.id, want, what + " ids");
    eqArr(out.v, wantV, what + " values");
}

__kitS.section("random masks of every density keep exactly the nonzero rows in order", () => {
    for (const n of [1, 2, 5, 64, 1000, 65537])
        for (const d of [0, 0.02, 0.5, 0.98, 1]) check(n, d, "n=" + n + " density=" + d);
});

__kitS.section("a mask whose last byte is the only nonzero keeps the last row", () => {
    const n = 9, m = new Uint8Array(n); m[n - 1] = 200;
    const out = new DataFrame({ id: Int32Array.from({ length: n }, (_, i) => i) }).FILTER(m).TO_COLUMNS();
    eqArr(out.id, [n - 1], "last row only");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_dataframe_data_frame_filter_mask_oracle: " + checks + " checks passed");
__kitS.end();
