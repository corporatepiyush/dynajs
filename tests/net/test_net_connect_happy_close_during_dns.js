// covers: dyna:net.connectHappy
// F38: closing a connectHappy handle while its name lookup ran on the pool orphaned the lookup job; the closed handle
// dropped the last reactor reference, so the loop ended and freed the reactor with the finished job still queued:
// the job, its eb_dns_t and the host copy leaked (LSan: dyn_aio_offload / dyn_tcp_connect_happy, found by
// tests/api/test_api_surface.js). The orphaned lookup now holds a reactor reference until its completion frees it.
// Run under ASan (detect_leaks=1). Mutation: not taking that reference leaks 3 allocations per orphan: red under ASan.
import { suite } from "../lib/kit.js";
import { connectHappy } from "dyna:net";

const t = suite("net.connect_happy.close_during_dns");

t.test("close right after connectHappy on a name lookup: the process ends cleanly", (a) => {
    for (let i = 0; i < 4; i++) {
        const h = connectHappy(`leak-check-${i}.invalid`, 9, {}, { connect() {}, close() {} });
        h.close();
    }
    a.ok(true, "four handles closed while their lookups ran");
});

await t.run();
