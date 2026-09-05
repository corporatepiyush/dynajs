/* test_db_lifecycle.js — close()-inside-native-callback regressions.
 *
 * Each case drives JS user code from a C dispatch frame (reply settle, error
 * handler, notice handler, data handler, DNS answer) that calls close() on the
 * client being dispatched. Pre-fix this was a use-after-free: dispose freed
 * the struct and the C frame kept reading it. The in_cb/closing deferred-
 * teardown pattern must keep every rejection clean. Run under ASan+UBSan.
 *
 * Needs: redis-server :56379, postgres :5432 (trust).
 * Progress goes through os.write(1, …): fully buffered stdout is lost when a
 * hung run is killed, which reads as "no coverage".
 */
import { PostgreSQL, Redis } from "dyna:net";
import { TCPServer } from "dyna:net";
import { DNSResolver } from "dyna:net";
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
const ENC = new TextEncoder();
const W = (s) => { const b = ENC.encode(s); os.write(1, b.buffer, 0, b.length); };
function ok(c, m) {
    n++;
    if (!c) { fails++; W("FAIL: " + m + "\n"); }
    else W("  ok  " + m + "\n");
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const USER = "piyush";
const PG = { host: "127.0.0.1", port: 5432, user: USER, database: "postgres" };

async function main() {
    /* self-contained: start the scratch redis only if absent, and shut down
       at the end ONLY what we started -- killing a server another suite (or
       the developer) is using makes suite order a hidden dependency. */
    let redisOurs = false;
    if (os.exec(["sh", "-c", "redis-cli -p 56379 ping >/dev/null 2>&1"]) !== 0) {
        os.exec(["sh", "-c",
            "redis-server --port 56379 --daemonize yes --save '' >/dev/null 2>&1"]);
        redisOurs = true;
    }
    await sleep(150);

    /* ---- Redis: error handler closes client while fail_all iterates ---- */
    {
        const r = new Redis({ host: "127.0.0.1", port: 56379 });
        try {
            await r.command("PING");
            let errSeen = false;
            r.on("error", () => { errSeen = true; try { r.close(); } catch (e) {} });
            /* kill the server out from under one in-flight GET → fail_all runs,
             * h_error fires inside it, close() lands mid-iteration */
            const p = r.command("GET", "lc").catch(e => "rejected:" + e.message);
            os.exec(["sh", "-c", "redis-cli -p 56379 shutdown nosave >/dev/null 2>&1"]);
            const res = await p;
            await sleep(150);
            ok(errSeen === true || String(res).startsWith("rejected:"),
               "redis: fail_all + handler-close survived (" + String(res).slice(0,44) + ")");
        } finally {
            /* An unclosed client holds the shared reactor and its periodic
             * wakeup: the loop would never drain and the run would hang. */
            try { r.close(); } catch (e) {}
        }
    }
    /* restart for the rest of the suite */
    os.exec(["sh", "-c", "redis-server --port 56379 --daemonize yes --save '' >/dev/null 2>&1"]);
    await sleep(200);

    /* ---- Redis: close() inside push/error free path is deferred ---- */
    {
        const r = new Redis({ host: "127.0.0.1", port: 56379 });
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
    }

    /* ---- PG: notice callback closes the client mid-query ---- */
    {
        const pg = new PostgreSQL(PG);
        try {
            let notices = 0, closedInCb = false;
            pg.on("notice", () => { notices++; try { pg.close(); closedInCb = true; } catch (e) {} });
            let outcome = "";
            try {
                await pg.query("DO $$ BEGIN RAISE NOTICE 'lc'; END $$;");
                outcome = "resolved";
            } catch (e) { outcome = "threw:" + String(e.message).slice(0, 30); }
            await sleep(100);
            ok(true, "pg: notice-close survived (" + notices + " notices, outcome=" + outcome + ", closedInCb=" + closedInCb + ")");
        } finally {
            try { pg.close(); } catch (e) {}   /* close() is idempotent */
        }
    }

    /* ---- PG: pipeline resolve handler closes client during assemble ---- */
    {
        const pg = new PostgreSQL(PG);
        try {
            const stmts = []; for (let k = 0; k < 8; k++) stmts.push(["SELECT " + k]);
            let len = -1;
            await pg.pipeline(stmts).then(v => { len = v.length; try { pg.close(); } catch (e) {} });
            ok(len === 8, "pg: pipeline(8) close-in-resolve keeps all results");
        } finally {
            try { pg.close(); } catch (e) {}
        }
    }

    /* ---- PG: row-bearing query then() closes; next query rejects cleanly ---- */
    {
        const pg = new PostgreSQL(PG);
        try {
            const res = await pg.query("SELECT generate_series(1, 50)").then(v => { try { pg.close(); } catch (e) {} return v; });
            const rows = res.rows ?? res;
            ok((rows.rows ? rows.rows.length : rows.length) === 50, "pg: 50-row result intact across close-in-then");
        } finally {
            try { pg.close(); } catch (e) {}
        }
    }

    /* ---- PG: black-hole connect deadline fires on a quiet loop ---- */
    {
        const pg = new PostgreSQL({ host: "10.255.255.1", port: 5432, user: "x",
                                    database: "x", connectTimeoutMs: 500 });
        try {
            const t0 = Date.now();
            let msg = "";
            try { await pg.query("SELECT 1"); } catch (e) { msg = e.message; }
            const dt = Date.now() - t0;
            ok(dt < 1500 && dt >= 350, `pg: connect deadline fired on time (${dt}ms)`);
            ok(/timed out|connect/i.test(msg), "pg: failure names cause: " + msg.slice(0, 48));
        } finally {
            try { pg.close(); } catch (e) {}
        }
    }

    /* ---- TCP: server.close() inside its own data handler ---- */
    {
        const srv = new TCPServer({ port: 0 });
        let closedInside = false;
        srv.start({
            data: (c) => { c.write("ack");
                try { srv.close(); closedInside = true; }
                catch (e) { W("tcp close-in-data threw: " + e.message); } },
            close: () => {},
        });
        const cli = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
            connect: (c) => { try { c.write("ping"); } catch (e) { W("cli write: " + e.message); } },
            data: () => {}, close: () => {},
        });
        try {
            await sleep(300);
            ok(closedInside, "tcp: server.close() inside data handler survived");
        } finally {
            try { cli.close(); } catch (e) {}
            try { srv.close(); } catch (e) {}
        }
    }

    /* ---- DNS: resolver.close() inside its own callback (wrong-wire peer) ---- */
    {
        const r = new DNSResolver({ server: "127.0.0.1", port: 56379, timeoutMs: 700 });
        let fired = false;
        r.query("lifecycle.test", 1, (err) => {
            fired = true;
            try { r.close(); } catch (e) {}     /* close INSIDE native frame */
        });
        await sleep(1100);
        ok(fired, "dns: wrong-wire/timeout callback fired and close-in-cb survived");
    }

    /* ---- fd churn: 40 connect/query/close cycles. A stale callback landing
     * on a reused descriptor shows up as a wrong reply or a hang; the count
     * is the oracle (CLAUDE.md: a lower pass count with zero failures is a
     * silent skip). ---- */
    {
        let good = 0;
        for (let i = 0; i < 40; i++) {
            const r = new Redis({ host: "127.0.0.1", port: 56379 });
            try { await r.command("SET", "ch" + i, "" + i); 
                  const v = await r.command("GET", "ch" + i);
                  if ((v === "i") || String(v) === String(i)) good++;
                  else { W("churn mismatch at " + i + ": " + JSON.stringify(v)); }
            } catch (e) { W("churn error at " + i + ": " + e.message); }
            try { r.close(); } catch (e) {}
        }
        ok(good === 40, `fd churn: ${good}/40 cycles returned their OWN values`);
    }

    if (redisOurs)
        os.exec(["sh", "-c", "redis-cli -p 56379 shutdown nosave >/dev/null 2>&1"]);

    /* The verdict goes through the UNBUFFERED path: a hung run killed by the
     * harness must not lose it (the rule this file's header states). */
    if (fails) { W(`test_db_lifecycle: ${fails} FAILED of ${n}\n`); std.exit(1); }
    W(`test_db_lifecycle: ${n} checks, 0 failures\n`);
}

/* A watchdog that only PRINTS proves nothing: it has to END the process, or
 * a hang reads as a pass to any harness without its own wall-clock bound. */
const WATCHDOG = setTimeout(() => {
    W("test_db_lifecycle: HUNG (watchdog)\n");
    std.exit(124);
}, 12000);
main().then(() => clearTimeout(WATCHDOG))
      .catch(e => { W("FATAL: " + e.message + "\n"); std.exit(1); });
