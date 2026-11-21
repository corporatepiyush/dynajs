// covers: c:src/modules/encoding/encoding-qr.c
// QREncode picks the mask with the lowest penalty; the penalty scans every row
// and column once with a rolling 11-bit window for the 1:1:3:1:1 finder-like
// pattern (0x5D0 / 0x05D) instead of an 11-cell compare at every offset, the
// mask loop is unswitched per mask, the Reed-Solomon step hoists log(f) out of
// the generator loop, and only size*size cells are copied per candidate mask.
// Modperf round 2026-11-15: 4 encodes (versions 2-8) 1.08 ms -> 0.46 ms.
// Oracles, all computed here in JS from ISO 18004 rules: (1) every pair of
// forced masks differs exactly by the mask formulas on non-function cells;
// (2) the automatic mask is the first argmin of a JS penalty (N1 runs, N2
// 2x2 blocks, N3 finder patterns, N4 balance) over the 8 forced grids;
// (3) for versions 1-4 (and 5-M, two interleaved blocks whose degree-24
// generator holds a coefficient of 1) the zigzag read-back of the unmasked grid holds the
// byte-mode bit stream and a Reed-Solomon tail equal to a JS GF(256) encoder.
import { suite } from "../lib/kit.js";
import * as e from "dyna:encoding";

const t = suite("test_encoding_qr_encode_penalty");

const MASKS = [
    (x, y) => (x + y) % 2 === 0,
    (x, y) => y % 2 === 0,
    (x, y) => x % 3 === 0,
    (x, y) => (x + y) % 3 === 0,
    (x, y) => (Math.floor(y / 2) + Math.floor(x / 3)) % 2 === 0,
    (x, y) => ((x * y) % 2) + ((x * y) % 3) === 0,
    (x, y) => (((x * y) % 2) + ((x * y) % 3)) % 2 === 0,
    (x, y) => (((x + y) % 2) + ((x * y) % 3)) % 2 === 0,
];

function isFunction(v, s, x, y) {
    if ((x < 9 && y < 9) || (x >= s - 8 && y < 9) || (x < 9 && y >= s - 8)) return true;
    if (x === 6 || y === 6) return true;
    if (v >= 2 && Math.abs(x - (s - 7)) <= 2 && Math.abs(y - (s - 7)) <= 2) return true;
    return false;
}

function penalty(m, s) {
    let score = 0, dark = 0;
    const at = (x, y) => m[y * s + x] & 1;
    for (let y = 0; y < s; y++)
        for (let x = 0; x < s; x++) {
            dark += at(x, y);
            if (x + 1 < s && y + 1 < s && at(x, y) === at(x + 1, y) && at(x, y) === at(x, y + 1) && at(x, y) === at(x + 1, y + 1)) score += 3;
        }
    const P1 = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0], P2 = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1];
    for (let dir = 0; dir < 2; dir++)
        for (let a = 0; a < s; a++) {
            const line = [];
            for (let b = 0; b < s; b++) line.push(dir ? at(a, b) : at(b, a));
            let run = 1;
            for (let b = 1; b <= s; b++) {
                if (b < s && line[b] === line[b - 1]) run++;
                else {
                    if (run >= 5) score += run - 2;
                    run = 1;
                }
            }
            for (let b = 0; b + 11 <= s; b++) {
                if (P1.every((p, k) => line[b + k] === p)) score += 40;
                if (P2.every((p, k) => line[b + k] === p)) score += 40;
            }
        }
    const pct = Math.floor((dark * 100) / (s * s));
    score += Math.floor(Math.abs(pct - 50) / 5) * 10;
    return score;
}

const EXP = new Uint8Array(512), LOG = new Uint8Array(256);
for (let i = 0, x = 1; i < 255; i++) {
    EXP[i] = x;
    LOG[x] = i;
    x <<= 1;
    if (x & 256) x ^= 0x11d;
}
for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
const gmul = (a, b) => (a && b ? EXP[LOG[a] + LOG[b]] : 0);
function rsEncode(data, n) {
    let g = [1];
    for (let i = 0; i < n; i++) {
        const ng = new Array(g.length + 1).fill(0);
        for (let j = 0; j < g.length; j++) {
            ng[j] ^= g[j];
            ng[j + 1] ^= gmul(g[j], EXP[i]);
        }
        g = ng;
    }
    const r = new Array(n).fill(0);
    for (const d of data) {
        const f = d ^ r[0];
        r.shift();
        r.push(0);
        for (let j = 0; j < n; j++) r[j] ^= gmul(g[j + 1], f);
    }
    return r;
}

const ECC = { 1: { L: [7, 19], M: [10, 16], Q: [13, 13], H: [17, 9] }, 2: { L: [10, 34], M: [16, 28], Q: [22, 22], H: [28, 16] }, 3: { L: [15, 55], M: [26, 44] }, 4: { L: [20, 80] } };

let seed = 0x1234567;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);
const text = (n) => {
    let s = "";
    for (let i = 0; i < n; i++) s += String.fromCharCode(32 + (rnd() % 95));
    return s;
};

t.test("forced masks differ exactly by the mask formulas on data cells", ({ eq }) => {
    for (const n of [5, 30, 70, 150]) {
        const s0 = text(n);
        const g = MASKS.map((_, k) => e.QREncode(s0, { ecc: "L", mask: k }));
        const v = g[0].version, s = g[0].size;
        if (v >= 7) continue;
        let bad = 0;
        for (let k = 1; k < 8; k++)
            for (let y = 0; y < s; y++)
                for (let x = 0; x < s; x++) {
                    if (isFunction(v, s, x, y)) continue;
                    const want = (MASKS[0](x, y) ? 1 : 0) ^ (MASKS[k](x, y) ? 1 : 0);
                    if (((g[0].modules[y * s + x] ^ g[k].modules[y * s + x]) & 1) !== want) bad++;
                }
        eq(bad, 0, "n=" + n + " v=" + v + " cells off");
    }
});

t.test("automatic mask is the first argmin of the JS penalty", ({ eq }) => {
    for (let c = 0; c < 24; c++) {
        const s0 = text(1 + (rnd() % 200));
        const ecc = "LMQH"[c & 3];
        const g = MASKS.map((_, k) => e.QREncode(s0, { ecc, mask: k }));
        const sc = g.map((q) => penalty(q.modules, q.size));
        const best = sc.indexOf(Math.min(...sc));
        const auto = e.QREncode(s0, { ecc });
        eq(Array.from(auto.modules).join(""), Array.from(g[best].modules).join(""), "case " + c + " picks mask " + best + " scores " + sc.join(","));
    }
});

t.test("versions 1-4: read-back data stream and Reed-Solomon tail match a JS encoder", ({ eq }) => {
    for (const [vs, lv] of Object.entries(ECC))
        for (const [ecc, [nec, nd]] of Object.entries(lv)) {
            const v = +vs;
            const maxBytes = nd - 2;
            for (const n of [0, 1, Math.floor(maxBytes / 2), maxBytes]) {
                const s0 = text(n);
                const q = e.QREncode(s0, { ecc, version: v, mask: 3 });
                eq(q.version, v, "version");
                const s = q.size, bits = [];
                for (let right = s - 1; right >= 1; right -= 2) {
                    if (right === 6) right = 5;
                    for (let vert = 0; vert < s; vert++)
                        for (let j = 0; j < 2; j++) {
                            const x = right - j, up = ((right + 1) & 2) === 0, y = up ? s - 1 - vert : vert;
                            if (isFunction(v, s, x, y)) continue;
                            bits.push((q.modules[y * s + x] & 1) ^ (MASKS[3](x, y) ? 1 : 0));
                        }
                }
                const cw = [];
                for (let i = 0; i + 8 <= bits.length && cw.length < nd + nec; i += 8) {
                    let b = 0;
                    for (let k = 0; k < 8; k++) b = (b << 1) | bits[i + k];
                    cw.push(b);
                }
                const data = cw.slice(0, nd), ec = cw.slice(nd, nd + nec);
                const head = (data[0] << 8) | data[1];
                eq(head >> 12, 4, "byte mode v" + v + ecc + " n=" + n);
                eq((head >> 4) & 0xff, n, "count v" + v + ecc + " n=" + n);
                let ok = true;
                for (let i = 0; i < n; i++) {
                    const b = ((data[1 + i] & 0xf) << 4) | (data[2 + i] >> 4);
                    if (b !== s0.charCodeAt(i)) ok = false;
                }
                eq(ok, true, "payload v" + v + ecc + " n=" + n);
                eq(ec.join(","), rsEncode(data, nec).join(","), "RS v" + v + ecc + " n=" + n);
            }
        }
});

t.test("version 5-M: two interleaved blocks with 24 EC codewords (degree-24 generator has a coefficient of 1)", ({ eq }) => {
    const v = 5, nec = 24, nd = 43, nb = 2;
    for (const n of [0, 1, 40, 84]) {
        const s0 = text(n);
        const q = e.QREncode(s0, { ecc: "M", version: v, mask: 3 });
        eq(q.version, v, "version");
        const s = q.size, bits = [];
        for (let right = s - 1; right >= 1; right -= 2) {
            if (right === 6) right = 5;
            for (let vert = 0; vert < s; vert++)
                for (let j = 0; j < 2; j++) {
                    const x = right - j, up = ((right + 1) & 2) === 0, y = up ? s - 1 - vert : vert;
                    if (isFunction(v, s, x, y)) continue;
                    bits.push((q.modules[y * s + x] & 1) ^ (MASKS[3](x, y) ? 1 : 0));
                }
        }
        const cw = [];
        for (let i = 0; i + 8 <= bits.length && cw.length < nb * (nd + nec); i += 8) {
            let b = 0;
            for (let k = 0; k < 8; k++) b = (b << 1) | bits[i + k];
            cw.push(b);
        }
        const blocks = [[], []], ecs = [[], []];
        for (let i = 0; i < nd; i++) for (let k = 0; k < nb; k++) blocks[k].push(cw[i * nb + k]);
        for (let i = 0; i < nec; i++) for (let k = 0; k < nb; k++) ecs[k].push(cw[nb * nd + i * nb + k]);
        const data = blocks[0].concat(blocks[1]);
        const head = (data[0] << 8) | data[1];
        eq(head >> 12, 4, "byte mode n=" + n);
        eq((head >> 4) & 0xff, n, "count n=" + n);
        let ok = true;
        for (let i = 0; i < n; i++) {
            const b = ((data[1 + i] & 0xf) << 4) | (data[2 + i] >> 4);
            if (b !== s0.charCodeAt(i)) ok = false;
        }
        eq(ok, true, "payload n=" + n);
        for (let k = 0; k < nb; k++) eq(ecs[k].join(","), rsEncode(blocks[k], nec).join(","), "RS block " + k + " n=" + n);
    }
});

await t.run();
