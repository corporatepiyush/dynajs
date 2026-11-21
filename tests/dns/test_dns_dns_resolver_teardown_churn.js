// flags: --std
// timeout: 300
// covers: dyna:dns.DNSResolver
// Runtime teardown with live DNS queries to a black-holed server (the DNS share of
// the old tests/net/test_net_teardown_churn.js; driver: tests/lib/teardown_churn.js).
import { DNSResolver } from "dyna:dns";
import { suite } from "../lib/kit.js";
import { teardownChurn } from "../lib/teardown_churn.js";

await teardownChurn("test_dns_dns_resolver_teardown_churn", {
  cyclic: () => {
    const D = new DNSResolver({ server: "10.255.255.1", port: 53, timeoutMs: 60000 });
    D.query("never.test", 1, () => { D.close(); });
    return [D];
  },
  plain: () => {
    const D = new DNSResolver({ server: "10.255.255.1", port: 53, timeoutMs: 60000 });
    D.query("never.test", 1, () => {});
  },
}, suite);
