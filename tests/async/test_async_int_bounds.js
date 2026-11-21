// covers: c:src/dyna-async.c
// Integer arguments of dyna:async at the int32 boundary. The contract (dynajs.d.ts, from the original JS `(x | 0) === x` checks) is an
// integer that fits int32: Semaphore n, Pool n, Queue/Channel cap and pmap concurrency are 1..2147483647 (cap may also be Infinity), retry
// retries is 0..2147483647; anything else is refused, never truncated or wrapped (4294967297 would wrap to 1 under ToInt32). fix9b: the C
// checks compared d with (double)(int32_t)d, an undefined double->int conversion for |d| >= 2^31 (UBSan: "outside the range of
// representable values"); they now range-check before converting. Run under .obj-san/dynajs with UBSAN_OPTIONS=halt_on_error=1 to see the
// old UB as a crash.
import { suite } from "../lib/kit.js";
import { Semaphore, Pool, Queue, Channel, pmap, retry } from "dyna:async";

const t = suite("async_int_bounds");
const MAX = 2147483647;
const OUT = [MAX + 1, 4294967297, 1e10, 2 ** 53, -2147483649, -(2 ** 53), NaN, -Infinity];
function kind(f) { try { f(); return "ok"; } catch (e) { return e.constructor.name; } }
async function akind(f) { try { await f(); return "ok"; } catch (e) { return e.constructor.name; } }

t.test("Semaphore and Pool n: 2147483647 accepted, beyond int32 refused with TypeError", (a) => {
    a.eq(new Semaphore(MAX).available, MAX, "Semaphore(2^31-1)");
    a.eq(kind(() => new Pool(MAX)), "ok", "Pool(2^31-1)");
    for (const v of OUT) {
        a.eq(kind(() => new Semaphore(v)), "TypeError", "Semaphore(" + v + ")");
        a.eq(kind(() => new Pool(v)), "TypeError", "Pool(" + v + ")");
    }
    a.eq(kind(() => new Semaphore(0)), "TypeError", "Semaphore(0)");
});

t.test("Queue and Channel cap: 2147483647 and Infinity accepted, beyond int32 refused", (a) => {
    a.eq(kind(() => new Queue(MAX)), "ok", "Queue(2^31-1)");
    a.eq(kind(() => new Channel(MAX)), "ok", "Channel(2^31-1)");
    a.eq(kind(() => new Queue(Infinity)), "ok", "Queue(Infinity)");
    for (const v of [MAX + 1, 4294967297, 1e10, 2 ** 53, -2147483649]) {
        a.eq(kind(() => new Queue(v)), "RangeError", "Queue(" + v + ")");
        a.eq(kind(() => new Channel(v)), "RangeError", "Channel(" + v + ")");
    }
    a.eq(kind(() => new Queue(NaN)), "TypeError", "Queue(NaN)");
});

t.test("pmap concurrency and retry retries at the boundary", async (a) => {
    a.deep(await pmap([1, 2], (x) => x * 2, { concurrency: MAX }), [2, 4], "concurrency 2^31-1");
    for (const v of OUT) a.eq(await akind(() => pmap([1], (x) => x, { concurrency: v })), "TypeError", "concurrency " + v);
    a.eq(await retry(async () => 7, { retries: MAX }), 7, "retries 2^31-1");
    a.eq(await retry(async () => 8, { retries: -0 }), 8, "retries -0");
    for (const v of OUT) a.eq(await akind(() => retry(async () => 1, { retries: v })), "TypeError", "retries " + v);
});

await t.run();
