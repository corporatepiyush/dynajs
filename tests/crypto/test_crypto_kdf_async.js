// timeout: 300
// covers: contract dyna:crypto.Bcrypt.hash dyna:crypto.Bcrypt.verify dyna:crypto.Argon2id.hash dyna:crypto.Argon2id.verify dyna:crypto.PBKDF2 dyna:crypto.Scrypt -- T2: the password KDFs return Promises and run on the io pool
// T2 (owner 11-26, TRAPS): Bcrypt, Scrypt, PBKDF2 and Argon2id return Promises and run on the existing io pool by
// default; the synchronous forms are gone (a clean break). Argument checks and the password/salt copy happen on the
// calling thread, then the derivation is one pool job. Owner 11-26 decisions: argument errors (type, range, unknown option
// key, malformed PHC string) throw synchronously; only a failure of the derivation itself rejects. opts.dyna.maxConcurrent
// caps memory-hard derivations (Argon2id, Scrypt) in flight on this thread below the pool width (the test-only hook Argon2id[Symbol.for("dyna.crypto.kdfStats")]() pins it: inFlight and peak in flight, reset with true; owner 11-26: not public API).
// The --timeout-ms hard stop (exit 113) while a derivation runs is pinned by tests/sys/test_sys_audit_w2_native_1.js.
// Pending-at-return is checked by mechanism, not timing: a pool job can only settle after the loop runs its
// completion, so Promise.race against an already resolved marker picks the marker; inline (--io-threads 0) the
// promise is already settled when the call returns. Mutation: making one export synchronous again turns the
// "returns a Promise" and "pending" rows red.
import { suite } from "../lib/kit.js";
import { Bcrypt, Argon2id, PBKDF2, Scrypt } from "dyna:crypto";
const KDFStats = Argon2id[Symbol.for("dyna.crypto.kdfStats")];
import { Exec, args } from "dyna:sys";

const t = suite("crypto.kdf_async");
const BIN = args()[0];
const SALT = new Uint8Array(16).fill(7);
const PENDING = Symbol("pending");
const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
const small = { iterations: 1, memory: 64, parallelism: 1 };

const CALLS = [
    ["Bcrypt.hash", () => Bcrypt.hash("pw", 4)],
    ["Bcrypt.verify", () => Bcrypt.verify("pw", "$2b$04$" + "C".repeat(53))],
    ["Argon2id.hash", () => Argon2id.hash("pw", SALT, small)],
    ["Argon2id.verify", () => Argon2id.verify("$argon2id$v=19$m=64,t=1,p=1$BwcHBwcHBwcHBwcHBwcHBw$" + "A".repeat(43), "pw")],
    ["PBKDF2", () => PBKDF2({ password: "pw", salt: SALT, iterations: 1000 })],
    ...(typeof Scrypt === "function" ? [["Scrypt", () => Scrypt("pw", SALT, { N: 1024 })]] : []),
];

t.test("every password KDF returns a Promise", async (a) => {
    for (const [name, call] of CALLS) {
        const p = call();
        a.ok(p instanceof Promise, `${name} returns a Promise`);
        await p;
    }
});

t.test("with the pool, the derivation is still pending when the call returns", async (a) => {
    for (const [name, call] of CALLS) {
        const p = call();
        a.eq(await Promise.race([p, Promise.resolve(PENDING)]), PENDING, `${name} is pending at return`);
        await p;
    }
});

t.test("argument errors throw synchronously; nothing is queued", (a) => {
    const BAD = [
        ["Bcrypt.hash rounds 3", () => Bcrypt.hash("x", 3), RangeError],
        ["Bcrypt.hash 73-byte password", () => Bcrypt.hash("x".repeat(73), 4), RangeError],
        ["Bcrypt.hash no password", () => Bcrypt.hash(), TypeError],
        ["Bcrypt.verify no hash", () => Bcrypt.verify("x"), TypeError],
        ["Argon2id.hash short salt", () => Argon2id.hash("pw", new Uint8Array(4), small), RangeError],
        ["Argon2id.hash unknown option", () => Argon2id.hash("pw", SALT, { ...small, bogus: 1 }), TypeError],
        ["Argon2id.hash iterations 0", () => Argon2id.hash("pw", SALT, { ...small, iterations: 0 }), RangeError],
        ["Argon2id.verify no arguments", () => Argon2id.verify(), TypeError],
        ["Argon2id.verify malformed PHC", () => Argon2id.verify("$argon2id$nope", "pw"), TypeError],
        ["Argon2id.verify PHC over the memory cap", () => Argon2id.verify("$argon2id$v=19$m=2097152,t=1,p=1$BwcHBwcHBwcHBwcHBwcHBw$" + "A".repeat(43), "pw"), RangeError],
        ["Argon2id.verify PHC unknown opts key", () => Argon2id.verify("$argon2id$nope", "pw", { zz: 1 }), TypeError],
        ["PBKDF2 no salt", () => PBKDF2({ password: "pw" }), TypeError],
        ["PBKDF2 iterations 0", () => PBKDF2({ password: "pw", salt: SALT, iterations: 0 }), RangeError],
        ["PBKDF2 unknown option", () => PBKDF2({ password: "pw", salt: SALT, bogus: 1 }), TypeError],
        ...(typeof Scrypt === "function" ? [["Scrypt N 100", () => Scrypt("pw", SALT, { N: 100 }), RangeError], ["Scrypt unknown option", () => Scrypt("pw", SALT, { bogus: 1 }), TypeError]] : []),
    ];
    for (const [name, call, Err] of BAD) a.throws(call, Err, `${name}: throws ${Err.name} synchronously`);
    a.eq(KDFStats().inFlight, 0, "no derivation was queued by a refused call");
});

t.test("a failure of the derivation itself rejects the promise", (a) => {
    const src = 'import { Argon2id } from "dyna:crypto"; let p; try { p = Argon2id.hash("pw", new Uint8Array(16), { memory: 524288, iterations: 1, parallelism: 1, encoded: false }); } catch (e) { print("threw " + e.name); }' +
        'try { await p; print("resolved"); } catch (e) { print("rejected " + e.name + " " + e.message); }';
    for (const extra of [[], ["--io-threads", "0"]]) {
        const r = Exec(BIN, ["--module", ...extra, "--native-memory-limit", "33554432", "-e", src], { timeoutMs: 60000, encoding: "utf8" });
        a.eq((r.stdout || "").trim(), "rejected InternalError out of memory", "Argon2id over the native memory limit returns a promise that rejects" + (extra.length ? " (inline, --io-threads 0)" : ""));
    }
});

t.test("dyna.maxConcurrent caps memory-hard derivations in flight", async (a) => {
    const opts = { iterations: 1, memory: 8192, parallelism: 1, encoded: false };
    const pws = Array.from({ length: 10 }, (_, i) => "cap-" + i);
    const serial = [];
    for (const pw of pws) serial.push(hex(await Argon2id.hash(pw, SALT, opts)));
    for (const cap of [1, 2, 3]) {
        KDFStats(true);
        const got = (await Promise.all(pws.map((pw) => Argon2id.hash(pw, SALT, { ...opts, dyna: { maxConcurrent: cap } })))).map(hex);
        const st = KDFStats(true);
        a.ok(st.peak <= cap && st.peak >= 1, `Argon2id.hash cap ${cap}: peak in flight ${st.peak} <= ${cap}`);
        a.deep(got, serial, `Argon2id.hash cap ${cap}: results equal the serial ones, in order`);
        a.eq(st.inFlight, 0, `cap ${cap}: nothing left in flight`);
    }
    KDFStats(true);
    const raw = await Promise.all(pws.map((pw, i) => Argon2id.verify(pw, SALT, new Uint8Array(32), { ...opts, dyna: { maxConcurrent: 2 } })));
    a.ok(raw.every((v) => v === false) && KDFStats(true).peak <= 2, "raw Argon2id.verify honours the cap");
    const phc = await Argon2id.hash("pw", SALT, { iterations: 1, memory: 4096, parallelism: 1 });
    const vs = await Promise.all(Array.from({ length: 6 }, () => Argon2id.verify(phc, "pw", { dyna: { maxConcurrent: 1 } })));
    a.ok(vs.every((v) => v === true) && KDFStats(true).peak === 1, "PHC Argon2id.verify takes the bag and honours cap 1");
    if (typeof Scrypt === "function") {
        const ks = await Promise.all(pws.map((pw) => Scrypt(pw, SALT, { N: 4096, dyna: { maxConcurrent: 2 } })));
        a.ok(ks.length === 10 && KDFStats(true).peak <= 2, "Scrypt honours the cap");
    }
    const mixed = await Promise.all([...pws.slice(0, 4).map((pw) => Argon2id.hash(pw, SALT, opts)), ...pws.slice(4).map((pw) => Argon2id.hash(pw, SALT, { ...opts, dyna: { maxConcurrent: 1 } }))]);
    a.deep(mixed.map(hex), serial, "uncapped and capped calls mixed still give the serial results");
    a.eq(KDFStats().inFlight, 0, "nothing left in flight after the mixed batch");
});

t.test("dyna.maxConcurrent validation (synchronous)", (a) => {
    for (const [v, Err] of [[0, RangeError], [1.5, RangeError], [-1, RangeError], [2 ** 31, RangeError], [NaN, RangeError], ["2", TypeError]])
        a.throws(() => Argon2id.hash("pw", SALT, { ...small, dyna: { maxConcurrent: v } }), Err, `maxConcurrent ${String(v)} refused with ${Err.name}`);
    a.throws(() => Argon2id.hash("pw", SALT, { ...small, dyna: { cap: 1 } }), TypeError, "unknown dyna key");
    a.throws(() => Argon2id.hash("pw", SALT, { ...small, dyna: 3 }), TypeError, "dyna must be an object");
    if (typeof Scrypt === "function") a.throws(() => Scrypt("pw", SALT, { dyna: { maxConcurrent: 0 } }), RangeError, "Scrypt validates maxConcurrent");
    a.ok(Argon2id.hash("pw", SALT, { ...small, dyna: { maxConcurrent: 2 ** 31 - 1 } }) instanceof Promise, "2^31-1 is accepted");
});

t.test("results equal the published vectors and round-trip through verify", async (a) => {
    a.eq(hex(await PBKDF2({ hash: "sha256", password: "password", salt: "salt", iterations: 1, length: 32 })),
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", "PBKDF2-SHA256 c=1 (RFC 6070 series)");
    if (typeof Scrypt === "function")
        a.eq(hex(await Scrypt("password", "NaCl", { N: 1024, r: 8, p: 16, keyLen: 64 })),
            "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640", "Scrypt RFC 7914 s12");
    const bh = await Bcrypt.hash("secret", 4);
    a.eq(await Bcrypt.verify("secret", bh), true, "Bcrypt verify accepts its hash");
    a.eq(await Bcrypt.verify("secreT", bh), false, "Bcrypt verify rejects a wrong password");
    const phc = await Argon2id.hash("secret", SALT, small);
    a.eq(await Argon2id.verify(phc, "secret"), true, "Argon2id PHC verify accepts");
    a.eq(await Argon2id.verify(phc, "secreT"), false, "Argon2id PHC verify rejects");
    const raw = await Argon2id.hash("secret", SALT, { ...small, encoded: false });
    a.eq(await Argon2id.verify("secret", SALT, raw, small), true, "Argon2id raw verify accepts");
});

t.test("the event loop keeps ticking during a heavy derivation", async (a) => {
    let ticks = 0;
    const iv = setInterval(() => ticks++, 1);
    await Argon2id.hash("pw", SALT, { iterations: 3, memory: 65536, parallelism: 1 });
    const argon = ticks;
    ticks = 0;
    await Bcrypt.hash("pw", 11);
    clearInterval(iv);
    a.ok(argon > 5, `timers fired during Argon2id 64 MiB t=3 (${argon})`);
    a.ok(ticks > 5, `timers fired during bcrypt cost 11 (${ticks})`);
});

t.test("concurrent derivations each get their own result", async (a) => {
    const pws = Array.from({ length: 16 }, (_, i) => "pw-" + i);
    const opts = { iterations: 1, memory: 4096, parallelism: 1, encoded: false };
    const serial = [];
    for (const pw of pws) serial.push(hex(await Argon2id.hash(pw, SALT, opts)));
    const par = (await Promise.all(pws.map((pw) => Argon2id.hash(pw, SALT, opts)))).map(hex);
    a.deep(par, serial, "16 concurrent Argon2id hashes equal the serial ones, in order");
    a.eq(new Set(par).size, 16, "all 16 differ");
    const hs = await Promise.all(pws.slice(0, 8).map((pw) => Bcrypt.hash(pw, 5)));
    const vs = await Promise.all(hs.map((h, i) => Bcrypt.verify(pws[i], h)));
    a.ok(vs.every((v) => v === true), "8 concurrent bcrypt hashes verify");
    const ks = await Promise.all(pws.map((pw) => PBKDF2({ password: pw, salt: SALT, iterations: 2000 })));
    for (let i = 0; i < pws.length; i++)
        a.eq(hex(ks[i]), hex(await PBKDF2({ password: pws[i], salt: SALT, iterations: 2000 })), `PBKDF2 #${i} concurrent equals serial`);
});

t.test("the password and salt are copied at the call", async (a) => {
    const pw = new TextEncoder().encode("bytes password");
    const salt = SALT.slice();
    const want = hex(await PBKDF2({ password: pw, salt, iterations: 50000 }));
    const p = PBKDF2({ password: pw, salt, iterations: 50000 });
    pw.fill(0);
    salt.fill(0);
    a.eq(hex(await p), want, "PBKDF2 derives from the bytes as they were at the call");
    const pw2 = new TextEncoder().encode("argon bytes");
    const want2 = hex(await Argon2id.hash(pw2, SALT, { ...small, encoded: false }));
    const p2 = Argon2id.hash(pw2, SALT, { ...small, encoded: false });
    pw2.fill(1);
    a.eq(hex(await p2), want2, "Argon2id derives from the bytes as they were at the call");
});

t.test("--io-threads 0: no pool, the promise is already settled when the call returns", (a) => {
    const src = 'import { Bcrypt, Argon2id, PBKDF2 } from "dyna:crypto";' +
        'const M = Symbol();' +
        'const out = [];' +
        'for (const p of [Bcrypt.hash("pw", 4), Argon2id.hash("pw", new Uint8Array(16), { iterations: 1, memory: 64, parallelism: 1 }), PBKDF2({ password: "pw", salt: "saltsalt", iterations: 10 })])' +
        '  out.push(p instanceof Promise && (await Promise.race([p, Promise.resolve(M)])) !== M);' +
        'print(out.join(","));';
    const r = Exec(BIN, ["--module", "--io-threads", "0", "-e", src], { timeoutMs: 60000, encoding: "utf8" });
    a.eq(r.code, 0, "child exits 0 (" + (r.stderr || "").trim().slice(0, 120) + ")");
    a.eq((r.stdout || "").trim(), "true,true,true", "each call returned a Promise that was already settled");
});

await t.run();
