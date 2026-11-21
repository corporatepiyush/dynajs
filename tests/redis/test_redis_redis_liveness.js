// covers: dyna:redis.Redis
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_redis_redis_liveness");
import { Redis } from "dyna:redis";

let n = 0, fails = 0;
const ok = (c, m) => { n++; if (c) print("  ok  " + m); else { fails++; print("FAIL: " + m); } };

const rd = new Redis({ host: "127.0.0.1", port: 1, connectTimeoutMs: 500 });

await __kitS.section("redis: dead port must fail", async () => {
try { await rd.command("PING"); ok(false, "redis: dead port must fail"); }
catch (e) { ok(/connect/i.test(e.message), "redis: refused connect rejects (got: " + e.message.slice(0, 40) + ")"); }
});

__kitS.end();

print("test_redis_redis_liveness: " + n + " checks, " + fails + " failures (unclosed clients left live on purpose)");
if (fails) throw new Error("test_redis_redis_liveness: " + fails + " failures");
