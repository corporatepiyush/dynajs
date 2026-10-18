// Transparency log — an RFC 6962 / RFC 9162 Merkle log with inclusion and consistency proofs, served over HTTP and
// audited by two independent monitors that catch a log rewriting its history.
//
// WHAT IT IS
//   The append-only log behind Certificate Transparency, sigstore's Rekor and Go's checksum database:
//
//   tree        leaves hashed as SHA-256(0x00 || entry), interior nodes as SHA-256(0x01 || left || right); every
//               complete power-of-two subtree is stored once, so any root or proof node is O(log n) stored lookups
//   proofs      PATH (inclusion) and PROOF/SUBPROOF (consistency) written exactly as RFC 6962 §2.1.1-2.1.2 define
//               them; the verifiers are the iterative bit-walk of RFC 9162 §2.1.3.2 / §2.1.4.2 — a different shape
//               of code, so a proof that both sides accept was not just echoed back
//   oracle      the root of every size 0..300 is recomputed from the raw entries by the recursive MTH definition,
//               and MTH({}) must be SHA-256 of the empty string (FIPS 180-4 test vector)
//   service     POST /add returns a signed certificate timestamp (Ed25519); GET /sth, /proof, /consistency, /entries;
//               duplicate submissions return the original index (Interner over the leaf hash)
//   audit       two monitors poll STHs, verify every new head is consistent with the last, and check inclusion of
//               what they submitted. A second, forked log rewrites entry 5: its consistency proof fails, and the
//               monitors gossiping heads see two roots for one size — the split view CT exists to expose
//
// MODULES  dyna:hash (SHA256) · dyna:bytes (concat, equal) · dyna:crypto (Ed25519) · dyna:encoding (HexEncode,
//          HexDecode) · dyna:structures (Interner, OrderedMap, Multimap) · dyna:http (App, fetch) · dyna:random
//
// RUN      dynajs examples/security/transparency-log.js

import { SHA256 } from "dyna:hash";
import { concat, equal } from "dyna:bytes";
import { Ed25519Generate, Ed25519Sign, Ed25519Verify } from "dyna:crypto";
import { HexEncode, HexDecode } from "dyna:encoding";
import { Interner, OrderedMap, Multimap } from "dyna:structures";
import { App } from "dyna:http";
import { Random } from "dyna:random";

const results = {};
const enc = new TextEncoder();
const LEAF = new Uint8Array([0]),
    NODE = new Uint8Array([1]);
const leafHash = (entry) => SHA256(concat([LEAF, typeof entry === "string" ? enc.encode(entry) : entry]));
const nodeHash = (l, r) => SHA256(concat([NODE, l, r]));
const largestPow2Below = (n) => {
    let k = 1;
    while (k << 1 < n) k <<= 1;
    return k;
};

// =============================================================================
// The log: complete subtrees by level
// =============================================================================
class MerkleLog {
    constructor() {
        this.levels = [[]];
        this.entries = [];
        this.ids = new Interner();
    }
    get size() {
        return this.entries.length;
    }
    append(entry) {
        const lh = leafHash(entry),
            key = HexEncode(lh);
        const known = this.ids.lookup(key);
        if (known !== undefined) return { index: known, duplicate: true };
        const index = this.entries.length;
        this.ids.intern(key);
        this.entries.push(entry);
        let h = lh,
            i = index;
        for (let level = 0; ; level++) {
            (this.levels[level] ??= []).push(h);
            if ((i & 1) === 0) break;
            h = nodeHash(this.levels[level][i - 1], h);
            i >>= 1;
        }
        return { index, duplicate: false };
    }
    // MTH(D[lo:hi]) from stored complete subtrees; recursion only over the right spine.
    hash(lo, hi) {
        const n = hi - lo;
        if (n === 0) return SHA256(new Uint8Array(0));
        if ((n & (n - 1)) === 0 && lo % n === 0) return this.levels[Math.log2(n)][lo / n];
        const k = largestPow2Below(n);
        return nodeHash(this.hash(lo, lo + k), this.hash(lo + k, hi));
    }
    root(size = this.size) {
        return this.hash(0, size);
    }
    inclusion(m, size, lo = 0) {
        const n = size - lo;
        if (n <= 1) return [];
        const k = largestPow2Below(n);
        return m - lo < k
            ? [...this.inclusion(m, lo + k, lo), this.hash(lo + k, lo + n)]
            : [...this.inclusion(m, lo + n, lo + k), this.hash(lo, lo + k)];
    }
    consistency(m, size) {
        return m === 0 || m === size ? [] : this.subproof(m, 0, size, true);
    }
    subproof(m, lo, hi, complete) {
        const n = hi - lo;
        if (m === n) return complete ? [] : [this.hash(lo, hi)];
        const k = largestPow2Below(n);
        return m <= k
            ? [...this.subproof(m, lo, lo + k, complete), this.hash(lo + k, hi)]
            : [...this.subproof(m - k, lo + k, hi, false), this.hash(lo, lo + k)];
    }
}

// RFC 9162 §2.1.3.2
function verifyInclusion(index, size, leaf, proof, root) {
    if (index >= size) return false;
    let fn = index,
        sn = size - 1,
        r = leaf;
    for (const p of proof) {
        if (sn === 0) return false;
        if (fn & 1 || fn === sn) {
            r = nodeHash(p, r);
            if (!(fn & 1))
                while (fn !== 0 && !(fn & 1)) {
                    fn >>= 1;
                    sn >>= 1;
                }
        } else r = nodeHash(r, p);
        fn >>= 1;
        sn >>= 1;
    }
    return sn === 0 && equal(r, root);
}
// RFC 9162 §2.1.4.2
function verifyConsistency(first, second, firstRoot, secondRoot, proof) {
    if (first === second) return proof.length === 0 && equal(firstRoot, secondRoot);
    if (first === 0) return proof.length === 0;
    if (first > second || proof.length === 0) return false;
    const path = (first & (first - 1)) === 0 ? [firstRoot, ...proof] : proof;
    let fn = first - 1,
        sn = second - 1;
    while (fn & 1) {
        fn >>= 1;
        sn >>= 1;
    }
    let fr = path[0],
        sr = path[0];
    for (const c of path.slice(1)) {
        if (sn === 0) return false;
        if (fn & 1 || fn === sn) {
            fr = nodeHash(c, fr);
            sr = nodeHash(c, sr);
            if (!(fn & 1))
                while (fn !== 0 && !(fn & 1)) {
                    fn >>= 1;
                    sn >>= 1;
                }
        } else sr = nodeHash(sr, c);
        fn >>= 1;
        sn >>= 1;
    }
    return sn === 0 && equal(fr, firstRoot) && equal(sr, secondRoot);
}
// The definition, straight from RFC 6962 §2.1, for the oracle.
function mth(entries) {
    if (entries.length === 0) return SHA256(new Uint8Array(0));
    if (entries.length === 1) return leafHash(entries[0]);
    const k = largestPow2Below(entries.length);
    return nodeHash(mth(entries.slice(0, k)), mth(entries.slice(k)));
}

// =============================================================================
// Oracle: roots and proofs for every small size
// =============================================================================
{
    const log = new MerkleLog(),
        raw = [];
    results["empty tree hash is SHA-256 of nothing (FIPS 180-4)"] =
        HexEncode(log.root()) === "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    let rootsOk = true,
        inclOk = true,
        consOk = true,
        tamperCaught = true;
    for (let n = 1; n <= 300; n++) {
        raw.push(`entry-${n - 1}`);
        log.append(raw[n - 1]);
        if (!equal(log.root(), mth(raw))) rootsOk = false;
    }
    for (let n = 1; n <= 300; n += 7)
        for (let m = 0; m < n; m += 3) {
            const p = log.inclusion(m, n);
            if (!verifyInclusion(m, n, leafHash(raw[m]), p, log.root(n))) inclOk = false;
            if (p.length && verifyInclusion(m, n, leafHash(raw[m] + "x"), p, log.root(n))) tamperCaught = false;
        }
    for (let n = 1; n <= 300; n += 5)
        for (let m = 1; m <= n; m += 4) {
            const p = log.consistency(m, n);
            if (!verifyConsistency(m, n, log.root(m), log.root(n), p)) consOk = false;
            if (m < n - 1 && verifyConsistency(m, n, log.root(m), log.root(n - 1), p)) tamperCaught = false;
        }
    results["stored-subtree roots equal the recursive MTH for sizes 1..300"] = rootsOk;
    results["every inclusion proof verifies"] = inclOk;
    results["every consistency proof verifies"] = consOk;
    results["tampered leaves and wrong heads are rejected"] = tamperCaught;
    results["proof length is logarithmic"] = log.inclusion(0, 256).length === 8 && log.inclusion(255, 257).length === 9;
}

// =============================================================================
// The service
// =============================================================================
class LogServer {
    constructor(name) {
        this.name = name;
        this.log = new MerkleLog();
        this.keys = Ed25519Generate();
        this.app = new App({ port: 0 });
        this.clock = Date.UTC(2026, 9, 1);
        this.app.post("/add", (req) => this.add(req.body));
        this.app.get("/sth", () => this.sth());
        this.app.get("/proof", (req) => {
            const index = Number(req.query.index),
                size = Number(req.query.size);
            if (!(index < size && size <= this.log.size)) return { status: 400, body: "index/size out of range" };
            return { index, size, path: this.log.inclusion(index, size).map(HexEncode) };
        });
        this.app.get("/consistency", (req) => {
            const first = Number(req.query.first),
                second = Number(req.query.second);
            if (!(first <= second && second <= this.log.size)) return { status: 400, body: "bad sizes" };
            return { first, second, path: this.log.consistency(first, second).map(HexEncode) };
        });
        this.app.get("/entries", (req) => this.log.entries.slice(Number(req.query.start), Number(req.query.end) + 1));
        this.app.start();
    }
    get base() {
        return `http://127.0.0.1:${this.app.port}`;
    }
    add(entry) {
        const { index, duplicate } = this.log.append(entry);
        this.clock += 1000;
        const sct = `${this.name}|${index}|${HexEncode(leafHash(entry))}`;
        return { index, duplicate, sct, sig: HexEncode(Ed25519Sign(this.keys.privateKey, sct)) };
    }
    sth() {
        const size = this.log.size,
            root = HexEncode(this.log.root());
        const body = `${this.name}|${size}|${root}|${this.clock}`;
        return { size, root, timestamp: this.clock, sig: HexEncode(Ed25519Sign(this.keys.privateKey, body)) };
    }
    close() {
        this.app.close();
    }
}

class Monitor {
    constructor(name, server) {
        this.name = name;
        this.server = server;
        this.heads = new OrderedMap();
        this.submitted = [];
        this.alarms = [];
    }
    async get(path) {
        return (await fetch(this.server.base + path)).json();
    }
    async submit(entry) {
        const r = await (await fetch(this.server.base + "/add", { method: "POST", body: entry })).json();
        if (!Ed25519Verify(this.server.keys.publicKey, r.sct, HexDecode(r.sig)))
            this.alarms.push(`bad SCT for ${entry}`);
        this.submitted.push({ entry, index: r.index });
        return r;
    }
    async poll() {
        const sth = await this.get("/sth");
        const body = `${this.server.name}|${sth.size}|${sth.root}|${sth.timestamp}`;
        if (!Ed25519Verify(this.server.keys.publicKey, body, HexDecode(sth.sig))) {
            this.alarms.push("bad STH signature");
            return sth;
        }
        const last = this.heads.lastKey();
        if (last !== undefined) {
            const prev = this.heads.get(last);
            if (sth.size < prev.size) this.alarms.push(`log shrank ${prev.size} -> ${sth.size}`);
            else if (sth.size > prev.size) {
                const c = await this.get(`/consistency?first=${prev.size}&second=${sth.size}`);
                if (
                    !verifyConsistency(
                        prev.size,
                        sth.size,
                        HexDecode(prev.root),
                        HexDecode(sth.root),
                        c.path.map(HexDecode),
                    )
                )
                    this.alarms.push(`inconsistent heads ${prev.size} -> ${sth.size}`);
            }
        }
        this.heads.set(String(sth.size).padStart(10, "0"), sth);
        return sth;
    }
    async audit(sth) {
        let bad = 0;
        for (const { entry, index } of this.submitted) {
            if (index >= sth.size) continue;
            const p = await this.get(`/proof?index=${index}&size=${sth.size}`);
            if (!verifyInclusion(index, sth.size, leafHash(entry), p.path.map(HexDecode), HexDecode(sth.root))) bad++;
        }
        if (bad) this.alarms.push(`${bad} submitted entries not included`);
        return bad;
    }
}

const rng = new Random(6962);
const honest = new LogServer("honest-log");
const alice = new Monitor("alice", honest),
    bob = new Monitor("bob", honest);
for (let round = 0; round < 6; round++) {
    const batch = [];
    for (let i = 0; i < 40; i++)
        batch.push((i & 1 ? alice : bob).submit(`cert:${round}:${i}:${rng.nextU53().toString(36)}`));
    await Promise.all(batch);
    const [sa, sb] = [await alice.poll(), await bob.poll()];
    await alice.audit(sa);
    await bob.audit(sb);
}
const dup = await alice.submit(alice.submitted[3].entry);
results["duplicate submission returns the original index"] = dup.duplicate && dup.index === alice.submitted[3].index;
results["honest log raises no alarms"] = alice.alarms.length === 0 && bob.alarms.length === 0;
results["monitors saw growing heads"] = alice.heads.keys().length === 6;
const all = await alice.get(`/entries?start=0&end=${honest.log.size - 1}`);
results["entries endpoint replays to the advertised root"] = HexEncode(mth(all)) === (await alice.get("/sth")).root;
results["out-of-range proof requests are refused"] =
    (await fetch(honest.base + "/proof?index=5&size=99999")).status === 400;

// =============================================================================
// Attack: a fork that rewrites history after publishing a head
// =============================================================================
const evil = new LogServer("evil-log");
const carol = new Monitor("carol", evil),
    dave = new Monitor("dave", evil);
for (let i = 0; i < 20; i++) await carol.submit(`pkg:v${i}`);
await carol.poll();
await dave.poll();
// The log replaces entry 5 (a malicious package build) and rebuilds; everyone who polls later is served the fork.
const forked = new MerkleLog();
for (let i = 0; i < evil.log.size; i++) forked.append(i === 5 ? "pkg:v5-backdoored" : evil.log.entries[i]);
evil.log = forked;
for (let i = 20; i < 30; i++) await carol.submit(`pkg:v${i}`);
await carol.poll();
results["fork is caught by consistency checking"] = carol.alarms.some((a) =>
    a.startsWith("inconsistent heads 20 -> 30"),
);
results["fork is caught by inclusion audit"] = (await carol.audit(await carol.poll())) === 1;

// Gossip: dave polled before the fork. Anyone can replay the first 20 entries the log serves today; a root that
// differs from a signed head of the same size is equivocation, provable without trusting either monitor.
const gossip = new Multimap();
for (const m of [carol, dave]) for (const [, h] of m.heads.entries()) gossip.put(String(h.size), h.root);
const replayed = await dave.get("/entries?start=0&end=19");
gossip.put("20", HexEncode(mth(replayed)));
const splitView = gossip.keys().filter((size) => new Set(gossip.get(size)).size > 1);
results["gossip exposes two roots for size 20"] = splitView.length === 1 && splitView[0] === "20";
results["signed pre-fork head still verifies, so the log cannot deny it"] = (() => {
    const h = dave.heads.get(String(20).padStart(10, "0"));
    return Ed25519Verify(evil.keys.publicKey, `evil-log|20|${h.root}|${h.timestamp}`, HexDecode(h.sig));
})();

// =============================================================================
// Scale: appending and proving over a larger log
// =============================================================================
{
    const big = new MerkleLog();
    const t0 = performance.now();
    for (let i = 0; i < 20000; i++) big.append(`bulk-${i}`);
    const appendMs = performance.now() - t0;
    const t1 = performance.now();
    let ok = 0;
    for (let i = 0; i < 500; i++) {
        const m = rng.nextBounded(20000);
        if (verifyInclusion(m, 20000, leafHash(`bulk-${m}`), big.inclusion(m, 20000), big.root())) ok++;
    }
    const proveMs = performance.now() - t1;
    results["20k-entry log proves 500 random inclusions"] = ok === 500;
    results["stored subtrees total 2n - popcount(n)"] =
        big.levels.reduce((s, l) => s + l.length, 0) ===
        2 * 20000 - [...(20000).toString(2)].filter((b) => b === "1").length;
    print(
        `append ${(appendMs / 20).toFixed(2)} us/entry, prove+verify ${(proveMs / 500).toFixed(3)} ms/proof, root ${HexEncode(big.root()).slice(0, 16)}...`,
    );
}

honest.close();
evil.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
