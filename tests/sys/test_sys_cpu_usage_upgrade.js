// covers: dyna:sys.cpuUsage
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_cpu_usage_upgrade");
import {
    platform, arch, uname, cpuUsage, rusage,
    getuid, getgid, setUid, setGid, hostName, memoryUsage,
} from "dyna:sys";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertNum(v, msg) {
    assert(typeof v === "number", msg + " (is number, got " + typeof v + ")");
    assert(Number.isFinite(v), msg + " (finite)");
}
function assertThrows(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught !== null, msg + " (expected throw)");
    assert(caught instanceof Error, msg + " (Error, got " + caught + ")");
    return caught;
}

__kitS.section("cpuUsage returns an object", () => {
    const c1 = cpuUsage();
    assert(c1 !== null && typeof c1 === "object", "cpuUsage returns an object");
    assertNum(c1.user, "cpuUsage().user");
    assertNum(c1.system, "cpuUsage().system");
    assert(c1.user >= 0, "cpuUsage().user >= 0");
    assert(c1.system >= 0, "cpuUsage().system >= 0");

    let spin = 0;
    for (let i = 0; i < 300000; i++) spin += i % 7;
    if (spin === -1) throw new Error("unreachable");
    const c2 = cpuUsage();
    assert(c2.user >= c1.user, "cpuUsage().user monotonic non-decreasing");
    assert(c2.system >= c1.system, "cpuUsage().system monotonic non-decreasing");
});
__kitS.end();

print("test_sys_upgrade: all tests passed (" + n + " assertions)");
