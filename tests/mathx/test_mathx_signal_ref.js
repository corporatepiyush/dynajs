// covers: integration dyna:mathx.signal -- every signal function against numpy.fft / scipy.signal reference vectors
// Vectors: tests/corpus/ref/signal_vectors.js from tests/ref/gen_signal_ref_vectors.py. Inputs are rebuilt here by sig(), the same
// closed form as the generator. Each value must agree to 1e-9 relative, with an absolute floor of 1e-11 of its part's peak.
import { signal as S } from "dyna:mathx";
import V from "../corpus/ref/signal_vectors.js";

function sig(n, seed) {
    const x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = Math.sin(0.37 * seed * i + seed) + 0.5 * Math.cos(1.3 * i + 0.11 * seed) + 0.01 * seed * ((i * 7 + seed) % 5);
    return x;
}
function arg(a) {
    if (Array.isArray(a) && a[0] === "g") return sig(a[1], a[2]);
    if (Array.isArray(a) && a[0] === "butter") return S.butter(a[1], a[2], a[3]);
    return a === null ? undefined : a;
}
function parts(op, r) {
    if (typeof r === "number") return [[r]];
    if (r instanceof Float64Array) return [Array.from(r)];
    if (op === "kaiserOrd") return [[r.numtaps], [r.beta]];
    if (op === "goertzel") return [[r.re], [r.im]];
    if (op === "welch") return [Array.from(r.f), Array.from(r.pxx)];
    if (r.w) return [Array.from(r.w), Array.from(r.re), Array.from(r.im)];
    return [Array.from(r.re), Array.from(r.im)];
}

let pass = 0, fail = 0, worst = 0, worstAt = "";
for (const c of V.cases) {
    const args = c.args.map(arg), stride = c.stride ?? 1;
    const tag = `${c.op}(${JSON.stringify(c.args).slice(0, 80)})`;
    let got;
    try {
        got = parts(c.op, S[c.op](...args));
    } catch (e) {
        fail++;
        print(`FAIL ${tag} threw ${e}`);
        continue;
    }
    let at = 0, bad = "";
    for (const p of got) {
        const g = p.filter((_, i) => i % stride === 0), w = c.want.slice(at, at + g.length);
        at += g.length;
        const peak = w.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
        g.forEach((v, i) => {
            const e = Math.abs(v - w[i]), rel = e / Math.max(Math.abs(w[i]), 1e-2 * peak, 1e-300);
            if (rel > worst) { worst = rel; worstAt = ` at ${tag}[${i}]`; }
            if (!(e <= 1e-9 * Math.abs(w[i]) + 1e-11 * peak) && !bad) bad = `[${at - g.length + i}] got ${v}, want ${w[i]}`;
        });
    }
    if (at !== c.want.length) bad ||= `length ${at} vs ${c.want.length}`;
    if (bad) { fail++; print(`FAIL ${tag} ${bad}`); } else pass++;
}
print(`test_mathx_signal_ref: ${pass} passed, ${fail} failed (numpy ${V.numpy}, scipy ${V.scipy}; worst relative error ${worst.toExponential(2)}${worstAt})`);
if (fail) throw new Error(`${fail} reference mismatches`);
