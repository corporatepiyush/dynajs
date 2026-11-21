// flags: --std
// covers: dyna:net.TCPServer
import { TCPServer } from "dyna:net";
import * as os from "os";
import { suite } from "../lib/kit.js";

// Split from tests/net/test_db_lifecycle.js (one module per suite); the case opens
// its own listener, so nothing starts under --list.
const t = suite("test_net_tcp_server_lifecycle");
const ENC = new TextEncoder();
const W = (s) => { const b = ENC.encode(s); os.write(1, b.buffer, 0, b.length); };
let A = null;
const ok = (c, m) => { A.ok(c, m); if (c) W("  ok  " + m + "\n"); };
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
test("tcp: server.close() inside a data handler is survived", async () => {
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
});

await t.run();
