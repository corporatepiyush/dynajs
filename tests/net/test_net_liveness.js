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

const tc = TCPServer.connect({ host: "127.0.0.1", port: 1, connectTimeoutMs: 500 }, {
  connect: (conn, err) => { ok(!!err, "tcp: refused connect reports the error"); },
});

print("test_net_liveness: " + n + " checks, " + fails + " failures (unclosed clients left live on purpose)");
if (fails) throw new Error("test_net_liveness: " + fails + " failures");
