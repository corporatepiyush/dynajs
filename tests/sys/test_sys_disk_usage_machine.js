// covers: dyna:sys.diskUsage
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_disk_usage_machine");
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, platform } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}

const c = cpuInfo();

const d = diskUsage("/");

ok(d.free <= d.total, "free does not exceed total");

ok(d.available <= d.free, "available does not exceed free");

threw(() => diskUsage(), "diskUsage with no path");
threw(() => diskUsage("/no/such/path/anywhere"), "diskUsage on a missing path");
threw(() => diskUsage("/\u0000tmp"), "diskUsage refuses a NUL-bearing path");

__kitS.section("a directory reports its volume's totals", () => {
    const f = diskUsage("/");
    ok(diskUsage("/etc").total === f.total,
       "a directory reports its volume's totals");
    // not /etc/hosts: a container runtime may bind-mount that from another volume
    ok(diskUsage("/etc/passwd").total === f.total,
       "a file reports its volume's totals, not its own size");
});
__kitS.end();

if (fails) {
    print("test_sys_machine: " + fails + " FAILED of " + n);
    throw new Error("test_sys_machine failed");
}
print("test_sys_machine: " + n + " assertions, 0 failures");
