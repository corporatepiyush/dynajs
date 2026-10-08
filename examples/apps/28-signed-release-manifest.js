// 28 · Signed release manifest — signs a set of build artifacts and verifies them before install.
//
// WHAT IT SHOWS
//   - dyna:crypto Ed25519: small keys, fast signatures, no parameters to get wrong
//   - signing a canonical manifest of digests rather than every file separately
//   - dyna:encoding StableStringify: the SAME bytes on signer and verifier regardless of key order
//   - the verifier trusting a pinned public key, never one that travels with the download
//
// RUN      dynajs examples/apps/28-signed-release-manifest.js
//          keygen / sign / verify are the three functions to lift into a release pipeline.

import { Ed25519Generate, Ed25519Sign, Ed25519Verify, Ed25519PemFromRaw, Ed25519PemToRaw } from "dyna:crypto";
import { SHA256Hex } from "dyna:hash";
import { StableStringify, Base64URLEncode, Base64URLDecode } from "dyna:encoding";
import { glob, readBytes, readFile, writeFile, makeDir, makeTempDir, removeAll, stat } from "dyna:file";

// ---- release side ----------------------------------------------------------
function keygen() {
    const pair = Ed25519Generate();
    return Ed25519PemFromRaw(pair);            // PEM text: private goes to the CI secret store
}

function sign(releaseDir, version, privateKeyPem) {
    const files = {};
    for (const rel of glob("**", { cwd: releaseDir })) {
        const name = String(rel);
        const path = releaseDir.join(name);
        if (!stat(path).isFile || name === "MANIFEST.json") continue;
        const data = readBytes(path);
        files[name] = { sha256: SHA256Hex(data), size: data.length };
    }
    const manifest = { version, files };
    // Sign the canonical form. Plain JSON.stringify depends on insertion
    // order, so a verifier that rebuilt the object could compute different
    // bytes for the same content.
    const payload = StableStringify(manifest);
    const { privateKey } = Ed25519PemToRaw(privateKeyPem);
    const signature = Base64URLEncode(Ed25519Sign(privateKey, payload));
    writeFile(releaseDir.join("MANIFEST.json"), JSON.stringify({ manifest, signature }, null, 2));
    return Object.keys(files).length;
}

// ---- install side ----------------------------------------------------------
// `trustedPublicKeyPem` is compiled into the installer or fetched over a
// separate authenticated channel. If it came from the same server as the
// files, an attacker who replaced the files would replace it too.
function verify(releaseDir, trustedPublicKeyPem) {
    const { manifest, signature } = JSON.parse(readFile(releaseDir.join("MANIFEST.json")));
    const { publicKey } = Ed25519PemToRaw(trustedPublicKeyPem);
    if (!Ed25519Verify(publicKey, StableStringify(manifest), Base64URLDecode(signature)))
        throw new Error("manifest signature is invalid");

    // The signature covers the digests; now the files must match the digests.
    const present = new Set();
    for (const rel of glob("**", { cwd: releaseDir })) {
        const name = String(rel);
        if (name === "MANIFEST.json" || !stat(releaseDir.join(name)).isFile) continue;
        present.add(name);
        const expected = manifest.files[name];
        if (!expected) throw new Error("file not in the manifest: " + name);
        const data = readBytes(releaseDir.join(name));
        if (data.length !== expected.size || SHA256Hex(data) !== expected.sha256)
            throw new Error("file does not match the manifest: " + name);
    }
    for (const name of Object.keys(manifest.files))
        if (!present.has(name)) throw new Error("file missing from the release: " + name);
    return manifest.version;
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const refusal = (fn) => { try { fn(); return ""; } catch (e) { return e.message; } };

const dir = makeTempDir("release");
makeDir(dir.join("bin"));
writeFile(dir.join("bin", "app"), "#!/bin/sh\necho release 2.1.0\n");
writeFile(dir.join("README.txt"), "Release notes\n");
writeFile(dir.join("config.default.toml"), "port = 8080\n");

const keys = keygen();
check(keys.privateKey.includes("PRIVATE KEY") && keys.publicKey.includes("PUBLIC KEY"), "keys are PEM");
check(sign(dir, "2.1.0", keys.privateKey) === 3, "three artifacts are listed");
check(verify(dir, keys.publicKey) === "2.1.0", "an untouched release verifies");

// 1. A modified artifact.
const original = readFile(dir.join("bin", "app"));
writeFile(dir.join("bin", "app"), original.replace("release", "malware"));
check(refusal(() => verify(dir, keys.publicKey)).includes("does not match"), "a changed file is refused");
writeFile(dir.join("bin", "app"), original);

// 2. An extra file slipped into the release.
writeFile(dir.join("bin", "extra"), "surprise");
check(refusal(() => verify(dir, keys.publicKey)).includes("not in the manifest"), "an unlisted file is refused");
removeAll(dir.join("bin", "extra"));

// 3. The manifest edited to match a changed file (digest updated, signature not).
const signed = JSON.parse(readFile(dir.join("MANIFEST.json")));
signed.manifest.version = "9.9.9";
writeFile(dir.join("MANIFEST.json"), JSON.stringify(signed));
check(refusal(() => verify(dir, keys.publicKey)).includes("signature is invalid"), "an edited manifest is refused");

// 4. A correctly signed release from someone else's key.
const attacker = keygen();
sign(dir, "2.1.0", attacker.privateKey);
check(refusal(() => verify(dir, keys.publicKey)).includes("signature is invalid"), "a release signed by another key is refused");

sign(dir, "2.1.0", keys.privateKey);
check(verify(dir, keys.publicKey) === "2.1.0", "re-signing with the real key verifies again");
console.log("self-test passed: tamper, injection, manifest edit and wrong-key are all refused");
removeAll(dir);
