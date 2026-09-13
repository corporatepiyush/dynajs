/* test_net_liveness.js -- a DEAD client must not hold the event loop open.
 *
 * pg_fail_all / redis_fail_all close the fd and settle every queue, but the
 * client kept its dyn_net_on_drain tick hook and its reactor ref: a script
 * that forgot .close() then hung js_std_loop forever on the 250 ms tick,
 * waking to do nothing (the fd was already gone). DEAD is terminal -- there
 * is no reconnect API -- so the fix returns the liveness holdings at the
 * dead-transition.
 *
 * THE TEST IS THE EXIT: this file deliberately does NOT close its clients.
 * If the liveness release regresses, the suite's watchdog kills the run
 * (exit 124) rather than these checks failing -- the hang IS the assertion.
 * The rejects still get asserted so a fast-failing regression is readable.
 *
 * Needs no servers: 10.255.255.1 is non-routable (the pg connect deadline
 * fires first), and 127.0.0.1:1 refuses immediately.
 */
import { PostgreSQL, Redis, TCPServer } from "dyna:net";

let n = 0, fails = 0;
const ok = (c, m) => { n++; if (c) print("  ok  " + m); else { fails++; print("FAIL: " + m); } };

const t0 = Date.now();
const pg = new PostgreSQL({ host: "10.255.255.1", port: 5432, user: "x",
                            database: "x", connectTimeoutMs: 500 });
try { await pg.query("SELECT 1"); ok(false, "pg: blackhole connect must fail"); }
catch (e) { ok(/timed out/.test(e.message), "pg: deadline rejects (got: " + e.message.slice(0, 40) + ")"); }
const pgDt = Date.now() - t0;
ok(pgDt < 5000, "pg: the deadline fired on time (" + pgDt + "ms)");

const rd = new Redis({ host: "127.0.0.1", port: 1, connectTimeoutMs: 500 });
try { await rd.command("PING"); ok(false, "redis: dead port must fail"); }
catch (e) { ok(/connect/i.test(e.message), "redis: refused connect rejects (got: " + e.message.slice(0, 40) + ")"); }

/* TCP: the same shape via a refused connect. This case also pins the
 * gc_mark fix it depends on -- before dyn_tcp_gc_mark marked the handlers
 * and self_pending, the resource->t->handler->env->resource cycle was
 * invisible to the collector: the leaked handler aborted JS_FreeRuntime
 * with "Object leaks" the moment the liveness fix let the process EXIT
 * instead of hanging forever. */
const tc = TCPServer.connect({ host: "127.0.0.1", port: 1, connectTimeoutMs: 500 }, {
  connect: (conn, err) => { ok(!!err, "tcp: refused connect reports the error"); },
});

/* No .close() anywhere: all three clients stay JS-reachable past this print.
 * Reaching the print at all is half the test; the process exit after it is
 * the other half (watchdog-enforced, and the exit must also clear
 * JS_FreeRuntime -- a clean 0, not an abort). */
print("test_net_liveness: " + n + " checks, " + fails + " failures (unclosed clients left live on purpose)");
if (fails) throw new Error("test_net_liveness: " + fails + " failures");
