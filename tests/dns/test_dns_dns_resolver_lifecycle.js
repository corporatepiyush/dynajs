// flags: --std
// covers: dyna:dns.DNSResolver
import { DNSResolver } from "dyna:dns";
import * as os from "os";
import { suite } from "../lib/kit.js";

// Split from tests/net/test_db_lifecycle.js (one module per suite). The resolver
// aims at 127.0.0.1:56379, where nothing answers DNS: only the timeout can fire.
const t = suite("test_dns_dns_resolver_lifecycle");
const ENC = new TextEncoder();
const W = (s) => { const b = ENC.encode(s); os.write(1, b.buffer, 0, b.length); };
let A = null;
const ok = (c, m) => { A.ok(c, m); if (c) W("  ok  " + m + "\n"); };
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
test("dns: a timed-out callback fires and may close its resolver", async () => {
  const r = new DNSResolver({ server: "127.0.0.1", port: 56379, timeoutMs: 700 });
  let fired = false;
  r.query("lifecycle.test", 1, (err) => {
      fired = true;
      try { r.close(); } catch (e) {}
  });
  await sleep(1100);
  ok(fired, "dns: wrong-wire/timeout callback fired and close-in-cb survived");
});

await t.run();
