// covers: integration c:src/vm/interpreter.inc.c c:src/builtins/builtin-promise.c c:src/object/property_set_convert.inc.c c:src/builtins/disposable.inc.c c:src/runtime/using.inc.c c:src/serialize/bc_read.inc.c c:src/parser/parser.inc.c c:src/mm/js_malloc.inc.c -- the --timeout-ms interrupt stays uncatchable through async code
// The interrupt that --timeout-ms raises is uncatchable: no catch, finally,
// rejection handler or iterator return() may run after it fires, and the
// process must exit non-zero with "interrupted" soon after the deadline.
// Async functions, the Promise executor, reaction and thenable jobs,
// Promise.try/all/race/any/allSettled/allKeyed, async generators,
// async-from-sync iteration, using/DisposableStack disposal, Array.fromAsync,
// module evaluation and dynamic import used to turn it into
// an ordinary rejection, so user code observed it ("LEAK" lines) or kept
// running past the deadline. Each case runs in its own child process.
import { args, Exec } from "dyna:sys";
import { makeTempDir, removeAll, writeFile } from "dyna:file";
import { suite } from "../lib/kit.js";

const BIN = args()[0];
const DEADLINE_MS = 200;
const LIMIT_MS = 3000;
const t = suite("interrupt_async");

const spin = "function spin(){ for(;;){} }\n";
const it = (next, ret) => `({ [Symbol.iterator](){ return { next(){ ${next} }, return(){ ${ret || 'console.log("LEAK return"); return {};'} } }; } })`;

const CASES = [
    ["async function body", `(async () => { spin(); })(); console.log("LEAK after call");`],
    ["await then spin, caught in a loop", `(async () => { for (;;) { try { await 0; spin(); } catch (e) { console.log("LEAK caught", e); } } })();`],
    ["await then spin, finally", `(async () => { try { await 0; spin(); } finally { console.log("LEAK finally"); } })();`],
    ["async rejection handler", `(async () => { await 0; spin(); })().catch((e) => console.log("LEAK rejected", e));`],
    ["Promise executor", `new Promise(() => spin()); console.log("LEAK after executor");`],
    ["dynamic import specifier toString", `import({ toString() { spin(); } }).catch((e) => console.log("LEAK", e)); console.log("LEAK after import");`],
    ["Promise executor in a loop", `for (;;) { new Promise(() => { for (let i = 0; i < 1e6; i++) {} }).catch(() => console.log("LEAK")); }`],
    ["then reaction", `Promise.resolve().then(() => spin()).catch((e) => console.log("LEAK", e));`],
    ["thenable job", `Promise.resolve({ then() { spin(); } }).catch((e) => console.log("LEAK", e));`],
    ["then getter on a resolution", `Promise.resolve().then(() => ({ get then() { spin(); } })).catch((e) => console.log("LEAK", e));`],
    ["async return with then getter", `(async () => ({ get then() { spin(); } }))().catch((e) => console.log("LEAK", e));`],
    ["Promise.try", `Promise.try(() => spin()).catch((e) => console.log("LEAK", e)); console.log("LEAK after try");`],
    ["Promise.all iterator", `Promise.all(${it("spin();")}).catch((e) => console.log("LEAK", e)); console.log("LEAK after all");`],
    ["Promise.race iterator", `Promise.race(${it("spin();")}).catch((e) => console.log("LEAK", e)); console.log("LEAK after race");`],
    ["Promise.any iterator", `Promise.any(${it("spin();")}).catch((e) => console.log("LEAK", e)); console.log("LEAK after any");`],
    ["Promise.allSettled iterator", `Promise.allSettled(${it("spin();")}).catch((e) => console.log("LEAK", e)); console.log("LEAK after allSettled");`],
    ["Promise.all resolve getter closes nothing", `class P extends Promise { static get resolve() { spin(); } } P.all(${it("return { value: 1, done: false };")}).catch(() => console.log("LEAK")); console.log("LEAK after all");`],
    ["Promise.allKeyed", `Promise.allKeyed({ get a() { spin(); } }).catch((e) => console.log("LEAK", e)); console.log("LEAK after allKeyed");`],
    ["Promise.all closes nothing after then throws", `class P extends Promise { static resolve() { return { then() { spin(); } }; } } P.all(${it("return { value: 1, done: false };")}).catch(() => console.log("LEAK")); console.log("LEAK after all");`],
    ["Array.fromAsync next", `Array.fromAsync({ [Symbol.asyncIterator]() { return { next() { spin(); } }; } }).catch((e) => console.log("LEAK", e)); console.log("LEAK after fromAsync");`],
    ["Array.fromAsync in a loop", `(async () => { for (;;) { try { await Array.fromAsync({ [Symbol.asyncIterator]() { return { next() { for (let i = 0; i < 1e6; i++) {} return { done: true }; } }; } }); } catch (e) { console.log("LEAK", e); } } })();`],
    ["Array.fromAsync array-like element", `Array.fromAsync({ length: 1, get 0() { spin(); } }).catch((e) => console.log("LEAK", e)); console.log("LEAK after fromAsync");`],
    ["Array.fromAsync mapfn", `Array.fromAsync([1], () => spin()).catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
    ["Array.fromAsync iterator return", `Array.fromAsync({ [Symbol.asyncIterator]() { return { next() { return { value: 1, done: false }; }, return() { spin(); } }; } }, () => { throw 1; }).catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
    ["Array.fromAsync return getter", `Array.fromAsync({ [Symbol.asyncIterator]() { return { next() { return { value: 1, done: false }; }, get return() { spin(); } }; } }, () => { throw 1; }).catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
    ["Array.fromAsync return() result then getter", `Array.fromAsync({ [Symbol.asyncIterator]() { return { next() { return { value: 1, done: false }; }, return() { return { get then() { spin(); } }; } }; } }, () => { throw 1; }).catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
    ["Array.fromAsync resolve with a then getter (iterable)", `Object.defineProperty(Array.prototype, "then", { get() { spin(); }, configurable: true }); Array.fromAsync([1]).catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
    ["Array.fromAsync resolve with a then getter (array-like)", `Object.defineProperty(Array.prototype, "then", { get() { spin(); }, configurable: true }); Array.fromAsync({ length: 1, 0: 1 }).catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
    ["async generator body", `async function* g() { spin(); } g().next().catch((e) => console.log("LEAK", e)); console.log("LEAK after next");`],
    ["async generator await, caught", `async function* g() { for (;;) { try { await 0; spin(); } catch (e) { console.log("LEAK caught", e); } } } (async () => { for await (const x of g()); })();`],
    ["async generator return() with then getter", `async function* g() {} const i = g(); i.next(); i.return({ get then() { spin(); } }).catch((e) => console.log("LEAK", e));`],
    ["async generator return() then sync code", `async function* g() {} const i = g(); i.next(); i.return({ get then() { spin(); } }); console.log("LEAK after return");`],
    ["for await over a sync iterator", `(async () => { try { for await (const x of ${it("spin();")}); } catch (e) { console.log("LEAK", e); } })();`],
    ["for await body, iterator return()", `(async () => { try { for await (const x of ${it("return { value: 1, done: false };")}) { spin(); } } catch (e) { console.log("LEAK", e); } })();`],
    ["for await body over an async generator", `async function* g() { try { for (;;) yield 1; } finally { console.log("LEAK gen finally"); } } (async () => { for await (const x of g()) { await 0; spin(); } })();`],
    ["AsyncIterator dispose", `const AIP = Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf((async function* () {})())));\nAIP[Symbol.asyncDispose].call({ return() { spin(); } }).catch((e) => console.log("LEAK", e)); console.log("LEAK after dispose");`],
    ["await using dispose", `(async () => { try { await using x = { async [Symbol.asyncDispose]() { spin(); } }; } catch (e) { console.log("LEAK", e); } })();`],
    ["using dispose with catch and finally", `try { using x = { [Symbol.dispose]() { spin(); } }; } catch (e) { console.log("LEAK", e); } finally { console.log("LEAK finally"); }`],
    ["DisposableStack dispose", `const s = new DisposableStack(); s.defer(() => console.log("LEAK second")); s.defer(() => spin()); try { s.dispose(); } catch (e) { console.log("LEAK", e); }`],
    ["AsyncDisposableStack disposeAsync", `const s = new AsyncDisposableStack(); s.defer(() => spin()); s.disposeAsync().catch((e) => console.log("LEAK", e)); console.log("LEAK after disposeAsync");`],
    ["AsyncDisposableStack sync resource", `const s = new AsyncDisposableStack(); s.use({ [Symbol.dispose]() { spin(); } }); s.disposeAsync().catch((e) => console.log("LEAK", e)); console.log("LEAK after disposeAsync");`],
    ["AsyncDisposableStack result constructor getter", `const s = new AsyncDisposableStack(); s.defer(() => { const p = Promise.resolve(); Object.defineProperty(p, "constructor", { get() { spin(); } }); return p; }); s.disposeAsync().catch((e) => console.log("LEAK", e)); setTimeout(() => console.log("LEAK timer"), 50);`],
];

for (const [name, body] of CASES) {
    t.test(name, (a) => {
        const t0 = Date.now();
        const r = Exec(BIN, ["--timeout-ms", String(DEADLINE_MS), "--eval", spin + body], { timeoutMs: 10000 });
        const ms = Date.now() - t0, out = String(r.stdout), err = String(r.stderr);
        a.ok(r.code !== 0, `exits non-zero (code ${r.code})`);
        a.ok(err.includes("interrupted"), `reports interrupted (stderr ${JSON.stringify(err.slice(0, 160))})`);
        a.ok(!out.includes("LEAK"), `no user code ran after the interrupt (stdout ${JSON.stringify(out.slice(0, 160))})`);
        a.ok(ms < LIMIT_MS, `exits within ${LIMIT_MS} ms (took ${ms} ms)`);
    });
}

t.test("dynamic import of a module whose body spins", (a) => {
    const dir = makeTempDir();
    try {
        const mod = `${dir}/spin.mjs`;
        writeFile(mod, "function spin(){ for(;;){} }\nspin();\n");
        const src = `import(${JSON.stringify(mod)}).catch((e) => console.log("LEAK", e)).then(() => console.log("LEAK after"));`;
        const r = Exec(BIN, ["--timeout-ms", String(DEADLINE_MS), "--eval", src], { timeoutMs: 10000 });
        const out = String(r.stdout), err = String(r.stderr);
        a.ok(r.code !== 0, `exits non-zero (code ${r.code})`);
        a.ok(err.includes("interrupted"), "reports interrupted");
        a.ok(!out.includes("LEAK"), `no user code ran after the interrupt (stdout ${JSON.stringify(out.slice(0, 160))})`);
    } finally {
        removeAll(dir);
    }
});

t.test("ordinary errors in async code are still catchable", (a) => {
    const src = `(async () => {
        try { await Promise.reject(new Error("x")); } catch (e) { console.log("OK caught", e.message); }
        try { await (async () => { throw new TypeError("y"); })(); } catch (e) { console.log("OK async", e.name); }
        await new Promise(() => { throw 1; }).catch((v) => console.log("OK executor", v));
        await Promise.try(() => { throw 2; }).catch((v) => console.log("OK try", v));
        await Promise.all(${it("throw 3;")}).catch((v) => console.log("OK all", v));
        async function* g() { try { await 0; throw 4; } catch (v) { yield v; } }
        for await (const v of g()) console.log("OK gen", v);
        try { for await (const v of ${it("throw 5;")}); } catch (v) { console.log("OK forawait", v); }
    })();`;
    const r = Exec(BIN, ["--timeout-ms", "5000", "--eval", src], { timeoutMs: 10000 });
    const out = String(r.stdout);
    a.eq(r.code, 0, `exit code (stderr ${JSON.stringify(String(r.stderr).slice(0, 160))})`);
    for (const m of ["OK caught x", "OK async TypeError", "OK executor 1", "OK try 2", "OK all 3", "OK gen 4", "OK forawait 5"])
        a.ok(out.includes(m), `prints ${m}`);
});

await t.run();
