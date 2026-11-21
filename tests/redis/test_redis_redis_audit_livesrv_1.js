// flags: --std
// timeout: 120
// covers: dyna:redis.Redis
// Real-server integration layer for the Redis client (split from
// tests/net/test_audit_livesrv_1.js; the PostgreSQL half is
// tests/postgres/test_postgres_postgre_sql_audit_livesrv_1.js).
// Where the mock-based suites test the driver's assumptions, this suite runs
// against an actual server: it re-validates the N3-16 (PG must authenticate)
// and N3-01..05 (Redis coerce/copy/resume/re-entrancy) fixes on real wire
// behaviour, and re-runs as a regression layer wherever servers exist.
//
// Every probe is bounded: connectTimeoutMs/queryTimeoutMs inside the engine,
// a Promise-race race-guard around awaits, and an external suite watchdog.
// When a server is not reachable the dependent sections SKIP by name and the
// suite exits 0 -- CI without servers must stay green. LIVESRV_REQUIRE=1
// turns those skips into a failure where servers are supposed to exist.
//
// The server is tests/lib/services.js ensureRedis (REDIS_PORT, started and
// stopped by this suite when none answers). A case SKIPs only when ensureRedis
// cannot run one (no redis-server); LIVESRV_REQUIRE=1 turns that skip into a failure.

import { Redis } from "dyna:redis";
import { memoryUsage } from "dyna:sys";
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { ensureRedis, stopStarted, REDIS_PORT } from "../lib/services.js";

const t = suite("test_redis_redis_audit_livesrv_1");
let A = null;
const ok = (c, m) => { A.ok(c, m); if (c) print("  ok  " + m); };
const note = (m) => print("  ..  " + m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const env = (k, d) => {
    const v = std.getenv(k);
    return (v === undefined || v === "") ? d : v;
};
const REDIS_CFG = { host: "127.0.0.1", port: REDIS_PORT };
const REQUIRE = env("LIVESRV_REQUIRE", "0") === "1";
const NS = "livesrv:" + (Date.now() % 10000000) + ":";
const key = (s) => NS + s;

function withTimeout(p, ms, what) {
    let timer;
    return Promise.race([p, new Promise((_, rej) => {
        timer = setTimeout(() => rej(new Error("suite race-guard: " + what +
            " did not settle within " + ms + "ms")), ms);
    })]).finally(() => clearTimeout(timer));
}

async function waitFor(fn, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (fn()) return true;
        await sleep(20);
    }
    return fn();
}

function fdCount() {
    for (const d of ["/proc/self/fd", "/dev/fd"]) {
        try {
            const r = os.readdir(d);
            if (r && r[1] === 0) return r[0].length;
        } catch (e) {}
    }
    return -1;
}

function bytesEqual(a, b) {
    if (!(a instanceof Uint8Array) || !(b instanceof Uint8Array)) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

function errText(e) {
    return String((e && e.message) ? e.message : e);
}

async function probeRedis(cfg) {
    const c = new Redis(Object.assign({}, cfg,
        { connectTimeoutMs: 1200, commandTimeoutMs: 3000 }));
    try {
        await withTimeout(c.command("PING"), 5000, "redis probe");
        return { client: c, err: "" };
    } catch (e) {
        try { c.close(); } catch (e2) {}
        return { client: null, err: errText(e) };
    }
}

async function churnSection(label, cfg, make, oneOp) {
    const N = 40, WARM = 5;
    const fd0Enabled = fdCount() >= 0;
    for (let i = 0; i < WARM; i++) {
        const c = make(cfg);
        try { await oneOp(c); } catch (e) {}
        try { c.close(); } catch (e) {}
    }
    std.gc();
    const fd0 = fdCount(), nat0 = memoryUsage().nativeSize;
    let failuresInLoop = 0;
    for (let i = 0; i < N; i++) {
        const c = make(cfg);
        try { await oneOp(c); } catch (e) { failuresInLoop++; }
        try { c.close(); } catch (e) { failuresInLoop++; }
    }
    await sleep(30);
    std.gc();
    const fd1 = fdCount(), nat1 = memoryUsage().nativeSize;
    const grow = nat1 - nat0;
    ok(failuresInLoop === 0, label + " churn: " + N + " open/use/close cycles all clean");
    if (fd0Enabled && fd1 >= 0)
        ok(fd1 - fd0 <= 4,
            label + " churn: fd count does not grow without bound (" + fd0 + " -> " + fd1 + ")");
    else
        note(label + " churn: fd count unavailable on this host, fd delta not asserted");
    ok(grow <= 1048576,
        label + " churn: nativeSize growth bounded (" + grow + " bytes over " +
        N + " cycles, " + nat0 + " -> " + nat1 + ")");
}

async function redisBasicAndBinary(r) {
    print("-- Redis basics, binary payloads (N3-03), pipelines");
    ok(r.ready === true && r.protocol >= 2,
        "redis: handshake ready (protocol " + r.protocol + ")");
    ok((await withTimeout(r.command("PING"), 4000, "PING")) === "PONG",
        "redis: PING -> PONG");
    await r.command("SET", key("str"), "hello world");
    ok((await r.command("GET", key("str"))) === "hello world",
        "redis: SET/GET string round-trip");

    const b = new Redis(Object.assign({}, REDIS_CFG, { binary: true }));
    await withTimeout(b.command("PING"), 4000, "binary PING");
    const payload = new Uint8Array([0, 1, 2, 0x7f, 0x80, 0xfe, 0xff, 0, 0x41, 0x0a, 0x0d]);
    ok((await b.command("SET", key("bin"), payload)) === "OK",
        "redis binary: SET of a Uint8Array with NUL and high bytes accepted");
    const got = await b.command("GET", key("bin"));
    ok(got instanceof Uint8Array && bytesEqual(got, payload),
        "redis binary: NUL+high-byte value round-trips byte-exact (N3-03)");
    ok((await b.command("STRLEN", key("bin"))) === payload.length,
        "redis binary: server STRLEN agrees with the payload length");
    const ab = new ArrayBuffer(5);
    new Uint8Array(ab).set([0xde, 0xad, 0x00, 0xbe, 0xef]);
    await b.command("SET", key("ab"), ab);
    const abLen = await b.command("STRLEN", key("ab"));
    const gab = await b.command("GET", key("ab"));
    ok(gab instanceof Uint8Array && bytesEqual(gab, new Uint8Array(ab)),
        "redis binary: ArrayBuffer argument round-trips byte-exact (STRLEN=" + abLen + ")");
    const backing = new Uint8Array([9, 8, 7, 0, 0xff, 0x80, 6, 5, 4]);
    const view = backing.subarray(2, 7);
    await b.command("SET", key("view"), view);
    const gview = await b.command("GET", key("view"));
    ok(gview instanceof Uint8Array && bytesEqual(gview, view),
        "redis binary: subarray view (byteOffset!=0) round-trips byte-exact");

    const N = 1024 * 1024;
    const large = new Uint8Array(N);
    for (let i = 0; i < N; i++) large[i] = (i * 31 + 7) & 0xff;
    ok((await b.command("SET", key("large"), large)) === "OK",
        "redis binary: 1 MiB SET accepted");
    const glarge = await withTimeout(b.command("GET", key("large")), 10000, "large GET");
    ok(glarge instanceof Uint8Array && glarge.length === N && bytesEqual(glarge, large),
        "redis binary: 1 MiB value round-trips byte-exact (" + (glarge && glarge.length) + " bytes)");

    const M = 300;
    const sets = [];
    for (let i = 0; i < M; i++) sets.push(["SET", key("p" + i), "v" + i]);
    const res = await withTimeout(b.pipeline(sets), 8000, "pipeline SETs");
    ok(Array.isArray(res) && res.length === M,
        "redis pipeline: " + M + " SETs in one round trip (" +
        (Array.isArray(res) ? res.length : "?") + " replies)");
    ok(Array.isArray(res) && res.every((x) => x === "OK"),
        "redis pipeline: every reply matched its command (strict FIFO)");
    const ENC = new TextEncoder();
    const gets = [];
    for (let i = 0; i < M; i++) gets.push(["GET", key("p" + i)]);
    const gres = await withTimeout(b.pipeline(gets), 8000, "pipeline GETs");
    ok(Array.isArray(gres) && gres.length === M &&
        gres.every((x, i) => x instanceof Uint8Array && bytesEqual(x, ENC.encode("v" + i))),
        "redis pipeline: " + M + " GET replies in order and byte-exact");

    const del = [];
    for (const k of ["str", "bin", "ab", "view", "large"])
        del.push(["DEL", key(k)]);
    for (let i = 0; i < M; i++) del.push(["DEL", key("p" + i)]);
    try { await b.pipeline(del); } catch (e) {}
    b.close();
}

async function redisReentrancySection() {
    print("-- Redis close/dispose re-entrancy (N3-01/N3-02)");
    {
        const c = new Redis(Object.assign({}, REDIS_CFG));
        await withTimeout(c.command("PING"), 4000, "close-cb warmup");
        let closedInCb = false;
        const v = await withTimeout(c.command("PING").then((x) => {
            c.close();
            closedInCb = true;
            return x;
        }), 4000, "close-in-command-callback");
        ok(v === "PONG" && closedInCb,
            "redis: close() INSIDE a command callback resolves and closes");
        ok(c.closed === true, "redis: client reports closed after close-in-callback");
        let post = "";
        try {
            await withTimeout(c.command("PING"), 3000, "post-close command");
            post = "resolved";
        } catch (e) { post = "rejected"; }
        ok(post === "rejected",
            "redis: command after close-in-callback rejects promptly (no hang)");
    }
    {
        const c = new Redis(Object.assign({}, REDIS_CFG));
        await withTimeout(c.command("PING"), 4000, "dispose-cb warmup");
        let disposedInCb = false;
        const v = await withTimeout(c.command("PING").then((x) => {
            c.dispose();
            disposedInCb = true;
            return x;
        }), 4000, "dispose-in-command-callback");
        ok(v === "PONG" && disposedInCb,
            "redis: dispose() INSIDE a command callback resolves and disposes");
        ok(c.closed === true, "redis: client reports closed after dispose-in-callback");
    }
    {
        const sub = new Redis(Object.assign({}, REDIS_CFG));
        const pub = new Redis(Object.assign({}, REDIS_CFG));
        const chan = key("chan");
        let msgs = 0, closedInHandler = false;
        sub.on("message", (m) => {
            msgs++;
            if (!closedInHandler) {
                try { sub.close(); closedInHandler = true; } catch (e) {}
            }
        });
        const ack = await withTimeout(sub.command("SUBSCRIBE", chan), 4000, "SUBSCRIBE");
        ok(Array.isArray(ack) && ack[0] === "subscribe",
            "redis: SUBSCRIBE acknowledged (" + JSON.stringify(ack) + ")");
        await withTimeout(pub.command("PUBLISH", chan, "m1"), 4000, "PUBLISH");
        await waitFor(() => msgs > 0, 3000);
        ok(msgs >= 1 && closedInHandler,
            "redis: close() INSIDE the message handler delivered then closed (N3-01 shape)");
        ok(sub.closed === true, "redis: subscriber is closed from the handler");
        pub.close();
    }
    {
        const sub = new Redis(Object.assign({}, REDIS_CFG));
        const pub = new Redis(Object.assign({}, REDIS_CFG));
        const chan = key("chan2");
        let msgs = 0, disposedInHandler = false;
        sub.on("message", (m) => {
            msgs++;
            if (!disposedInHandler) {
                try { sub.dispose(); disposedInHandler = true; } catch (e) {}
            }
        });
        await withTimeout(sub.command("SUBSCRIBE", chan), 4000, "SUBSCRIBE2");
        await withTimeout(pub.command("PUBLISH", chan, "m2"), 4000, "PUBLISH2");
        await waitFor(() => msgs > 0, 3000);
        ok(msgs >= 1 && disposedInHandler,
            "redis: dispose() INSIDE the message handler delivered then disposed");
        ok(sub.closed === true, "redis: subscriber is disposed from the handler");
        pub.close();
    }
}

// The server is probed in the kit's before hook (never under --list); a case
// whose server is unreachable is a SKIP, and with LIVESRV_REQUIRE=1 a FAIL.
let rd = null, WATCHDOG = null, svcOk = false, svcWhy = "unreachable";
t.before(async () => {
    WATCHDOG = setTimeout(() => {
        print("test_redis_redis_audit_livesrv_1: HUNG (suite watchdog)");
        std.exit(124);
    }, 90000);
    print("test_redis_redis_audit_livesrv_1: live Redis audit");
    const svc = await ensureRedis();
    svcOk = svc.ok;
    if (!svc.ok) svcWhy = svc.why;
    rd = await probeRedis(REDIS_CFG);
});
t.after(async () => {
    if (rd && rd.client) { try { rd.client.close(); } catch (e) {} }
    if (WATCHDOG) clearTimeout(WATCHDOG);
    await stopStarted();
});
const unavailable = (a, why) => {
    if (REQUIRE) a.must(false, "LIVESRV_REQUIRE=1 and a section was skipped: " + why);
    if (svcOk) a.must(false, "the services.js Redis is up, so nothing may skip: " + why);
    why += " (" + svcWhy + ")";
    a.skip(why);
};
const needRedis = (a) => { if (!rd.client) unavailable(a, "Redis " + REDIS_CFG.host + ":" + REDIS_CFG.port + " unreachable (" + rd.err.slice(0, 80) + "): redis sections skipped"); };

test("redis: basics, binary payloads and pipelines", async (a) => {
    needRedis(a);
    await redisBasicAndBinary(rd.client);
});

test("redis: close and dispose re-entrancy", async (a) => {
    needRedis(a);
    await redisReentrancySection();
});

test("redis: client churn releases its descriptors", async (a) => {
    needRedis(a);
    await churnSection("redis", REDIS_CFG,
        (cfg) => new Redis(Object.assign({}, cfg,
            { connectTimeoutMs: 1500, commandTimeoutMs: 3000 })),
        async (c) => {
            const v = await withTimeout(c.command("PING"), 5000, "redis churn PING");
            if (v !== "PONG") throw new Error("redis churn PING wrong reply");
        });
});

await t.run();
