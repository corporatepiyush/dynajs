import { TCPServer, connectHappy } from "dyna:net";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const srv = new TCPServer({ port: 0 });
srv.start({});

function churn(N, done) {
    let i = 0, errs = 0;
    (function step() {
        if (i++ >= N) { done(errs); return; }
        const h = connectHappy("localhost", srv.port, { fallbackMs: 500 }, {
            connect: (c, err) => {
                if (err) errs++;
                if (c) c.close();
                h.close();
                step();
            },
        });
        if (!h) { errs++; done(errs); }
    })();
}

if (scriptArgs[1]) {
    const N = parseInt(scriptArgs[1], 10);
    churn(N, (errs) => {
        print("cycles=" + N + " errors=" + errs);
        srv.close();
    });
} else {
    let v4ok = false, v4err = null;
    const a = connectHappy("127.0.0.1", srv.port, { fallbackMs: 250 }, {
        connect: (c, err) => { if (err) v4err = String(err); else { v4ok = true; c.close(); } },
    });

    let v6ok = false, v6err = null;
    const b = connectHappy("::ffff:127.0.0.1", srv.port, { fallbackMs: 250 }, {
        connect: (c, err) => { if (err) v6err = String(err); else { v6ok = true; c.close(); } },
    });

    let refErr = null, refOk = false;
    const r = connectHappy("127.0.0.1", 1, { fallbackMs: 500 }, {
        connect: (c, err) => { if (err) refErr = String(err); else refOk = true; },
    });

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
