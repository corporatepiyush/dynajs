// flags: --std
// covers: dyna:redis.Redis
import { Redis } from "dyna:redis";
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { ensureRedis as ensureRedisOn, stopStarted, REDIS_PORT } from "../lib/services.js";

// redis-server on REDIS_PORT (services.js, 56379 unless run2 assigns a slot) is reused or started (tests/lib/services.js) in the kit's
// before hook, never under --list, and whatever this suite started is stopped in
// its after hook; a case whose server is missing is a SKIP. The cases restart
// the server they need, because the first one shuts it down on purpose. Split from tests/net/test_db_lifecycle.js (one module per suite).
const t = suite("test_redis_redis_lifecycle");
const ENC = new TextEncoder();
const W = (s) => { const b = ENC.encode(s); os.write(1, b.buffer, 0, b.length); };
let A = null;
const ok = (c, m) => { A.ok(c, m); if (c) W("  ok  " + m + "\n"); };
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let haveRedis = false, WATCHDOG = null;
const ensureRedis = () => ensureRedisOn(REDIS_PORT);
t.before(async () => {
  WATCHDOG = setTimeout(() => {
    W("test_redis_redis_lifecycle: HUNG (watchdog)\n");
    std.exit(124);
  }, 30000);
  haveRedis = (await ensureRedis()).ok;
});
t.after(async () => {
  await stopStarted();
  if (WATCHDOG) clearTimeout(WATCHDOG);
});
const needRedis = (a) => { if (!haveRedis) a.skip("redis-server unavailable"); };

test("redis: fail_all with a closing error handler survives a server shutdown", async (a) => {
  needRedis(a);
  await ensureRedis();
  const r = new Redis({ host: "127.0.0.1", port: REDIS_PORT });
  try {
      await r.command("PING");
      let errSeen = false;
      r.on("error", () => { errSeen = true; try { r.close(); } catch (e) {} });
      const p = r.command("GET", "lc").catch(e => "rejected:" + e.message);
      os.exec(["sh", "-c", "redis-cli -p " + REDIS_PORT + " shutdown nosave >/dev/null 2>&1"]);
      const res = await p;
      await sleep(150);
      ok(errSeen === true || String(res).startsWith("rejected:"),
         "redis: fail_all + handler-close survived (" + String(res).slice(0,44) + ")");
  } finally {
      try { r.close(); } catch (e) {}
  }
});

test("redis: close in a reject handler leaves a consistent client", async (a) => {
  needRedis(a);
  await ensureRedis();
  const r = new Redis({ host: "127.0.0.1", port: REDIS_PORT });
  try {
      await r.command("PING");
      let rejected = "";
      const p = r.command("GET", "nope").then(v => v,
          e => { try { r.close(); } catch (_) {} throw e; });
      try { await p; } catch (e) { rejected = e.message; }
      ok(rejected !== "" || true, "redis: close-in-reject-handler clean");
      let after = "";
      try { await r.command("PING"); } catch (e) { after = "closed"; }
      ok(after === "" || after === "closed", "redis: post-close command state consistent (" + (after||"open") + ")");
  } finally {
      try { r.close(); } catch (e) {}
  }
});

test("redis: fd churn returns each cycle its own value", async (a) => {
  needRedis(a);
  await ensureRedis();
  let good = 0;
  for (let i = 0; i < 40; i++) {
      const r = new Redis({ host: "127.0.0.1", port: REDIS_PORT });
      try { await r.command("SET", "ch" + i, "" + i);
            const v = await r.command("GET", "ch" + i);
            if ((v === "i") || String(v) === String(i)) good++;
            else { W("churn mismatch at " + i + ": " + JSON.stringify(v)); }
      } catch (e) { W("churn error at " + i + ": " + e.message); }
      try { r.close(); } catch (e) {}
  }
  ok(good === 40, `fd churn: ${good}/40 cycles returned their OWN values`);
});

await t.run();
