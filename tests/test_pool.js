/* test_pool.js — PgPool, RedisPool, and PostgreSQL queryIter streaming
 *
 * Tests the new pooling and streaming APIs added to dyna:net.
 * Requires: postgres :5432 (brew postgresql@16), redis :56379
 */

import { PgPool, RedisPool } from "../src/pool.js";
import * as os from "os";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsAsync(fn, m) {
    n++;
    return fn().then(() => { fails++; print("FAIL: " + m + " did not throw"); }, () => {});
}

const PG_OPTS = { host: "127.0.0.1", port: 5432, user: "piyush", database: "postgres" };

/* self-contained: ensure the scratch redis is up (another suite's cleanup
   may have stopped it -- suite order must not be a hidden dependency). */
if (os.exec(["sh", "-c", "redis-cli -p 56379 ping >/dev/null 2>&1"]) !== 0) {
    os.exec(["sh", "-c",
        "redis-server --port 56379 --daemonize yes --save '' >/dev/null 2>&1"]);
    await new Promise(r => setTimeout(r, 300));
}

// ------------------------------------------------------------------ PgPool

{
    // Basic acquire/release and stats
    const pool = new PgPool({ ...PG_OPTS, size: 2, idleMs: 1000, acquireTimeoutMs: 1000 });
    ok(pool.stats.total === 0 && pool.stats.free === 0, "initial stats empty");
    const c1 = await pool.acquire();
    ok(pool.stats.used === 1 && pool.stats.total === 1, "acquire one");
    const c2 = await pool.acquire();
    ok(pool.stats.used === 2 && pool.stats.total === 2, "acquire two up to size");
    // Third acquire should wait and then timeout
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
    // Second close is harmless
    await pool.close();
    ok(true, "double close harmless");
}

{
    // query() auto acquire/release
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const r = await pool.query("SELECT 1 as n");
    const rows = r.rows ?? r;
    ok(rows[0].n === 1, "pool.query returns rows");
    ok(pool.stats.used === 0 && pool.stats.free === 1, "pool.query auto-releases");
    await pool.close();
}

{
    // pipeline() via pool
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const stmts = [];
    for (let i = 0; i < 5; i++) stmts.push([`SELECT ${i} as n`]);
    const res = await pool.pipeline(stmts);
    ok(Array.isArray(res) && res.length === 5, "pool.pipeline returns 5 results");
    await pool.close();
}

{
    // with() pins a client for transaction
    const pool = new PgPool({ ...PG_OPTS, size: 2 });
    const val = await pool.with(async (c) => {
        await c.query("SELECT 1");
        return 42;
    });
    ok(val === 42, "pool.with returns fn result");
    ok(pool.stats.used === 0, "with() releases after fn");
    // with() with error should still release
    try {
        await pool.with(async (c) => { throw new Error("oops"); });
        ok(false, "with should propagate error");
    } catch (e) {
        ok(e.message === "oops", "with propagates error");
        ok(pool.stats.used === 0, "with releases even on throw");
    }
    await pool.close();
}

{
    // idle eviction
    const pool = new PgPool({ ...PG_OPTS, size: 2, idleMs: 300 });
    const c1 = await pool.acquire();
    pool.release(c1);
    ok(pool.stats.free === 1, "one idle");
    await new Promise(r => setTimeout(r, 600));
    ok(pool.stats.free === 0 && pool.stats.total === 0, "idle client evicted after idleMs");
    await pool.close();
}

{
    // Concurrent queries via pool (4 clients, 8 concurrent queries)
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

// ---------------------------------------------------------------- RedisPool

{
    const pool = new RedisPool({ host: "127.0.0.1", port: 56379, size: 2 });
    const r1 = await pool.acquire();
    ok(r1 !== null, "redis acquire one");
    const r2 = await pool.acquire();
    ok(pool.stats.used === 2, "redis acquire two");
    let timedOut = false;
    pool.acquire().catch(e => { timedOut = /timed out/i.test(e.message); });
    await new Promise(r => setTimeout(r, 1200));
    // Need to set acquireTimeoutMs to 1000 for this test, but default is 5000, so we need to create with short timeout
    // This test uses default 5000, so waiting 1200 won't timeout. Let's just test close with waiters.
    // For timeout test, use a pool with short timeout
    const pool2 = new RedisPool({ host: "127.0.0.1", port: 56379, size: 1, acquireTimeoutMs: 800 });
    const a = await pool2.acquire();
    let to = false;
    pool2.acquire().catch(e => { to = /timed out/i.test(e.message); });
    await new Promise(r => setTimeout(r, 1200));
    ok(to, "redisPool acquire timeout");
    pool2.release(a);
    await pool2.close();
    pool.release(r1);
    pool.release(r2);
    await pool.close();
    ok(pool.stats.closed, "redisPool closed");
}

{
    const pool = new RedisPool({ host: "127.0.0.1", port: 56379, size: 2 });
    const v = await pool.command("SET", "pooltest", "hello");
    ok(v === "OK", "redisPool.command SET");
    const g = await pool.command("GET", "pooltest");
    ok(g === "hello", "redisPool.command GET");
    const p = await pool.pipeline([["SET", "a", "1"], ["SET", "b", "2"], ["GET", "a"]]);
    ok(Array.isArray(p) && p.length === 3, "redisPool.pipeline");
    await pool.close();
}

// ---------------------------------------------------------------- queryIter streaming

{
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
    // After early break, connection should still be usable
    const r = await client.query("SELECT 1 as n");
    const rows = r.rows ?? r;
    ok(rows[0].n === 1, "client usable after early break");
    pool.release(client);

    // Full iteration
    const client2 = await pool.acquire();
    let count2 = 0;
    for await (const row of client2.queryIter("SELECT generate_series(1, 100) as n", [], { batch: 10 })) {
        count2++;
    }
    ok(count2 === 100, "queryIter full iteration 100 rows with batch 10");
    pool.release(client2);

    // Streaming should be memory efficient: compare with materialized
    // Materialized 100k rows would be ~12MB, streaming should be similar but we test with 10k
    const memBefore = os ? 0 : 0; // placeholder for RSS check
    const client3 = await pool.acquire();
    let count3 = 0;
    for await (const row of client3.queryIter("SELECT generate_series(1, 10000) as n", [], { batch: 500 })) {
        count3++;
    }
    ok(count3 === 10000, "queryIter 10k rows");
    pool.release(client3);

    // maxRows via queryIter
    const client4 = await pool.acquire();
    let count4 = 0;
    for await (const row of client4.queryIter("SELECT generate_series(1, 100) as n", [], { maxRows: 10 })) {
        count4++;
    }
    ok(count4 === 10, "queryIter maxRows 10");
    pool.release(client4);

    await pool.close();
}

if (fails) { print(`test_pool: ${fails} FAILED of ${n}`); throw new Error("failed"); }
print(`test_pool: ${n} checks, 0 failures`);
