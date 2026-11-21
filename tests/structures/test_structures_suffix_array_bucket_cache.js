// covers: dyna:structures.SuffixArray -- the opt-in first-bytes bucket table ({ cache: true | { bucket: true } }) answers count/find exactly as the plain search
// With the cache on, count/find start their binary searches inside the block of suffixes that share
// the pattern's first byte (texts of 1024+ bytes, 1 KiB table) or first two bytes (2^18+ bytes,
// 256 KiB, at most a quarter of the suffix array); shorter texts and shorter patterns use the plain
// search. The one-byte final suffix "x" is keyed as "x\0"; it sorts first in that block, so the
// lower search steps past it. The table is built once (the array is immutable) and never changes results; a
// loaded or deserialized array starts without it. Oracle: the same queries on a cached and an
// uncached array over texts below and above each threshold.
import { suite } from "../lib/kit.js";
import { SuffixArray } from "dyna:structures";

const t = suite("suffix_array_bucket_cache");

t.test("cached and uncached arrays agree on count and find", (a) => {
    let s = 3;
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    for (const [n, alpha] of [[500, 4], [1023, 3], [1024, 2], [5000, 256], [5000, 3], [262143, 4], [262144, 4], [300000, 256]]) {
        const u = new Uint8Array(n);
        for (let i = 0; i < n; i++) u[i] = alpha === 256 ? rnd(256) : [0, 255, 97, 1][rnd(alpha)];
        u[n - 1] = 255;
        const on = new SuffixArray(u, { cache: { bucket: true } });
        const off = new SuffixArray(u);
        const pats = [new Uint8Array([255]), new Uint8Array([255, 0]), new Uint8Array([254, 255]), new Uint8Array([0]), new Uint8Array([0, 0]), new Uint8Array([255, 255, 255]), new Uint8Array([7, 7, 7, 7])];
        for (let q = 0; q < 400; q++) {
            const o = rnd(n), m = 1 + rnd(5);
            pats.push(u.slice(o, Math.min(n, o + m)));
        }
        pats.push(u.slice(n - 1), u.slice(n - 2), u.slice(0, 3));
        let bad = 0;
        for (const p of pats) {
            if (on.count(p) !== off.count(p)) bad++;
            else if (off.count(p) < 2000 && on.find(p).join() !== off.find(p).join()) bad++;
        }
        a.eq(bad, 0, "n " + n + " alphabet " + alpha + ": " + pats.length + " patterns agree");
    }
});

t.test("string texts and the option", (a) => {
    const text = ("héllo wörld \u{1F600} ").repeat(300) + "end";
    const on = new SuffixArray(text, { cache: true }), off = new SuffixArray(text);
    for (const p of ["h", "é", "l", "lo", "\u{1F600}", " ", "d", "end", "nd", "zz", "o w"]) {
        a.eq(on.count(p), off.count(p), "count " + JSON.stringify(p));
        a.eq(on.find(p).join(), off.find(p).join(), "find " + JSON.stringify(p));
    }
    a.eq(on.array().join() === off.array().join(), true, "same array");
    a.throws(() => new SuffixArray("abc", { cache: { jump: true } }), /unknown cache option "jump"/);
    a.throws(() => new SuffixArray("abc", { cahce: true }), /unknown constructor option "cahce"/);
    a.throws(() => new SuffixArray("abc", 3), TypeError);
    a.eq(new SuffixArray("abcabc", { cache: false }).count("bc"), 2);
});

await t.run();
