// covers: dyna:sys.loadAvg
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_load_avg_machine");
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, platform } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}

const c = cpuInfo();

const la = loadAvg();

__kitS.section("loadAvg is three numbers", () => {
ok(Array.isArray(la) && la.length === 3, "loadAvg is three numbers");
});
__kitS.end();

if (fails) {
    print("test_sys_machine: " + fails + " FAILED of " + n);
    throw new Error("test_sys_machine failed");
}
print("test_sys_machine: " + n + " assertions, 0 failures");
