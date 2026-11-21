// covers: integration c:src/modules/libc/libc-wst-core.c c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-ws.c c:src/modules/libc/libc-wst-ts.c c:src/modules/libc/libc-wst-bytes.c c:src/modules/libc/libc-wst-misc.c c:src/modules/libc/libc-wst-pipe.c c:src/modules/libc/libc-wst-adapt.c c:src/modules/stream/stream-cps.c c:src/builtins/builtin-promise.c c:src/mm/js_malloc.inc.c -- the --timeout-ms interrupt stays uncatchable through web streams
// The interrupt that --timeout-ms raises is uncatchable. Stream algorithms
// (start, pull, write, close, abort, cancel, transform, flush, size), the
// thenables they return, the pipe engine and the dyna:stream duck adapters
// used to turn it into an ordinary stream error, so user code saw it ("LEAK"
// lines) and a pull loop could even exit 0 after the deadline.
// Each case runs in its own child process.
import { args, Exec } from "dyna:sys";
import { suite } from "../lib/kit.js";

const BIN = args()[0];
const DEADLINE_MS = 200;
const LIMIT_MS = 3000;
const t = suite("web_streams_interrupt");

const pre = "function spin(){ for(;;){} }\nconst leak = (e) => console.log(\"LEAK\", e);\nsetTimeout(() => console.log(\"LEAK timer\"), 100);\n";
const ds = (body) => `import("dyna:stream").then(({ toReadableStream, toWritableStream, lines }) => { ${body} });`;
const thenSpin = "{ get then() { spin(); } }";

const CASES = [
    ["ReadableStream pull", `new ReadableStream({ pull() { spin(); } }).getReader().read().catch(leak);`],
    ["ReadableStream pull loop", `(async () => { const r = new ReadableStream({ pull(c) { for (let i = 0; i < 1e6; i++) {} c.enqueue(1); } }).getReader(); for (;;) { try { await r.read(); } catch (e) { leak(e); return; } } })();`],
    ["ReadableStream pull loop, no catch", `(async () => { const r = new ReadableStream({ pull(c) { for (let i = 0; i < 1e6; i++) {} c.enqueue(1); } }).getReader(); for (;;) await r.read(); })();`],
    ["ReadableStream pull returns a then getter", `new ReadableStream({ pull() { return ${thenSpin}; } }).getReader().read().catch(leak);`],
    ["ReadableStream start", `try { new ReadableStream({ start() { spin(); } }); } catch (e) { leak(e); } console.log("LEAK after ctor");`],
    ["ReadableStream start returns a then getter", `try { new ReadableStream({ start() { return ${thenSpin}; } }); } catch (e) { leak(e); } console.log("LEAK after ctor");`],
    ["ReadableStream cancel", `new ReadableStream({ cancel() { spin(); } }).cancel().catch(leak); console.log("LEAK after cancel");`],
    ["ReadableStream size", `new ReadableStream({ start(c) { try { c.enqueue(1); } catch (e) { leak(e); } } }, { size() { spin(); } }); console.log("LEAK after ctor");`],
    ["owning ReadableStream structuredClone", `new ReadableStream({ type: "owning", start(c) { globalThis.structuredClone = () => spin(); try { c.enqueue({}, { transfer: [] }); } catch (e) { leak(e); } } }); console.log("LEAK after ctor");`],
    ["byte ReadableStream pull", `new ReadableStream({ type: "bytes", pull() { spin(); } }).getReader().read().catch(leak);`],
    ["byte ReadableStream BYOB pull", `new ReadableStream({ type: "bytes", pull() { spin(); } }).getReader({ mode: "byob" }).read(new Uint8Array(4)).catch(leak);`],
    ["ReadableStream.from next", `ReadableStream.from({ [Symbol.asyncIterator]() { return { next() { spin(); } }; } }).getReader().read().catch(leak);`],
    ["ReadableStream tee pull", `new ReadableStream({ pull() { spin(); } }).tee()[0].getReader().read().catch(leak);`],
    ["ReadableStream async iterator", `(async () => { try { for await (const x of new ReadableStream({ pull() { spin(); } })); } catch (e) { leak(e); } })();`],
    ["WritableStream start", `try { new WritableStream({ start() { spin(); } }); } catch (e) { leak(e); } console.log("LEAK after ctor");`],
    ["WritableStream write", `new WritableStream({ write() { spin(); } }).getWriter().write(1).catch(leak);`],
    ["WritableStream write returns a then getter", `new WritableStream({ write() { return ${thenSpin}; } }).getWriter().write(1).catch(leak);`],
    ["WritableStream close", `new WritableStream({ close() { spin(); } }).getWriter().close().catch(leak);`],
    ["WritableStream abort", `new WritableStream({ abort() { spin(); } }).abort().catch(leak);`],
    ["WritableStream size", `new WritableStream({}, { size() { spin(); } }).getWriter().write(1).catch(leak); console.log("LEAK after write");`],
    ["TransformStream start", `try { new TransformStream({ start() { spin(); } }); } catch (e) { leak(e); } console.log("LEAK after ctor");`],
    ["TransformStream transform", `const ts = new TransformStream({ transform() { spin(); } }); ts.readable.getReader().read().catch(leak); ts.writable.getWriter().write(1).catch(leak);`],
    ["TransformStream transform returns a then getter", `const ts = new TransformStream({ transform() { return ${thenSpin}; } }); ts.readable.getReader().read().catch(leak); ts.writable.getWriter().write(1).catch(leak);`],
    ["TransformStream flush", `new TransformStream({ flush() { spin(); } }).writable.getWriter().close().catch(leak);`],
    ["TransformStream cancel", `new TransformStream({ cancel() { spin(); } }).readable.cancel().catch(leak);`],
    ["pipeTo a spinning sink", `new ReadableStream({ start(c) { c.enqueue(1); } }).pipeTo(new WritableStream({ write() { spin(); } })).catch(leak);`],
    ["pipeTo from a spinning source", `new ReadableStream({ pull() { spin(); } }).pipeTo(new WritableStream()).catch(leak);`],
    ["pipeThrough a spinning transform", `new ReadableStream({ start(c) { c.enqueue(1); c.close(); } }).pipeThrough(new TransformStream({ transform() { spin(); } })).pipeTo(new WritableStream()).catch(leak);`],
    ["pipeTo abort signal, source cancel spins", `const ac = new AbortController(); new ReadableStream({ cancel() { spin(); } }).pipeTo(new WritableStream(), { signal: ac.signal }).catch(leak); ac.abort();`],
    ["pipeTo abort signal, sink abort spins", `const ac = new AbortController(); new ReadableStream().pipeTo(new WritableStream({ abort() { spin(); } }), { signal: ac.signal }).catch(leak); ac.abort();`],
    ["pipeTo source error, sink abort spins", `new ReadableStream({ start(c) { c.error(1); } }).pipeTo(new WritableStream({ abort() { spin(); } })).catch(leak);`],
    ["pipeTo sink error, source cancel spins", `new ReadableStream({ cancel() { spin(); } }).pipeTo(new WritableStream({ start(c) { c.error(1); } })).catch(leak);`],
    ["Response body from a spinning stream", `new Response(new ReadableStream({ pull() { spin(); } })).text().catch(leak);`],
    ["toReadableStream duck read", ds(`toReadableStream({ read() { spin(); }, close() {} }).getReader().read().catch(leak);`)],
    ["toReadableStream duck read returns a then getter", ds(`toReadableStream({ read() { return ${thenSpin}; }, close() {} }).getReader().read().catch(leak);`)],
    ["toReadableStream duck read(buf)", ds(`toReadableStream({ read(b) { spin(); }, close() {} }).getReader({ mode: "byob" }).read(new Uint8Array(4)).catch(leak);`)],
    ["toWritableStream duck write", ds(`toWritableStream({ write() { spin(); }, close() {} }).getWriter().write(new Uint8Array(1)).catch(leak);`)],
    ["toWritableStream duck write returns a then getter", ds(`toWritableStream({ write() { return ${thenSpin}; }, close() {} }).getWriter().write(new Uint8Array(1)).catch(leak);`)],
    ["toWritableStream duck close", ds(`toWritableStream({ write() {}, close() { spin(); } }).getWriter().close().catch(leak);`)],
    ["duck to duck pipe, spinning sink", ds(`toReadableStream({ read(b) { b[0] = 1; return Promise.resolve(1); }, close() {} }).pipeTo(toWritableStream({ write() { spin(); }, close() {} })).catch(leak);`)],
    ["duck to duck pipe, spinning source", ds(`toReadableStream({ read() { spin(); }, close() {} }).pipeTo(toWritableStream({ write() {}, close() {} })).catch(leak);`)],
    ["lines over a spinning duck", ds(`(async () => { try { for await (const l of lines({ read() { spin(); }, close() {} })); } catch (e) { leak(e); } })();`)],
    ["duck to duck pipe, read returns a spinning thenable", ds(`toReadableStream({ read() { return { then() { spin(); } }; }, close() {} }).pipeTo(toWritableStream({ write() {}, close() {} })).catch(leak);`)],
    ["toReadableStream cancel, duck close", ds(`toReadableStream({ read() {}, close() { spin(); } }).cancel().then(() => console.log("LEAK resolved"), leak);`)],
    ["toWritableStream abort, duck close", ds(`toWritableStream({ write() {}, close() { spin(); } }).abort().then(() => console.log("LEAK resolved"), leak);`)],
];

for (const [name, body] of CASES) {
    t.test(name, (a) => {
        const t0 = Date.now();
        const r = Exec(BIN, ["--timeout-ms", String(DEADLINE_MS), "--eval", pre + body], { timeoutMs: 10000 });
        const ms = Date.now() - t0, out = String(r.stdout), err = String(r.stderr);
        a.ok(r.code !== 0, `exits non-zero (code ${r.code})`);
        a.ok(err.includes("interrupted"), `reports interrupted (stderr ${JSON.stringify(err.slice(0, 160))})`);
        a.ok(!out.includes("LEAK"), `no user code ran after the interrupt (stdout ${JSON.stringify(out.slice(0, 160))})`);
        a.ok(ms < LIMIT_MS, `exits within ${LIMIT_MS} ms (took ${ms} ms)`);
    });
}

t.test("ordinary errors in stream algorithms still error the stream", (a) => {
    const src = `(async () => {
        await new ReadableStream({ pull() { throw 1; } }).getReader().read().catch((v) => console.log("OK pull", v));
        await new WritableStream({ write() { throw 2; } }).getWriter().write(0).catch((v) => console.log("OK write", v));
        { const ts = new TransformStream({ transform() { throw 3; } }); ts.readable.getReader().read().catch(() => {}); await ts.writable.getWriter().write(0).catch((v) => console.log("OK transform", v)); }
        await new ReadableStream({ start(c) { c.enqueue(1); } }).pipeTo(new WritableStream({ write() { throw 4; } })).catch((v) => console.log("OK pipe", v));
        try { new ReadableStream({ start(c) { c.enqueue(1); } }, { size() { throw 5; } }); } catch (v) { console.log("OK size", v); }
        await new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } }).pipeTo(new WritableStream({ write(v) { console.log("OK wrote", v); } })).then(() => console.log("OK piped"));
        { const ac = new AbortController(); const p = new ReadableStream().pipeTo(new WritableStream(), { signal: ac.signal }); ac.abort(7); await p.catch((v) => console.log("OK aborted", v)); }
        const { toReadableStream } = await import("dyna:stream");
        await toReadableStream({ read() { throw 6; }, close() {} }).getReader().read().catch((v) => console.log("OK duck", v));
    })();`;
    const r = Exec(BIN, ["--timeout-ms", "5000", "--eval", src], { timeoutMs: 10000 });
    const out = String(r.stdout);
    a.eq(r.code, 0, `exit code (stderr ${JSON.stringify(String(r.stderr).slice(0, 160))})`);
    for (const m of ["OK pull 1", "OK write 2", "OK transform 3", "OK pipe 4", "OK wrote 1", "OK wrote 2", "OK piped", "OK aborted 7", "OK duck 6"])
        a.ok(out.includes(m), `prints ${m} (stdout ${JSON.stringify(out.slice(0, 300))})`);
});

await t.run();
