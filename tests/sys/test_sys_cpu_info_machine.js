// covers: dyna:sys.cpuInfo
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_cpu_info_machine");
import { cpuInfo, memInfo, loadAvg, uptime, diskUsage, platform } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL " + m); } }
function threw(fn, m) {
    n++;
    try { fn(); fails++; print("FAIL " + m + ": did not throw"); } catch (e) { }
}

const c = cpuInfo();

__kitS.section("cpuInfo returns an object", () => {
ok(typeof c === "object", "cpuInfo returns an object");
});

__kitS.section("features is an array", () => {
ok(Array.isArray(c.features), "features is an array");
});

__kitS.section("every feature is a string", () => {
ok(c.features.every((f) => typeof f === "string"), "every feature is a string");
});

__kitS.section("threads, when reported, is positive", () => {
ok(c.threads === undefined || c.threads > 0, "threads, when reported, is positive");
});

__kitS.section("cores, when reported, is positive", () => {
ok(c.cores === undefined || c.cores > 0, "cores, when reported, is positive");
});

const la = loadAvg();

__kitS.section("each load average is >= 0", () => {
ok(la.every((x) => typeof x === "number" && x >= 0), "each load average is >= 0");
});

const d = diskUsage("/");

__kitS.section("the root filesystem has a size", () => {
ok(d.total > 0, "the root filesystem has a size");
});
__kitS.end();

if (fails) {
    print("test_sys_machine: " + fails + " FAILED of " + n);
    throw new Error("test_sys_machine failed");
}
print("test_sys_machine: " + n + " assertions, 0 failures");
