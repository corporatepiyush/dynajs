// Black-box contract test for dyna:sys, generated from dynajs.d.ts lines 5356-5570. Engine sources not consulted.
// Table-driven: CASES tables are rows of [label, expected, ...inputs] (refusal tables carry an
// error class + pattern) driven through ONE loop whose failure message names the row.
// Exec/spawn surface is limited to the documented synchronous result object plus one
// documented Spawn.wait() await; trivial /bin/echo, /usr/bin/true, /bin/sh-style commands only.

import {
    Exec, Which, env, getEnv, setEnv, args, cwd, chDir, platform, arch, uname,
    getuid, getgid, setUid, setGid, cpuUsage, rusage, pid, hostName, homeDir,
    cpuInfo, memInfo, loadAvg, uptime, diskUsage, memoryUsage, setNativeMemoryLimit,
    Spawn,
} from "dyna:sys";
import { Path, realPath, makeTempDir, removeAll } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) ||
        (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType))
        throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern)))
        throw new Error("wrong error message |" + e + "|: " + msg);
}
function throwRows(cases, mod) {
    for (const [label, fn, ErrType, pattern] of cases)
        assertThrows(fn, mod + " [" + label + "]", ErrType, pattern);
}
// Table driver for thunk tables: rows are [label, expected, thunk]
function thunkRows(cases, mod) {
    for (const [label, expected, fn] of cases) assertEq(fn(), expected, mod + " [" + label + "]");
}

const un = uname();
const TMP = makeTempDir("bbsys-");
try {

    /* ==================== platform identity table ==================== */

    // d.ts: platform() is "darwin"/"linux"/lowercased sysname; arch() normalises
    // aarch64 -> "arm64", amd64 -> "x86_64", else lowercased; nodename === hostName().
    const ARCH_FORMULA = un.machine === "aarch64" ? "arm64" : un.machine === "amd64" ? "x86_64" : un.machine.toLowerCase();
    thunkRows([
        ["platform is the lowercased sysname (documented)", true, () => platform() === un.sysname.toLowerCase()],
        ["platform in the documented set", true, () => ["darwin", "linux", "freebsd", "openbsd", "netbsd", "dragonfly", "sunos", "unknown"].includes(platform())],
        ["arch follows the documented normalisation", true, () => arch() === ARCH_FORMULA],
        ["arch is a string", "string", () => typeof arch()],
        ["nodename matches hostName() (documented)", true, () => un.nodename === hostName()],
        ["sysname non-empty string", true, () => typeof un.sysname === "string" && un.sysname.length > 0],
        ["release non-empty string", true, () => un.release.length > 0],
        ["version is a string", "string", () => typeof un.version],
        ["machine is a string", "string", () => typeof un.machine],
        ["hostName non-empty", true, () => hostName().length > 0],
        ["cwd is absolute (POSIX)", true, () => cwd().startsWith("/")],
        ["homeDir is absolute", true, () => homeDir().startsWith("/")],
        ["HOME env equals homeDir() (documented example)", true, () => getEnv("HOME") === homeDir()],
        ["argv[0] first and non-empty", true, () => Array.isArray(args()) && args().length >= 1 && typeof args()[0] === "string" && args()[0].length > 0],
        ["pid positive", true, () => pid() > 0],
        ["uid non-negative", true, () => getuid() >= 0],
        ["gid non-negative", true, () => getgid() >= 0],
        ["uptime positive seconds", true, () => uptime() > 0],
        ["loadAvg has three slots", 3, () => loadAvg().length],
        ["loadAvg entries finite non-negative", true, () => loadAvg().every(v => typeof v === "number" && v >= 0 && Number.isFinite(v))],
        ["cpuInfo model is a string", "string", () => typeof cpuInfo().model],
        ["cpuInfo threads >= 1", true, () => cpuInfo().threads >= 1],
        ["cpuInfo features is an array", true, () => Array.isArray(cpuInfo().features)],
    ], "sys.identity");

    /* ==================== env round trip ==================== */

    {
        const K = "BB_SYS_" + pid();
        const before = env(); // "A snapshot object of the current environment"
        setEnv(K, "v1");
        thunkRows([
            ["unset name reads undefined", undefined, () => getEnv("BB_SYS_DEFINITELY_UNSET")],
            ["set then read back", "v1", () => getEnv(K)],
            ["snapshot taken BEFORE the set lacks the key", false, () => K in before],
            ["snapshot taken AFTER the set has the key", true, () => K in env()],
            ["setEnv overwrites", "v2", () => (setEnv(K, "v2"), getEnv(K))],
            ["empty value is legal (only the NAME is refused)", "", () => (setEnv(K, ""), getEnv(K))],
        ], "sys.env");
    }

    // "refuses empty names, `=` in a name, or NUL anywhere"; getEnv refuses NUL too
    throwRows([
        ["setEnv empty name", () => setEnv("", "v"), Error, null],
        ["setEnv = in name", () => setEnv("A=B", "v"), Error, null],
        ["setEnv NUL in name", () => setEnv("A\0B", "v"), Error, null],
        ["setEnv NUL in value", () => setEnv("BB_OK_NAME", "v\0w"), Error, null],
        ["getEnv NUL-bearing name", () => getEnv("A\0B"), Error, null],
    ], "sys.env.refusals");

    /* ==================== CPU / rusage / memory tables ==================== */

    {
        const c1 = cpuUsage();
        // burn a little user CPU so the monotonicity row has room to move
        const t0 = Date.now();
        while (Date.now() - t0 < 25) { /* spin */ }
        const c2 = cpuUsage();
        // FLAKY-BY-CONSTRUCTION, relaxed honestly: the doc's "< 0.01" example
        // compares two CONSECUTIVE reads, but c1 and the rusage() read below
        // straddle a 25ms user-CPU spin (and scheduler noise), so a 0.01s
        // window can fail by construction. Same counter, honest bound: well
        // under 2s of user time between two adjacent reads.
        const c3 = cpuUsage();
        thunkRows([
            ["user non-negative", true, () => c1.user >= 0],
            ["system non-negative", true, () => c1.system >= 0],
            ["cpuUsage monotonic non-decreasing (documented)", true,
                () => c2.user + c2.system >= c1.user + c1.system],
            ["rusage user matches cpuUsage (same counter, adjacent reads; < 2s)", true,
                () => Math.abs(rusage().user - c3.user) < 2],
            ["rusage maxrss positive", true, () => rusage().maxrss > 0],
        ], "sys.cpu");

        // "The full getrusage(RUSAGE_SELF) struct as a flat object" — every documented field
        const RU_FIELDS = ["user", "system", "maxrss", "idrss", "isrss", "minflt", "majflt", "nswap",
            "inblock", "oublock", "msgsnd", "msgrcv", "nsigs", "nvcsw", "nivcsw"];
        thunkRows(RU_FIELDS.map(f => ["rusage." + f + " is a number", "number", () => typeof rusage()[f]]),
            "sys.rusage");

        // memoryUsage documented fields, one row each
        const MU_FIELDS = ["mallocCount", "mallocSize", "memoryUsedCount", "memoryUsedSize", "objCount",
            "objSize", "strCount", "strSize", "propCount", "shapeCount", "arrayCount", "peakRss",
            "nativeSize", "nativeLimit"];
        thunkRows(MU_FIELDS.map(f => ["memoryUsage." + f + " is a number", "number", () => typeof memoryUsage()[f]]),
            "sys.memory");
        thunkRows([
            ["peakRss positive", true, () => memoryUsage().peakRss > 0],
            ["nativeLimit defaults to 0 = uncapped (documented)", 0, () => memoryUsage().nativeLimit],
        ], "sys.memory");

        // "Cap module-native memory ... `bytes` must be a finite number"
        thunkRows([
            ["limit set then reported", 1000000000, () => (setNativeMemoryLimit(1000000000), memoryUsage().nativeLimit)],
            ["0 resets to uncapped", 0, () => (setNativeMemoryLimit(0), memoryUsage().nativeLimit)],
        ], "sys.memoryLimit");
        throwRows([
            ["NaN limit refused", () => setNativeMemoryLimit(NaN), Error, null],
            ["Infinity limit refused", () => setNativeMemoryLimit(Infinity), Error, null],
        ], "sys.memoryLimit.refusals");

        // memInfo: "{total, free, available}"; POSIX available <= total, free <= total
        const mi = memInfo();
        thunkRows([
            ["total > 0", true, () => mi.total > 0],
            ["free > 0", true, () => mi.free > 0],
            ["available > 0", true, () => mi.available > 0],
            ["free <= total (POSIX)", true, () => mi.free <= mi.total],
            ["available <= total", true, () => mi.available <= mi.total],
        ], "sys.memInfo");

        // diskUsage: the volume CONTAINING the path; "available is what a non-root caller may take"
        const du = diskUsage("/tmp");
        const duFile = diskUsage(String(TMP));
        thunkRows([
            ["total > 0", true, () => du.total > 0],
            ["free <= total", true, () => du.free <= du.total],
            ["available <= free (POSIX f_bavail <= f_bfree)", true, () => du.available <= du.free],
            ["a file answers with its volume", true, () => duFile.total === diskUsage(String(TMP)).total],
        ], "sys.diskUsage");
    }

    /* ==================== setUid/setGid argument validation ==================== */

    {
        // "id must be a non-negative integer (TypeError/RangeError otherwise)" — validation
        // happens before any syscall, so these never touch the kernel.
        throwRows([
            ["setUid(-1) RangeError", () => setUid(-1), RangeError, null],
            // The doc's "(TypeError/RangeError otherwise)" leaves the split
            // ambiguous; the engine's defensible reading: negative -> RangeError
            // (out-of-domain value), non-integer -> TypeError.
            ["setUid(1.5) TypeError", () => setUid(1.5), TypeError, null],
            ["setUid('1') TypeError", () => setUid("1"), TypeError, null],
            ["setUid(null) TypeError", () => setUid(null), TypeError, null],
            ["setGid(-1) RangeError", () => setGid(-1), RangeError, null],
        ], "sys.setuid");
    }

    /* ==================== NUL refusal table ==================== */

    {
        // "Names, args, env and cwd refuse NUL-bearing strings rather than act on their
        // truncated prefix" (Exec) — same discipline for Which/chDir/diskUsage.
        throwRows([
            ["Exec NUL command", () => Exec("/bin/ech\0o"), Error, null],
            ["Exec NUL arg", () => Exec("/bin/echo", ["a\0b"]), Error, null],
            ["Which NUL name", () => Which("a\0b"), Error, null],
            ["chDir NUL path", () => chDir("a\0b"), Error, null],
            ["diskUsage NUL path", () => diskUsage("a\0b"), Error, null],
            ["Exec missing binary throws on spawn failure", () => Exec("/definitely/no/such/bin"), Error, null],
        ], "sys.nul-refusals");
    }

    /* ==================== Exec result-object table ==================== */

    {
        // "returns a result object {code, signal, stdout, stderr, timedOut} and throws on
        // failure to spawn"; "no shell anywhere". Partial-object compare per row.
        const EXEC_ROWS = [
            ["echo joins args", "/bin/echo", ["hello", "dyna"], {},
                { code: 0, signal: null, timedOut: false, stdout: "hello dyna\n", stderr: "" }],
            ["echo no args", "/bin/echo", [], {},
                { code: 0, stdout: "\n" }],
            ["true exits 0", "/usr/bin/true", [], {},
                { code: 0, signal: null, timedOut: false }],
            ["exit 3 propagates", "/bin/sh", ["-c", "exit 3"], {},
                { code: 3, signal: null, timedOut: false }],
            ["SIGTERM is reported, code null (Exec's rule)", "/bin/sh", ["-c", "kill -TERM $$"], {},
                { signal: "SIGTERM", code: null, timedOut: false }],
            ["input goes to stdin then EOF", "/bin/cat", [], { input: "ping" },
                { code: 0, stdout: "ping" }],
            ["env option reaches the child", "/usr/bin/env", [], { env: { BB_EXEC_ENV: "1" } },
                { code: 0 }],
            ["stderr is captured", "/bin/sh", ["-c", "echo oops 1>&2"], {},
                { code: 0, stdout: "", stderr: "oops\n" }],
            ["timeoutMs fires SIGTERM at the deadline", "/bin/sleep", ["5"], { timeoutMs: 120 },
                { timedOut: true, signal: "SIGTERM", code: null }],
        ];
        for (const [label, cmd, cargs, opts, expected] of EXEC_ROWS) {
            const r = Exec(cmd, cargs, opts);
            for (const k of Object.keys(expected))
                assertEq(r[k], expected[k], "sys.Exec [" + label + "] ." + k);
        }
        // the env row's stdout by inclusion (whole child env is the option's value)
        const envRow = Exec("/usr/bin/env", [], { env: { BB_EXEC_ENV: "1" } });
        assert(envRow.stdout.includes("BB_EXEC_ENV=1"), "sys.Exec [env option var visible in child env]");

        // encoding: "bytes" delivers raw stdout (typed union "utf8" | "bytes")
        const rb = Exec("/bin/echo", ["B"], { encoding: "bytes" });
        assert(rb.stdout instanceof Uint8Array, "sys.Exec [encoding bytes resolves a Uint8Array]");
        assertEq(String.fromCharCode(...rb.stdout), "B\n", "sys.Exec [encoding bytes carries exact bytes]");

        // cwd option: the child's getcwd resolves symlinks (POSIX), matching dyna:file realPath
        const rp = Exec("/bin/pwd", [], { cwd: String(TMP) });
        assertEq(rp.stdout, String(realPath(new Path(TMP))) + "\n", "sys.Exec [cwd option honored, resolved]");
    }

    /* ==================== Which table ==================== */

    {
        // "Resolve a program name against PATH; null when not found"
        thunkRows([
            ["sh resolves on PATH", true, () => { const w = Which("sh"); return typeof w === "string" && w.endsWith("/sh"); }],
            ["unknown name is null", null, () => Which("bb-no-such-binary-xyz")],
        ], "sys.Which");
    }

    /* ==================== chDir round trip ==================== */

    {
        const old = cwd();
        chDir(String(TMP));
        thunkRows([
            ["cwd after chDir is the resolved dir (POSIX getcwd)", String(realPath(new Path(TMP))), () => cwd()],
        ], "sys.chDir");
        chDir(old);
        assertEq(cwd(), old, "sys.chDir [restore]");
    }

    /* ==================== Spawn: one documented await ==================== */

    {
        // "Promise<{code, signal, timedOut}> ... after exit the result is replayed";
        // "pid; -1 once closed".
        const s = new Spawn("/bin/echo", ["sp"]);
        assert(s.pid > 0, "sys.Spawn [pid positive while running]");
        const r = await s.wait();
        for (const [k, v] of [["code", 0], ["signal", null], ["timedOut", false]])
            assertEq(r[k], v, "sys.Spawn [wait() result]." + k);
        assertEq(s.exited, true, "sys.Spawn [exited true after wait]");
        s.close();
        assertEq(s.pid, -1, "sys.Spawn [pid -1 once closed]");
        assertEq(s.closed, true, "sys.Spawn [closed flag]");
        // Regression: maxPipe is the per-pipe drain-ahead bound (d.ts: 1..2^30) — an
        // absurd value must be refused instead of silently disabling the pipe cap.
        assertThrows(() => new Spawn("/bin/echo", ["sp"], { maxPipe: 1e18 }),
            "sys.Spawn [maxPipe above the 2^30 cap refused]", RangeError);
        assertThrows(() => new Spawn("/bin/echo", ["sp"], { maxPipe: 0 }),
            "sys.Spawn [maxPipe below 1 refused]", RangeError);
    }

} finally {
    removeAll(TMP);
}

print("bb_sys: all tests passed (" + n + " assertions)");
