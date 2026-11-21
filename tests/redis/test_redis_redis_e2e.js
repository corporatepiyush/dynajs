// flags: --std
// covers: dyna:redis.Redis
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_redis_redis_e2e");
import * as std from "std";
import { ensureRedis, stopStarted, REDIS_PORT } from "../lib/services.js";
import { Redis } from "dyna:redis";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const svc = __kitS.listing ? null : await ensureRedis(REDIS_PORT);
if (svc && !svc.ok) print("test_redis_redis_e2e: " + svc.why);

await __kitS.section("test_redis_redis_e2e: redis SKIPPED", async () => {
  let r = null;
  try {
    r = new Redis({ host: "127.0.0.1", port: REDIS_PORT });
    await r.command("PING");
  } catch (e) {
    print("test_redis_redis_e2e: redis SKIPPED (no server on 127.0.0.1:" +
          REDIS_PORT + ")");
    if (r) r.close();
    r = null;
  }
  if (r) {
    print("  redis " + r.protocol);
    check(await r.command("SET", "e2e:key", "v1") === "OK", "SET");
    check(await r.command("GET", "e2e:key") === "v1", "GET round trip");
    check(await r.command("INCR", "e2e:counter") >= 1, "INCR");
    const evil = "x\r\n*1\r\n$8\r\nFLUSHALL\r\n";
    await r.command("SET", "e2e:evil", evil);
    check(await r.command("GET", "e2e:evil") === evil,
          "a CRLF+command value must round trip as DATA");
    const p = await r.pipeline([["SET", "e2e:p1", "a"], ["GET", "e2e:p1"],
                                ["PING"]]);
    check(Array.isArray(p) && p.length === 3 && p[1] === "a" && p[2] === "PONG",
          "pipeline order/values: " + JSON.stringify(p));
    await r.command("SET", "e2e:ttl", "x", "PX", 150);
    await new Promise((res) => setTimeout(res, 900));
    check(await r.command("GET", "e2e:ttl") === null, "PX expiry honoured");
    r.close();
  }
});
__kitS.end();
await stopStarted();

if (fails === 0) print("test_redis_redis_e2e: all " + n + " checks passed");
else { print("test_redis_redis_e2e: " + fails + " FAILED of " + n); throw new Error("test_redis_redis_e2e: " + fails + " failures"); }
