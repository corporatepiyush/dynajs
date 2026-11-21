// covers: contract dyna:mathx.signal.bls -- the phase fold computes fmod(t, P) exactly without the libm call
// Each BLS period folds every time stamp with fmod(t, P). The fold now computes it as x - n*P with one fused
// multiply-add, n = trunc(x / P) corrected by one step when the quotient rounded across an integer; that difference is
// the exact fmod result (it is representable), so the fused operation returns it unrounded, and zero keeps the sign of
// t. Quotients of 2^52 or more, non-finite inputs and non-positive periods still call fmod. Oracle: a JS BLS over the
// same arithmetic (JS % is IEEE fmod) must match power, depth, epoch and duration bit for bit for positive, negative
// and huge time stamps, and periods that divide the time stamps exactly.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_signal_bls_fold_oracle");
import { signal } from "dyna:mathx";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (!Object.is(got, want)) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
function blsRef(t, y, periods, durs, nb, minp) {
    const n = t.length;
    let mean = 0;
    for (let i = 0; i < n; i++) mean += y[i];
    mean /= n;
    const yc = Float64Array.from(y, (v) => v - mean), N = n;
    const out = { power: [], duration: [], depth: [], epoch: [] };
    for (const P of periods) {
        const bs = new Float64Array(nb), bn = new Float64Array(nb);
        for (let i = 0; i < n; i++) {
            let ph = t[i] % P;
            if (ph < 0) ph += P;
            const b = Math.floor((ph / P) * nb), bi = b >= nb ? nb - 1 : b;
            bs[bi] += yc[i];
            bn[bi] += 1;
        }
        const cs = new Float64Array(2 * nb + 1), cn = new Float64Array(2 * nb + 1);
        for (let s = 0; s < 2 * nb; s++) { const m = s < nb ? s : s - nb; cs[s + 1] = cs[s] + bs[m]; cn[s + 1] = cn[s] + bn[m]; }
        let best = 0, bdur = NaN, bdep = NaN, bep = NaN;
        for (const D of durs) {
            let wd = Math.round((D / P) * nb);
            if (wd < 1) wd = 1;
            if (wd >= nb) continue;
            for (let s = 0; s < nb; s++) {
                const sr = cs[s + wd] - cs[s], cnt = cn[s + wd] - cn[s];
                if (cnt < minp || cnt >= N) continue;
                const r = cnt / N, pw = (sr * sr) / (N * r * (1 - r));
                if (pw > best) { best = pw; bdur = D; bdep = -sr / cnt; bep = ((s + wd / 2) / nb) * P; if (bep >= P) bep -= P; }
            }
        }
        out.power.push(best); out.duration.push(bdur); out.depth.push(bdep); out.epoch.push(bep);
    }
    return out;
}

__kitS.section("power, depth, epoch and duration equal a JS fold with %", () => {
    let seed = 1234;
    const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
    const cases = [
        ["positive", (i) => i * 0.37 + rnd() * 0.1],
        ["negative", (i) => -500 + i * 0.41 + rnd() * 0.05],
        ["huge", (i) => 1e12 + i * 1.7],
        ["beyond2^52", (i) => 1e17 + i * 64],
        ["multiples", (i) => i * 0.25],
        ["mixed", (i) => (i % 2 ? -1 : 1) * i * 0.13 + (i % 3) * 1e-9],
    ];
    for (const [nm, tf] of cases) {
        const n = 1500, t = Float64Array.from({ length: n }, (_, i) => tf(i)), y = Float64Array.from(t, (x, i) => (Math.abs(x) % 3.1 < 0.25 ? -1 : 0) + ((i * 7919) % 13) / 40);
        const periods = Float64Array.from({ length: 120 }, (_, i) => 0.25 + i * 0.05), durs = [0.02, 0.1, 0.3];
        for (const [nb, minp] of [[400, 5], [57, 1]]) {
            const got = signal.bls(t, y, periods, durs, { bins: nb, minPoints: minp }), ref = blsRef(t, y, periods, durs, nb, minp);
            let bad = 0, first = "";
            for (const key of ["power", "duration", "depth", "epoch"])
                for (let k = 0; k < periods.length; k++)
                    if (!Object.is(got[key][k], ref[key][k])) { bad++; if (!first) first = key + "[" + k + "] " + got[key][k] + " vs " + ref[key][k]; }
            eq(bad, 0, nm + " bins=" + nb + " mismatches (" + first + ")");
        }
    }
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_mathx_signal_bls_fold_oracle: " + checks + " checks passed");
__kitS.end();
