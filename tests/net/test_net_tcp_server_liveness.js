// covers: dyna:net.TCPServer
// A refused connect reports its error to the connect handler (the tcp case of the
// old liveness suites, split out of tests/redis and tests/postgres in B1e).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_tcp_server_liveness");
import { TCPServer } from "dyna:net";

let n = 0, fails = 0, seen = false;
const ok = (c, m) => { n++; if (c) print("  ok  " + m); else { fails++; print("FAIL: " + m); } };

await __kitS.section("tcp: refused connect reports the error", async () => {
const tc = TCPServer.connect({ host: "127.0.0.1", port: 1, connectTimeoutMs: 500 }, {
  connect: (conn, err) => { seen = true; ok(!!err, "tcp: refused connect reports the error"); },
});
for (let i = 0; i < 100 && !seen; i++) await new Promise((r) => setTimeout(r, 10));
ok(seen, "tcp: the connect handler ran for the refused connect");
});
__kitS.end();

print("test_net_tcp_server_liveness: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_net_tcp_server_liveness: " + fails + " failures");
