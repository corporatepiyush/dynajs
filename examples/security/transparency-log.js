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
//   witnesses   three witnesses cosign a head only if it is consistent with the last one they cosigned for that log
//               (refusing rollbacks, equivocating re-signed heads and forks); a verifier accepts a checkpoint with a
//               quorum of DISTINCT witnesses, which is what protects a client that has never seen the log before
//   tiles       the C2SP tlog-tiles read path: the server serves raw 256-hash tiles (a partial one at the right edge,
//               only as wide as it is complete); the client computes roots, inclusion and consistency proofs itself,
//               fetching what one pass asked for in a single parallel round, and matches the server byte for byte
//
// MODULES  dyna:hash (SHA256) · dyna:bytes (concat, equal) · dyna:crypto (Ed25519) · dyna:encoding (HexEncode,
//          HexDecode) · dyna:structures (Interner, OrderedMap, Multimap, LRU) · dyna:http (App, fetch) · dyna:random
//          · Iterator.range
//
// RUN      dynajs examples/security/transparency-log.js

import { SHA256 } from "dyna:hash";
import { concat, equal } from "dyna:bytes";
import { Ed25519Generate, Ed25519Sign, Ed25519Verify } from "dyna:crypto";
import { HexEncode, HexDecode } from "dyna:encoding";
import { Interner, OrderedMap, Multimap, LRU } from "dyna:structures";
import { App } from "dyna:http";
import { Random } from "dyna:random";

const results = {};
const enc = new TextEncoder();
const LEAF = new Uint8Array([0]);
const leafHash = (entry) => SHA256(concat([LEAF, typeof entry === "string" ? enc.encode(entry) : entry]));
// one 65-byte scratch for every interior node: 0.27 us vs 0.47 us for concat([NODE, l, r]) (no array, no allocation)
const NODE_IN = new Uint8Array(65);
NODE_IN[0] = 1;
const nodeHash = (l, r) => {
    NODE_IN.set(l, 1);
    NODE_IN.set(r, 33);
    return SHA256(NODE_IN);
};
const largestPow2Below = (n) => {
    let k = 1;
    while (k << 1 < n) k <<= 1;
    return k;
};

// =============================================================================
// Reading a tree: every root and proof is assembled from complete subtrees, node(h, i) = MTH(D[i*2^h : (i+1)*2^h])
// =============================================================================
class TreeReader {
    // MTH(D[lo:hi]) from stored complete subtrees; recursion only over the right spine.
    hash(lo, hi) {
        const n = hi - lo;
        if (n === 0) return SHA256(new Uint8Array(0));
        if ((n & (n - 1)) === 0 && lo % n === 0) return this.node(Math.log2(n), lo / n);
        const k = largestPow2Below(n);
        return nodeHash(this.hash(lo, lo + k), this.hash(lo + k, hi));
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

// =============================================================================
// The log: complete subtrees by level
// =============================================================================
class MerkleLog extends TreeReader {
    constructor() {
        super();
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
    node(h, i) {
        return this.levels[h][i];
    }
    root(size = this.size) {
        return this.hash(0, size);
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
        // C2SP tlog-tiles: tile (L, N) holds the hashes of tree level L*TILE_H, nodes N*256 .. N*256+width-1, raw
        // 32-byte hashes back to back; a partial tile at the right edge is served only as wide as it is complete
        this.app.get("/tile", (req) => {
            const L = Number(req.query.level),
                N = Number(req.query.index),
                width = Number(req.query.width);
            const row = this.log.levels[L * TILE_H] ?? [];
            if (!(width >= 1 && width <= TILE_W && N * TILE_W + width <= row.length))
                return { status: 400, body: "tile not complete" };
            return {
                body: concat(row.slice(N * TILE_W, N * TILE_W + width)),
                contentType: "application/octet-stream",
            };
        });
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

// =============================================================================
// Tiles: a client that computes roots and proofs itself from immutable, cacheable tiles
// =============================================================================
// A node at tree level h lies in the tile of level floor(h / 8); it is the hash of 2^(h mod 8) aligned hashes of
// that tile's row, folded pairwise. A computation runs against a view that answers from cached tiles and RECORDS
// the ones it lacked (answering a placeholder); those are fetched in one parallel round and the computation re-run.
// Which tiles a root or proof needs depends only on the sizes, never on hash values, so one round always suffices.
const TILE_H = 8,
    TILE_W = 1 << TILE_H,
    MISSING = new Uint8Array(32);
class TileView extends TreeReader {
    constructor(client, size) {
        super();
        this.client = client;
        this.size = size;
        this.missing = new Map();
    }
    node(h, i) {
        const L = Math.floor(h / TILE_H),
            k = h - L * TILE_H,
            span = 1 << k,
            first = i * span,
            N = Math.floor(first / TILE_W),
            offset = first - N * TILE_W;
        const rowLen = Math.floor(this.size / 2 ** (L * TILE_H)),
            width = Math.min(TILE_W, rowLen - N * TILE_W);
        const tile = this.client.cached(L, N, width);
        if (!tile) {
            this.missing.set(`${L}/${N}/${width}`, [L, N, width]);
            return MISSING;
        }
        let row = [];
        for (let j = 0; j < span; j++) row.push(tile.subarray((offset + j) * 32, (offset + j + 1) * 32));
        while (row.length > 1)
            row = Iterator.range(0, row.length, 2)
                .map((j) => nodeHash(row[j], row[j + 1]))
                .toArray();
        return row[0];
    }
}
class TileClient {
    constructor(base) {
        this.base = base;
        this.tiles = new LRU(64);
        this.fetched = 0;
        this.full = new Map();
        this.rounds = 0;
    }
    cached(L, N, width) {
        // a full tile serves every size; a partial one only the width it was fetched at
        return this.tiles.get(`${L}/${N}/${TILE_W}`) ?? this.tiles.get(`${L}/${N}/${width}`);
    }
    async fetchTile(L, N, width) {
        const r = await fetch(`${this.base}/tile?level=${L}&index=${N}&width=${width}`);
        if (r.status !== 200) throw new Error(`tile ${L}/${N}/${width}: ${r.status}`);
        const bytes = new Uint8Array(await r.arrayBuffer());
        if (bytes.length !== width * 32) throw new Error(`tile ${L}/${N}: ${bytes.length} bytes for width ${width}`);
        this.fetched++;
        if (width === TILE_W) this.full.set(`${L}/${N}`, (this.full.get(`${L}/${N}`) ?? 0) + 1);
        this.tiles.set(`${L}/${N}/${width}`, bytes);
    }
    async compute(size, f) {
        for (let pass = 0; ; pass++) {
            const view = new TileView(this, size),
                out = f(view);
            if (!view.missing.size) return { out, passes: pass + 1 };
            this.rounds++;
            await Promise.all([...view.missing.values()].map(([L, N, w]) => this.fetchTile(L, N, w)));
        }
    }
}

// =============================================================================
// Witnesses: cosign a checkpoint only if it extends everything they have cosigned for that log
// =============================================================================
class Witness {
    constructor(name) {
        this.name = name;
        this.keys = Ed25519Generate();
        this.latest = new Map();
        this.refusals = [];
    }
    refuse(server, sth, why) {
        this.refusals.push({ log: server.name, size: sth.size, why });
        return null;
    }
    async cosign(server, sth) {
        const body = `${server.name}|${sth.size}|${sth.root}|${sth.timestamp}`;
        if (!Ed25519Verify(server.keys.publicKey, body, HexDecode(sth.sig)))
            return this.refuse(server, sth, "bad log signature");
        const prev = this.latest.get(server.name);
        if (prev) {
            if (sth.size < prev.size) return this.refuse(server, sth, "rollback");
            if (sth.size === prev.size && sth.root !== prev.root) return this.refuse(server, sth, "equivocation");
            if (sth.size > prev.size) {
                const c = await (
                    await fetch(`${server.base}/consistency?first=${prev.size}&second=${sth.size}`)
                ).json();
                const ok = verifyConsistency(
                    prev.size,
                    sth.size,
                    HexDecode(prev.root),
                    HexDecode(sth.root),
                    c.path.map(HexDecode),
                );
                if (!ok) return this.refuse(server, sth, "inconsistent");
            }
        }
        this.latest.set(server.name, sth);
        return { witness: this.name, sig: HexEncode(Ed25519Sign(this.keys.privateKey, `cosign|${body}`)) };
    }
}
// A verifier's policy: the log's own signature, plus valid cosignatures from at least `quorum` DISTINCT trusted
// witnesses. Without witnesses a client seeing a log for the first time cannot tell a fork from the real thing.
function acceptCheckpoint(server, sth, cosigs, trusted, quorum) {
    const body = `${server.name}|${sth.size}|${sth.root}|${sth.timestamp}`;
    if (!Ed25519Verify(server.keys.publicKey, body, HexDecode(sth.sig))) return false;
    const signers = new Set();
    for (const c of cosigs) {
        const key = c && trusted.get(c.witness);
        if (key && Ed25519Verify(key, `cosign|${body}`, HexDecode(c.sig))) signers.add(c.witness);
    }
    return signers.size >= quorum;
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
const witnesses = ["w-armory", "w-mirror", "w-tessera"].map((n) => new Witness(n));
const trusted = new Map(witnesses.map((w) => [w.name, w.keys.publicKey]));
const cosignAll = (server, sth) => Promise.all(witnesses.map((w) => w.cosign(server, sth)));
const honestAccepted = [];
for (let round = 0; round < 6; round++) {
    const batch = [];
    for (let i = 0; i < 40; i++)
        batch.push((i & 1 ? alice : bob).submit(`cert:${round}:${i}:${rng.nextU53().toString(36)}`));
    await Promise.all(batch);
    const [sa, sb] = [await alice.poll(), await bob.poll()];
    for (const sth of [sa, sb])
        honestAccepted.push(acceptCheckpoint(honest, sth, await cosignAll(honest, sth), trusted, 3));
    await alice.audit(sa);
    await bob.audit(sb);
}
const dup = await alice.submit(alice.submitted[3].entry);
results["duplicate submission returns the original index"] = dup.duplicate && dup.index === alice.submitted[3].index;
results["honest log raises no alarms"] = alice.alarms.length === 0 && bob.alarms.length === 0;
results["monitors saw growing heads"] = alice.heads.keys().length === 6;
const all = await alice.get(`/entries?start=0&end=${honest.log.size - 1}`);
results["entries endpoint replays to the advertised root"] = HexEncode(mth(all)) === (await alice.get("/sth")).root;
results[`witnesses cosigned all ${honestAccepted.length} honest checkpoints (3 of 3, re-presented heads included)`] =
    honestAccepted.length === 12 && honestAccepted.every(Boolean) && witnesses.every((w) => !w.refusals.length);
{
    // a rollback to the first head, and a log key that did not sign
    const first = alice.heads.get(alice.heads.firstKey());
    const rolled = await cosignAll(honest, first);
    const forged = await cosignAll(honest, { ...(await alice.get("/sth")), sig: first.sig });
    results["witnesses refuse a rollback and an unsigned head"] =
        rolled.every((c) => c === null) &&
        forged.every((c) => c === null) &&
        witnesses.every((w) => w.refusals.map((r) => r.why).join() === "rollback,bad log signature");
    witnesses.forEach((w) => (w.refusals.length = 0));
}
results["out-of-range proof requests are refused"] =
    (await fetch(honest.base + "/proof?index=5&size=99999")).status === 400;

// =============================================================================
// Attack: a fork that rewrites history after publishing a head
// =============================================================================
const evil = new LogServer("evil-log");
const carol = new Monitor("carol", evil),
    dave = new Monitor("dave", evil);
for (let i = 0; i < 20; i++) await carol.submit(`pkg:v${i}`);
const preFork = await carol.poll();
await dave.poll();
const preForkAccepted = acceptCheckpoint(evil, preFork, await cosignAll(evil, preFork), trusted, 2);
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
{
    // A client meeting this log for the first time sees a validly signed head and valid proofs inside it: only the
    // witnesses, who remember size 20, can tell. The log also re-signs a size-20 head over the forked tree.
    const forkHead = await carol.get("/sth");
    const forkCosigs = await cosignAll(evil, forkHead);
    const resigned = (() => {
        const root = HexEncode(evil.log.root(20)),
            timestamp = evil.clock;
        return {
            size: 20,
            root,
            timestamp,
            sig: HexEncode(Ed25519Sign(evil.keys.privateKey, `evil-log|20|${root}|${timestamp}`)),
        };
    })();
    const resignedCosigs = await cosignAll(evil, resigned);
    const reasons = witnesses.map((w) => w.refusals.map((r) => r.why).join());
    results["witnesses refuse the fork and the re-signed equivocating head; quorum policy rejects it"] =
        preForkAccepted &&
        forkCosigs.every((c) => c === null) &&
        resignedCosigs.every((c) => c === null) &&
        reasons.every((r) => r === "inconsistent,equivocation") &&
        !acceptCheckpoint(evil, forkHead, forkCosigs, trusted, 2) &&
        acceptCheckpoint(evil, forkHead, [], trusted, 0);
    // one witness's cosignature presented twice is still one witness
    const one = await witnesses[0].cosign(honest, await alice.get("/sth"));
    const sth = await alice.get("/sth");
    results["a quorum counts distinct witnesses, not signatures"] =
        !acceptCheckpoint(honest, sth, [one, one], trusted, 2) &&
        acceptCheckpoint(honest, sth, [one, await witnesses[1].cosign(honest, sth)], trusted, 2);
}

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
// Tiles: the read path without server-side proof computation
// =============================================================================
{
    const tiled = new LogServer("tiled-log");
    for (let i = 0; i < 5000; i++) tiled.log.append(`mod:${i}`);
    const client = new TileClient(tiled.base);
    // roots at sizes on both sides of every tile boundary the tree has (256 leaves, 256 level-8 nodes is 65536)
    const sizes = [1, 2, 255, 256, 257, 511, 512, 513, 4095, 4096, 4097, 4999, 5000];
    let rootsOk = true,
        passesOk = true;
    for (const n of sizes) {
        const { out, passes } = await client.compute(n, (v) => v.hash(0, n));
        if (!equal(out, tiled.log.root(n))) rootsOk = false;
        if (passes > 2) passesOk = false;
    }
    results[`tile-computed roots equal the log's at ${sizes.length} sizes across tile edges`] = rootsOk;
    const fetchedForRoots = client.fetched;
    let proofsOk = true,
        consOk = true,
        n200 = 0;
    for (let i = 0; i < 200; i++) {
        const n = 1 + rng.nextBounded(5000),
            m = rng.nextBounded(n);
        const { out: mine, passes } = await client.compute(n, (v) => v.inclusion(m, n));
        if (passes > 2) passesOk = false;
        const theirs = (await (await fetch(`${tiled.base}/proof?index=${m}&size=${n}`)).json()).path;
        if (mine.map(HexEncode).join() !== theirs.join()) proofsOk = false;
        if (!verifyInclusion(m, n, leafHash(`mod:${m}`), mine, tiled.log.root(n))) proofsOk = false;
        n200++;
    }
    for (let i = 0; i < 60; i++) {
        const n = 2 + rng.nextBounded(4999),
            m = 1 + rng.nextBounded(n - 1);
        const { out } = await client.compute(n, (v) => v.consistency(m, n));
        if (!verifyConsistency(m, n, tiled.log.root(m), tiled.log.root(n), out)) consOk = false;
        if (out.map(HexEncode).join() !== tiled.log.consistency(m, n).map(HexEncode).join()) consOk = false;
    }
    results[`${n200} client-built inclusion proofs equal the server's byte for byte and verify`] = proofsOk;
    results["60 client-built consistency proofs equal the server's and verify"] = consOk;
    results["every computation needed at most one parallel round of tile fetches"] = passesOk;
    // A full tile is immutable: fetched once, then every size reuses it. The right-edge partial tile is the only
    // per-size fetch (one per level a computation touches), so 213 distinct sizes cost at most 2 x 213 partials.
    const fullOnce = [...client.full.values()].every((c) => c === 1),
        partials = client.fetched - client.full.size;
    results[`${client.full.size} full tiles fetched once each, ${partials} edge partials for 273 computations`] =
        fullOnce && client.full.size <= 19 && partials <= 2 * 213 && client.tiles.stats.hits > 1000;
    results["a tile wider than its complete part is refused"] =
        (await fetch(`${tiled.base}/tile?level=0&index=19&width=256`)).status === 400 &&
        (await fetch(`${tiled.base}/tile?level=1&index=0&width=20`)).status === 400 &&
        (await fetch(`${tiled.base}/tile?level=1&index=0&width=19`)).status === 200;
    tiled.close();
}

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
