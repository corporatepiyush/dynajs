// flags: --std
// covers: c:src/builtins/builtin-promise.c
// E2-A regression: the argument buffer a frame materialises `arguments`,
// `arguments` mapped traps and rest parameters FROM.
//
// The frame setup in src/vm/interpreter.inc.c picks its argument buffer in
// three ways (restart_call): the caller's argv (fast path), the frame's own
// alloca (copy path, when arg_allocated_size != 0) and the malloc'd tc_argv
// (tail-call path). The three argument opcodes must read a buffer holding all
// `argc` entries. Two wrong answers are pinned here:
//   * reading the truncated alloca copy (only b->arg_count entries are copied,
//     the rest aliases the frame's locals) -- what the previous lane's patch
//     did, which made a debounced rest parameter come back as [1, obj, obj];
//   * reading a buffer freed at the tail-call transition -- the E2-A
//     use-after-free, guarded here by keeping the transition copy alive.
import * as os from "os";
import { sections } from "../lib/kit.js";

let n = 0, failures = 0;
function assert(c, m) {
    n++;
    if (!c) { failures++; print("FAIL: " + m); }
}
function eq(got, want, m) {
    assert(got === want, m + " (got " + JSON.stringify(got) +
           ", want " + JSON.stringify(want) + ")");
}
function eqJ(got, want, m) {
    eq(JSON.stringify(got), JSON.stringify(want), m);
}
function tailLocals() {
    var p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12;
}

const S = sections("test_core_builtin_promise_audit_engine_1");
const asyncSeen = [];

async function asyncArgs(a) { return [arguments.length, arguments[0], arguments[1], arguments[2]]; }

async function asyncRest(...r) { return r; }

async function asyncClosure(a) {
    const inner = () => [arguments.length, arguments[1], arguments[2]];
    return inner();
}

asyncArgs(1, 2, 3).then(v => asyncSeen.push(["async-args", v]));
asyncRest("a", "b", "c").then(v => asyncSeen.push(["async-rest", v]));
asyncClosure(1, 2, 3).then(v => asyncSeen.push(["async-closure", v]));
[1, 2].map(function () { return asyncArgs(1, 2, 3); }).forEach(
    p => p.then(v => asyncSeen.push(["async-from-callback", v])));

const debounceSeen = [];

const debounced = function (...args) { debounceSeen.push([this && this.tag, args]); };

const receiver = { tag: "obj" };

receiver.run = debounced.debounce(5);
receiver.run(1, 2, 3);

os.setTimeout(() => {
    S.section("async functions keep every argument, rest parameters and closed-over arguments", () => {
    eqJ(asyncSeen[0], ["async-args", [3, 1, 2, 3]], "async: arguments beyond the formals");
    eqJ(asyncSeen[1], ["async-rest", ["a", "b", "c"]], "async: rest parameter");
    eqJ(asyncSeen[2], ["async-closure", [3, 2, 3]], "async: an arrow closing over the arguments");
    eqJ(asyncSeen[3], ["async-from-callback", [3, 1, 2, 3]], "async: started from a native callback");
    });

    S.section("a debounced rest parameter keeps all three arguments", () => {
    eqJ(debounceSeen, [["obj", [1, 2, 3]]],
         "debounced rest parameter keeps all three arguments");
    });
    S.end();

    if (failures)
        throw new Error("test_audit_engine_1: " + failures + " failures");
    print("test_audit_engine_1: all " + n + " assertions passed");
}, 80);