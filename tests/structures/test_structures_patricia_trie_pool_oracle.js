// covers: contract dyna:structures.PatriciaTrie dyna:structures.Trie -- PatriciaTrie storage moved from one heap block per node (with a key copy per node) to a node pool with u32 children and bit-only descent; Trie answers are pinned alongside so its node-array work is held to the same digests. keysWithPrefix order is not part of the Trie contract (sibling insertion order, and delete unlinks emptied branches since 11-20), so prefix lists and iteration are sorted before hashing and checked against the JS model; the digests depend only on the stored set and the answers
// The digests below were computed by the fixA6 build over seeded random sequences (string keys,
// byte keys and CIDRs for PatriciaTrie; strings for Trie) at N = 128, 1000 and 20000: the first
// digest covers every answer plus the final contents in iteration order, the second the
// serialize() bytes (CRC trailer excluded). Any rewrite must reproduce both. A JS reference model
// checks get/has/delete/longestPrefix/withPrefix while pinning, so the pins are not a self-check.
import { suite } from "../lib/kit.js";
import { PatriciaTrie, Trie } from "dyna:structures";
import { CRC32C } from "dyna:hash";

const t = suite("patricia_trie_pool_oracle");
const enc = new TextEncoder();
const PINS = {
    "pat-string128":["dd0f1456","aece95d9"],
    "pat-string1000":["580ecc63","f9b78347"],
    "pat-string20000":["1900567d","bf001782"],
    "pat-bytes128":["1a8d7d56","eb72c46f"],
    "pat-bytes1000":["452ce3b2","8c7905a1"],
    "pat-bytes20000":["b35f6d05","d93c9659"],
    "pat-cidr128":["4f155830","39f215be"],
    "pat-cidr1000":["ced01b78","8d9bae4a"],
    "pat-cidr20000":["dc461f90","abd8a414"],
    "trie128":["49332731","99c7717f"],
    "trie1000":["be0fd14c","6253f8fd"],
    "trie20000":["4a18c54d","af79e416"],
};
const hex = (u) => (CRC32C(u) >>> 0).toString(16);
const recDigest = (m) => { const rec = m.serialize(); return hex(rec.subarray(0, rec.length - 4)); };
const show = (x) => x === undefined ? "u" : x instanceof Uint8Array ? "b" + Array.from(x).join(".") : Array.isArray(x) ? "[" + x.map(show).join(",") + "]" : String(x);

function runPat(mode, N, model) {
    let s = N * 31 + (mode === "bytes" ? 1 : mode === "cidr" ? 2 : 0);
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    const m = new PatriciaTrie(mode === "bytes" ? { keys: "bytes" } : undefined);
    const ref = new Map();
    let log = "", bad = 0;
    const strKey = () => { let k = ""; const L = rnd(7); for (let i = 0; i < L; i++) k += "abcz"[rnd(4)]; return k + (rnd(3) ? "" : String(rnd(N))); };
    const byteKey = () => Uint8Array.from({ length: rnd(5) }, () => rnd(4) * 60);
    const cidr = () => `${10 + rnd(3)}.${rnd(4)}.${rnd(256)}.0/${[8, 16, 20, 24, 32][rnd(5)]}`;
    const ip = () => `${10 + rnd(3)}.${rnd(4)}.${rnd(256)}.${rnd(256)}`;
    for (let op = 0; op < N * 4; op++) {
        const c = rnd(10);
        if (mode === "cidr") {
            if (c < 5) m.addCIDR(cidr(), op);
            else if (c < 7) log += m.deleteCIDR(cidr()) ? "d" : "n";
            else log += show(m.matchIP(ip())) + ";";
            continue;
        }
        const k = mode === "bytes" ? byteKey() : strKey(), rk = show(k);
        if (c < 5) { m.set(k, op); ref.set(rk, op); }
        else if (c < 7) { const d = m.delete(k); log += d ? "d" : "n"; if (model && d !== ref.delete(rk)) bad++; }
        else if (c < 8) { const g = m.get(k); log += show(g) + "," + m.has(k) + ";"; if (model && g !== ref.get(rk)) bad++; }
        else if (c < 9) log += show(m.longestPrefix(k)) + ";";
        else log += show(m.withPrefix(mode === "bytes" ? k.subarray(0, 1) : k.slice(0, 2))) + ";";
    }
    log += m.size + "|" + (mode === "cidr" ? "" : show(m.withPrefix()));
    if (model && mode !== "cidr" && m.size !== ref.size) bad++;
    return [m, hex(enc.encode(log)), bad];
}

function runTrie(N, model) {
    let s = N * 17 + 5;
    const rnd = (n) => (s = (s * 1103515245 + 12345) >>> 0) % n;
    const m = new Trie(), ref = new Set();
    let log = "", bad = 0;
    const key = () => { let k = ""; const L = 1 + rnd(8); for (let i = 0; i < L; i++) k += "abcdz"[rnd(5)]; return k + (rnd(4) ? "" : String(rnd(N))); };
    for (let op = 0; op < N * 4; op++) {
        const k = key(), c = rnd(10);
        if (c < 5) { m.insert(k); ref.add(k); }
        else if (c < 7) { const d = m.delete(k); log += d ? "d" : "n"; if (model && d !== ref.delete(k)) bad++; }
        else if (c < 8) { const h = m.has(k); log += h; if (model && h !== ref.has(k)) bad++; }
        else if (c < 9) {
            const q = k + "q", lp = m.longestPrefix(q);
            log += lp + ";";
            if (model && N <= 1000) { let w = ""; for (const r of ref) if (q.startsWith(r) && r.length > w.length) w = r; if (lp !== w) bad++; }
        } else {
            const p = k.slice(0, 2), got = m.keysWithPrefix(p).sort().join(",");
            log += got + ";";
            if (model && N <= 1000 && got !== [...ref].filter((r) => r.startsWith(p)).sort().join(",")) bad++;
        }
    }
    const all = [...m].sort().join(",");
    log += m.size + "|" + all;
    if (model && (m.size !== ref.size || all !== [...ref].sort().join(","))) bad++;
    return [m, hex(enc.encode(log)), bad];
}

const CASES = [];
for (const mode of ["string", "bytes", "cidr"]) for (const N of [128, 1000, 20000]) CASES.push(["pat-" + mode + N, () => runPat(mode, N, true), (r) => PatriciaTrie.deserialize(r)]);
for (const N of [128, 1000, 20000]) CASES.push(["trie" + N, () => runTrie(N, true), (r) => Trie.deserialize(r)]);

if (globalThis.scriptArgs?.[1] === "--pin") {
    const out = {};
    for (const [name, run] of CASES) { const [m, d, bad] = run(); if (bad) throw new Error(name + ": model mismatch " + bad); out[name] = [d, recDigest(m)]; }
    print(JSON.stringify(out));
} else {
    for (const [name, run, de] of CASES)
        t.test(name + ": answers and records equal the fixA6 build", (a) => {
            const [m, d, bad] = run();
            a.eq(bad, 0, "the JS model agrees");
            a.eq(d, PINS[name][0], "answers digest");
            a.eq(recDigest(m), PINS[name][1], "record digest");
            a.eq(recDigest(de(m.serialize())), PINS[name][1], "the record reloads to the same bytes");
        });
    await t.run();
}
