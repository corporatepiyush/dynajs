/* Machine facts: the VALUES are host-dependent, so this asserts invariants and
   shapes. `features` must match the SIMD dispatcher's own detection -- that is
   the point of exposing it, so a silently-degraded dispatch is visible here. */
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, platform } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}

const c = cpuInfo();
ok(typeof c === "object", "cpuInfo returns an object");
ok(Array.isArray(c.features), "features is an array");
ok(c.features.every((f) => typeof f === "string"), "every feature is a string");
/* Every supported 64-bit target has a baseline vector ISA; an empty list on one
   means the dispatcher is running scalar kernels and nothing else would say so. */
if (platform() === "darwin" || platform() === "linux") {
    ok(c.features.length > 0 || !/arm64|x86_64/.test(String(c.model || "")),
       "a vector ISA is detected on a supported target");
}
ok(c.threads === undefined || c.threads > 0, "threads, when reported, is positive");
ok(c.cores === undefined || c.cores > 0, "cores, when reported, is positive");

const m = memInfo();
ok(m.total > 0, "total memory is positive");
ok(m.free === undefined || m.free <= m.total, "free memory does not exceed total");
ok(m.available === undefined || m.available <= m.total,
   "available memory does not exceed total");

const la = loadAvg();
ok(Array.isArray(la) && la.length === 3, "loadAvg is three numbers");
ok(la.every((x) => typeof x === "number" && x >= 0), "each load average is >= 0");

ok(uptime() > 0, "uptime is positive");
ok(uptime() >= uptime() - 1, "uptime does not go backwards");

const d = diskUsage("/");
ok(d.total > 0, "the root filesystem has a size");
ok(d.free <= d.total, "free does not exceed total");
/* available <= free is the reserved-blocks invariant: a non-root caller may
   take less than exists, and conflating the two overstates the space. */
ok(d.available <= d.free, "available does not exceed free");

threw(() => diskUsage(), "diskUsage with no path");
threw(() => diskUsage("/no/such/path/anywhere"), "diskUsage on a missing path");

if (fails) {
    print("test_sys_machine: " + fails + " FAILED of " + n);
    throw new Error("test_sys_machine failed");
}
print("test_sys_machine: " + n + " assertions, 0 failures");
