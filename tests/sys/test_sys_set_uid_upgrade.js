// covers: dyna:sys.setUid
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_set_uid_upgrade");
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

__kitS.section("getuid() >= 0", () => {
    const u = getuid();
    const g = getgid();
    assertNum(u, "getuid()");
    assertNum(g, "getgid()");
    assert(u >= 0, "getuid() >= 0");
    assert(g >= 0, "getgid() >= 0");
    assert(getuid() === u, "getuid() stable");

    const te = assertThrows(() => setUid("0"), "setUid(string) is a TypeError");
    assert(te instanceof TypeError, "setUid(string) throws TypeError");
    const rg = assertThrows(() => setGid(-1), "setGid(-1) is a RangeError");
    assert(rg instanceof RangeError, "setGid(-1) throws RangeError");
    assertThrows(() => setGid("0"), "setGid(string) is a TypeError");
    const fr = assertThrows(() => setUid(1.5),
        "setUid(1.5) is a TypeError (a truncated id would setuid(1))");
    assert(fr instanceof TypeError, "setUid(fractional) throws TypeError");
    assertThrows(() => setGid(2.5), "setGid(fractional) is a TypeError");
    assertThrows(() => setUid(NaN), "setUid(NaN) is a TypeError");

    if (u !== 0) {
        const e = assertThrows(() => setUid(u + 1),
            "unprivileged setUid throws");
        assert(e.code === "EPERM", "setUid refusal has code EPERM, got " +
            e.code);
        assert(e.errno === 1, "setUid refusal has errno EPERM");
        const e2 = assertThrows(() => setGid(g + 1),
            "unprivileged setGid throws");
        assert(e2.code === "EPERM", "setGid refusal has code EPERM");
    } else {
        setUid(0);
        setGid(0);
    }
});
__kitS.end();

print("test_sys_upgrade: all tests passed (" + n + " assertions)");
