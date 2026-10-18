// flags: --std
// timeout: 120
// Real-server integration layer for the PostgreSQL and Redis clients.
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
// Configuration (defaults are the local verification servers):
//   LIVESRV_REDIS_HOST/PORT        default 127.0.0.1:6379
//   LIVESRV_PG_HOST/PORT           default 127.0.0.1:5432 (trust)
//   LIVESRV_PG_USER/DATABASE       default piyush/postgres
//   LIVESRV_PG_SCRAM_PORT          default 127.0.0.1:55432 (scram-sha-256)
//   LIVESRV_PG_PASSWORD            required for the SCRAM sections; falls
//                                  back to PGPASSWORD; never hardcoded here
//   LIVESRV_REQUIRE=1              skips become failures

import { PostgreSQL, Redis } from "dyna:net";
import { memoryUsage } from "dyna:sys";
import * as os from "os";
import * as std from "std";

let checks = 0, fails = 0;
const skipReasons = [];
const ok = (c, m) => {
    checks++;
    if (c) print("  ok  " + m);
    else { fails++; print("FAIL: " + m); }
};
const note = (m) => print("  ..  " + m);
const skip = (m) => { skipReasons.push(m); print("  SKIP  " + m); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const env = (k, d) => {
    const v = std.getenv(k);
    return (v === undefined || v === "") ? d : v;
};
const enuint = (k, d) => {
    const n = parseInt(env(k, String(d)), 10);
    return Number.isFinite(n) ? n : d;
};

const PG_CFG = {
    host: env("LIVESRV_PG_HOST", "127.0.0.1"),
    port: enuint("LIVESRV_PG_PORT", 5432),
    user: env("LIVESRV_PG_USER", "piyush"),
    database: env("LIVESRV_PG_DB", "postgres"),
};
const SCRAM_CFG = {
    host: PG_CFG.host,
    port: enuint("LIVESRV_PG_SCRAM_PORT", 55432),
    user: PG_CFG.user,
    database: PG_CFG.database,
};
const REDIS_CFG = {
    host: env("LIVESRV_REDIS_HOST", "127.0.0.1"),
    port: enuint("LIVESRV_REDIS_PORT", 6379),
};
const PG_PW = std.getenv("LIVESRV_PG_PASSWORD") || std.getenv("PGPASSWORD") || "";
const REQUIRE = env("LIVESRV_REQUIRE", "0") === "1";
const NS = "livesrv:" + (Date.now() % 10000000) + ":";
const key = (s) => NS + s;

function withTimeout(p, ms, what) {
    return Promise.race([p, new Promise((_, rej) =>
        setTimeout(() => rej(new Error("suite race-guard: " + what +
            " did not settle within " + ms + "ms")), ms))]);
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

async function probePG(cfg, what) {
    const c = new PostgreSQL(Object.assign({}, cfg,
        { connectTimeoutMs: 1200, queryTimeoutMs: 3000 }));
    try {
        await withTimeout(c.query("SELECT 1"), 5000, what);
        return { client: c, err: "" };
    } catch (e) {
        try { c.close(); } catch (e2) {}
        return { client: null, err: errText(e) };
    }
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

async function pgParamsSection(pg, cfg) {
    print("-- PostgreSQL extended-protocol parameter binding");
    const DANGER = [
        ["single quote", "O'Brien"],
        ["semicolon", "a;b;c"],
        ["statement-shaped", "'); DROP TABLE " + NS + "audit; --"],
        ["backslash+quote", "\\'; SELECT 1; --"],
        ["long", "livesrv_long_" + "L".repeat(100000)],
    ];
    const ENC = new TextEncoder();
    const bytes = new PostgreSQL(Object.assign({}, cfg, { bytes: true }));
    const tbl = (NS + "audit").replace(/[^a-z0-9_]/gi, "_");
    let tblOk = true;
    try {
        await pg.query("CREATE TEMP TABLE " + tbl + " (id serial, v text)");
    } catch (e) {
        tblOk = false;
        ok(false, "pg params: temp table create failed: " + errText(e));
    }
    for (const [label, val] of DANGER) {
        let got = null, err = "";
        try {
            got = (await withTimeout(pg.query("SELECT $1::text AS v", [val]),
                5000, "pg param " + label)).rows[0].v;
        } catch (e) { err = errText(e); }
        ok(got === val && got.length === val.length,
            "pg params: " + label + " arrives as DATA and reads back exact (" +
            val.length + " chars)" + (err ? " [" + err + "]" : ""));

        const wantBytes = ENC.encode(val);
        let gotBytes = null;
        try {
            gotBytes = (await withTimeout(bytes.query(
                "SELECT convert_to($1::text, 'UTF8') AS b", [val]),
                5000, "pg byte param " + label)).rows[0].b;
        } catch (e) {}
        ok(gotBytes instanceof Uint8Array && bytesEqual(gotBytes, wantBytes),
            "pg params: " + label + " bytes survive the extended protocol exactly (" +
            (gotBytes && gotBytes.length) + "/" + wantBytes.length + ")");

        if (tblOk) {
            try {
                await withTimeout(pg.query("INSERT INTO " + tbl + " (v) VALUES ($1::text)", [val]),
                    5000, "pg insert " + label);
            } catch (e) {
                ok(false, "pg params: " + label + " insert failed: " + errText(e));
            }
        }
    }
    if (tblOk) {
        let rows = null;
        try {
            rows = (await withTimeout(pg.query("SELECT v FROM " + tbl + " ORDER BY id"),
                5000, "pg table readback")).rows;
        } catch (e) {}
        const allExact = rows !== null && rows.length === DANGER.length &&
            rows.every((r, i) => r.v === DANGER[i][1]);
        ok(allExact, "pg params: every dangerous value was stored as data, in order (" +
            (rows ? rows.length : "?") + "/" + DANGER.length + ")");
        let count = -1;
        try {
            count = (await withTimeout(pg.query("SELECT count(*)::int AS c FROM " + tbl),
                5000, "pg count")).rows[0].c;
        } catch (e) {}
        ok(count === DANGER.length,
            "pg params: statement was NOT altered -- table intact, count=" + count);
        try { await pg.query("DROP TABLE " + tbl); } catch (e) {}
    }
    {
        const payload = new Uint8Array([0, 255, 128, 65, 0, 1, 254]);
        const rr = await withTimeout(bytes.query(
            "SELECT $1::bytea AS b, octet_length($1::bytea) AS n", [payload]),
            5000, "pg bytea param");
        ok(rr.rows[0].b instanceof Uint8Array && bytesEqual(rr.rows[0].b, payload) &&
            rr.rows[0].n === payload.length,
            "pg params: Uint8Array bytea (NUL+high bytes) round-trips byte-exact");
        const ab = new ArrayBuffer(4);
        new Uint8Array(ab).set([0xde, 0xad, 0x00, 0xbe]);
        const ra = await withTimeout(bytes.query("SELECT $1::bytea AS b", [ab]),
            5000, "pg ArrayBuffer param");
        ok(ra.rows[0].b instanceof Uint8Array &&
            bytesEqual(ra.rows[0].b, new Uint8Array(ab)),
            "pg params: ArrayBuffer bytea round-trips byte-exact");
    }
    ok(pg.transactionStatus === "I",
        "pg params: session still idle after the dangerous values (status " +
        pg.transactionStatus + ")");
    bytes.close();
}

async function pgDeadTargetSection() {
    print("-- PostgreSQL unreachable targets");
    {
        const t0 = Date.now();
        const c = new PostgreSQL({ host: "127.0.0.1", port: 1, user: "x",
            database: "x", connectTimeoutMs: 700 });
        let rejected = false, msg = "";
        try {
            await withTimeout(c.query("SELECT 1"), 5000, "dead-port connect");
        } catch (e) { rejected = true; msg = errText(e); }
        const dt = Date.now() - t0;
        ok(rejected, "pg dead port: 127.0.0.1:1 fails cleanly (" + msg.slice(0, 40) + ")");
        ok(dt < 3000, "pg dead port: failure prompt, " + dt + "ms (bound 700ms)");
        ok(c.pending === 0, "pg dead port: nothing left pending after the failure");
        c.close();
        ok(c.closed === true, "pg dead port: close() after failure leaves it closed");
    }
    {
        const t0 = Date.now();
        const c = new PostgreSQL({ host: "10.255.255.1", port: 5432, user: "x",
            database: "x", connectTimeoutMs: 600 });
        let rejected = false, msg = "";
        try {
            await withTimeout(c.query("SELECT 1"), 5000, "blackhole connect");
        } catch (e) { rejected = true; msg = errText(e); }
        const dt = Date.now() - t0;
        ok(rejected, "pg blackhole: unroutable address fails, no hang (" +
            msg.slice(0, 48) + ")");
        ok(dt < 3000, "pg blackhole: bounded, " + dt + "ms (bound 600ms)");
        if (/timed out/i.test(msg))
            ok(dt >= 300, "pg blackhole: the connect timer actually fired (dt=" + dt + "ms)");
        c.close();
        ok(c.closed === true, "pg blackhole: close() clean after the deadline");
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

async function main() {
    print("test_audit_livesrv_1: live PostgreSQL + Redis audit");

    print("-- PostgreSQL 5432-family (trust)");
    const trust = await probePG(PG_CFG, "pg trust probe");
    let livePG = trust.client, liveCfg = PG_CFG;
    if (!livePG) {
        skip("PostgreSQL " + PG_CFG.host + ":" + PG_CFG.port +
            " unreachable (" + trust.err.slice(0, 80) + "): trust PG sections skipped");
    } else {
        const rows = (await withTimeout(livePG.query("select 1 as one"), 5000, "select 1")).rows;
        ok(rows.length === 1 && rows[0].one === 1,
            "pg trust: connect + select 1 (" + JSON.stringify(rows) + ")");
        ok(livePG.ready === true && livePG.backendPid > 0,
            "pg trust: ready with backendPid " + livePG.backendPid);
        ok(livePG.transactionStatus === "I", "pg trust: idle after select");
    }

    print("-- PostgreSQL 55432 (scram-sha-256, real authentication)");
    if (!PG_PW) {
        skip("PostgreSQL SCRAM auth sections: LIVESRV_PG_PASSWORD (or PGPASSWORD) is not set");
    } else {
        const s = await probePG(Object.assign({}, SCRAM_CFG, { password: PG_PW }),
            "pg scram probe");
        if (!s.client) {
            skip("PostgreSQL SCRAM " + SCRAM_CFG.host + ":" + SCRAM_CFG.port +
                " unreachable or login refused (" + s.err.slice(0, 80) +
                "): auth sections skipped");
        } else {
            const rows = (await withTimeout(s.client.query("select current_user as u"),
                5000, "current_user")).rows;
            ok(rows.length === 1 && rows[0].u === SCRAM_CFG.user,
                "pg scram: correct password authenticates as " + SCRAM_CFG.user);
            if (!livePG) { livePG = s.client; liveCfg = Object.assign({}, SCRAM_CFG, { password: PG_PW }); }
            else s.client.close();

            const wrong = "livesrv-wrong-" + Date.now();
            const t0 = Date.now();
            const w = new PostgreSQL(Object.assign({}, SCRAM_CFG,
                { password: wrong, connectTimeoutMs: 1500 }));
            let rejected = false, msg = "";
            try {
                await withTimeout(w.query("select 1"), 5000, "wrong-password query");
            } catch (e) { rejected = true; msg = errText(e); }
            const dt = Date.now() - t0;
            ok(rejected, "pg scram: WRONG password is REJECTED by the real server (N3-16)");
            ok(/password authentication failed/i.test(msg),
                "pg scram: rejection names the auth failure: " + msg.slice(0, 72));
            ok(dt < 3000, "pg scram: rejection is prompt (" + dt + "ms)");
            ok(w.pending === 0, "pg scram: no command left pending after auth failure");
            w.close();
            ok(w.closed === true, "pg scram: failed client closes cleanly");
        }
    }

    if (livePG) {
        if (livePG === trust.client) {
            await pgParamsSection(livePG, PG_CFG);
        } else {
            await pgParamsSection(livePG, liveCfg);
        }
        await churnSection("pg", liveCfg,
            (cfg) => new PostgreSQL(Object.assign({}, cfg,
                { connectTimeoutMs: 1500, queryTimeoutMs: 3000 })),
            async (c) => {
                const r = await withTimeout(c.query("SELECT 1 AS one"), 5000, "churn query");
                if (r.rows[0].one !== 1) throw new Error("churn query wrong row");
            });
        livePG.close();
    } else {
        skip("PostgreSQL unreachable: parameter-binding, churn and teardown sections skipped");
    }

    await pgDeadTargetSection();

    print("-- Redis");
    const rd = await probeRedis(REDIS_CFG);
    if (!rd.client) {
        skip("Redis " + REDIS_CFG.host + ":" + REDIS_CFG.port + " unreachable (" +
            rd.err.slice(0, 80) + "): redis sections skipped");
    } else {
        await redisBasicAndBinary(rd.client);
        await redisReentrancySection();
        await churnSection("redis", REDIS_CFG,
            (cfg) => new Redis(Object.assign({}, cfg,
                { connectTimeoutMs: 1500, commandTimeoutMs: 3000 })),
            async (c) => {
                const v = await withTimeout(c.command("PING"), 5000, "redis churn PING");
                if (v !== "PONG") throw new Error("redis churn PING wrong reply");
            });
        rd.client.close();
    }

    const skipped = skipReasons.length;
    print("test_audit_livesrv_1: " + checks + " checks, " + fails +
        " failures, " + skipped + " skipped section(s)");
    for (const s of skipReasons) print("  skipped: " + s);
    if (fails || (REQUIRE && skipped)) {
        print("test_audit_livesrv_1: FAIL" +
            (fails ? "" : " (LIVESRV_REQUIRE=1 and sections were skipped)"));
        std.exit(1);
    }
    print("test_audit_livesrv_1: PASS");
    std.exit(0);
}

const WATCHDOG = setTimeout(() => {
    print("test_audit_livesrv_1: HUNG (suite watchdog)");
    std.exit(124);
}, 90000);

main().then(() => clearTimeout(WATCHDOG))
    .catch((e) => {
        print("FATAL: " + errText(e) + (e && e.stack ? "\n" + e.stack : ""));
        std.exit(1);
    });
