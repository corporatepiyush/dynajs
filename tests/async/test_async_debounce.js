// covers: dyna:async.debounce
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_debounce");
import { pmap, Pool, Channel, Queue, Semaphore, sleep, withTimeout, retry, debounce, throttle } from "dyna:async";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function sleepMs(ms) { return new Promise(res => setTimeout(res, ms)); }
function withTestTimeout(p, ms, what) {
    let timer;
    const deadline = new Promise((_, rej) => { timer = setTimeout(() => rej(new Error("TIMEOUT: " + what)), ms); });
    return Promise.race([p, deadline]).finally(() => clearTimeout(timer));
}
async function t(fn, ms) { await withTestTimeout(fn(), ms, fn.name); }

await __kitS.section("nothing fired inside the window", async () => {
await t(async function debounceShape() {
    let calls = 0;
    const d = debounce(() => calls++, 30);
    d(); await sleepMs(10); d(); await sleepMs(10); d();
    eq(calls, 0, "nothing fired inside the window");
    await sleepMs(60);
    eq(calls, 1, "the LAST call inside the window fired once");
    d(); d.cancel();
    await sleepMs(60);
    eq(calls, 1, "cancel stops the pending call");
}, 8000);
});
await __kitS.section("the trailing call keeps every argument and its receiver", async () => {
await t(async function debounceArgs() {
    let got = null;
    const obj = { tag: "o" };
    const d = debounce(function (...a) { got = [this && this.tag, ...a]; }, 10);
    d.call({ tag: "early" }, 9);
    d.call(obj, 1, "two", 3);
    await sleepMs(40);
    eq(JSON.stringify(got), JSON.stringify(["o", 1, "two", 3]), "last call's receiver and all arguments");
    eq(typeof d, "function", "debounce returns a function");
    eq(typeof d.call, "function", "it inherits Function.prototype");
    eq(d.pending(), false, "nothing pending after the edge");
}, 8000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
