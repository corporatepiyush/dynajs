// Supply-chain signing — reproducible release artifacts, an SBOM, signed provenance, a transparency log with Merkle
// proofs, TUF-style update metadata, and the verifier a deploy pipeline runs before installing anything.
//
// WHAT IT IS
//   Everything between "CI built it" and "production may run it":
//
//   build        a project tree is packed into a deterministic tar.gz (sorted entries, fixed mtimes, no owner
//                names) so two builds of the same commit are byte-identical — and that is checked
//   SBOM         a CycloneDX document generated from the lockfile: every component with its version, license and
//                hash, plus the dependency graph
//   policy       license allow-list, banned packages, and known-vulnerable ranges from an advisory feed matched
//                with semver ranges — a violation stops the release before anything is signed
//   provenance   an in-toto statement (subject digests, builder, materials) wrapped in a DSSE envelope and signed
//                with Ed25519
//   log          every signature is appended to a transparency log, a Merkle tree (RFC 6962 hashing); the
//                verifier checks an inclusion proof and that the log only ever grew (consistency proof)
//   updates      TUF-style metadata — root (keys and thresholds), targets, snapshot, timestamp — each signed and
//                expiring; a client refuses rollback, expired timestamps, and a targets file signed by too few keys
//   verify       the deploy-side check runs every one of the above and prints a verdict per rule
//   blast radius the lockfile's dependency graph is a dyna:structures Graph; a vulnerable package is reported with
//                every component that pulls it in (a BFS on the reversed graph), so the fix lands where it must
//   bytes        Merkle leaf/node preimages and DSSE PAE strings are assembled with dyna:bytes concat, not by
//                spreading every byte into a JS array
//
// MODULES  dyna:compress (TarPack, gzip) · dyna:crypto (Ed25519) · dyna:hash (SHA256, BLAKE3) · dyna:semver
//          · dyna:serialize (CBORCanonical) · dyna:encoding (Base64) · dyna:file · dyna:json (Pointer) · dyna:time
//          · dyna:structures (Graph) · dyna:bytes (concat)
//
// RUN      dynajs examples/security/supply-chain-signing.js     builds, signs, logs and verifies two releases

import { TarPack, TarList, gzip, gunzip } from "dyna:compress";
import * as crypto from "dyna:crypto";
import { SHA256, SHA256Hex, BLAKE3Hex } from "dyna:hash";
import * as semver from "dyna:semver";
import { Graph } from "dyna:structures";
import { concat } from "dyna:bytes";
import { CBORCanonical } from "dyna:serialize";
import { Base64Encode, Base64Decode, HexEncode } from "dyna:encoding";
import { makeTempDir, makeDir, writeFile, readFile, readBytes, readDir, removeAll } from "dyna:file";
import { Pointer as JSONPointer } from "dyna:json";
import { formatRFC3339 } from "dyna:time";

const work = makeTempDir("supply");
const enc = new TextEncoder();
const HAVE_ED25519 = typeof crypto.Ed25519Generate === "function";
const scheme = HAVE_ED25519 ? "ed25519" : "hmac-sha256 (no Ed25519 in this build)";
function keypair() {
    if (HAVE_ED25519) {
        const k = crypto.Ed25519Generate();
        return { id: SHA256Hex(k.publicKey).slice(0, 16), priv: k.privateKey, pub: k.publicKey };
    }
    const s = crypto.RandomBytes(32);
    return { id: SHA256Hex(s).slice(0, 16), priv: s, pub: s };
}
const sign = (k, msg) => (HAVE_ED25519 ? crypto.Ed25519Sign(k.priv, msg) : crypto.HMAC("sha256", k.priv, msg));
const verifySig = (pub, msg, sig) =>
    HAVE_ED25519 ? crypto.Ed25519Verify(pub, msg, sig) : crypto.TimingSafeEqual(crypto.HMAC("sha256", pub, msg), sig);

// =============================================================================
// The project being released
// =============================================================================
const src = work.join("src");
function writeProject(version, extraDep) {
    removeAll(src);
    makeDir(src.join("lib"), { recursive: true });
    writeFile(
        src.join("package.json"),
        JSON.stringify({ name: "acme-billing", version, license: "Apache-2.0" }, null, 2),
    );
    writeFile(
        src.join("lib/index.js"),
        `export const VERSION = "${version}";\nexport function total(xs) { return xs.reduce((a, b) => a + b, 0); }\n`,
    );
    writeFile(src.join("lib/tax.js"), "export const rate = (country) => ({ DE: 0.19, FR: 0.2 })[country] ?? 0;\n");
    writeFile(src.join("README"), "Billing service.\n");
    const lock = {
        "left-pad": { version: "1.3.0", license: "WTFPL", deps: [] },
        "decimal-lite": { version: "2.4.1", license: "MIT", deps: [] },
        "http-kit": { version: "5.2.0", license: "Apache-2.0", deps: ["url-parse", "decimal-lite"] },
        "url-parse": { version: "1.5.2", license: "MIT", deps: [] },
        "yaml-tiny": { version: "0.9.4", license: "ISC", deps: [] },
        ...extraDep,
    };
    writeFile(src.join("lockfile.json"), JSON.stringify(lock, null, 2));
    return lock;
}

// =============================================================================
// Deterministic build
// =============================================================================
function walk(dir, prefix = "") {
    return readDir(dir)
        .sort((a, b) => a.name.localeCompare(b.name))
        .flatMap((e) =>
            e.isDir
                ? walk(dir.join(e.name), `${prefix}${e.name}/`)
                : [{ name: `${prefix}${e.name}`, data: readBytes(dir.join(e.name)) }],
        );
}
function build(version) {
    const entries = walk(src).map((f) => ({
        name: `acme-billing-${version}/${f.name}`,
        data: f.data,
        mode: 0o644,
        mtime: 315532800,
    }));
    const tar = TarPack(entries);
    return { name: `acme-billing-${version}.tar.gz`, bytes: gzip(tar, { level: 9 }), files: entries.length };
}

// =============================================================================
// SBOM and policy
// =============================================================================
function sbom(version, lock, artifact) {
    return {
        bomFormat: "CycloneDX",
        specVersion: "1.5",
        version: 1,
        serialNumber: `urn:uuid:${SHA256Hex(artifact.bytes).slice(0, 8)}-0000-4000-8000-${SHA256Hex(version).slice(0, 12)}`,
        metadata: {
            component: {
                type: "application",
                name: "acme-billing",
                version,
                hashes: [{ alg: "SHA-256", content: SHA256Hex(artifact.bytes) }],
            },
        },
        components: Object.entries(lock)
            .sort()
            .map(([name, c]) => ({
                type: "library",
                "bom-ref": `pkg:npm/${name}@${c.version}`,
                name,
                version: c.version,
                licenses: [{ license: { id: c.license } }],
                hashes: [{ alg: "BLAKE3", content: BLAKE3Hex(`${name}@${c.version}`) }],
            })),
        dependencies: Object.entries(lock).map(([name, c]) => ({
            ref: `pkg:npm/${name}@${c.version}`,
            dependsOn: c.deps.map((d) => `pkg:npm/${d}@${lock[d].version}`),
        })),
    };
}
const ADVISORIES = [
    {
        id: "GHSA-u-p-01",
        package: "url-parse",
        range: "<1.5.9",
        severity: "high",
        summary: "authorization bypass via hostname spoofing",
    },
    {
        id: "GHSA-y-t-02",
        package: "yaml-tiny",
        range: ">=0.9.0 <0.9.3",
        severity: "critical",
        summary: "prototype pollution",
    },
    { id: "GHSA-h-k-03", package: "http-kit", range: "^4.0.0", severity: "medium", summary: "header injection" },
];
const LICENSES = new Set(["MIT", "Apache-2.0", "ISC", "BSD-3-Clause"]);
const BANNED = { "event-stream": "*", "left-pad": ">=2.0.0" };
function dependants(bom) {
    // package -> every component that pulls it in
    const names = bom.components.map((c) => c.name),
        id = new Map(names.map((n, i) => [n, i]));
    const reversed = new Graph({ directed: true });
    for (let i = 0; i < names.length; i++) reversed.addNode();
    const root = reversed.addNode();
    for (const d of bom.dependencies) {
        const from = d.ref.replace(/^pkg:npm\/|@[^@]*$/g, "");
        for (const to of d.dependsOn) reversed.addEdge(id.get(to.replace(/^pkg:npm\/|@[^@]*$/g, "")), id.get(from));
        reversed.addEdge(id.get(from), root);
    }
    return (pkg) =>
        reversed
            .bfs(id.get(pkg))
            .filter((n) => n !== id.get(pkg))
            .map((n) => (n === root ? "acme-billing" : names[n]));
}
function policy(bom) {
    const findings = [],
        via = dependants(bom);
    for (const c of bom.components) {
        const lic = c.licenses[0].license.id;
        if (!LICENSES.has(lic))
            findings.push({
                rule: "license",
                component: `${c.name}@${c.version}`,
                detail: `${lic} is not on the allow-list`,
                blocking: lic !== "WTFPL",
            });
        if (BANNED[c.name] && semver.satisfies(c.version, BANNED[c.name]))
            findings.push({
                rule: "banned",
                component: `${c.name}@${c.version}`,
                detail: "banned package",
                blocking: true,
            });
        for (const a of ADVISORIES)
            if (a.package === c.name && semver.satisfies(c.version, a.range))
                findings.push({
                    rule: "vulnerability",
                    component: `${c.name}@${c.version}`,
                    detail: `${a.id} (${a.severity}) ${a.summary}; pulled in by ${via(c.name).join(" <- ")}`,
                    blocking: ["high", "critical"].includes(a.severity),
                    via: via(c.name),
                });
    }
    return findings;
}

// =============================================================================
// DSSE envelopes, in-toto provenance
// =============================================================================
const PAE = (type, body) => concat([enc.encode(`DSSEv1 ${enc.encode(type).length} ${type} ${body.length} `), body]);
function envelope(payloadType, payload, keys) {
    const body = enc.encode(JSON.stringify(payload));
    return {
        payloadType,
        payload: Base64Encode(body),
        signatures: keys.map((k) => ({ keyid: k.id, sig: Base64Encode(sign(k, PAE(payloadType, body))) })),
    };
}
function openEnvelope(env, trusted, threshold = 1) {
    const body = Base64Decode(env.payload),
        msg = PAE(env.payloadType, body);
    const valid = new Set(
        env.signatures
            .filter((s) => trusted.has(s.keyid) && verifySig(trusted.get(s.keyid), msg, Base64Decode(s.sig)))
            .map((s) => s.keyid),
    );
    return { ok: valid.size >= threshold, valid: valid.size, payload: JSON.parse(new TextDecoder().decode(body)) };
}
const provenance = (artifact, commit, builder) => ({
    _type: "https://in-toto.io/Statement/v1",
    subject: [{ name: artifact.name, digest: { sha256: SHA256Hex(artifact.bytes) } }],
    predicateType: "https://slsa.dev/provenance/v1",
    predicate: {
        buildDefinition: {
            buildType: "https://acme.example/ci/v2",
            externalParameters: {
                repository: "git+https://git.acme.example/billing",
                ref: "refs/tags/release",
                commit,
            },
        },
        runDetails: {
            builder: { id: builder },
            metadata: { invocationId: `ci-${commit.slice(0, 7)}`, startedOn: formatRFC3339(1772524800) },
        },
    },
});

// =============================================================================
// Transparency log (RFC 6962 Merkle tree)
// =============================================================================
const LEAF = new Uint8Array([0]),
    NODE = new Uint8Array([1]);
const leafHash = (data) => SHA256(concat([LEAF, data]));
const nodeHash = (l, r) => SHA256(concat([NODE, l, r]));
const largestPow2Below = (n) => {
    let k = 1;
    while (k * 2 < n) k *= 2;
    return k;
};
class TransparencyLog {
    constructor() {
        this.leaves = [];
    }
    append(entry) {
        this.leaves.push(leafHash(CBORCanonical(entry)));
        return this.leaves.length - 1;
    }
    root(n = this.leaves.length, lo = 0) {
        if (n === 1) return this.leaves[lo];
        const k = largestPow2Below(n);
        return nodeHash(this.root(k, lo), this.root(n - k, lo + k));
    }
    inclusion(m, n = this.leaves.length, lo = 0) {
        if (n === 1) return [];
        const k = largestPow2Below(n);
        return m < k
            ? [...this.inclusion(m, k, lo), this.root(n - k, lo + k)]
            : [...this.inclusion(m - k, n - k, lo + k), this.root(k, lo)];
    }
    consistency(m, n = this.leaves.length, lo = 0, complete = true) {
        if (m === n) return complete ? [] : [this.root(m, lo)];
        const k = largestPow2Below(n);
        return m <= k
            ? [...this.consistency(m, k, lo, complete), this.root(n - k, lo + k)]
            : [...this.consistency(m - k, n - k, lo + k, false), this.root(k, lo)];
    }
}
function verifyInclusion(leaf, index, size, proof, root) {
    // RFC 9162 section 2.1.3.2
    let fn = index,
        sn = size - 1,
        r = leaf;
    for (const p of proof) {
        if (sn === 0) return false;
        if (fn % 2 === 1 || fn === sn) {
            r = nodeHash(p, r);
            if (fn % 2 === 0)
                while (fn % 2 === 0 && fn !== 0) {
                    fn >>= 1;
                    sn >>= 1;
                }
        } else r = nodeHash(r, p);
        fn >>= 1;
        sn >>= 1;
    }
    return sn === 0 && HexEncode(r) === HexEncode(root);
}
function verifyConsistency(m, n, proof, oldRoot, newRoot) {
    // RFC 9162 section 2.1.4.2
    if (m === n) return proof.length === 0 && HexEncode(oldRoot) === HexEncode(newRoot);
    let path = proof.slice();
    if ((m & (m - 1)) === 0) path = [oldRoot, ...path];
    let fn = m - 1,
        sn = n - 1;
    while (fn % 2 === 1) {
        fn >>= 1;
        sn >>= 1;
    }
    let fr = path[0],
        sr = path[0];
    for (const c of path.slice(1)) {
        if (sn === 0) return false;
        if (fn % 2 === 1 || fn === sn) {
            fr = nodeHash(c, fr);
            sr = nodeHash(c, sr);
            if (fn % 2 === 0)
                while (fn % 2 === 0 && fn !== 0) {
                    fn >>= 1;
                    sn >>= 1;
                }
        } else sr = nodeHash(sr, c);
        fn >>= 1;
        sn >>= 1;
    }
    return sn === 0 && HexEncode(fr) === HexEncode(oldRoot) && HexEncode(sr) === HexEncode(newRoot);
}

// =============================================================================
// TUF-style metadata
// =============================================================================
const NOW = Date.UTC(2026, 5, 1);
const rootKeys = [keypair(), keypair(), keypair()],
    targetsKeys = [keypair(), keypair()],
    snapKey = keypair(),
    timeKey = keypair();
const keyMap = (ks) => new Map(ks.map((k) => [k.id, k.pub]));
const ROOT = {
    version: 1,
    expires: NOW + 365 * 86400_000,
    roles: {
        root: { keys: rootKeys.map((k) => k.id), threshold: 2 },
        targets: { keys: targetsKeys.map((k) => k.id), threshold: 2 },
        snapshot: { keys: [snapKey.id], threshold: 1 },
        timestamp: { keys: [timeKey.id], threshold: 1 },
    },
};
const allKeys = keyMap([...rootKeys, ...targetsKeys, snapKey, timeKey]);
const rootEnv = envelope("application/vnd.tuf.root+json", ROOT, rootKeys.slice(0, 2));
function publishTuf(version, artifact, signers = targetsKeys) {
    const targets = {
        version,
        expires: NOW + 90 * 86400_000,
        targets: { [artifact.name]: { length: artifact.bytes.length, hashes: { sha256: SHA256Hex(artifact.bytes) } } },
    };
    const tEnv = envelope("application/vnd.tuf.targets+json", targets, signers);
    const snapshot = {
        version,
        expires: NOW + 7 * 86400_000,
        meta: { "targets.json": { version, sha256: SHA256Hex(JSON.stringify(tEnv)) } },
    };
    const sEnv = envelope("application/vnd.tuf.snapshot+json", snapshot, [snapKey]);
    const timestamp = {
        version,
        expires: NOW + 86400_000,
        meta: { "snapshot.json": { version, sha256: SHA256Hex(JSON.stringify(sEnv)) } },
    };
    return { tEnv, sEnv, tsEnv: envelope("application/vnd.tuf.timestamp+json", timestamp, [timeKey]) };
}
class UpdateClient {
    constructor(rootEnvelope) {
        const r = openEnvelope(rootEnvelope, keyMap(rootKeys), 2);
        if (!r.ok) throw new Error("untrusted root");
        this.root = r.payload;
        this.seen = { targets: 0, snapshot: 0, timestamp: 0 };
    }
    role(name, env) {
        const spec = this.root.roles[name];
        const r = openEnvelope(env, new Map(spec.keys.map((id) => [id, allKeys.get(id)])), spec.threshold);
        if (!r.ok) throw new Error(`${name}: ${r.valid} of ${spec.threshold} required signatures`);
        if (r.payload.version < this.seen[name])
            throw new Error(`${name}: rollback from v${this.seen[name]} to v${r.payload.version}`);
        if (r.payload.expires <= this.now) throw new Error(`${name}: expired`);
        this.seen[name] = r.payload.version;
        return r.payload;
    }
    update(meta, artifact, now) {
        this.now = now;
        const ts = this.role("timestamp", meta.tsEnv);
        if (ts.meta["snapshot.json"].sha256 !== SHA256Hex(JSON.stringify(meta.sEnv)))
            throw new Error("snapshot does not match timestamp");
        const snap = this.role("snapshot", meta.sEnv);
        if (snap.meta["targets.json"].sha256 !== SHA256Hex(JSON.stringify(meta.tEnv)))
            throw new Error("targets does not match snapshot");
        const t = this.role("targets", meta.tEnv).targets[artifact.name];
        if (!t || t.length !== artifact.bytes.length || t.hashes.sha256 !== SHA256Hex(artifact.bytes))
            throw new Error("artifact does not match targets");
        return true;
    }
}

// =============================================================================
// Release pipeline and the deploy-side verifier
// =============================================================================
const ciKey = keypair(),
    log = new TransparencyLog();
const checkpoints = [];
function release(version, extraDep = {}) {
    const lock = writeProject(version, extraDep);
    const a = build(version),
        b = build(version);
    const reproducible = HexEncode(SHA256(a.bytes)) === HexEncode(SHA256(b.bytes));
    const bom = sbom(version, lock, a);
    const findings = policy(bom);
    if (findings.some((f) => f.blocking)) return { version, refused: true, findings, reproducible };
    const prov = envelope(
        "application/vnd.in-toto+json",
        provenance(a, SHA256Hex(version + "commit"), "https://ci.acme.example/runners/hosted"),
        [ciKey],
    );
    const bomEnv = envelope("application/vnd.cyclonedx+json", bom, [ciKey]);
    const index = log.append({
        artifact: SHA256Hex(a.bytes),
        provenance: SHA256Hex(prov.payload),
        sbom: SHA256Hex(bomEnv.payload),
        signer: ciKey.id,
    });
    checkpoints.push({ size: log.leaves.length, root: log.root() });
    writeFile(work.join(a.name), a.bytes);
    return {
        version,
        artifact: a,
        bom,
        prov,
        bomEnv,
        index,
        findings,
        reproducible,
        tuf: publishTuf(checkpoints.length, a),
    };
}
function verifyRelease(r, client, now) {
    const results = [];
    const rule = (name, fn) => {
        try {
            const ok = fn();
            results.push([name, ok === false ? "FAIL" : "ok"]);
        } catch (e) {
            results.push([name, `FAIL: ${e.message}`]);
        }
    };
    const bytes = readBytes(work.join(r.artifact.name));
    rule(
        "artifact digest matches provenance subject",
        () => openEnvelope(r.prov, keyMap([ciKey])).payload.subject[0].digest.sha256 === SHA256Hex(bytes),
    );
    rule("provenance signed by the CI key", () => openEnvelope(r.prov, keyMap([ciKey])).ok);
    rule("builder is the hosted runner", () =>
        JSONPointer.get(openEnvelope(r.prov, keyMap([ciKey])).payload, "/predicate/runDetails/builder/id").startsWith(
            "https://ci.acme.example/",
        ),
    );
    rule("SBOM signed and describes this artifact", () => {
        const s = openEnvelope(r.bomEnv, keyMap([ciKey]));
        return s.ok && s.payload.metadata.component.hashes[0].content === SHA256Hex(bytes);
    });
    rule(
        "no blocking policy findings",
        () => !policy(openEnvelope(r.bomEnv, keyMap([ciKey])).payload).some((f) => f.blocking),
    );
    rule("included in the transparency log", () => {
        const cp = checkpoints[checkpoints.length - 1];
        return verifyInclusion(
            leafHash(
                CBORCanonical({
                    artifact: SHA256Hex(bytes),
                    provenance: SHA256Hex(r.prov.payload),
                    sbom: SHA256Hex(r.bomEnv.payload),
                    signer: ciKey.id,
                }),
            ),
            r.index,
            cp.size,
            log.inclusion(r.index, cp.size),
            cp.root,
        );
    });
    rule("log is append-only since the last checkpoint", () =>
        checkpoints.every(
            (c, i) =>
                i === 0 ||
                verifyConsistency(
                    checkpoints[i - 1].size,
                    c.size,
                    log.consistency(checkpoints[i - 1].size, c.size),
                    checkpoints[i - 1].root,
                    c.root,
                ),
        ),
    );
    rule("TUF metadata chain", () => client.update(r.tuf, { name: r.artifact.name, bytes }, now));
    rule("archive contents", () =>
        TarList(gunzip(bytes)).every((e) => e.name.startsWith(`acme-billing-${r.version}/`) && !e.name.includes("..")),
    );
    return results;
}

const results = {};
const r1 = release("1.4.0");
const r2 = release("1.5.0", { "url-parse": { version: "1.5.10", license: "MIT", deps: [] } });
const r3 = release("1.6.0", {
    "yaml-tiny": { version: "0.9.2", license: "ISC", deps: [] },
    "gpl-widget": { version: "1.0.0", license: "GPL-3.0", deps: [] },
});
for (let k = 0; k < 9; k++) log.append({ other: k }); // other projects share the log
checkpoints.push({ size: log.leaves.length, root: log.root() });

const client = new UpdateClient(rootEnv);
const v2 = verifyRelease(r2, client, NOW + 7200_000);
results["two builds of the same tree are byte-identical"] = r1.reproducible && r2.reproducible;
results["v1.4.0 is refused: url-parse 1.5.2 is in a vulnerable range"] =
    r1.findings.some((f) => f.rule === "vulnerability" && /GHSA-u-p-01/.test(f.detail) && f.blocking) || r1.refused;
results[`v1.5.0 passes every verification rule (${v2.filter(([, s]) => s !== "ok").map(([n, s]) => `${n}: ${s}`)})`] =
    !r2.refused && v2.every(([, s]) => s === "ok");
results["v1.6.0 is refused for a critical advisory and a copyleft license"] =
    r3.refused && r3.findings.filter((f) => f.blocking).length >= 2;
results["the url-parse advisory names the chain that pulls it in (http-kit, then the application)"] =
    r1.findings.find((f) => f.rule === "vulnerability" && f.component.startsWith("url-parse"))?.via.join() ===
    "http-kit,acme-billing";
results["a non-blocking license note (WTFPL) is reported, not fatal"] = r2.findings.some(
    (f) => f.rule === "license" && !f.blocking,
);

// Attacks the verifier must catch.
const tampered = readBytes(work.join(r2.artifact.name));
tampered[100] ^= 1;
writeFile(work.join(r2.artifact.name), tampered);
const vt = verifyRelease(r2, new UpdateClient(rootEnv), NOW + 7200_000);
results["a modified artifact fails digest and TUF checks"] =
    vt.find(([n]) => n.startsWith("artifact digest"))[1] === "FAIL" &&
    vt.find(([n]) => n === "TUF metadata chain")[1].startsWith("FAIL");
writeFile(work.join(r2.artifact.name), r2.artifact.bytes);
const rollback = new UpdateClient(rootEnv);
rollback.update(r2.tuf, r2.artifact, NOW + 3600_000);
let refusedRollback = "accepted";
try {
    rollback.update(publishTuf(0, r2.artifact), r2.artifact, NOW + 3600_000);
} catch (e) {
    refusedRollback = e.message;
}
results[`rollback refused (${refusedRollback})`] = /rollback/.test(refusedRollback);
let expired = "accepted";
try {
    new UpdateClient(rootEnv).update(r2.tuf, r2.artifact, NOW + 2 * 86400_000);
} catch (e) {
    expired = e.message;
}
results["a stale timestamp is refused (freeze attack)"] = /timestamp: expired/.test(expired);
let underSigned = "accepted";
try {
    new UpdateClient(rootEnv).update(publishTuf(5, r2.artifact, targetsKeys.slice(0, 1)), r2.artifact, NOW + 3600_000);
} catch (e) {
    underSigned = e.message;
}
results["targets signed by one of two required keys is refused"] = /targets: 1 of 2/.test(underSigned);
const forged = JSON.parse(JSON.stringify(r2.prov));
forged.payload = Base64Encode(
    enc.encode(new TextDecoder().decode(Base64Decode(forged.payload)).replace("hosted", "attacker")),
);
results["an edited provenance statement no longer verifies"] = !openEnvelope(forged, keyMap([ciKey])).ok;
const oldRoot = checkpoints[0].root,
    oldSize = checkpoints[0].size;
results["consistency proof from the first checkpoint to now"] = verifyConsistency(
    oldSize,
    log.leaves.length,
    log.consistency(oldSize, log.leaves.length),
    oldRoot,
    log.root(),
);
const rewritten = new TransparencyLog();
rewritten.leaves = log.leaves.slice();
rewritten.leaves[0] = leafHash(enc.encode("rewritten history"));
results["a log that rewrote history fails consistency"] = !verifyConsistency(
    oldSize,
    rewritten.leaves.length,
    rewritten.consistency(oldSize, rewritten.leaves.length),
    oldRoot,
    rewritten.root(),
);

print("supply-chain-signing");
print(`  signing    ${scheme}; CI key ${ciKey.id}, root threshold 2 of 3, targets threshold 2 of 2`);
for (const r of [r1, r2, r3])
    print(
        `  release    ${r.version}: ${r.refused ? "REFUSED" : `signed, log index ${r.index}, ${r.artifact.files} files, ${r.artifact.bytes.length} bytes`} — ${r.findings.map((f) => `${f.rule}${f.blocking ? "!" : ""} ${f.component}`).join("; ") || "no findings"}`,
    );
print(`  log        ${log.leaves.length} entries, root ${HexEncode(log.root()).slice(0, 16)}…`);
for (const [name, status] of v2) print(`  verify     ${status === "ok" ? "✓" : "✗"} ${name}`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
