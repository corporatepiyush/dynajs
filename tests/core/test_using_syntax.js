// covers: integration c:src/parser/parser.inc.c c:src/runtime/using.inc.c c:src/builtins/builtin-promise.c c:src/builtins/disposable.inc.c -- one language feature: syntax, scope-exit disposal, async ticks and AsyncIterator dispose share one model
// F-USING: `using` / `await using` (explicit resource management, test262 feature explicit-resource-management).
// Why: the parser support sat behind an undefined CONFIG_USING gate, so `using x = ...` was "SyntaxError: expecting
// ';'" while d.ts said the engine honors it. With the gate removed three spec gaps showed and are fixed here:
// (1) %AsyncIteratorPrototype%[Symbol.asyncDispose] was missing; (2) `for (using x of ...)` disposed only on normal
// completion, not on break/return/throw/continue-outer; (3) `await using` disposal awaited through a promise loop
// (an extra tick per record, sync records awaited too) instead of the spec's inline Await per async record with
// needsAwait/hasAwaited coalescing of null records. The differential cases compare against tests/lib/using_progs.js,
// a hand-desugared model of DisposeResources that was checked against V8 (node 26): 5999 of 6000 programs agree, the
// one difference being a V8 bug (it calls a sync-throwing [Symbol.asyncDispose] twice; the spec calls it once).
import { suite } from "../lib/kit.js";
import { res, ares, ser, prng, genProgram, runSync, runAsync } from "../lib/using_progs.js";

const t = suite("test_using_syntax");

function compiles(src) {
    try { new Function(src); return true; } catch (e) { if (e instanceof SyntaxError) return false; throw e; }
}

t.test("basic block disposal", (a) => {
    const log = [];
    {
        using x = res(log, 1, "ok");
        log.push("in");
        a.eq(x.id, 1);
    }
    log.push("out");
    a.deep(log, ["in", "d1", "out"]);
});

t.test("reverse order, multiple bindings in one declaration", (a) => {
    const log = [];
    {
        using x = res(log, 1, "ok"), y = res(log, 2, "ok");
        using z = res(log, 3, "ok");
    }
    a.deep(log, ["d3", "d2", "d1"]);
});

t.test("null and undefined initializers are skipped", (a) => {
    const log = [];
    {
        using x = null, y = undefined, z = res(log, 3, "ok");
    }
    a.deep(log, ["d3"]);
});

t.test("non-object initializer is a TypeError at the declaration", (a) => {
    for (const v of [1, "s", true, Symbol("q"), 1n]) {
        const log = [];
        a.throws(() => { using p = res(log, 1, "ok"); using x = v; log.push("unreached"); }, TypeError, typeof v);
        a.deep(log, ["d1"], "earlier resource still disposed for " + typeof v);
    }
});

t.test("object without Symbol.dispose is a TypeError", (a) => {
    a.throws(() => { using x = {}; }, TypeError);
    a.throws(() => { using x = { [Symbol.dispose]: 1 }; }, TypeError);
    a.throws(() => { using x = { [Symbol.dispose]: null }; }, TypeError);
    a.throws(() => { using x = { [Symbol.asyncDispose]() {} }; }, TypeError, "sync using ignores asyncDispose");
});

t.test("dispose method is read once, at the declaration", (a) => {
    const log = [];
    let reads = 0;
    const o = { get [Symbol.dispose]() { reads++; return function () { log.push("first:" + (this === o)); }; } };
    {
        using x = o;
        Object.defineProperty(o, Symbol.dispose, { value() { log.push("second"); } });
        a.eq(reads, 1);
    }
    a.deep(log, ["first:true"]);
    a.eq(reads, 1);
});

t.test("dispose is called with no arguments and the resource as this", (a) => {
    let seen;
    { using x = { [Symbol.dispose](...args) { seen = [this === x, args.length]; } }; }
    a.deep(seen, [true, 0]);
});

t.test("binding is const", (a) => {
    a.throws(() => { using x = null; x = 1; }, TypeError);
    a.eq(compiles("{ using x = null; x = 1; }"), true, "assignment is a runtime error, not early");
});

t.test("TDZ before the declaration", (a) => {
    a.throws(() => { x; using x = null; }, ReferenceError);
});

t.test("single error during disposal is rethrown as is", (a) => {
    const log = [];
    const e = a.throws(() => { using x = res(log, 1, "throw"); }, Error);
    a.eq(e && e.message, "x1");
    a.eq(e instanceof SuppressedError, false);
});

t.test("body error plus disposal errors chain SuppressedError(new, previous)", (a) => {
    const log = [];
    try {
        using p = res(log, 1, "throw");
        using q = res(log, 2, "throw");
        throw new Error("body");
    } catch (e) {
        a.eq(ser(e), "S(x1,S(x2,body))");
        a.eq(e.error.message, "x1");
    }
    a.deep(log, ["d2", "d1"]);
});

t.test("SuppressedError shape", (a) => {
    try {
        using p = res([], 1, "throw");
        throw 42;
    } catch (e) {
        a.ok(e instanceof SuppressedError);
        a.eq(e.suppressed, 42);
        a.eq(e.error.message, "x1");
        a.eq(Object.getPrototypeOf(e), SuppressedError.prototype);
    }
});

t.test("return, break, continue, labelled break dispose on exit", (a) => {
    const log = [];
    function f() { using x = res(log, 1, "ok"); log.push("ret"); return 7; }
    a.eq(f(), 7);
    for (let i = 0; i < 3; i++) {
        using x = res(log, 10 + i, "ok");
        if (i === 0) continue;
        if (i === 1) break;
    }
    outer: for (;;) {
        for (;;) {
            using y = res(log, 20, "ok");
            break outer;
        }
    }
    a.deep(log, ["ret", "d1", "d10", "d11", "d20"]);
});

t.test("return value computed before disposal; dispose error overrides return", (a) => {
    const log = [];
    function f() { using x = res(log, 1, "ok"); return log.push("v"); }
    a.eq(f(), 1);
    a.deep(log, ["v", "d1"]);
    function g() { using x = res([], 2, "throw"); return 1; }
    a.throws(g, /x2/);
});

t.test("for (using x of ...) disposes each iteration value", (a) => {
    const log = [];
    for (using x of [res(log, 1, "ok"), null, res(log, 3, "ok")]) {
        log.push("b" + (x ? x.id : "n"));
    }
    a.deep(log, ["b1", "d1", "bn", "b3", "d3"]);
});

t.test("for (using x of ...) with break and throw closes the iterator", (a) => {
    const log = [];
    function* gen() { try { yield res(log, 1, "ok"); yield res(log, 2, "ok"); } finally { log.push("gclose"); } }
    for (using x of gen()) { log.push("b" + x.id); break; }
    a.deep(log, ["b1", "d1", "gclose"]);
    log.length = 0;
    a.throws(() => { for (using x of gen()) { throw new Error("t"); } }, /^t$/);
    a.deep(log, ["d1", "gclose"]);
});

t.test("for-of using heads dispose on every abrupt exit (sync and await)", async (a) => {
    const L = [];
    const mk = (id) => res(L, id, "ok");
    function ret() { for (using x of [mk(1), mk(2)]) { L.push("r"); return 5; } }
    a.eq(ret(), 5);
    let n = 0;
    for (using x of [mk(3), mk(4)]) { L.push("c"); if (n++ === 0) continue; L.push("after"); }
    outer: for (let k = 0; k < 2; k++) { for (using x of [mk(5)]) { L.push("o" + k); continue outer; } }
    function* g() { for (using x of [mk(6), mk(7)]) yield x.id; }
    const it = g();
    it.next();
    it.return();
    a.deep(L, ["r", "d1", "c", "d3", "c", "after", "d4", "o0", "d5", "o1", "d5", "d6"]);
    L.length = 0;
    const am = (id) => ares(L, id, "ok");
    async function aret() { for (await using x of [am(1), am(2)]) { L.push("r"); return 5; } }
    a.eq(await aret(), 5);
    async function abrk() { for (await using x of [am(3), am(4)]) { L.push("b"); break; } L.push("out"); }
    await abrk();
    async function athr() { for (await using x of [am(5)]) throw new Error("q"); }
    await a.rejects(athr(), /^q$/);
    async function* ag() { for await (await using x of [am(6), am(7)]) yield 1; }
    const ait = ag();
    await ait.next();
    await ait.return();
    a.deep(L, ["r", "a1", "A1", "b", "a3", "A3", "out", "a5", "A5", "a6", "A6"]);
});

t.test("for (using x = ...; ;) disposes once after the loop", (a) => {
    const log = [];
    for (using x = res(log, 1, "ok"), i = null; log.length < 2;) log.push("it");
    a.deep(log, ["it", "it", "d1"]);
});

t.test("switch case block and class static block", (a) => {
    const log = [];
    switch (1) { case 1: { using x = res(log, 1, "ok"); log.push("c"); } }
    class K { static { using y = res(log, 2, "ok"); log.push("s"); } }
    a.deep(log, ["c", "d1", "s", "d2"]);
});

t.test("generator return() and throw() dispose", (a) => {
    const log = [];
    function* g() { using x = res(log, 1, "ok"); yield 1; yield 2; }
    const it = g();
    it.next();
    it.return(5);
    a.deep(log, ["d1"]);
    const it2 = g();
    it2.next();
    a.throws(() => it2.throw(new Error("gt")), /gt/);
    a.deep(log, ["d1", "d1"]);
});

t.test("closures see the resource; disposal happens at block exit only", (a) => {
    const log = [];
    let f;
    { using x = res(log, 1, "ok"); f = () => x.id; }
    a.eq(f(), 1);
    a.deep(log, ["d1"]);
});

t.test("`using` is still an identifier where no declaration parses", (a) => {
    a.eq(compiles("var using = 1; using = 2; using++; using[0]; using.x; using(1); using\nx = 1;"), true);
    a.eq(compiles("let using; for (using of []) ;"), true, "for (using of ...) is a for-of over the identifier");
    a.eq(compiles("var using; for (using.x of []) ;"), true);
    a.eq(compiles("var using; for (using in {}) ;"), true);
    a.eq(compiles("function using() {} using\n(1)"), true);
    a.eq(compiles("using: for(;;) break using;"), true);
    a.eq(compiles("var using, of; for (using of of [0]) ;"), true, "for (using of of[0]): identifier using, element access");
    a.eq(compiles("var using, of; for (using of of []) ;"), false, "of[] is an empty element access");
    a.eq(compiles("var using, a; { using [a] = null; }"), true, "using [a] = ... is element-access assignment");
    a.eq(compiles("{ using\nx = null; }"), true, "line terminator after using: ASI, not a declaration");
    let using = 3, x = 4;
    a.eq(using + x, 7);
});

t.test("early errors", (a) => {
    a.eq(compiles("{ using x; }"), false, "initializer required");
    a.eq(compiles("{ using {a} = null; }"), false, "no destructuring");
    a.eq(compiles("{ using [] = null; }"), false, "no array destructuring");
    a.eq(compiles("{ using x = null; let x; }"), false, "redeclaration");
    a.eq(compiles("{ using let = null; }"), false, "let as binding");
    a.eq(compiles("switch (0) { case 0: using x = null; }"), false, "directly in case clause");
    a.eq(compiles("{ await using x = null; }"), false, "await using outside async");
    a.eq(compiles("for (using x in {}) ;"), false, "for-in");
    a.eq(compiles("for (using x of [], y of []) ;"), false);
    a.eq(compiles("if (1) using x = null;"), false, "not a statement position");
    a.eq(compiles("label: using x = null;"), false, "labelled");
});

t.test("script top level rejects using, module top level accepts", async (a) => {
    a.throws(() => (0, eval)("using x = null;"), SyntaxError);
    a.throws(() => eval("using x = null;"), SyntaxError, "direct eval body is script code");
    a.eq((0, eval)("{ using x = null; 5 }"), 5, "inside a block in script code is fine");
    const log = [];
    globalThis.__usingLog = log;
    globalThis.__usingRes = (id) => ({ [Symbol.dispose]() { log.push("m" + id); } });
    const ns = await import("../lib/fixture_using_module.js");
    a.eq(ns.v, 1);
    a.deep(log, ["body", "m2", "m1"]);
});

using __moduleTop = { [Symbol.dispose]() {} };
await using __moduleTopAsync = null;

t.test("await using basic ordering and awaiting", async (a) => {
    const log = [];
    async function f() {
        await using x = ares(log, 1, "ok"), y = ares(log, 2, "ok");
        log.push("body");
    }
    const p = f();
    log.push("sync-after-call");
    await p;
    a.deep(log, ["body", "a2", "sync-after-call", "A2", "a1", "A1"]);
});

t.test("await using falls back to Symbol.dispose; result is awaited", async (a) => {
    const log = [];
    async function f() { await using x = ares(log, 1, "sync"); log.push("b"); }
    await f();
    a.deep(log, ["b", "s1"]);
    let thenCalled = false;
    async function g() {
        await using x = { [Symbol.dispose]() { return { then() { thenCalled = true; } }; } };
    }
    await g();
    a.eq(thenCalled, false, "sync dispose result is not awaited as a thenable");
});

t.test("await using null still awaits once", async (a) => {
    const log = [];
    async function f() { { await using x = null; log.push("in"); } log.push("after"); }
    const p = f();
    log.push("caller");
    await p;
    a.deep(log, ["in", "caller", "after"]);
    log.length = 0;
    async function g() { { using x = null; log.push("in"); } log.push("after"); }
    const q = g();
    log.push("caller");
    await q;
    a.deep(log, ["in", "after", "caller"], "plain using null does not await");
});

t.test("await using errors chain and rejections", async (a) => {
    const log = [];
    async function f() {
        await using x = ares(log, 1, "throw"), y = ares(log, 2, "throw");
        throw new Error("body");
    }
    const e = await a.rejects(f(), SuppressedError);
    a.eq(ser(e), "S(y1,S(y2,body))");
    async function g() { await using x = 5; }
    await a.rejects(g(), TypeError);
    async function h() { await using x = { [Symbol.asyncDispose]: 3 }; }
    await a.rejects(h(), TypeError);
});

t.test("for await (await using x of ...) and for (await using x of ...)", async (a) => {
    const log = [];
    async function* src() { yield ares(log, 1, "ok"); yield ares(log, 2, "ok"); }
    for await (await using x of src()) log.push("b");
    a.deep(log, ["b", "a1", "A1", "b", "a2", "A2"]);
    log.length = 0;
    for (await using x of [ares(log, 3, "ok")]) log.push("c");
    a.deep(log, ["c", "a3", "A3"]);
});

t.test("%AsyncIteratorPrototype%[Symbol.asyncDispose]", async (a) => {
    const AIP = Object.getPrototypeOf(Object.getPrototypeOf(async function* () {}.prototype));
    const d = Object.getOwnPropertyDescriptor(AIP, Symbol.asyncDispose);
    a.must(d, "own property exists");
    a.eq(typeof d.value, "function");
    a.eq(d.writable && !d.enumerable && d.configurable, true);
    a.eq(d.value.length, 0);
    a.eq(d.value.name, "[Symbol.asyncDispose]");
    const log = [];
    async function* g() { try { yield 1; yield 2; } finally { log.push("fin"); } }
    const it = g();
    await it.next();
    const r = it[Symbol.asyncDispose]();
    a.ok(r instanceof Promise);
    a.eq(await r, undefined);
    a.deep(log, ["fin"]);
    a.eq(await d.value.call({}), undefined, "no return method resolves undefined");
    let args;
    await d.value.call({ return(...x) { args = x; return 9; } });
    a.deep(args, [undefined], "return called with one undefined argument");
    await a.rejects(d.value.call({ return() { throw new Error("rt"); } }), /rt/);
    await a.rejects(d.value.call({ return: 1 }), TypeError);
    await a.rejects(d.value.call({ get return() { throw new Error("getter"); } }), /getter/);
    await a.rejects(d.value.call({ return() { return Promise.reject(new Error("rej")); } }), /rej/);
    await a.rejects(d.value.call(undefined), TypeError, "this undefined rejects, does not throw");
    {
        await using it2 = g();
        await it2.next();
    }
    a.deep(log, ["fin", "fin"]);
});

t.test("Iterator.prototype[Symbol.dispose] closes sync iterators under using", (a) => {
    const log = [];
    function* g() { try { yield 1; } finally { log.push("fin"); } }
    { using it = g(); it.next(); }
    a.deep(log, ["fin"]);
});

t.test("differential: random programs vs the DisposeResources model", (a) => {
    const rnd = prng(0x5eed1234);
    let mismatches = 0, usingCount = 0;
    for (let i = 0; i < 3000; i++) {
        const p = genProgram(rnd, false);
        if (p.u.includes("using")) usingCount++;
        const got = runSync(p.u), want = runSync(p.m);
        if (got !== want && mismatches++ < 3) a.eq(got, want, "program " + i + ": " + p.u);
    }
    a.eq(mismatches, 0, "mismatching programs");
    a.ok(usingCount > 2000, "programs exercise using: " + usingCount);
});

t.test("differential: random await using programs vs the model, tick-exact", async (a) => {
    const rnd = prng(0xa5a5);
    let mismatches = 0;
    for (let i = 0; i < 3000; i++) {
        const p = genProgram(rnd, true);
        const got = await runAsync(p.u), want = await runAsync(p.m);
        if (got !== want && mismatches++ < 3) a.eq(got, want, "program " + i + ": " + p.u);
    }
    a.eq(mismatches, 0, "mismatching async programs");
});

await t.run();
