// flags: --std
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

{
    function plain(a) { return [arguments.length, arguments[0], arguments[1], arguments[2]]; }
    eqJ(plain(1, 2, "three"), [3, 1, 2, "three"], "plain call: arguments beyond the formals");

    function zero() { return [arguments.length, arguments[0], arguments[1]]; }
    eqJ(zero("x", "y"), [2, "x", "y"], "plain call: no formals at all");

    function noArgs() { return [arguments.length, arguments[0]]; }
    eqJ(noArgs(), [0, undefined], "plain call: no arguments passed");

    function rest(a) { return Array.prototype.slice.call(arguments); }
    eqJ(rest(1, 2, 3), [1, 2, 3], "plain call: arguments spread into an array");

    function restP(a, ...r) { return [a, r]; }
    eqJ(restP(1, 2, 3), [1, [2, 3]], "plain call: rest parameter past the first formal");

    function restOnly(...r) { return r; }
    eqJ(restOnly(1, 2, 3), [1, 2, 3], "plain call: rest-only parameter list");

    function named(a, b, c) { return [arguments.length, a, b, c]; }
    eqJ(named(1), [1, 1, undefined, undefined], "plain call: fewer arguments than formals");
}

{
    const src = [10, 20];
    eqJ(src.map(function (a) { return [arguments.length, arguments[0], arguments[1]]; }),
         [[3, 10, 0], [3, 20, 1]], "native callback: map sees all three arguments");

    eqJ(src.map(function (...args) { return args; }),
         [[10, 0, [10, 20]], [20, 1, [10, 20]]],
         "native callback: rest parameter spans the whole argument list");

    const fe = [];
    src.forEach(function (...a) { fe.push(a); });
    eqJ(fe, [[10, 0, [10, 20]], [20, 1, [10, 20]]],
         "native callback: forEach rest parameter");

    const fe2 = [];
    src.forEach(function (a) { fe2.push([arguments.length, arguments[1], arguments[2]]); });
    eqJ(fe2, [[3, 0, [10, 20]], [3, 1, [10, 20]]],
         "native callback: forEach arguments beyond the first formal");

    eqJ([3, 1, 2].sort(function (...a) { return a[0] - a[1]; }), [1, 2, 3],
         "native callback: sort comparator sees both operands");

    eqJ(Array.from([1, 2], function (...a) { return a; }), [[1, 0], [2, 1]],
         "native callback: Array.from mapper arguments");

    eqJ([10, 20].filter(function (...a) { return a[1] === 0; }), [10],
         "native callback: filter callback arguments");

    eqJ([5, 6].map(function (a) { return [arguments.length, arguments[1]]; }),
         [[3, 0], [3, 1]], "native callback: map arguments[1] is the index");
}

{
    const applyRest = function (...a) { return a; };
    eqJ(applyRest.apply(null, [1, 2, 3]), [1, 2, 3],
         "apply: rest parameter over a spread argument list");

    const applyArgs = function (a) { return [arguments.length, arguments[0], arguments[1], arguments[2]]; };
    eqJ(applyArgs.apply(null, [1, 2, 3]), [3, 1, 2, 3],
         "apply: arguments beyond the formals");

    const applyMore = function (a, b) { return [arguments.length, arguments[0], arguments[1], arguments[2]]; };
    eqJ(applyMore.apply(null, [1, 2, 3, 4]), [4, 1, 2, 3],
         "apply: more arguments than formals");

    const callRest = function (...a) { return a; };
    eqJ(callRest.call(null, 1, 2, 3), [1, 2, 3], "call: rest parameter");
}

{
    eqJ([10, 20].map(function (a) { return [a, arguments[0], arguments[1], arguments[2]]; }),
         [[10, 10, 0, [10, 20]], [20, 20, 1, [10, 20]]],
         "unmapped arguments: the tail past the formals reads the real arguments");

    function unmapped(a) { return [arguments.length, arguments[0], arguments[1]]; }
    eqJ(unmapped(1, 2), [2, 1, 2], "unmapped arguments: strict mode does not link");

    function strictWrite(a) { arguments[0] = 9; return a; }
    eqJ(strictWrite(1), 1, "unmapped arguments: a strict write does not touch the formal");

    const sloppyWrite = new Function("a", "arguments[0] = a + 100; return a;");
    eqJ([10, 20].map(sloppyWrite), [110, 120],
         "mapped arguments: writing arguments[0] writes the formal");

    const sloppyMapped = new Function("a", "b",
        "arguments[0] = 9; return [a, arguments[0], arguments[1], arguments[2]];");
    eqJ([10, 20].map(sloppyMapped), [[9, 9, 0, [10, 20]], [9, 9, 1, [10, 20]]],
         "mapped arguments: the unmapped tail still reads the real arguments");

    const sloppyRead = new Function("a",
        "return [arguments.length, arguments[0], arguments[1], arguments[2]];");
    eqJ([10, 20].map(sloppyRead), [[3, 10, 0, [10, 20]], [3, 20, 1, [10, 20]]],
         "mapped arguments: a mapped arguments object still sees every argument");
}

{
    function tcCallee3(a, b) { return [arguments.length, arguments[0], arguments[1], arguments[2]]; }
    function tcCaller3() { tailLocals(); return tcCallee3(1, 2, "three"); }
    eqJ(tcCaller3(), [3, 1, 2, "three"], "tail call: more arguments than formals");

    function tcCallee1(a) { return [arguments.length, arguments[0], arguments[1], arguments[2]]; }
    function tcCaller1() { tailLocals(); return tcCallee1(1, 2, "three"); }
    eqJ(tcCaller1(), [3, 1, 2, "three"], "tail call: fewer formals than arguments");

    function tcRest(...r) { return r; }
    function tcCallerRest() { tailLocals(); return tcRest(1, 2, "three"); }
    eqJ(tcCallerRest(), [1, 2, "three"], "tail call: rest parameter in the reused frame");

    const tcMapped = new Function("a", "b",
        "arguments[0] = 7; return [a, arguments[0], arguments[1], arguments[2]];");
    function tcCallerMapped() { tailLocals(); return tcMapped(1, 2, "three"); }
    eqJ(tcCallerMapped(), [7, 7, 2, "three"], "tail call: mapped arguments in the reused frame");

    function tcChain(a, b, c) { return [arguments.length, arguments[0], arguments[1], arguments[2], arguments[3]]; }
    function tcMid1() { tailLocals(); return tcChain(1, 2, 3, "four"); }
    function tcMid2() { tailLocals(); return tcMid1(); }
    function tcMid3() { tailLocals(); return tcMid2(); }
    eqJ(tcMid3(), [4, 1, 2, 3, "four"], "tail call: three tail calls deep");

    function tcNested(a, b) { return [arguments.length, arguments[0], arguments[1]]; }
    function tcInner() { tailLocals(); return tcNested("p", "q"); }
    function tcOuter() { tailLocals(); return [1, 2].map(function () { return tcInner(); })[0]; }
    eqJ(tcOuter(), [2, "p", "q"], "tail call: out of a native callback");
}

{
    function* genArgs(a) {
        yield [arguments.length, arguments[1], arguments[2]];
        yield arguments[3];
    }
    const it = genArgs(1, 2, 3);
    eqJ([it.next().value, it.next().value], [[3, 2, 3], undefined],
         "generator: arguments across yields");

    function* genRest(...r) { yield r.length; yield r[1]; }
    const it2 = genRest(7, 8, 9);
    eqJ([it2.next().value, it2.next().value], [3, 8], "generator: rest parameter");

    function* genMany(a, b, c) { yield [arguments.length, arguments[0], arguments[1], arguments[2], arguments[3]]; }
    eqJ([9].map(function () { return genMany(1, 2, 3, 4).next().value; }), [[4, 1, 2, 3, 4]],
         "generator: created and resumed inside a native callback");

    eqJ([9].map(function () { const g = genRest(1, 2, 3); g.next(); return g.next().value; }), [2],
         "generator: rest parameter resumed inside a native callback");

    function* genNested(a) { yield [arguments.length, arguments[0], arguments[1]]; }
    const g3 = [9].map(function () { return genNested(1, 2, 3); })[0];
    eqJ([g3.next().value, g3.next().value], [[3, 1, 2], undefined],
         "generator: the frame outlives the callback that created it");
}

{
    const mk = () => [10, 20].map(function (a) { const g = () => [arguments.length, arguments[1]]; return g(); });
    eqJ(mk(), [[3, 0], [3, 1]], "closure: an arrow closing over the callback arguments");

    const mk2 = () => [10, 20].map(function (...r) { const g = () => r; return g(); });
    eqJ(mk2(), [[10, 0, [10, 20]], [20, 1, [10, 20]]],
         "closure: an arrow closing over a callback rest parameter");

    const mk3 = () => [10, 20].map(function (a) { const g = () => arguments; return g()[1]; });
    eqJ(mk3(), [0, 1], "closure: an arrow closing over the arguments object");

    const mk4 = () => [10, 20].map(function (a) { const g = () => arguments.length; return g(); });
    eqJ(mk4(), [3, 3], "closure: an arrow closing over arguments.length");

    function escapingClosure(a) { return () => [arguments.length, arguments[1], arguments[2]]; }
    const escaped = [10, 20].map(function () { return escapingClosure(1, 2); })[0];
    eqJ([escaped(), escaped()], [[2, 2, undefined], [2, 2, undefined]],
         "closure: the arguments object outlives the frame");
}

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
    eqJ(asyncSeen[0], ["async-args", [3, 1, 2, 3]], "async: arguments beyond the formals");
    eqJ(asyncSeen[1], ["async-rest", ["a", "b", "c"]], "async: rest parameter");
    eqJ(asyncSeen[2], ["async-closure", [3, 2, 3]], "async: an arrow closing over the arguments");
    eqJ(asyncSeen[3], ["async-from-callback", [3, 1, 2, 3]], "async: started from a native callback");

    eqJ(debounceSeen, [["obj", [1, 2, 3]]],
         "debounced rest parameter keeps all three arguments");

    if (failures)
        throw new Error("test_audit_engine_1: " + failures + " failures");
    print("test_audit_engine_1: all " + n + " assertions passed");
}, 80);