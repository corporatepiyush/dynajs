// flags: --std
// covers: c:src/pool.js
// The PgPool half of tests/net/test_pool.js (one module per suite); RedisPool is
// tests/redis/test_redis_pool.js.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_pool", { chain: true });
import { PgPool } from "../../src/pool.js";
import * as os from "os";
import { PostgreSQL } from "dyna:postgres";
import { ensurePostgres, stopStarted, PG_PORT } from "../lib/services.js";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsAsync(fn, m) {
    n++;
    return fn().then(() => { fails++; print("FAIL: " + m + " did not throw"); }, () => {});
}

const svc = __kitS.listing ? null : await ensurePostgres();
const PG_OPTS = svc && svc.ok ? svc.conn : { host: "127.0.0.1", port: PG_PORT, user: "postgres", password: "pw", database: "postgres" };

let havePG = false;
try {
    const probe = new PostgreSQL({ ...PG_OPTS, connectTimeoutMs: 800 });
    try { await probe.query("SELECT 1"); havePG = true; } catch (e) {}
    finally { try { probe.close(); } catch (e) {} }
} catch (e) {}
if (!havePG && svc && svc.ok) ok(false, "the services.js PostgreSQL on " + PG_OPTS.port + " refused a SCRAM login");
if (!havePG && !__kitS.listing) print("  SKIP  no PostgreSQL (" + (svc && svc.why || "unreachable") + "): the PgPool sections are skipped");

await __kitS.section("initial stats empty", async () => {
if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 2, idleMs: 1000, acquireTimeoutMs: 1000 });
    ok(pool.stats.total === 0 && pool.stats.free === 0, "initial stats empty");
    const c1 = await pool.acquire();
    ok(pool.stats.used === 1 && pool.stats.total === 1, "acquire one");
    const c2 = await pool.acquire();
    ok(pool.stats.used === 2 && pool.stats.total === 2, "acquire two up to size");
    let timedOut = false;
    const p3 = pool.acquire().catch(e => { timedOut = /timed out/i.test(e.message); });
    await new Promise(r => setTimeout(r, 1200));
    ok(timedOut, "third acquire times out");
    pool.release(c1);
    ok(pool.stats.free === 1 && pool.stats.used === 1, "release one to free");
    const c3 = await pool.acquire();
    ok(c3 === c1, "acquire reuses freed client");
    ok(pool.stats.free === 0 && pool.stats.used === 2, "stats after reuse");
    pool.release(c2);
    pool.release(c3);
    ok(pool.stats.free === 2 && pool.stats.used === 0, "all released to free");
    await pool.close();
    ok(pool.stats.closed, "pool marked closed");
    try { await pool.acquire(); ok(false, "acquire after close should throw"); } catch (e) { ok(/closed/i.test(e.message), "acquire after close throws"); }
    await pool.close();
    ok(true, "double close harmless");
}
});

await __kitS.section("SELECT 1 as n", async () => {
if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const r = await pool.query("SELECT 1 as n");
    const rows = r.rows ?? r;
    ok(rows[0].n === 1, "pool.query returns rows");
    ok(pool.stats.used === 0 && pool.stats.free === 1, "pool.query auto-releases");
    await pool.close();
}
});

await __kitS.section("pool.pipeline returns 5 results", async () => {
if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const stmts = [];
    for (let i = 0; i < 5; i++) stmts.push([`SELECT ${i} as n`]);
    const res = await pool.pipeline(stmts);
    ok(Array.isArray(res) && res.length === 5, "pool.pipeline returns 5 results");
    await pool.close();
}
});

if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const val = await pool.with(async (c) => {
        await c.query("SELECT 1");
        return 42;
    });
    ok(val === 42, "pool.with returns fn result");
    ok(pool.stats.used === 0, "with() releases after fn");
    try {
        await pool.with(async (c) => { throw new Error("oops"); });
        ok(false, "with should propagate error");
    } catch (e) {
        ok(e.message === "oops", "with propagates error");
        ok(pool.stats.used === 0, "with releases even on throw");
    }
    await pool.close();
}

await __kitS.section("one idle", async () => {
if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 2, idleMs: 300 });
    const c1 = await pool.acquire();
    pool.release(c1);
    ok(pool.stats.free === 1, "one idle");
    await new Promise(r => setTimeout(r, 600));
    ok(pool.stats.free === 0 && pool.stats.total === 0, "idle client evicted after idleMs");
    await pool.close();
}
});

await __kitS.section("8 concurrent queries via pool size 4", async () => {
if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 4 });
    const promises = [];
    for (let i = 0; i < 8; i++) promises.push(pool.query(`SELECT ${i} as n`));
    const results = await Promise.all(promises);
    ok(results.length === 8, "8 concurrent queries via pool size 4");
    for (let i = 0; i < 8; i++) {
        const rows = results[i].rows ?? results[i];
        ok(rows[0].n === i, `concurrent result ${i} correct`);
    }
    ok(pool.stats.used === 0, "all concurrent queries released");
    await pool.close();
}
});

await __kitS.section("SELECT generate_series(1, 100) as n", async () => {
if (havePG) {
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const client = await pool.acquire();
    let count = 0;
    let sum = 0;
    for await (const row of client.queryIter("SELECT generate_series(1, 100) as n", [], { maxRows: 50 })) {
        count++;
        sum += row.n;
    }
    ok(count === 50, "queryIter early break at 50 via maxRows");
    ok(sum === 1275, "queryIter sum 1..50 via maxRows");
    const r = await client.query("SELECT 1 as n");
    const rows = r.rows ?? r;
    ok(rows[0].n === 1, "client usable after early break");
    pool.release(client);

    const client2 = await pool.acquire();
    let count2 = 0;
    for await (const row of client2.queryIter("SELECT generate_series(1, 100) as n", [], { batch: 10 })) {
        count2++;
    }
    ok(count2 === 100, "queryIter full iteration 100 rows with batch 10");
    pool.release(client2);

    const memBefore = os ? 0 : 0;
    const client3 = await pool.acquire();
    let count3 = 0;
    for await (const row of client3.queryIter("SELECT generate_series(1, 10000) as n", [], { batch: 500 })) {
        count3++;
    }
    ok(count3 === 10000, "queryIter 10k rows");
    pool.release(client3);

    const client4 = await pool.acquire();
    let count4 = 0;
    for await (const row of client4.queryIter("SELECT generate_series(1, 100) as n", [], { maxRows: 10 })) {
        count4++;
    }
    ok(count4 === 10, "queryIter maxRows 10");
    pool.release(client4);

    await pool.close();
}
});
await stopStarted();
__kitS.end();

if (fails) { print(`test_postgres_pool: ${fails} FAILED of ${n}`); throw new Error("failed"); }
print(`test_postgres_pool: ${n} checks, 0 failures`);
