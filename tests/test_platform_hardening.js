// timeout: 120
// flags: --std
import * as std from "std";
import * as os from "os";
import { Table, Command } from "dyna:cli";
import { Path, stat } from "dyna:file";
import { chDir } from "dyna:sys";
import { stats, cumsum, cumprod, diff, abs, bitLen, popcount, isPrime, nchoosek, besselk, besselkScaled } from "dyna:mathx";
import { BLAKE3Hex } from "dyna:hash";
import { Semaphore } from "dyna:async";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throwsRange(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof RangeError; }
    assert(t, msg);
}
function throwsType(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof TypeError; }
    assert(t, msg);
}

{
    assert(std.sprintf("%5d", 42) === "   42", "printf width still pads");
    assert(std.sprintf("%.3f", 1.5) === "1.500", "printf precision still rounds");
    assert(std.sprintf("%5c", 65) === "    A", "printf %c honours width (COMPAT-167)");
    assert(std.sprintf("%c", 65) === "A", "printf %c without width unchanged");
    throwsRange(() => std.sprintf("%2000000000d", 1), "printf width above 1e6 is refused");
    throwsRange(() => std.sprintf("%.2000000000f", 1), "printf precision above 1e6 is refused");
    throwsRange(() => std.sprintf("%*d", 2000000000, 1), "printf star width above 1e6 is refused");
    throwsRange(() => std.sprintf("%.*f", -2000000000, 1), "printf star precision magnitude above 1e6 is refused");
    assert(std.sprintf("%*d", 6, 42) === "    42", "printf star width still works");
}

{
    const huge = new Array(4294967295);
    const t0 = Date.now();
    throwsRange(() => stats.sum(huge), "stats.sum on a 2^32-1 array-like throws RangeError");
    const dt = Date.now() - t0;
    assert(dt < 2000, "stats.sum refuses before iterating (" + dt + " ms)");
    assert(stats.sum([1, 2, 3]) === 6, "stats.sum still sums small arrays");
    assert(stats.mean(new Float64Array([1, 2, 3])) === 2, "stats.mean typed fast path intact");
    throwsRange(() => cumsum(huge), "cumsum refuses an oversized array-like too");
    throwsRange(() => cumprod(huge), "cumprod refuses an oversized array-like too");
    throwsRange(() => diff(huge), "diff refuses an oversized array-like too");
}

{
    const t0 = Date.now();
    assert(besselk(1e9, 0.1) === Infinity, "huge-order besselk answers +Infinity");
    assert(besselkScaled(3e9, 0.1) === Infinity, "huge-order besselkScaled answers +Infinity");
    assert(nchoosek(1e18, 5e17) === Infinity, "nchoosek overflows promptly");
    assert(Date.now() - t0 < 2000, "DoS probes stay fast");
}

{
    const M = (1n << 64n) - 1n;
    assert(abs(-M) === M, "abs accepts the full negative uint64 magnitude");
    assert(bitLen(-M) === 64, "bitLen of -2^64+1 magnitude is 64");
    assert(popcount(-M) === 64, "popcount of -2^64+1 magnitude is 64");
    assert(abs(-5n) === 5n, "abs(-5n) is 5n");
    assert(isPrime(-5n) === false, "isPrime(-5n) is false, not 2^64-5");
    assert(isPrime(7n) === true, "isPrime(7n) still true");
    assert(abs(3n) === 3n && bitLen(8n) === 4 && popcount(7n) === 3, "small BigInt cases intact");
    let over = false;
    try { abs(-(M + 1n)); } catch (e) { over = e instanceof RangeError; }
    assert(over, "an over-2^64 magnitude is still refused");
}

{
    const a = BLAKE3Hex("data", { context: "a\u0000b" });
    const b = BLAKE3Hex("data", { context: "a" });
    assert(a !== b, "a NUL-bearing context separates domains (COMPAT-180)");
}

{
    const sem = new Semaphore(1);
    const rel1 = await sem.acquire();
    let over = false;
    rel1();
    rel1();
    assert(sem.available === 1, "a double release does not over-release");
    const rel2 = await sem.acquire();
    assert(sem.available === 0, "one acquire consumes the single slot");
    rel2();
    assert(sem.available === 1, "the release restores the slot once");
    assert(!over, "no anomaly");
}

{
    const E = "a\u001b[2Jb";
    let leaked = 0;
    const check = (fn, what) => {
        let m = "";
        try { fn(); } catch (e) { m = String(e.message || e); }
        assert(m.length > 0, what + " throws");
        if (m.indexOf("\u001b") >= 0) leaked++;
        assert(m.indexOf("\\x1b") >= 0, what + " escapes the control byte");
    };
    check(() => Table([["x"]], { format: E }), "Table format");
    check(() => Table([["x"]], { align: [E] }), "Table align");
    check(() => { const c = new Command("t"); c.option("--" + E); c.option("--" + E); },
        "Command duplicate option");
    check(() => stat(new Path("/definitely-missing-" + E)), "file.stat");
    check(() => chDir(new Path("/definitely-missing-" + E)), "sys.chDir");
    assert(leaked === 0, "no raw control bytes in any diagnostic");
}

print("test_platform_hardening: all tests passed (" + n + " assertions)");
