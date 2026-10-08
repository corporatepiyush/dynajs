// 26 · Encrypted secrets vault — a password-protected store of named secrets in one authenticated file.
//
// WHAT IT SHOWS
//   - dyna:crypto Argon2id as a key-derivation function (memory-hard, so guessing is expensive)
//   - dyna:crypto AESGCM: authenticated encryption; a wrong password or a flipped bit fails loudly
//   - binding ciphertext to its context with associated data (the header cannot be swapped)
//   - an atomic save: write a temp file, then rename over the old one
//
// FILE LAYOUT   "VLT1" | 16-byte salt | 12-byte nonce | ciphertext+tag
//
// RUN      dynajs examples/apps/26-encrypted-secrets-vault.js
//          The Vault class below is the reusable part.

import { Argon2id, AESGCM, RandomBytes } from "dyna:crypto";
import { Path, readBytes, writeFile, rename, exists, makeTempDir, removeAll } from "dyna:file";

const MAGIC = new TextEncoder().encode("VLT1");
// Tune these so one derivation takes roughly a quarter of a second on your
// hardware. They are part of the file's security, not a performance knob.
const KDF = { memory: 19456, iterations: 2, parallelism: 1, hashLen: 32, encoded: false };

class Vault {
    constructor(path, password, secrets, salt) {
        this.path = path; this.password = password; this.secrets = secrets; this.salt = salt;
    }

    static create(path, password) {
        if (exists(path)) throw new Error("refusing to overwrite an existing vault");
        const vault = new Vault(path, password, {}, RandomBytes(16));
        vault.save();
        return vault;
    }

    static open(path, password) {
        const file = readBytes(path);
        if (file.length < 4 + 16 + 12 + 16 || !MAGIC.every((b, i) => file[i] === b))
            throw new Error("not a vault file");
        const salt = file.subarray(4, 20), nonce = file.subarray(20, 32), sealed = file.subarray(32);
        const key = Argon2id.hash(password, salt, KDF);
        const aead = new AESGCM(key);
        let plain;
        try {
            // The header is passed as associated data: it is not encrypted,
            // but changing a single byte of it makes this call throw.
            plain = aead.open(nonce, sealed, file.subarray(0, 20));
        } catch (e) {
            // Deliberately one message for both causes: do not tell an
            // attacker whether the password or the file was wrong.
            throw new Error("wrong password or corrupted vault");
        } finally {
            aead.close();
            key.fill(0);                              // do not leave key bytes lying around
        }
        return new Vault(path, password, JSON.parse(new TextDecoder().decode(plain)), salt.slice());
    }

    save() {
        const key = Argon2id.hash(this.password, this.salt, KDF);
        const aead = new AESGCM(key);
        const header = new Uint8Array(20);
        header.set(MAGIC); header.set(this.salt, 4);
        // sealRandom draws a fresh nonce every time: a nonce must never repeat
        // under the same key, and saving twice is exactly how it would.
        const { nonce, sealed } = aead.sealRandom(JSON.stringify(this.secrets), header);
        aead.close(); key.fill(0);

        const out = new Uint8Array(32 + sealed.length);
        out.set(header); out.set(nonce, 20); out.set(sealed, 32);
        // Write beside the target, then rename: a crash mid-save leaves either
        // the old vault or the new one, never half of each.
        const tmp = new Path(String(this.path) + ".tmp");
        writeFile(tmp, out);
        rename(tmp, this.path);
    }

    set(name, value) { this.secrets[name] = value; this.save(); }
    get(name) { return this.secrets[name]; }
    remove(name) { delete this.secrets[name]; this.save(); }
    list() { return Object.keys(this.secrets).sort(); }

    // Re-encrypt under a new password with a new salt.
    changePassword(next) { this.password = next; this.salt = RandomBytes(16); this.save(); }
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const throwsWith = (fn) => { try { fn(); return ""; } catch (e) { return e.message; } };
const dir = makeTempDir("vault");
const path = dir.join("secrets.vault");

const vault = Vault.create(path, "correct horse battery staple");
vault.set("db/password", "s3cr3t-Pa55");
vault.set("api/stripe", "sk_live_abc123");
check(vault.list().join() === "api/stripe,db/password", "secrets are listed by name");

const reopened = Vault.open(path, "correct horse battery staple");
check(reopened.get("db/password") === "s3cr3t-Pa55", "the right password opens the vault");
check(throwsWith(() => Vault.open(path, "wrong")) === "wrong password or corrupted vault", "a wrong password is refused");

// Nothing recognizable is on disk.
const onDisk = new TextDecoder("utf-8").decode(readBytes(path));
check(!onDisk.includes("s3cr3t") && !onDisk.includes("stripe"), "neither names nor values are stored in the clear");

// Flip one bit in the ciphertext, then one in the salt: both must fail.
for (const offset of [40, 6]) {
    const bytes = readBytes(path);
    bytes[offset] ^= 0x01;
    const damaged = dir.join("damaged-" + offset);
    writeFile(damaged, bytes);
    check(throwsWith(() => Vault.open(damaged, "correct horse battery staple")) !== "", "tampering at byte " + offset + " is detected");
}

// Two saves of identical content differ on disk (fresh nonce each time).
const before = readBytes(path).slice();
reopened.save();
check(!readBytes(path).every((b, i) => b === before[i]), "every save uses a fresh nonce");

reopened.changePassword("a much longer new passphrase");
check(throwsWith(() => Vault.open(path, "correct horse battery staple")) !== "", "the old password stops working");
check(Vault.open(path, "a much longer new passphrase").get("api/stripe") === "sk_live_abc123", "the new one works");
check(throwsWith(() => Vault.create(path, "x")).includes("refusing"), "create never clobbers an existing vault");
console.log("self-test passed: create, reopen, wrong password, tamper detection, rotation");
removeAll(dir);
