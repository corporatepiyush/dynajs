// flags: --std
// timeout: 120
// covers: dyna:postgres.PostgreSQL
// Real-server integration layer for the PostgreSQL client (split from
// tests/net/test_audit_livesrv_1.js; the Redis half is
// tests/redis/test_redis_redis_audit_livesrv_1.js).
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
// The server is tests/lib/services.js ensurePostgres (one port, started and
// stopped by this suite when none answers): role djtrust over trust for the
// trust sections, postgres/pw over scram-sha-256 for the auth sections.
// A case SKIPs only when ensurePostgres cannot run one (no initdb);
// LIVESRV_REQUIRE=1 turns that skip into a failure.

import { PostgreSQL } from "dyna:postgres";
import { memoryUsage } from "dyna:sys";
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { ensurePostgres, stopStarted, PG_PORT } from "../lib/services.js";

const t = suite("test_postgres_postgre_sql_audit_livesrv_1");
let A = null;
const ok = (c, m) => { A.ok(c, m); if (c) print("  ok  " + m); };
const note = (m) => print("  ..  " + m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const env = (k, d) => {
    const v = std.getenv(k);
    return (v === undefined || v === "") ? d : v;
};

let PG_CFG = { host: "127.0.0.1", port: PG_PORT, user: "djtrust", database: "postgres" };
let SCRAM_CFG = { host: "127.0.0.1", port: PG_PORT, user: "postgres", database: "postgres" };
let PG_PW = "";
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
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(dt < 4500, "pg dead port: failure prompt, " + dt + "ms (deadline 700ms; the 5000ms withTimeout is the hang it must beat)");
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
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(dt < 4500, "pg blackhole: bounded, " + dt + "ms (deadline 600ms; the 5000ms withTimeout is the hang it must beat)");
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

// The servers are probed in the kit's before hook (never under --list); a case
// whose server is unreachable is a SKIP, and with LIVESRV_REQUIRE=1 a FAIL.
let svcOk = false, trust = null, livePG = null, liveCfg = PG_CFG, scram = null, scramWhy = null, WATCHDOG = null;
t.before(async () => {
    WATCHDOG = setTimeout(() => {
        print("test_postgres_postgre_sql_audit_livesrv_1: HUNG (suite watchdog)");
        std.exit(124);
    }, 90000);
    print("test_postgres_postgre_sql_audit_livesrv_1: live PostgreSQL audit");
    const svc = await ensurePostgres();
    if (svc.ok) {
        svcOk = true;
        PG_CFG = svc.trust;
        const { password, ...rest } = svc.conn;
        SCRAM_CFG = rest;
        PG_PW = password;
    }
    trust = await probePG(PG_CFG, "pg trust probe");
    livePG = trust.client;
    if (!PG_PW) scramWhy = "PostgreSQL SCRAM auth sections: no PostgreSQL (" + (svc.why || "unreachable") + ")";
    else {
        scram = await probePG(Object.assign({}, SCRAM_CFG, { password: PG_PW }), "pg scram probe");
        if (!scram.client)
            scramWhy = "PostgreSQL SCRAM " + SCRAM_CFG.host + ":" + SCRAM_CFG.port +
                " unreachable or login refused (" + scram.err.slice(0, 80) + "): auth sections skipped";
        else if (!livePG) { livePG = scram.client; liveCfg = Object.assign({}, SCRAM_CFG, { password: PG_PW }); }
    }
});
t.after(async () => {
    if (livePG) { try { livePG.close(); } catch (e) {} }
    if (scram && scram.client && scram.client !== livePG) { try { scram.client.close(); } catch (e) {} }
    if (WATCHDOG) clearTimeout(WATCHDOG);
    await stopStarted();
});
const unavailable = (a, why) => {
    if (REQUIRE) a.must(false, "LIVESRV_REQUIRE=1 and a section was skipped: " + why);
    if (svcOk) a.must(false, "the services.js PostgreSQL is up, so nothing may skip: " + why);
    a.skip(why);
};
const needLivePG = (a) => { if (!livePG) unavailable(a, "PostgreSQL unreachable: parameter-binding, churn and teardown sections skipped"); };
test("pg trust: connect, select 1, ready and idle", async (a) => {
    if (!trust.client)
        unavailable(a, "PostgreSQL " + PG_CFG.host + ":" + PG_CFG.port +
            " unreachable (" + trust.err.slice(0, 80) + "): trust PG sections skipped");
    const livePG = trust.client;
    const rows = (await withTimeout(livePG.query("select 1 as one"), 5000, "select 1")).rows;
    ok(rows.length === 1 && rows[0].one === 1,
        "pg trust: connect + select 1 (" + JSON.stringify(rows) + ")");
    ok(livePG.ready === true && livePG.backendPid > 0,
        "pg trust: ready with backendPid " + livePG.backendPid);
    ok(livePG.transactionStatus === "I", "pg trust: idle after select");
});

test("pg scram: the right password authenticates and a wrong one is rejected promptly", async (a) => {
    if (scramWhy) unavailable(a, scramWhy);
    const rows = (await withTimeout(scram.client.query("select current_user as u"),
        5000, "current_user")).rows;
    ok(rows.length === 1 && rows[0].u === SCRAM_CFG.user,
        "pg scram: correct password authenticates as " + SCRAM_CFG.user);
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
    // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
    ok(dt < 4500, "pg scram: rejection is prompt, inside the 5000ms withTimeout (" + dt + "ms)");
    ok(w.pending === 0, "pg scram: no command left pending after auth failure");
    w.close();
    ok(w.closed === true, "pg scram: failed client closes cleanly");
});

test("pg: extended-protocol parameter binding", async (a) => {
    needLivePG(a);
    await pgParamsSection(livePG, liveCfg);
});

test("pg: client churn releases its descriptors", async (a) => {
    needLivePG(a);
    await churnSection("pg", liveCfg,
        (cfg) => new PostgreSQL(Object.assign({}, cfg,
            { connectTimeoutMs: 1500, queryTimeoutMs: 3000 })),
        async (c) => {
            const r = await withTimeout(c.query("SELECT 1 AS one"), 5000, "churn query");
            if (r.rows[0].one !== 1) throw new Error("churn query wrong row");
        });
});

test("pg: unreachable targets fail within their deadlines", async () => {
    await pgDeadTargetSection();
});

await t.run();
