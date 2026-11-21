// covers: dyna:sys.platform
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_platform_upgrade");
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

__kitS.section("platform() is a string", () => {
    const KNOWN = ["darwin", "linux", "freebsd", "openbsd", "netbsd",
        "dragonfly", "sunos", "unknown"];
    const p = platform();
    assert(typeof p === "string", "platform() is a string");
    assert(KNOWN.indexOf(p) >= 0,
        "platform() is a known name, got " + p);
    assert(p !== "unknown", "platform() identifies this platform: " + p);
    const un = uname();
    assert(p === un.sysname.toLowerCase(),
        "platform() is the lowercased uname sysname");

    const a = arch();
    assert(typeof a === "string" && a.length > 0, "arch() is a non-empty string");
    const ARCH_ALIASES = { aarch64: "arm64", amd64: "x86_64" };
    const expected = ARCH_ALIASES[un.machine.toLowerCase()] ||
        un.machine.toLowerCase();
    assert(a === expected,
        "arch() is the normalised uname machine (" + a + " vs " + expected + ")");

    assert(arch() === a, "arch() stable");
});
__kitS.end();

print("test_sys_upgrade: all tests passed (" + n + " assertions)");
