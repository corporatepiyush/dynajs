// Split from tests/test_optguide_regressions.js: the dataframe sections.
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
{
    const v = new Float64Array(600); v.fill(1); v[0] = 1e308; v[1] = 1e308;
    const df = new DataFrame({ v: v });
    for (const w of [255, 256, 257]) {
        const r = df.ROLLING_SUM("v", w);
        eq(r[599], w, "roll_sum w=" + w + ": a departed 1e308 must not poison the tail");
        ok(isFinite(r[599]), "roll_sum w=" + w + " tail is finite");
    }
    const r = df.ROLLING_SUM("v", 256);
    ok(r[255] > 1e307, "the window that really does overflow still reports it");

    const u = new Float64Array(2000);
    for (let i = 0; i < u.length; i++) u[i] = ((i * 7919) % 1000) / 7;
    const d2 = new DataFrame({ u: u });
    const refmax = (arr, w) => {
        let worst = 0;
        for (let i = w + 40; i < u.length; i++) {
            let s = 0; for (let k = i - w + 1; k <= i; k++) s += u[k];
            worst = Math.max(worst, Math.abs(arr[i] - s) / Math.abs(s || 1));
        }
        return worst;
    };
    const e255 = refmax(d2.ROLLING_SUM("u", 255), 255);
    const e256 = refmax(d2.ROLLING_SUM("u", 256), 256);
    ok(e255 < 1e-12, "w=255 exact path matches an independent sum (rel " + e255 + ")");
    ok(e256 < 1e-12, "w=256 block path matches an independent sum (rel " + e256 + ")");

    const w2 = new Float64Array(800); w2.fill(2); w2[0] = Infinity;
    const d3 = new DataFrame({ w: w2 });
    const r3 = d3.ROLLING_SUM("w", 256);
    ok(r3[255] === Infinity, "a window containing Inf reports Inf");
    eq(r3[799], 512, "a window past the Inf is unaffected");

    const w3 = new Float64Array(800); w3.fill(2); w3[0] = NaN;
    const r4 = new DataFrame({ w: w3 }).ROLLING_SUM("w", 256);
    ok(Number.isNaN(r4[255]), "a window containing NaN reports NaN");
    eq(r4[799], 512, "a window past the NaN is unaffected");

    const rm = df.ROLLING_MEAN("v", 256);
    eq(rm[599], 1, "ROLLING_MEAN recovers after the departed 1e308");
}
print("test_dataframe_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
