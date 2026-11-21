// flags: --std
// covers: integration c:src/modules/libc/libc-wst-ws.c c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-ts.c -- per-instance memory budgets span the three stream kinds
// Per-instance memory budgets for web streams: N streams are built synchronously, their start reactions drain at a macrotask boundary,
// then GC runs; the malloc growth per instance must stay under the budget. Budgets pin the v10b-8 layout: an algorithm slot's kind is
// derived from its fields (40 B), flags are bytes, the writable's close/abort state lives in a block allocated on the first close or
// abort, and the one-shot close/abort reactions are not cached on the controller; v10b-9: stream, controller, reader, writer, promise-reaction and internal promise objects start with a zero-capacity property array (JS_NewObjectProtoClassBare). Budgets sit between the old and new layouts, so
// a regression to the old layout fails. Ordinary objects with the stream prototypes are created (and kept empty) before any stream, so the zero-capacity lookup must not hand stream objects the ordinary 2-slot empty shape (find_hashed_shape_proto matches the capacity class). mallocSize counts allocator arenas in release builds (block size classes, freed transients
// v10b-20: stream objects are their own cached pull/write/transform reactions, controller objects are created on first JS use (native and default sources never create one), and a non-object start result settles through a direct job, so the budgets dropped again and fromFile/toFile (native adapters, no controller object) are pinned too. mallocSize counts allocator arenas in release builds (block size classes, freed transients
// kept in the arena) and exact malloc sizes in sanitizer builds, which are always smaller, so one budget serves both.
import { suite } from "../lib/kit.js";
import { memoryUsage } from "dyna:sys";
import { fromFile, toFile } from "dyna:stream";

const t = suite("web_streams_mem");
const ordinary = [TransformStream, ReadableStream, WritableStream, ReadableStreamDefaultController, WritableStreamDefaultController, TransformStreamDefaultController, Promise].map((C) => Object.create(C.prototype));
const N = 20000;
const kinds = {
    TransformStream: [() => new TransformStream(), 1790],
    WritableStream: [() => new WritableStream(), 590],
    ReadableStream: [() => new ReadableStream(), 450],
    "WritableStream+writer": [() => { const s = new WritableStream({ write() {} }); return [s, s.getWriter()]; }, 1425],
    "ReadableStream+reader": [() => { const s = new ReadableStream({ pull() {} }); return [s, s.getReader()]; }, 1210],
    "dyna:stream fromFile": [() => fromFile(import.meta.url.replace(/^file:\/\//, "")), 950],
    "dyna:stream toFile": [() => toFile(std.getenv("TMPDIR") ? std.getenv("TMPDIR") + "/wsm.out" : "/dev/null"), 1060],
};

async function perInstance(mk) {
    mk();
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const before = memoryUsage().mallocSize;
    const keep = [];
    for (let i = 0; i < N; i++) { const v = mk(); if (Array.isArray(v)) keep.push(v[0], v[1]); else keep.push(v); }
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const per = (memoryUsage().mallocSize - before) / N;
    keep.length = 0;
    return per;
}

for (const [name, [mk, budget]] of Object.entries(kinds)) {
    t.test(`${name}: malloc bytes per instance <= ${budget}`, async (a) => {
        const per = await perInstance(mk);
        a.ok(per <= budget, `${name} ${per.toFixed(0)} B/inst > ${budget}`);
    });
}

t.test("ReadableStream: malloc bytes per instance before the start reactions drain <= 560", async (a) => {
    new ReadableStream();
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const before = memoryUsage().mallocSize;
    const keep = [];
    for (let i = 0; i < N; i++) keep.push(new ReadableStream());
    const per = (memoryUsage().mallocSize - before) / N;
    a.ok(per <= 560, `sync ${per.toFixed(0)} B/inst > 560: a non-object start result must settle through a direct job, not a reaction object`);
    await new Promise((r) => setTimeout(r, 0));
    keep.length = 0;
});

t.test("a closed and an aborted writable allocate their close/abort state lazily and still settle", async (a) => {
    const log = [];
    const w1 = new WritableStream({ close() { log.push("close"); } });
    await w1.getWriter().close();
    const w2 = new WritableStream({ abort(r) { log.push("abort " + r); } });
    await w2.abort("x");
    const w3 = new WritableStream({ write() { return new Promise(() => {}); } });
    const wr = w3.getWriter();
    wr.write(1).catch(() => {});
    const c = wr.close();
    const ab = wr.abort("y");
    ab.catch(() => {});
    const ab2 = wr.abort("z");
    ab2.catch(() => {});
    wr.closed.catch(() => {});
    wr.ready.catch(() => {});
    let cs = "pending";
    c.then(() => { cs = "closed"; }, (e) => { cs = "rejected " + e; });
    await new Promise((r) => setTimeout(r, 0));
    a.eq(log.join(","), "close,abort x");
    a.eq(cs, "rejected y", "abort before start completes: finish-erroring rejects the queued close with the stored error");
    a.ok(ab instanceof Promise);
    a.ok(ab2 === ab, "a second abort while one is pending returns the pending abort request promise (WritableStreamAbort step 7)");
});

globalThis.__ordinary = ordinary;
await t.run();
