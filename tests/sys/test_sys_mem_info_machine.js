// covers: dyna:sys.memInfo
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_mem_info_machine");
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, platform } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}

const c = cpuInfo();

const m = memInfo();

__kitS.section("total memory is positive", () => {
ok(m.total > 0, "total memory is positive");
});

__kitS.section("free memory does not exceed total", () => {
ok(m.free === undefined || m.free <= m.total, "free memory does not exceed total");
});

__kitS.section("available memory does not exceed total", () => {
ok(m.available === undefined || m.available <= m.total,
   "available memory does not exceed total");
});
__kitS.end();

if (fails) {
    print("test_sys_machine: " + fails + " FAILED of " + n);
    throw new Error("test_sys_machine failed");
}
print("test_sys_machine: " + n + " assertions, 0 failures");
