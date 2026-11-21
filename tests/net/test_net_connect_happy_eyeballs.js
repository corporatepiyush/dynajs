// covers: dyna:net.connectHappy
// The listening TCPServer starts in the kit's before hook (never under --list);
// each case opens its own connectHappy handles and closes them. With a cycle
// count argument (./dynajs FILE N) the file is the churn driver instead.
import { TCPServer, connectHappy } from "dyna:net";
import { suite } from "../lib/kit.js";

let srv = null;
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
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(pred, ms) { for (let i = 0; i < ms / 10 && !pred(); i++) await sleep(10); }

if (scriptArgs[1]) {
    srv = new TCPServer({ port: 0 });
    srv.start({});
    const N = parseInt(scriptArgs[1], 10);
    churn(N, (errs) => {
        print("cycles=" + N + " errors=" + errs);
        srv.close();
    });
} else {
    const t = suite("test_net_eyeballs");
    let A = null;
    const check = (c, m) => A.ok(c, m);
    const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
    const handles = [];
    t.before(() => { srv = new TCPServer({ port: 0 }); srv.start({}); });
    t.after(() => {
        for (const h of handles) { try { h.close(); } catch (e) { } }
        if (srv) srv.close();
    });

    test("a v4-only fixture connects within the window", async () => {
        let v4ok = false, v4err = null;
        handles.push(connectHappy("127.0.0.1", srv.port, { fallbackMs: 250 }, {
            connect: (c, err) => { if (err) v4err = String(err); else { v4ok = true; c.close(); } },
        }));
        await until(() => v4ok || v4err !== null, 6000);
        check(v4ok && v4err === null,
              "v4-only fixture must connect within the window (err=" + v4err + ")");
    });

    test("a v6-mapped fixture connects within the window", async () => {
        let v6ok = false, v6err = null;
        handles.push(connectHappy("::ffff:127.0.0.1", srv.port, { fallbackMs: 250 }, {
            connect: (c, err) => { if (err) v6err = String(err); else { v6ok = true; c.close(); } },
        }));
        await until(() => v6ok || v6err !== null, 6000);
        check(v6ok && v6err === null,
              "v6-only fixture must connect within the window (err=" + v6err + ")");
    });

    test("a closed port is refused, not timed out", async () => {
        let refErr = null, refOk = false;
        handles.push(connectHappy("127.0.0.1", 1, { fallbackMs: 500 }, {
            connect: (c, err) => { if (err) refErr = String(err); else refOk = true; },
        }));
        await until(() => refErr !== null || refOk, 6000);
        check(refErr !== null && !refOk && refErr.indexOf("refused") >= 0,
              "a closed port must be refused, got success=" + refOk + " err=" + refErr);
        check(refErr === null || refErr.indexOf("timed out") < 0,
              "refusal must not be reported as a timeout: " + refErr);
    });

    test("the A+AAAA race has exactly one winner", async () => {
        let raceWins = 0, raceErrs = 0;
        handles.push(connectHappy("localhost", srv.port, { fallbackMs: 250 }, {
            connect: (c, err) => { if (err) raceErrs++; else { raceWins++; c.close(); } },
        }));
        await until(() => raceWins + raceErrs >= 1, 6000);
        await sleep(300);
        check(raceWins === 1 && raceErrs === 0,
              "the A+AAAA race must have exactly one winner (wins=" + raceWins +
              ", errors=" + raceErrs + ")");
    });

    // A host name is looked up off the event loop: connectHappy returns at
    // once and a failed lookup reaches the connect handler, not a throw.
    test("a host-name lookup runs off the caller's thread and reports its DNS error", async () => {
        let dnsSync = null, dnsErr = null, d = null;
        try {
            d = connectHappy(" ", srv.port, { fallbackMs: 250 }, {
                connect: (c, err) => { dnsErr = err ? String(err) : "connected"; },
            });
        } catch (e) { dnsSync = String(e); }
        if (d) handles.push(d);
        check(dnsSync === null, "a host-name lookup must not run on the caller's thread, got a synchronous " + dnsSync);
        await until(() => dnsErr !== null || dnsSync !== null, 5000);
        check(dnsErr !== null && dnsErr.indexOf("DNS resolution failed") >= 0,
              "a non-resolvable host must reach the handler with the DNS error, got " + dnsErr);
    });

    // close() while the lookup is still in flight: the handler never runs.
    test("a handle closed during its lookup never calls its handler", async () => {
        let lateCalled = false;
        const z = connectHappy("localhost", srv.port, { fallbackMs: 250 }, { connect: () => { lateCalled = true; } });
        z.close();
        await sleep(500);
        check(!lateCalled, "a handle closed during its lookup must never call its handler");
    });

    test("25 sequential races all settle cleanly", async () => {
        const errs = await new Promise((r) => churn(25, r));
        check(errs === 0, "25 sequential races must all settle cleanly, errors=" + errs);
    });

    await t.run();
}
