/* test_connect_resolve.js -- TCPServer.connect takes a NAME or an address.
 *
 * Before this, connect() ran inet_addr() on whatever it was given, so a
 * hostname became 255.255.255.255 and the caller was told "Address family not
 * supported by protocol family" -- an errno naming the wrong cause entirely.
 *
 * The literal-address path must stay resolver-free (AI_NUMERICHOST first), so
 * the control here is a loopback connect: it must still work and must not
 * depend on the network at all.
 */
import { TCPServer } from "dyna:net";
import { getEnv } from "dyna:sys";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}

/* Resolve+connect once, report {err} or {ok:true}. */
function dial(host, port, ms) {
    return new Promise((res) => {
        let settled = false;
        const done = (v) => { if (!settled) { settled = true; res(v); } };
        const t = setTimeout(() => done({ err: "timeout" }), ms || 12000);
        let c;
        try {
            c = TCPServer.connect({ host, port }, {
                connect(conn, err) {
                    clearTimeout(t);
                    done(err ? { err } : { ok: true });
                    c.close();
                },
                close() {},
            });
        } catch (e) {
            clearTimeout(t);
            done({ threw: String(e.message || e) });
        }
    });
}

async function main() {
    /* ---- CONTROL: a literal address on loopback, no network, no resolver ---- */
    {
        const srv = new TCPServer({ port: 0 });
        srv.start({ data: (c, b) => c.write(b) });
        const port = srv.port;
        const r = await dial("127.0.0.1", port, 4000);
        ok(r.ok === true, "CONTROL: a literal IPv4 address still connects on loopback",
           r.err || r.threw);
        srv.close();
    }

    /* ---- a NAME resolves ---- */
    {
        const r = await dial("example.com", 80);
        if (r.err === "timeout") skipped("example.com (no network)");
        else ok(r.ok === true, "a hostname resolves and connects", r.err || r.threw);
    }

    /* ---- IPv6: the old code opened AF_INET unconditionally ----
       There is no IPv6 LISTENER here (TCPServer binds v4 only -- a separate
       gap, recorded not fixed), so the observable is the ERROR: reaching ::1
       and being refused proves an AF_INET6 socket was opened and dialled.
       The old code could not get that far. */
    {
        const r = await dial("::1", 9, 4000);          /* discard port, closed */
        if (r.err === "timeout") skipped("IPv6 loopback unavailable");
        else {
            const msg = String(r.threw || r.err || "");
            ok(/refused|unreachable/i.test(msg),
               "an IPv6 literal is DIALLED, not rejected as an unsupported " +
               "family (got: " + msg.slice(0, 50) + ")");
        }
    }

    /* ---- a name that cannot resolve must fail SAYING SO ---- */
    {
        const r = await dial("no-such-host.invalid", 80, 8000);
        if (r.err === "timeout") skipped("negative lookup (resolver hung)");
        else {
            ok(!!(r.err || r.threw),
               "an unresolvable name fails rather than dialling something");
            const msg = String(r.threw || r.err);
            ok(!/Address family not supported/i.test(msg),
               "and the error does NOT blame the address family (got: " +
               msg.slice(0, 60) + ")");
        }
    }

    print("test_connect_resolve: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_connect_resolve: " + fail + " failures");
}

main();
