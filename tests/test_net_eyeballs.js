/* test_net_eyeballs.js -- Happy Eyeballs dual-stack connect (connectHappy).
 *
 * Every fixture is loopback; none depends on the network:
 *   (a) a v4-only host connects within the fallback window;
 *   (b) a v6-only host connects (::ffff:127.0.0.1 is AF_INET6 and reaches the
 *       0.0.0.0 listener through the standard v4-mapped path);
 *   (c) a closed port is REFUSED -- a different error from the timeout text;
 *   (d) the "localhost" A+AAAA race has exactly one winner and the loser
 *       never reports, and repeated races settle cleanly.
 *
 * With scriptArgs[1] = N it runs N sequential connect+close cycles only and
 * prints a machine-readable line; the test-net-eyeballs-churn target runs it
 * under a low ulimit, where one leaked loser descriptor becomes fatal
 * (test_net_fdchurn pattern).
 */
import { TCPServer, connectHappy } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const srv = new TCPServer({ port: 0 });
srv.start({});

/* ---- one race per cycle, sequential ------------------------------------ */
function churn(N, done) {
    let i = 0, errs = 0;
    (function step() {
        if (i++ >= N) { done(errs); return; }
        const h = connectHappy("localhost", srv.port, { fallbackMs: 500 }, {
            connect: (c, err) => {
                if (err) errs++;
                if (c) c.close();
                h.close();   /* dispose releases the reactor reference */
                step();
            },
        });
        if (!h) { errs++; done(errs); }
    })();
}

/* ---- churn-only mode for the ulimit target ----------------------------- */
if (scriptArgs[1]) {
    const N = parseInt(scriptArgs[1], 10);
    churn(N, (errs) => {
        print("cycles=" + N + " errors=" + errs);
        srv.close();
    });
} else {
    /* ---- (a) v4-only ----------------------------------------------------- */
    let v4ok = false, v4err = null;
    const a = connectHappy("127.0.0.1", srv.port, { fallbackMs: 250 }, {
        connect: (c, err) => { if (err) v4err = String(err); else { v4ok = true; c.close(); } },
    });

    /* ---- (b) v6-only: an AF_INET6 attempt reaching the v4 listener -------- */
    let v6ok = false, v6err = null;
    const b = connectHappy("::ffff:127.0.0.1", srv.port, { fallbackMs: 250 }, {
        connect: (c, err) => { if (err) v6err = String(err); else { v6ok = true; c.close(); } },
    });

    /* ---- (c) refusal: distinct from the timeout message ------------------ */
    let refErr = null, refOk = false;
    const r = connectHappy("127.0.0.1", 1, { fallbackMs: 500 }, {
        connect: (c, err) => { if (err) refErr = String(err); else refOk = true; },
    });

    /* ---- (d) the dual-stack race: exactly one winner, loser silent -------- */
    let raceWins = 0, raceErrs = 0;
    const l = connectHappy("localhost", srv.port, { fallbackMs: 250 }, {
        connect: (c, err) => { if (err) raceErrs++; else { raceWins++; c.close(); } },
    });

    function settled() {
        return (v4ok || v4err !== null) &&
               (v6ok || v6err !== null) &&
               (refErr !== null || refOk) &&
               (raceWins + raceErrs >= 1);
    }

    function finish() {
        check(v4ok && v4err === null,
              "v4-only fixture must connect within the window (err=" + v4err + ")");
        check(v6ok && v6err === null,
              "v6-only fixture must connect within the window (err=" + v6err + ")");
        check(refErr !== null && !refOk && refErr.indexOf("refused") >= 0,
              "a closed port must be refused, got success=" + refOk + " err=" + refErr);
        check(refErr === null || refErr.indexOf("timed out") < 0,
              "refusal must not be reported as a timeout: " + refErr);
        check(raceWins === 1 && raceErrs === 0,
              "the A+AAAA race must have exactly one winner (wins=" + raceWins +
              ", errors=" + raceErrs + ")");

        /* a DNS failure is its own error, deterministically (no network) */
        let dnsErr = null;
        try { const d = connectHappy(" ", srv.port, { fallbackMs: 250 }); if (d) d.close(); }
        catch (e) { dnsErr = String(e); }
        check(dnsErr !== null && dnsErr.indexOf("DNS resolution failed") >= 0,
              "a non-resolvable host must fail with the DNS error, got " + dnsErr);

        churn(25, (errs) => {
            check(errs === 0, "25 sequential races must all settle cleanly, errors=" + errs);
            a.close(); b.close(); r.close(); l.close();
            srv.close();
            if (fails === 0) print("test_net_eyeballs: all " + n + " checks passed");
            else print("test_net_eyeballs: " + fails + " FAILED");
        });
    }

    let spins = 0;
    const t = setInterval(() => {
        if (settled() || spins++ > 600) { clearInterval(t); finish(); }
    }, 10);
}
