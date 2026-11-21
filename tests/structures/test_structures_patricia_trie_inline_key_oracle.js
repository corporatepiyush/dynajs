// covers: dyna:structures.PatriciaTrie
// Each PatriciaTrie node carries its key bytes in the node's own allocation (after the value slot) instead of a
// second malloc, so a descent touches one block per node and an insert allocates once per node. (Resuming the
// prefix compare at the parent's bit length was also tried and measured slower: +3% on 1K gets.) The oracle is a JS
// model over keys with long shared prefixes, keys that are prefixes of other keys, the empty key, and odd bit lengths
// via CIDR blocks: every get/has/delete, longestPrefix and withPrefix walk equals the model after inserts and deletes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_patricia_trie_inline_key_oracle");
import { PatriciaTrie } from "dyna:structures";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
let seed = 4711;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const pre = ["", "a", "ab", "abcdefghij", "abcdefghik", "abcdefghijklmnop", "b", "é", "zzzzzzzzzzzzzzzzzzzzzzzzzzzz"];
const key = () => { const p = pre[(rnd() * pre.length) | 0]; const r = rnd(); return r < 0.25 ? p : p + String.fromCharCode(97 + ((rnd() * 4) | 0)) + (r < 0.6 ? "" : (rnd() * 40) | 0); };
const enc = new TextEncoder();
function byteCmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b), n = Math.min(x.length, y.length);
    for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}
const startsWithBytes = (k, p) => { const x = enc.encode(k), y = enc.encode(p); if (y.length > x.length) return false; for (let i = 0; i < y.length; i++) if (x[i] !== y[i]) return false; return true; };

__kitS.section("get, has, delete, longestPrefix and withPrefix match the model", () => {
    const t = new PatriciaTrie(), model = new Map();
    for (let round = 0; round < 6; round++) {
        for (let i = 0; i < 500; i++) { const k = key(), v = { round, i }; t.set(k, v); model.set(k, v); }
        for (let i = 0; i < 150; i++) { const k = key(); eq(t.delete(k), model.delete(k), "r" + round + " delete"); }
        eq(t.size, model.size, "r" + round + " size");
        let bad = 0;
        for (const [k, v] of model) if (t.get(k) !== v || !t.has(k)) bad++;
        for (let q = 0; q < 400; q++) {
            const k = key();
            if (t.get(k) !== model.get(k) || t.has(k) !== model.has(k)) bad++;
            let best;
            for (const m of model.keys()) if (startsWithBytes(k, m) && (best === undefined || enc.encode(m).length > enc.encode(best).length)) best = m;
            const lp = t.longestPrefix(k);
            if (best === undefined ? lp !== undefined : !lp || lp[0] !== best || lp[1] !== model.get(best)) bad++;
        }
        eq(bad, 0, "r" + round + " lookups");
        for (const p of pre.concat(["abc", "abcdefghijkl", "q"])) {
            const want = [...model.keys()].filter((k) => startsWithBytes(k, p)).sort(byteCmp);
            eq(t.withPrefix(p).map((e) => e[0]).join("|"), want.join("|"), "r" + round + " withPrefix " + JSON.stringify(p));
        }
    }
});

__kitS.section("CIDR blocks at odd bit lengths match the most specific block", () => {
    const t = new PatriciaTrie({ keys: "bytes" }), blocks = [];
    for (let i = 0; i < 300; i++) {
        const len = 1 + ((rnd() * 32) | 0), a = [0, 0, 0, 0].map(() => (rnd() * 4) | 0 ? 10 : (rnd() * 256) | 0);
        let ip = ((a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]) >>> 0;
        ip = len === 32 ? ip : (ip & ~((1 << (32 - len)) - 1)) >>> 0;
        const s = [ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255].join(".") + "/" + len;
        t.addCIDR(s, s);
        blocks.push([ip, len, s]);
    }
    const uniq = new Map(blocks.map((b) => [b[2], b]));
    let bad = 0;
    for (let q = 0; q < 2000; q++) {
        const ip = q % 2 ? (((10 << 24) | ((rnd() * 65536) << 8) | ((rnd() * 256) | 0)) >>> 0) : (rnd() * 4294967296) >>> 0;
        let best;
        for (const [bip, len, s] of uniq.values()) {
            const m = len === 0 ? 0 : (~0 << (32 - len)) >>> 0;
            if (((ip & m) >>> 0) === bip && (!best || len > best[1])) best = [bip, len, s];
        }
        const ips = [ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255].join(".");
        if (t.matchIP(ips) !== (best ? best[2] : undefined)) bad++;
    }
    eq(bad, 0, "matchIP vs most specific block");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_patricia_trie_inline_key_oracle: " + checks + " checks passed");
__kitS.end();
