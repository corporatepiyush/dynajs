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
    {
        const srv = new TCPServer({ port: 0 });
        srv.start({ data: (c, b) => c.write(b) });
        const port = srv.port;
        const r = await dial("127.0.0.1", port, 4000);
        ok(r.ok === true, "CONTROL: a literal IPv4 address still connects on loopback",
           r.err || r.threw);
        srv.close();
    }

    {
        const r = await dial("example.com", 80);
        if (r.err === "timeout") skipped("example.com (no network)");
        else ok(r.ok === true, "a hostname resolves and connects", r.err || r.threw);
    }

    {
        const r = await dial("::1", 9, 4000);
        if (r.err === "timeout") skipped("IPv6 loopback unavailable");
        else {
            const msg = String(r.threw || r.err || "");
            ok(/refused|unreachable/i.test(msg),
               "an IPv6 literal is DIALLED, not rejected as an unsupported " +
               "family (got: " + msg.slice(0, 50) + ")");
        }
    }

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
