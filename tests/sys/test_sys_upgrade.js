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

{
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
}

{
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
}

{
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
}

{
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
}

{
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
}

print("test_sys_upgrade: all tests passed (" + n + " assertions)");
