// covers: integration dyna:async.offload dyna:crypto.Bcrypt.hash dyna:crypto.Bcrypt.verify dyna:crypto.Scrypt dyna:crypto.PBKDF2 dyna:crypto.Argon2id.hash dyna:crypto.Argon2id.verify dyna:hash.* -- the offload contract (byte-identical result, loop ticks) checked for every marked function of the module
// Owner ask E, wave 4: password KDFs and the one-shot hashes mark their compute DYN_OFFLOAD_CPU. Under offload()
// a buffer input is copied first (a JS buffer may change or detach while the work runs off-thread; a string input
// is held as an immutable C string), and the KDFs copy the password and salt. Called directly nothing is copied.
// Per function: the offloaded result equals the direct call (Bcrypt draws a fresh salt, so its oracle is verify()
// both ways), and the promise is still pending when offload() returns (a mechanism, not a timer race: see tests/lib/offload_check.js). Mutation: removing one function's
// DYN_OFFLOAD_CPU makes its "loop ticks" check red (settled at return).
import { suite } from "../lib/kit.js";
import { same, loopTicks, text } from "../lib/offload_check.js";
import { offload } from "dyna:async";
import { Bcrypt, Scrypt, PBKDF2, Argon2id } from "dyna:crypto";
import * as H from "dyna:hash";

const t = suite("crypto.offload");
const DATA = text(64 << 20, 3);
const MID = text(4 << 20, 4);
const SALT = new Uint8Array(16).fill(9);
const PW = "correct horse battery staple";

t.test("Bcrypt: an offloaded hash verifies both ways, offloaded verify agrees", async (a) => {
    const h = await offload(Bcrypt.hash, PW, 10);
    a.ok(await Bcrypt.verify(PW, h), "direct verify accepts the offloaded hash");
    a.eq(await offload(Bcrypt.verify, PW, h), true, "offloaded verify accepts it");
    a.eq(await offload(Bcrypt.verify, PW + "x", h), false, "offloaded verify rejects a wrong password");
    const pwBytes = new TextEncoder().encode(PW);
    a.ok(await Bcrypt.verify(PW, await offload(Bcrypt.hash, pwBytes, 10)), "a byte password is copied and hashed the same");
});

t.test("Bcrypt: loop ticks", async (a) => {
    await loopTicks(a, "Bcrypt.hash", Bcrypt.hash, [PW, 10]);
    await loopTicks(a, "Bcrypt.verify", Bcrypt.verify, [PW, await Bcrypt.hash(PW, 10)]);
});

t.test("Scrypt, PBKDF2: offloaded keys equal the direct call", async (a) => {
    await same(a, "Scrypt", Scrypt, [PW, SALT, { N: 1 << 14, r: 8, p: 1 }]);
    await same(a, "Scrypt bytes", Scrypt, [new TextEncoder().encode(PW), SALT, { N: 1 << 12 }]);
    await same(a, "PBKDF2", PBKDF2, [{ hash: "sha256", password: PW, salt: SALT, iterations: 100000, length: 32 }]);
});

t.test("Scrypt, PBKDF2: loop ticks", async (a) => {
    await loopTicks(a, "Scrypt", Scrypt, [PW, SALT, { N: 1 << 14 }]);
    await loopTicks(a, "PBKDF2", PBKDF2, [{ hash: "sha256", password: PW, salt: SALT, iterations: 100000 }]);
});

t.test("Argon2id: hash (PHC and raw) and both verify forms equal the direct call", async (a) => {
    const o = { iterations: 2, memory: 16384, parallelism: 1 };
    const phc = await same(a, "Argon2id.hash", Argon2id.hash, [PW, SALT, o]);
    await same(a, "Argon2id.hash raw", Argon2id.hash, [PW, SALT, { ...o, encoded: false }]);
    a.eq(await offload(Argon2id.verify, phc, PW), true, "PHC verify");
    a.eq(await offload(Argon2id.verify, phc, PW + "x"), false, "PHC verify rejects");
    const tag = await Argon2id.hash(PW, SALT, { ...o, encoded: false });
    a.eq(await offload(Argon2id.verify, PW, SALT, tag, o), true, "raw verify");
});

t.test("Argon2id: loop ticks", async (a) => {
    const o = { iterations: 2, memory: 16384, parallelism: 1 };
    await loopTicks(a, "Argon2id.hash", Argon2id.hash, [PW, SALT, o]);
    const phc = await Argon2id.hash(PW, SALT, o);
    await loopTicks(a, "Argon2id.verify PHC", Argon2id.verify, [phc, PW]);
    await loopTicks(a, "Argon2id.verify raw", Argon2id.verify, [PW, SALT, await Argon2id.hash(PW, SALT, { ...o, encoded: false }), o]);
});

const EXTRA = { SHAKE128: [32], SHAKE128Hex: [32], SHAKE256: [64], SHAKE256Hex: [64], Murmur3_128: [7], Murmur3_128Hex: [7] };
const ONE_SHOT = Object.keys(H).filter((k) => k !== "Hasher" && k !== "CDCChunks").sort();
const HASHES = [
    ...ONE_SHOT.map((k) => [k, H[k], EXTRA[k] || []]),
    ["BLAKE3 keyed", H.BLAKE3, [{ key: new Uint8Array(32).fill(5) }]], ["BLAKE3 derive", H.BLAKE3, [{ context: "app v1" }]],
    ["BLAKE2b keyed", H.BLAKE2b, [{ key: new Uint8Array(64).fill(6) }]], ["BLAKE2s keyed", H.BLAKE2s, [{ key: new Uint8Array(32).fill(7) }]],
];

t.test("hashes: every one-shot export of dyna:hash is covered (Hasher streams and CDCChunks builds JS per chunk)", (a) => {
    a.eq(ONE_SHOT.length, 40, "40 one-shot digests");
});

t.test("hashes: offloaded digests equal the direct call, for bytes and string input", async (a) => {
    const small = "hello offload";
    for (const [name, fn, extra] of HASHES) {
        await same(a, name, fn, [MID, ...extra]);
        await same(a, name + " string", fn, [small, ...extra]);
        await same(a, name + " empty", fn, [new Uint8Array(0), ...extra]);
    }
});

t.test("hashes: loop ticks", async (a) => {
    for (const [name, fn, extra] of HASHES) await loopTicks(a, name, fn, [DATA, ...extra]);
});

t.test("hashes: the input is copied at the call, so changing it afterwards does not change the digest", async (a) => {
    const u = text(8 << 20, 11);
    const want = H.SHA256(u);
    const p = offload(H.SHA256, u);
    u.fill(0);
    a.eq(H.SHA256Hex(await p), H.SHA256Hex(want), "digest of the bytes at call time");
});

t.test("hashes: a BLAKE key is copied at the call, so changing it afterwards does not change the digest", async (a) => {
    const u = text(8 << 20, 13);
    for (const [name, fn, n] of [["BLAKE3", H.BLAKE3, 32], ["BLAKE2b", H.BLAKE2b, 64]]) {
        const key = new Uint8Array(n).fill(9);
        const want = H.SHA256Hex(fn(u, { key }));
        const p = offload(fn, u, { key });
        key.fill(0);
        a.eq(H.SHA256Hex(await p), want, `${name}: digest under the key at call time`);
    }
});

t.test("hashes: the input buffer cannot be detached while the offloaded call runs", async (a) => {
    const u = text(8 << 20, 12);
    const want = H.SHA256Hex(H.SHA256(u));
    const p = offload(H.SHA256, u);
    a.throws(() => u.buffer.transfer(), TypeError, "transfer refused while in use");
    a.eq(H.SHA256Hex(await p), want, "digest of the bytes at call time");
});

t.test("a long job does not hold back a short one: the short result and timer ticks arrive while bcrypt still runs", async (a) => {
    const order = [];
    let ticks = 0;
    const iv = setInterval(() => ticks++, 1);
    const long = offload(Bcrypt.hash, PW, 14).then(() => order.push("long:" + ticks));
    const short = offload(H.SHA256, MID).then(() => order.push("short"));
    await Promise.all([long, short]);
    clearInterval(iv);
    a.eq(order[0], "short", "the short job settles first");
    a.ok(+order[1].split(":")[1] > 5, `the loop kept ticking during the long job (${order[1]})`);
});

await t.run();
