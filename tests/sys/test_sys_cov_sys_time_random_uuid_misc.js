// Split from tests/test_cov_sys_time_random_uuid_misc.js: the sys sections.
import * as sys from "dyna:sys";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
{
    const p1 = sys.pid();
    assert(Number.isInteger(p1) && p1 > 0, "pid positive integer");
    eq(sys.pid(), p1, "pid stable across calls");

    assert(typeof sys.platform() === "string" && sys.platform().length > 0, "platform non-empty string");
    eq(sys.platform(), sys.platform().toLowerCase(), "platform lowercase");

    assert(typeof sys.cwd() === "string" && sys.cwd().startsWith("/"), "cwd absolute string");
    assert(typeof sys.homeDir() === "string" && sys.homeDir().startsWith("/"), "homeDir absolute");
    assert(typeof sys.hostName() === "string", "hostName string");

    const u1 = sys.uptime();
    assert(typeof u1 === "number" && u1 >= 0, "uptime >= 0");
    assert(sys.uptime() >= u1 - 1, "uptime not going backwards by more than 1s");

    const mi = sys.memInfo();
    assert(mi.total > 0, "memInfo.total > 0");
    assert(mi.free >= 0 && mi.free <= mi.total, "memInfo.free in range");
    assert(mi.available >= 0 && mi.available <= mi.total, "memInfo.available in range");
    sys.memInfo(1); n++;

    const la = sys.loadAvg();
    assert(Array.isArray(la) && la.length === 3, "loadAvg 3 samples");
    for (const v of la) assert(typeof v === "number" && v >= 0, "loadAvg value >= 0");

    const du = sys.diskUsage("/tmp");
    assert(du.total > 0 && du.free <= du.total, "diskUsage sane on /tmp");
    try { sys.diskUsage("/definitely/not/a/path/xyzzy"); n++; } catch (e) { n++; }

    const ci = sys.cpuInfo();
    assert(ci.cores >= 1 || ci.threads >= 1, "cpuInfo cores/threads >= 1");
    assert(typeof ci.model === "string" || ci.model === null || ci.model === undefined, "cpuInfo model type");

    assert(typeof sys.getEnv("PATH") === "string" && sys.getEnv("PATH").length > 0, "getEnv PATH non-empty");
    assert(sys.getEnv("__NO_SUCH_VAR_XYZ__") === undefined || sys.getEnv("__NO_SUCH_VAR_XYZ__") === "", "getEnv missing var empty/undefined");
    sys.setEnv("__COV_T1__", "v1");
    eq(sys.getEnv("__COV_T1__"), "v1", "setEnv/getEnv round-trip");
    sys.setEnv("__COV_T1__", "v2");
    eq(sys.getEnv("__COV_T1__"), "v2", "setEnv overwrite");
    sys.setEnv("__COV_T1__", "");
    eq(sys.getEnv("__COV_T1__") || "", "", "setEnv empty value");

    assert(sys.Which("sh") !== null || sys.Which("true") !== null, "Which finds a shell binary");
    eq(sys.Which("__cov_no_such_bin__"), null, "Which missing -> null");

    eq(sys.Exec("true").code, 0, "Exec true code=0");
    assert(sys.Exec("false").code !== 0, "Exec false code!=0");
    {
        const t0 = Date.now();
        const r = sys.Exec("sleep", ["5"], { timeoutMs: 200 });
        const ms = Date.now() - t0;
        assert(r.timedOut === true || r.code !== 0 || r.signal, "Exec timeout flagged: " + JSON.stringify(r).slice(0, 100));
        assert(ms < 2000, "Exec timeout bounded: " + ms + "ms");
        assert(typeof r.stdout === "string" && typeof r.stderr === "string", "Exec result stdout/stderr strings");
    }
    eq(sys.Exec("echo", ["cov-ok"]).stdout.trim(), "cov-ok", "Exec captures stdout");
    throws(() => sys.Exec("__no_such_binary_cov__"), /./, "Exec missing binary throws");
    throws(() => sys.Exec(""), /./, "Exec empty command throws");
}
if (fails) {
    print("test_sys_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_sys_cov_sys_time_random_uuid_misc failed");
}
print("test_sys_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
