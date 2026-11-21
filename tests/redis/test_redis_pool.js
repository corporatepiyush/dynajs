// flags: --std
// covers: c:src/pool.js
// The RedisPool half of tests/net/test_pool.js (one module per suite); PgPool is
// tests/postgres/test_postgres_pool.js.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_redis_pool", { chain: true });
import { RedisPool } from "../../src/pool.js";
import { ensureRedis, stopStarted, REDIS_PORT } from "../lib/services.js";
import { Redis } from "dyna:redis";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsAsync(fn, m) {
    n++;
    return fn().then(() => { fails++; print("FAIL: " + m + " did not throw"); }, () => {});
}

const haveRedis = !__kitS.listing && (await ensureRedis(REDIS_PORT)).ok;
if (!haveRedis) print("  SKIP  redis-server unavailable: the RedisPool sections are skipped");

await __kitS.section("redis acquire one", async () => {
if (haveRedis) {
    const pool = new RedisPool({ host: "127.0.0.1", port: REDIS_PORT, size: 2 });
    const r1 = await pool.acquire();
    ok(r1 !== null, "redis acquire one");
    const r2 = await pool.acquire();
    ok(pool.stats.used === 2, "redis acquire two");
    let timedOut = false;
    pool.acquire().catch(e => { timedOut = /timed out/i.test(e.message); });
    await new Promise(r => setTimeout(r, 1200));
    const pool2 = new RedisPool({ host: "127.0.0.1", port: REDIS_PORT, size: 1, acquireTimeoutMs: 800 });
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
});

await __kitS.section("redisPool.command SET", async () => {
if (haveRedis) {
    const pool = new RedisPool({ host: "127.0.0.1", port: REDIS_PORT, size: 2 });
    const v = await pool.command("SET", "pooltest", "hello");
    ok(v === "OK", "redisPool.command SET");
    const g = await pool.command("GET", "pooltest");
    ok(g === "hello", "redisPool.command GET");
    const p = await pool.pipeline([["SET", "a", "1"], ["SET", "b", "2"], ["GET", "a"]]);
    ok(Array.isArray(p) && p.length === 3, "redisPool.pipeline");
    await pool.close();
}
});


await __kitS.section("a pooled client is a dyna:redis Redis", async () => {
if (haveRedis) {
    const pool = new RedisPool({ host: "127.0.0.1", port: REDIS_PORT, size: 1 });
    const c = await pool.acquire();
    ok(c instanceof Redis, "RedisPool hands out dyna:redis Redis clients");
    pool.release(c);
    await pool.close();
}
});
__kitS.end();
await stopStarted();

if (fails) { print(`test_redis_pool: ${fails} FAILED of ${n}`); throw new Error("failed"); }
print(`test_redis_pool: ${n} checks, 0 failures`);
