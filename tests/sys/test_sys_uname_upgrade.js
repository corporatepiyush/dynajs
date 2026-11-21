// covers: dyna:sys.uname
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_uname_upgrade");
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

__kitS.section("uname returns an object", () => {
    const un = uname();
    assert(un !== null && typeof un === "object", "uname returns an object");
    for (const f of ["sysname", "nodename", "release", "version", "machine"]) {
        assert(typeof un[f] === "string",
            "uname()." + f + " is a string, got " + typeof un[f]);
        assert(un[f].length > 0, "uname()." + f + " is non-empty");
    }
    assert(un.nodename === hostName(),
        "uname().nodename agrees with hostName()");
    assert(typeof un.sysname === "string" && un.sysname.length < 64,
        "uname().sysname bounded");
});
__kitS.end();

print("test_sys_upgrade: all tests passed (" + n + " assertions)");
