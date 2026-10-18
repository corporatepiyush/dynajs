// flags: --std
// Split from tests/test_audit_w3_native_8.js: the core sections.
import * as os from "os";
import { Path, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- C1-11: JS timers ---------------------------------------------------
// ORACLE: a timer queue is a priority queue; set and clear are O(log n), so
// the cost per operation cannot grow tenfold when n grows tenfold. The old
// sorted list was linear per operation (measured 2.7 us at 2k, 28 us at 20k).
// CONTROL: firing order is by deadline, ties in creation order; a cleared
// timer never fires.
{
    const per = (n) => {
        const ids = [];
        const t = performance.now();
        for (let i = 0; i < n; i++) ids.push(setTimeout(() => {}, 200000 - i));
        for (const i of ids) clearTimeout(i);
        return (performance.now() - t) / n;
    };
    per(500);
    const small = per(2000), big = per(20000);
    ok(big < small * 4, "[red] C1-11: timer set+clear cost per op is flat (" + (small * 1e6).toFixed(0) + " ns at 2k, " + (big * 1e6).toFixed(0) + " ns at 20k)");

    // A deadline is creation time + delay, and a slow build (ASan) spreads the creations over
    // milliseconds; so equal delays must keep creation order, and deadlines 2+ ms apart must not swap.
    const order = [], due = [];
    const delays = [7, 3, 9, 3, 1, 5, 9, 1, 4];
    delays.forEach((d, i) => { due[i] = performance.now() + d; setTimeout(() => order.push(i), d); });
    const dead = setTimeout(() => order.push(-1), 2);
    clearTimeout(dead);
    await sleep(40);
    const bad = [];
    for (let x = 0; x < order.length; x++)
        for (let y = x + 1; y < order.length; y++) {
            const i = order[x], j = order[y];
            if ((delays[i] === delays[j] && i > j) || due[i] - due[j] >= 2) bad.push(i + " before " + j);
        }
    ok(order.length === delays.length && !order.includes(-1) && bad.length === 0,
        "control: timers fire by deadline, ties in creation order (" + order.map((i) => delays[i] * 100 + i).join() + (bad.length ? "; " + bad.join(", ") : "") + ")");
    ok(!order.includes(-1), "control: a cleared timer never fires");
}
// ORACLE: the live-timer cap counts LIVE timers. An interval that fires is
// still one timer, so firing must not consume the cap. The old requeue path
// incremented the count on every firing and never decremented it.
// CONTROL: setTimeout works before the firings.
{
    let n = 0;
    ok(kind(() => clearTimeout(setTimeout(() => {}, 0))) === "none", "control: setTimeout works");
    const ids = [];
    for (let k = 0; k < 500; k++) ids.push(setInterval(() => { n++; }, 1));
    const t0 = Date.now();
    while (n < 101000 && Date.now() - t0 < 20000) await sleep(5);
    const r = kind(() => clearTimeout(setTimeout(() => {}, 0)));
    for (const i of ids) clearInterval(i);
    ok(n >= 101000, "the intervals fired more than the 100000 cap (" + n + ")");
    ok(r === "none", "[red] C1-11: interval firings do not consume the live-timer cap (" + r + ")");
}
// ---- P1-21: os module ---------------------------------------------------
// ORACLE: kill(2): pid 0 is the caller's whole process group and pid -1 is
// every process the caller may signal; neither is ever what a script means.
// A C string ends at its first NUL, so an argument containing one would be
// silently shortened. POSIX: a new program starts with default dispositions
// unless the parent ignored a signal, and this runtime ignores SIGPIPE.
// CONTROL: signal 0 to our own pid succeeds; a plain exec returns 0.
{
    ok(os.kill(os.getpid(), 0) === 0, "control: os.kill(self, 0) succeeds");
    ok(kind(() => os.kill(-1, 0)) === "RangeError", "[red] P1-21: os.kill(-1, sig) is refused");
    ok(kind(() => os.kill(0, 0)) === "RangeError", "[red] P1-21: os.kill(0, sig) is refused");
    ok(kind(() => os.kill(os.getpid(), 4096)) === "RangeError", "[red] P1-21: an out-of-range signal number is refused");
    ok(os.exec(["/bin/sh", "-c", "exit 0"]) === 0, "control: os.exec runs a command");
    ok(kind(() => os.exec(["/bin/sh", "-c", "exit 0\0 ignored"])) === "TypeError", "[red] P1-21: os.exec refuses an argument containing NUL");
    ok(kind(() => os.exec(["/bin/sh", "-c", "exit 0"], { cwd: "/tmp\0x" })) === "TypeError", "[red] P1-21: os.exec refuses a cwd containing NUL");
    const rc = os.exec(["/bin/sh", "-c", "kill -PIPE $$; exit 0"]);
    ok(rc === -os.SIGPIPE, "[red] P1-21: an os.exec child has the default SIGPIPE disposition (" + rc + ")");
}
removeAll(new Path(String(T)));
console.log("test_core_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_core_audit_w3_native_8: " + fail + " failures");
