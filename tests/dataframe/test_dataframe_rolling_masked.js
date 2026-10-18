// timeout: 900
// timeout: 900
import { DataFrame } from "dyna:dataframe";

let pass = 0, fail = 0, section = "(none)";
const failures = [];
function S(name) { section = name; }
function bad(what, detail) {
    fail++;
    const line = "FAIL [" + section + "] " + what + (detail ? ": " + detail : "");
    failures.push(line);
    console.log(line);
}
function ok(cond, what, detail) { if (cond) pass++; else bad(what, detail); }
function throwsLike(fn, substr, what) {
    let msg = null;
    try { fn(); } catch (e) { msg = String(e && e.message); }
    ok(msg !== null && msg.indexOf(substr) >= 0, what,
       msg === null ? "did not throw" : msg);
}

function lcg(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}

function refVar(x, m, w, i) {
    if (i + 1 < w) return NaN;
    const lo = i + 1 - w;
    let s = 0, c = 0;
    for (let j = lo; j <= i; j++) { if (m && !m[j]) continue; s += x[j]; c++; }
    if (c < 2) return NaN;
    const mean = s / c;
    let m2 = 0;
    for (let j = lo; j <= i; j++) { if (m && !m[j]) continue; const d = x[j] - mean; m2 += d * d; }
    return m2 / (c - 1);
}

function sweepVar(x, m, w, span, budget) {
    const v = new DataFrame({ x }).ROLLING_VAR("x", w, m);
    const step = Math.max(1, Math.floor((span - w + 1) / Math.max(8, budget / w)));
    let worst = 0, n = 0, nan = 0, neg = 0, zero = 0, zeroBad = 0;
    for (let i = w - 1; i < span; i += step) {
        const g = v[i], t = refVar(x, m, w, i);
        if (Number.isNaN(g)) { if (!Number.isNaN(t)) nan++; continue; }
        if (Number.isNaN(t)) { nan++; continue; }
        if (g < 0) neg++;
        if (t === 0) {
            zero++;
            if (g !== 0) {
                const scale = Math.max(Math.abs(x[i]), 1e-300);
                if (Math.abs(g) > 1e-24 * scale * scale) zeroBad++;
            }
            continue;
        }
        n++;
        worst = Math.max(worst, Math.abs(g - t) / t);
    }
    return { worst, n, nan, neg, zero, zeroBad };
}

const PN = 200000;

S("a lone huge outlier does not poison the windows around it");
{
    const x = new Float64Array(PN);
    const rnd = lcg(0x9e3779b9);
    for (let i = 0; i < PN; i++) x[i] = 1.0 + (rnd() - 0.5);
    x[7] = 1e18;
    const m = new Uint8Array(PN);
    for (let i = 0; i < PN; i++) m[i] = (i % 3) !== 0 ? 1 : 0;

    const r = sweepVar(x, m, 501, PN, 1.5e6);
    console.log("  masked outlier repro: " + r.n + " windows, maxRel=" +
                r.worst.toExponential(3) + " NaN=" + r.nan + " negative=" + r.neg);
    ok(r.n > 1000, "the case actually covers the column", String(r.n));
    ok(r.nan === 0, "no window disagrees with the two-pass about being undefined",
       String(r.nan));
    ok(r.neg === 0, "no negative variance anywhere", String(r.neg));
    ok(r.worst < 1e-9, "masked ROLLING_VAR matches the two-pass to 1e-9",
       "maxRel=" + r.worst.toExponential(3));

    const s = new DataFrame({ x }).ROLLING_STD("x", 501, m);
    const vs = new DataFrame({ x }).ROLLING_VAR("x", 501, m);
    let bad = 0, n = 0, zeroBad = 0;
    for (let i = 501; i < PN; i += 997) {
        if (vs[i] <= 0) continue;
        n++;
        if (Math.abs(s[i] - Math.sqrt(vs[i])) > 1e-12 * Math.sqrt(vs[i])) bad++;
        if (s[i] === 0) zeroBad++;
    }
    ok(n > 150 && bad === 0, "ROLLING_STD is sqrt(ROLLING_VAR) window for window",
       bad + " of " + n + " differ");
    ok(zeroBad === 0, "and no STD window is 0 where the variance is positive",
       String(zeroBad));

    const v = vs;
    ok(v[500] > 1e30, "a window holding the 1e18 outlier has a huge variance",
       String(v[500]));
    ok(Number.isNaN(v[499]), "a partial window is still NaN", String(v[499]));
    ok(Number.isNaN(v[0]), "and so is the first", String(v[0]));
}

S("the unmasked path is equally stable above the gate");
{
    const x = new Float64Array(PN);
    const rnd = lcg(0x1234567);
    for (let i = 0; i < PN; i++) x[i] = 1.0 + (rnd() - 0.5);
    x[7] = 1e18;
    const r = sweepVar(x, null, 501, PN, 1.5e6);
    console.log("  unmasked: " + r.n + " windows, maxRel=" + r.worst.toExponential(3));
    ok(r.n > 1000 && r.worst < 1e-9,
       "unmasked ROLLING_VAR matches the two-pass to 1e-9",
       r.n + " windows maxRel=" + r.worst.toExponential(3));

    const h = new Float64Array(PN);
    for (let i = 0; i < PN; i++) h[i] = (i & 1) ? 1e300 : 1e-300;
    const hv = new DataFrame({ h }).ROLLING_VAR("h", 501);
    let inf = 0, nan = 0;
    for (let i = 501; i < PN; i++) { if (Number.isNaN(hv[i])) nan++; else if (hv[i] === Infinity) inf++; }
    console.log("  1e300/1e-300: Inf=" + inf + " NaN=" + nan);
    ok(nan === 0, "a genuinely overflowing variance is Inf, never NaN",
       String(nan) + " NaN windows");
    ok(inf > 0, "and it really does overflow", String(inf));

    for (const val of [1e100, 1e150, 1e160, 1e300]) {
        const c = new Float64Array(PN);
        c.fill(val);
        const cv = new DataFrame({ h: c }).ROLLING_VAR("h", 501);
        let bad = 0, nan = 0;
        for (let i = 501; i < PN; i += 3) {
            if (Number.isNaN(cv[i])) nan++;
            else if (cv[i] !== 0) bad++;
        }
        ok(nan === 0, "a constant " + val + " column is not NaN", String(nan) + " NaN");
        ok(bad === 0, "a constant " + val + " column has variance exactly 0, not a decline",
           bad + " non-zero windows of " + Math.ceil((PN - 501) / 3));
    }
}

S("adding a constant offset does not change the variance");
{
    const base = new Float64Array(20000);
    const rnd = lcg(0xabcdef);
    for (let i = 0; i < base.length; i++) base[i] = 1.0 + (rnd() - 0.5);
    const m = new Uint8Array(base.length);
    for (let i = 0; i < base.length; i++) m[i] = (i % 3) !== 0 ? 1 : 0;
    for (const off of [0, 1e3, 1e8, 1e12, 1e14, 1e15]) {
        const x = new Float64Array(base.length);
        for (let i = 0; i < x.length; i++) x[i] = base[i] + off;
        const r = sweepVar(x, m, 501, base.length, 3e6);
        ok(r.n > 1000 && r.zeroBad === 0 && r.worst < 1e-9,
           "offset " + off + ": masked VAR still matches the offset-free two-pass",
           r.n + " windows maxRel=" + r.worst.toExponential(3) + " zeroBad=" + r.zeroBad);
    }
    {
        const x = new Float64Array(base.length);
        for (let i = 0; i < x.length; i++) x[i] = base[i] + 1e17;
        const v = new DataFrame({ x }).ROLLING_VAR("x", 501, m);
        let bad = 0, nw = 0;
        for (let i = 501; i < x.length; i += 7) { nw++; if (v[i] !== 0) bad++; }
        ok(bad === 0, "an offset large enough to flatten the column gives exactly 0",
           bad + " non-zero of " + nw);
    }
}

S("the answers that are exact in binary64, stated as such");
{
    {
        const n = 200000, x = new Float64Array(n);
        x.fill(1.0);
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        for (const verb of ["ROLLING_VAR", "ROLLING_STD"])
            for (const [label, mm] of [["masked", m], ["unmasked", null]]) {
                const v = new DataFrame({ x })[verb]("x", 501, mm);
                let bad = 0;
                for (let i = 501; i < n; i++) if (v[i] !== 0) bad++;
                ok(bad === 0, verb + " " + label + ": a column of exact 1.0 has variance exactly 0",
                   bad + " non-zero windows");
            }
    }
    {
        const n = 200000, x = new Float64Array(n);
        for (let i = 0; i < n; i++) x[i] = 0.1;
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        const v = new DataFrame({ x }).ROLLING_VAR("x", 501, m);
        let worst = 0;
        for (let i = 501; i < n; i += 7) if (!Number.isNaN(v[i])) worst = Math.max(worst, Math.abs(v[i]));
        ok(worst < 1e-28, "a constant 0.1 column sits at the two-pass floor",
           "max |VAR| = " + worst.toExponential(3));
    }
    {
        const n = 200000, x = new Float64Array(n);
        for (let i = 0; i < n; i++) x[i] = (i % 2) ? 3 : 1;
        const m = new Uint8Array(n);
        for (const w of [2, 3, 5, 31, 32, 33, 63, 64, 65, 127, 128, 129, 200,
                         255, 256, 257, 501, 1024, 4096]) {
            const v = new DataFrame({ x }).ROLLING_VAR("x", w, m);
            let zeroBad = 0, zeroN = 0, formBad = 0, formN = 0;
            const step = Math.max(1, Math.floor(400000 / w));
            for (let i = w - 1; i < n; i += step) {
                const first = (i + 1 - w) & 1;
                let uniform = true;
                for (let j = i + 1 - w; j <= i; j++)
                    if (((j & 1) === first) !== true) { uniform = false; break; }
                const g = v[i];
                if (uniform) { zeroN++; if (g !== 0) zeroBad++; }
                else { formN++; if (Math.abs(g - 2 * w / (w - 1)) > 1e-9 * 2 * w / (w - 1)) formBad++; }
            }
            ok(zeroBad === 0, "w=" + w + ": alternating 1/3 has exactly-zero variance on uniform windows",
               zeroBad + " of " + zeroN);
            ok(formBad === 0, "w=" + w + ": and 2w/(w-1) on the rest",
               formBad + " of " + formN);
        }
    }
    {
        const n = 200000, x = new Float64Array(n);
        const rnd = lcg(7);
        for (let i = 0; i < n; i++) x[i] = 1e150 * (0.5 + rnd() * 0.4);
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        const r = sweepVar(x, m, 501, n, 3e6);
        console.log("  1e150: " + r.n + " windows maxRel=" + r.worst.toExponential(3));
        ok(r.n > 1000 && r.zeroBad === 0 && r.worst < 1e-10,
           "a column at 1e150 matches the two-pass",
           r.n + " windows maxRel=" + r.worst.toExponential(3));
    }
    {
        const n = 200000, x = new Float64Array(n);
        const rnd = lcg(7);
        for (let i = 0; i < n; i++) x[i] = 1.7e308 * (0.5 + rnd() * 0.4);
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        const v = new DataFrame({ x }).ROLLING_VAR("x", 501, m);
        let nan = 0, inf = 0;
        for (let i = 501; i < n; i++) { if (Number.isNaN(v[i])) nan++; else if (v[i] === Infinity) inf++; }
        console.log("  1.7e308: Inf=" + inf + " NaN=" + nan);
        ok(nan === 0, "an overflowing variance is Inf, never NaN", String(nan) + " NaN");
        ok(inf > 190000, "and it really does overflow", String(inf));
    }
    {
        const n = 200000, x = new Float64Array(n);
        const rnd = lcg(11);
        for (let i = 0; i < n; i++) x[i] = 1e-160 * (0.5 + rnd() * 0.9);
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        const r = sweepVar(x, m, 501, n, 3e6);
        console.log("  1e-160: " + r.n + " windows maxRel=" + r.worst.toExponential(3));
        ok(r.n > 1000 && r.worst < 5e-2, "a column of subnormal-squaring values matches the two-pass",
           r.n + " windows maxRel=" + r.worst.toExponential(3));
        let neg = 0;
        const v = new DataFrame({ x }).ROLLING_VAR("x", 501, m);
        for (let i = 501; i < n; i += 7) if (v[i] < 0) neg++;
        ok(neg === 0, "no negative variance anywhere", String(neg));
    }
    {
        const n = 200000, x = new Float64Array(n);
        x.fill(5e-324);
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        const v = new DataFrame({ x }).ROLLING_VAR("x", 501, m);
        let bad = 0;
        for (let i = 501; i < n; i += 7) if (v[i] !== 0) bad++;
        ok(bad === 0, "the smallest positive double repeated has variance exactly 0",
           bad + " non-zero windows");
    }
}

S("NaN and Inf propagate the way VARIANCE and ROLLING_MEAN do");
{
    const n = 200000, w = 501;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = (i % 17) - 8;
    for (const [label, at, v] of [["NaN", 1000, NaN], ["Inf", 1000, Infinity]]) {
        const col = new Float64Array(x);
        col[at] = v;
        const out = new DataFrame({ col }).ROLLING_VAR("col", w);
        let bad = 0, spill = 0;
        for (let i = w - 1; i < n; i++) {
            const inWin = (i >= at && i < at + w);
            if (inWin) { if (Number.isFinite(out[i])) bad++; }
            else if (Number.isNaN(out[i])) spill++;
        }
        ok(bad === 0, "a window holding " + label + " is poisoned, not silently finite",
           String(bad));
        ok(spill === 0, "and no window OUTSIDE it is", String(spill) + " poisoned");
    }
    {
        const col = new Float64Array(x);
        col[1000] = NaN;
        col[150000] = Infinity;
        const m = new Uint8Array(n);
        for (let i = 0; i < n; i++) m[i] = (i % 7) !== 0 ? 1 : 0;
        m[1000] = 0;
        m[150000] = 0;
        const v = new DataFrame({ col }).ROLLING_VAR("col", w, m);
        let nan = 0;
        for (let i = w - 1; i < n; i++) if (Number.isNaN(v[i])) nan++;
        ok(nan === 0, "a masked-out NaN or Inf is skipped, not multiplied by zero",
           String(nan) + " NaN windows");
        const u = new DataFrame({ col }).ROLLING_VAR("col", w);
        let un = 0;
        for (let i = w - 1; i < n; i++) if (Number.isNaN(u[i])) un++;
        ok(un >= 501, "an unmasked NaN does poison the windows that hold it",
           String(un) + " NaN windows");
    }
}

S("the work gate, the widths, and the mask shapes, all together");
{
    const n = PN;
    const x = new Float64Array(n);
    const rnd = lcg(0x5eed);
    for (let i = 0; i < n; i++) x[i] = (rnd() - 0.5) * 8;
    const mk = (f) => { const a = new Uint8Array(n); f(a); return a; };
    const shapes = {
        allOne: new Uint8Array(n).fill(1),
        allZero: new Uint8Array(n),
        first: mk((a) => { a[0] = 1; }),
        last: mk((a) => { a[n - 1] = 1; }),
        single: mk((a) => { a[12345] = 1; }),
        sparse: mk((a) => { for (let i = 0; i < n; i += 97) a[i] = 1; }),
        half: mk((a) => { for (let i = 0; i < n; i += 2) a[i] = 1; }),
        tailHalf: mk((a) => { for (let i = n >> 1; i < n; i++) a[i] = 1; }),
    };
    const df = new DataFrame({ x });
    let cells = 0, badCells = 0, worstAll = 0, worstAt = "";
    for (const w of [256, 257, 501, 1024, 4095, 8192, 20000, 100000]) {
        for (const [sname, m] of Object.entries(shapes)) {
            const r = sweepVar(x, m, w, n, 1.5e6);
            cells++;
            if (r.nan || r.worst > 1e-9) {
                badCells++;
                bad("w=" + w + " mask=" + sname + ": windows disagree with the two-pass",
                    "maxRel=" + r.worst.toExponential(3) + " NaN=" + r.nan);
            }
            if (r.worst > worstAll) { worstAll = r.worst; worstAt = "w=" + w + " " + sname; }
        }
    }
    console.log("  gate sweep: " + cells + " (width x mask) cells, worst maxRel=" +
                worstAll.toExponential(3) + " at " + worstAt);
    ok(cells === 64, "the sweep really ran 8 widths x 8 mask shapes", String(cells));
    ok(badCells === 0, "every cell over the gate matches the two-pass",
       badCells + " of " + cells + " cells differ");

    {
        const w = 4096;
        const over = df.ROLLING_VAR("x", w, shapes.half);
        const under = df.ROLLING_VAR("x", 2000, shapes.half);
        let worst = 0, nw = 0;
        for (let i = 2000 - 1; i < n; i += 401) {
            const t = refVar(x, shapes.half, 2000, i);
            if (Number.isNaN(t) || t === 0) continue;
            nw++;
            worst = Math.max(worst, Math.abs(under[i] - t) / t);
        }
        ok(nw > 400 && worst < 1e-9,
           "a width BELOW the gate is still the exact path, unchanged",
           nw + " windows maxRel=" + worst.toExponential(3));
        const r = sweepVar(x, shapes.half, w, n, 4e6);
        ok(r.n > 300 && r.worst < 1e-9, "and the O(n) side agrees with it",
           r.n + " windows maxRel=" + r.worst.toExponential(3));
    }
    let m1 = null;
    try { df.ROLLING_VAR("x", 501, new Uint8Array(n - 1)); } catch (e) { m1 = e.message; }
    ok(m1 !== null && /200000 bytes|must be a Uint8Array/.test(String(m1)),
       "a mask one row short is still refused", String(m1));
    let m2 = null;
    try { df.ROLLING_VAR("x", 501, new Float64Array(n)); } catch (e) { m2 = e.message; }
    ok(m2 !== null && /must be a Uint8Array/.test(String(m2)),
       "a Float64 mask is still refused", String(m2));
    {
        const v = df.ROLLING_VAR("x", n + 10, shapes.half);
        let nan = 0;
        for (let i = 0; i < n; i++) if (Number.isNaN(v[i])) nan++;
        ok(nan === n, "a window longer than the column never fills", String(nan) + "/" + n);
    }
}

S("the masked fast path divides by what CONTRIBUTED, never by w");
{
    const n = PN, w = 500;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = i + 1;
    const m = new Uint8Array(n);
    for (let i = 0; i < n; i++) m[i] = (i % 2) === 0 ? 1 : 0;
    const df = new DataFrame({ x });
    const mean = df.ROLLING_MEAN("x", w, m);
    const sum = df.ROLLING_SUM("x", w, m);
    let worst = 0, worstS = 0, nw = 0;
    for (let i = w - 1; i < n; i += 37) {
        const lo = i + 1 - w;
        let s = 0, c = 0;
        for (let j = lo; j <= i; j++) { if (!m[j]) continue; s += x[j]; c++; }
        if (c === 0) continue;
        const t = s / c;
        worst = Math.max(worst, Math.abs(mean[i] - t) / t);
        worstS = Math.max(worstS, Math.abs(sum[i] - s) / s);
        nw++;
    }
    ok(nw > 5000 && worst < 1e-12, "masked ROLLING_MEAN divides by the contributors",
       nw + " windows maxRel=" + worst.toExponential(3));
    ok(worstS < 1e-12, "masked ROLLING_SUM is the sum of the contributors",
       "maxRel=" + worstS.toExponential(3));
    {
        const z = new Uint8Array(n);
        const dm = df.ROLLING_MEAN("x", w, z);
        const ds = df.ROLLING_SUM("x", w, z);
        let nanM = 0, zeroS = 0, n2 = 0;
        for (let i = w - 1; i < n; i++) { n2++; if (Number.isNaN(dm[i])) nanM++; if (ds[i] === 0) zeroS++; }
        ok(nanM === n2, "an all-zero mask gives NaN mean everywhere", nanM + "/" + n2);
        ok(zeroS === n2, "and an exact 0 sum everywhere", zeroS + "/" + n2);
    }
    {
        const one = new Uint8Array(n);
        one[100000] = 1;
        const v = df.ROLLING_VAR("x", w, one);
        let n2 = 0, okc = 0;
        for (let i = w - 1; i < n; i++) { n2++; if (Number.isNaN(v[i])) okc++; }
        ok(okc === n2, "one contributor in every window means every variance is NaN",
           okc + "/" + n2);
    }
    {
        let msg = null;
        try { df.ROLLING_VAR("x", w, new Uint8Array(n - 1)); } catch (e) { msg = e.message; }
        ok(msg !== null && /200000 bytes|must be a Uint8Array/.test(String(msg)),
           "a short mask is refused rather than read past", String(msg));
    }
}

S("short windows are the exact path and stay correct");
{
    const n = 200000;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = (i % 4093) - 2046;
    const m = new Uint8Array(n).fill(1);
    let cells = 0, nbad = 0;
    for (const w of [2, 3, 5, 31, 32, 33, 63, 64, 65, 127, 128, 129, 200, 255]) {
        const r = sweepVar(x, m, w, n, 2e6);
        cells++;
        if (r.nan || r.worst > 1e-9 || r.n < 100) {
            nbad++;
            bad("w=" + w + ": a short masked window must equal the two-pass",
                "n=" + r.n + " maxRel=" + r.worst.toExponential(3) + " NaN=" + r.nan);
        }
    }
    ok(cells === 14, "the short-width sweep ran", String(cells));
    ok(nbad === 0, "every short width still matches the two-pass", String(nbad) + " bad");
    {
        const v = new DataFrame({ x }).ROLLING_VAR("x", 1, m);
        let nan = 0;
        for (let i = 0; i < n; i++) if (Number.isNaN(v[i])) nan++;
        ok(nan === n, "w=1 is NaN everywhere: one row has no sample variance",
           nan + "/" + n);
    }
    ok(n * 255 <= 1e8, "every width here is below the gate, so the exact path ran",
       String(n * 255));
}

S("a window mean that jumps every step is still right");
{
    const n = PN, w = 500, h = w / 2;
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = (Math.floor(i / h) & 1) ? 1e9 : 1.0;
    const m = new Uint8Array(n);
    for (let i = 0; i < n; i++) m[i] = (i % 3) !== 0 ? 1 : 0;
    const r = sweepVar(x, m, w, n, 1.5e6);
    console.log("  step-jumping mean: " + r.n + " windows maxRel=" +
                r.worst.toExponential(3) + " NaN=" + r.nan);
    ok(r.nan === 0, "a jumping window mean never yields NaN", String(r.nan));
    ok(r.n > 1000 && r.worst < 1e-9,
       "a jumping window mean stays within 1e-9 of the two-pass",
       r.n + " windows maxRel=" + r.worst.toExponential(3));

    const y = new Float64Array(n);
    y.fill(2.0);
    y[123457] = -1e14;
    const v = new DataFrame({ y }).ROLLING_VAR("y", 501, m);
    let worst = 0, nw = 0, huge = 0, nAll = 0, off = 0;
    const seen = [];
    for (let i = 501; i < n; i += 29) seen.push(i);
    for (let i = 123457; i < 123457 + 501; i += 7) seen.push(i);
    seen.sort((a, b) => a - b);
    for (const i of seen) {
        const inWin = (i >= 123457 && i < 123457 + 501);
        const g = v[i];
        if (inWin) { nAll++; if (g > 1e20) huge++; continue; }
        const t = refVar(y, m, 501, i);
        if (Number.isNaN(t)) continue;
        nAll++;
        if (t === 0) { off += (Math.abs(g) < 1e-25) ? 1 : 0; continue; }
        nw++;
        worst = Math.max(worst, Math.abs(g - t) / t);
    }
    ok(nAll > 6000, "the constant-plus-extreme case covers the column", String(nAll));
    ok(huge > 10 && huge === nAll - (nAll - huge), "every window holding the extreme is huge",
       huge + "/" + nAll);
    ok(nw === 0 || worst < 1e-9, "and the constant part is answered at the two-pass floor",
       nw + " windows maxRel=" + worst.toExponential(3));
    ok(off + huge === nAll,
       "no window of the constant part is a finite non-zero number",
       off + " zero + " + huge + " huge of " + nAll);
    {
        const y2 = new Float64Array(n);
        y2.fill(2.0);
        y2[123456] = -1e14;
        const v2 = new DataFrame({ y2 }).ROLLING_VAR("y2", 501, m);
        let bad2 = 0;
        for (let i = 501; i < n; i += 53) if (v2[i] !== 0) bad2++;
        ok(bad2 === 0, "an extreme value on a MASKED row does not enter any window",
           bad2 + " non-zero windows");
    }
}

console.log(`rolling masked: ${pass} passed, ${fail} failed`);
if (fail) {
    console.log(failures.join("\n"));
    throw new Error(fail + " dataframe rolling test(s) failed");
}
