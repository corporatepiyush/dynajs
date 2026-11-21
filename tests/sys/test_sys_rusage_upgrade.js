// covers: dyna:sys.rusage
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_rusage_upgrade");
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

__kitS.section("rusage returns an object", () => {
    const FIELDS = ["user", "system", "maxrss", "idrss", "isrss", "minflt",
        "majflt", "nswap", "inblock", "oublock", "msgsnd", "msgrcv", "nsigs",
        "nvcsw", "nivcsw"];
    const r = rusage();
    assert(r !== null && typeof r === "object", "rusage returns an object");
    for (const f of FIELDS)
        assertNum(r[f], "rusage()." + f);
    assert(r.maxrss > 0, "rusage().maxrss > 0 (bytes)");
    const m = memoryUsage();
    assert(m.peakRss > 0 && r.maxrss >= m.peakRss - 1048576,
        "rusage().maxrss agrees with memoryUsage().peakRss within 1MB " +
        "(maxrss=" + r.maxrss + " peakRss=" + m.peakRss + ")");
    const c = cpuUsage();
    assert(Math.abs(r.user - c.user) < 0.01,
        "rusage().user === cpuUsage().user within 10ms");
    assert(Math.abs(r.system - c.system) < 0.01,
        "rusage().system === cpuUsage().system within 10ms");
});
__kitS.end();

print("test_sys_upgrade: all tests passed (" + n + " assertions)");
