/* bench_idna.js -- UTS #46 domainToASCII/domainToUnicode + RFC 3492 punycode.
 * Sweeps input shapes: plain ASCII, punycode A-labels, non-ASCII IDNs, and
 * punycode codec calls. RATIO (native / JS reimplementation) is the metric
 * under emulation; goal native < 1.0x.
 * Run: dynajs tests/bench_idna.js */
import { domainToASCII, domainToUnicode, punycodeEncode } from "dyna:url";

function best(fn, iters = 7) {
    let b = Infinity, acc = 0;
    for (let k = 0; k < iters; k++) {
        const t0 = performance.now();
        acc ^= fn() | 0;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return [b, acc];
}

function row(name, native, js) {
    const [tn] = best(native), [tj] = best(js);
    const ratio = tn / tj;
    print(
        name.padEnd(20) +
        " native=" + tn.toFixed(2).padStart(8) + "ms" +
        " js=" + tj.toFixed(2).padStart(8) + "ms" +
        " ratio=" + ratio.toFixed(3) + (ratio < 1 ? "  (native faster)" : "  (JS faster)")
    );
}

const ITER = 200;

// A plausible address-bar mix: mostly ASCII, one IDN, one punycode label.
const ascii = "www.example.com";
const idn = "b\u00fccher.de";
const alabel = ("xn--bcher-kva.de." + "xn--ihqwcrb4cv8a8dqg056pqjye." + "xn--4dbcagdahymbxekheh6e0a7fei0b").repeat(60);
const puny = "\u4ED6\u4EEC\u4E3A\u4EC0\u4E48\u4E0D\u8BF4\u4E2D\u6587\u4E2D\u6587\u4E2D\u6587\u4E2D\u6587\u4E2D\u6587";

print("=== UTS #46 vs a JS re-implementation of the same processing ===");

// JS reference for toASCII over an already-valid domain: casefold via a
// hand-rolled ASCII fast path + punycode (approximate; the ratio is the point).
function jsPunycode(s) {
    // minimal RFC 3492 encode for strings with basic+non-basic mix
    let out = "", basic = "", n = 128, delta = 0, bias = 72;
    const cps = [];
    for (const ch of s) cps.push(ch.codePointAt(0));
    for (const c of cps) if (c < 0x80) basic += String.fromCodePoint(c);
    out = basic;
    if (basic.length) out += "-";
    let h = basic.length;
    const adapt = (d, np, first) => {
        d = first ? d / 700 : d / 2;
        d += d / np;
        let k = 0;
        while (d > 455) { d = (d / 35) | 0; k += 36; }
        return k + ((27 * d) / (d + 38)) | 0;
    };
    while (h < cps.length) {
        let m = 0x110000;
        for (const c of cps) if (c >= n && c < m) m = c;
        delta += (m - n) * (h + 1);
        n = m;
        for (const c of cps) {
            if (c < n) delta++;
            if (c === n) {
                let q = delta;
                for (let k = 36; ; k += 36) {
                    const t = k <= bias ? 1 : (k >= bias + 26 ? 26 : k - bias);
                    if (q < t) break;
                    const d = t + ((q - t) % (36 - t));
                    out += String.fromCharCode(d < 26 ? d + 97 : d - 26 + 48);
                    q = (q - t) / (36 - t) | 0;
                }
                out += String.fromCharCode(q < 26 ? q + 97 : q - 26 + 48);
                bias = adapt(delta, h + 1, h === basic.length);
                delta = 0;
                h++;
            }
        }
        delta++; n++;
    }
    return out;
}
const jsToASCII = (s) => {
    // lowercase ASCII, pass non-ASCII to the JS punycode (approximation)
    let out = s.toLowerCase();
    if (/[^\x00-\x7F]/.test(out)) {
        const labs = out.split(".");
        return labs.map(l => /[^\x00-\x7F]/.test(l) ? "xn--" + jsPunycode(l) : l).join(".");
    }
    return out;
};

row("toASCII(ascii)", () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= domainToASCII(ascii).length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= jsToASCII(ascii).length; return s; });

row("toASCII(idn)", () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= domainToASCII(idn).length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= jsToASCII(idn).length; return s; });

row("toASCII(alabel)", () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= domainToASCII(alabel).length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= jsToASCII(alabel).length; return s; });

row("toUnicode(alabel)", () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= domainToUnicode(alabel).length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= alabel.length; return s; });

row("punycodeEncode", () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= punycodeEncode(puny).length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= jsPunycode(puny).length; return s; });

print("=== absolute throughput (ns/op) ===");
{
    const small = "www.example.com";
    const t0 = performance.now();
    let acc = 0;
    const N = 20000;
    for (let i = 0; i < N; i++) acc ^= domainToASCII(small).length;
    const dt = performance.now() - t0;
    print("domainToASCII(18-char host)  " + (dt * 1e6 / N).toFixed(0).padStart(8) + " ns/op  (acc=" + acc + ")");
}
{
    const idn = "bücher.de";
    const t0 = performance.now();
    let acc = 0;
    const N = 5000;
    for (let i = 0; i < N; i++) acc ^= domainToASCII(idn).length;
    const dt = performance.now() - t0;
    print("domainToASCII(bücher.de)     " + (dt * 1e6 / N).toFixed(0).padStart(8) + " ns/op  (acc=" + acc + ")");
}
