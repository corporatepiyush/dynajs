// timeout: 120
// flags: --std
// Split from tests/test_platform_hardening.js: the async sections.
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
print("test_async_platform_hardening: all tests passed (" + n + " assertions)");
