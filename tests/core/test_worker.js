// flags: --std
// The worker is tests/core/worker_module_child.js (F30: was test_worker_module.js, which test_* globs ran alone
// and which failed there). The section waits for its done message and fails after 10 s without it: before,
// a worker that never loaded still passed.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_worker");
import * as std from "std";
import * as os from "os";

function assert(actual, expected, message) {
    if (arguments.length == 1)
        expected = true;

    if (actual === expected)
        return;

    if (actual !== null && expected !== null
    &&  typeof actual == 'object' && typeof expected == 'object'
    &&  actual.toString() === expected.toString())
        return;

    throw Error("assertion failed: got |" + actual + "|" +
                ", expected |" + expected + "|" +
                (message ? " (" + message + ")" : ""));
}

var worker;

function test_worker()
{
    var counter, done, timer;
    const finished = new Promise((res, rej) => { done = res; timer = os.setTimeout(() => {
        worker.onmessage = null;
        rej(new Error("the worker never answered done: tests/core/worker_module_child.js did not run"));
    }, 10000); });

    worker = new os.Worker("./worker_module_child.js");

    counter = 0;
    worker.onmessage = function (e) {
        var ev = e.data;
        switch(ev.type) {
        case "num":
            assert(ev.num, counter);
            counter++;
            if (counter == 10) {
                let sab = new SharedArrayBuffer(10);
                let buf = new Uint8Array(sab);
                worker.postMessage({ type: "sab", buf: buf });
            }
            break;
        case "sab_done":
            {
                let buf = ev.buf;
                assert(buf[2], 10);
                worker.postMessage({ type: "abort" });
            }
            break;
        case "done":
            worker.onmessage = null;
            os.clearTimeout(timer);
            done();
            break;
        }
    };
    return finished;
}

await __kitS.section("test_worker", () => test_worker());
__kitS.end();
