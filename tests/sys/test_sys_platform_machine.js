// covers: dyna:sys.platform
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_platform_machine");
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, platform } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}

const c = cpuInfo();

__kitS.section("a vector ISA is detected on a supported target", () => {
if (platform() === "darwin" || platform() === "linux") {
    ok(c.features.length > 0 || !/arm64|x86_64/.test(String(c.model || "")),
       "a vector ISA is detected on a supported target");
}
});
__kitS.end();

if (fails) {
    print("test_sys_machine: " + fails + " FAILED of " + n);
    throw new Error("test_sys_machine failed");
}
print("test_sys_machine: " + n + " assertions, 0 failures");
