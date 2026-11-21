// covers: c:src/modules/libc/libc-wst-rs.c
// WHATWG Streams, ReadableStream (default controller): cases derived from the spec algorithms
// (https://streams.spec.whatwg.org/) and the shape of the WPT readable-streams/ files, written here because
// the WPT checkout is not vendored. The core lives in C (src/modules/libc/libc-wst-*.c) and its classes are registered
// lazily on the first touch of a Streams global.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_readable");
const tick = () => new Promise((r) => setTimeout(r, 0));
const flush = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };
async function collect(rs) { const out = []; for await (const x of rs) out.push(x); return out; }

t.test("globals are lazy accessors that become writable, non-enumerable data properties", (a) => {
    const names = ["ReadableStream", "ReadableStreamDefaultReader", "ReadableStreamBYOBReader", "ReadableStreamDefaultController",
        "ReadableByteStreamController", "ReadableStreamBYOBRequest", "WritableStream", "WritableStreamDefaultWriter",
        "WritableStreamDefaultController", "TransformStream", "TransformStreamDefaultController", "ByteLengthQueuingStrategy",
        "CountQueuingStrategy", "TextEncoderStream", "TextDecoderStream", "CompressionStream", "DecompressionStream"];
    void globalThis.ReadableStream;
    for (const n of names) {
        const d = Object.getOwnPropertyDescriptor(globalThis, n);
        a.ok(d && typeof d.value === "function", n + " is a data property after first use");
        a.eq(d.enumerable, false, n + " not enumerable");
        a.eq(d.writable && d.configurable, true, n + " writable+configurable");
        a.eq(d.value.name, n, n + ".name");
    }
});

t.test("interface shape: prototype members enumerable, toStringTag, constructor lengths", (a) => {
    const d = Object.getOwnPropertyDescriptor(ReadableStream.prototype, "getReader");
    a.eq(d.enumerable, true, "operations are enumerable");
    a.eq(Object.getOwnPropertyDescriptor(ReadableStream.prototype, "locked").enumerable, true, "attributes are enumerable");
    a.eq(Object.prototype.toString.call(new ReadableStream()), "[object ReadableStream]");
    a.eq(ReadableStream.length, 0);
    a.eq(ReadableStream.prototype.pipeTo.length, 1);
    a.eq(ReadableStream.prototype.cancel.length, 0);
    a.eq(ReadableStreamDefaultReader.length, 1);
    a.eq(ReadableStream.from.length, 1);
    a.eq(ReadableStream.prototype[Symbol.asyncIterator], ReadableStream.prototype.values);
    a.eq(Object.getOwnPropertyDescriptor(ReadableStream.prototype, Symbol.asyncIterator).enumerable, false);
    a.throws(() => new ReadableStreamDefaultController(), TypeError, "controllers are not constructible");
    a.throws(() => ReadableStream(), TypeError, "call without new");
});

t.test("constructor argument validation", (a) => {
    a.throws(() => new ReadableStream(null), TypeError, "null source");
    a.throws(() => new ReadableStream(1), TypeError, "number source");
    a.throws(() => new ReadableStream({ start: 1 }), TypeError, "non-callable start");
    a.throws(() => new ReadableStream({ type: "nope" }), TypeError, "bad type");
    a.throws(() => new ReadableStream({}, { highWaterMark: -1 }), RangeError, "negative hwm");
    a.throws(() => new ReadableStream({}, { highWaterMark: NaN }), RangeError, "NaN hwm");
    a.throws(() => new ReadableStream({}, 5), TypeError, "non-object strategy");
    a.throws(() => new ReadableStream({ type: "bytes" }, { size() { return 1; } }), RangeError, "size on byte stream");
    a.throws(() => new ReadableStream({ type: "bytes", autoAllocateChunkSize: 0 }), TypeError, "autoAllocateChunkSize 0");
    new ReadableStream(undefined, undefined);
    new ReadableStream({}, null);
    a.ok(true);
});

t.test("strategy is converted before the source, and dictionary members in lexicographic order", (a) => {
    const seen = [];
    const src = new Proxy({}, { get(o, k) { seen.push("src." + String(k)); return undefined; } });
    const st = new Proxy({}, { get(o, k) { seen.push("st." + String(k)); return undefined; } });
    new ReadableStream(src, st);
    a.deep(seen, ["st.highWaterMark", "st.size", "src.autoAllocateChunkSize", "src.cancel", "src.pull", "src.start", "src.type"]);
});

t.test("start is called synchronously with the controller as argument and the source as this", (a) => {
    let ctl, self;
    const src = { start(c) { ctl = c; self = this; } };
    new ReadableStream(src);
    a.ok(ctl instanceof ReadableStreamDefaultController);
    a.eq(self, src);
    a.eq(ctl.desiredSize, 1);
});

t.test("enqueue/close/read round trip and reader.closed", async (a) => {
    const rs = new ReadableStream({ start(c) { c.enqueue("a"); c.enqueue("b"); c.close(); } });
    const r = rs.getReader();
    a.ok(rs.locked);
    a.deep(await r.read(), { value: "a", done: false });
    a.deep(await r.read(), { value: "b", done: false });
    a.deep(await r.read(), { value: undefined, done: true });
    a.eq(await r.closed, undefined);
    a.deep(await r.read(), { value: undefined, done: true }, "reads after close stay done");
});

t.test("pull is driven by desiredSize and pending reads", async (a) => {
    let pulls = 0;
    const rs = new ReadableStream({ pull(c) { pulls++; c.enqueue(pulls); } }, { highWaterMark: 2 });
    await flush();
    a.eq(pulls, 2, "fills to the high-water mark");
    const r = rs.getReader();
    a.eq((await r.read()).value, 1);
    await flush();
    a.eq(pulls, 3, "one read refills one");
});

t.test("pull is not reentered while its promise is pending (pullAgain)", async (a) => {
    let active = 0, maxActive = 0, n = 0;
    let release;
    const rs = new ReadableStream({
        pull(c) {
            active++; maxActive = Math.max(maxActive, active);
            return new Promise((res) => { release = () => { active--; c.enqueue(n++); res(); }; });
        },
    }, { highWaterMark: 0 });
    const r = rs.getReader();
    const p1 = r.read(), p2 = r.read();
    await flush();
    release(); await flush();
    release(); await flush();
    a.eq((await p1).value, 0);
    a.eq((await p2).value, 1);
    a.eq(maxActive, 1);
});

t.test("controller.error rejects pending reads and reader.closed", async (a) => {
    let ctl;
    const rs = new ReadableStream({ start(c) { ctl = c; } });
    const r = rs.getReader();
    const p = r.read();
    const boom = new Error("boom");
    ctl.error(boom);
    a.eq(await p.then(() => null, (e) => e), boom);
    a.eq(await r.closed.then(() => null, (e) => e), boom);
    a.eq(ctl.desiredSize, null);
    a.throws(() => ctl.enqueue(1), TypeError, "enqueue after error");
});

t.test("a throwing size() errors the stream and rethrows from enqueue", async (a) => {
    const boom = new Error("size");
    let ctl;
    const rs = new ReadableStream({ start(c) { ctl = c; } }, { size() { throw boom; }, highWaterMark: 5 });
    a.eq(a.throws(() => ctl.enqueue("x")), boom);
    await a.rejects(rs.getReader().closed, Error);
});

t.test("size() returning a negative or infinite number is a RangeError that errors the stream", async (a) => {
    for (const bad of [-1, Infinity, NaN]) {
        let ctl;
        const rs = new ReadableStream({ start(c) { ctl = c; } }, { size() { return bad; } });
        a.throws(() => ctl.enqueue("x"), RangeError, "size " + bad);
        await a.rejects(rs.getReader().closed, RangeError);
    }
});

t.test("desiredSize tracks the queue with a size function", async (a) => {
    let ctl;
    new ReadableStream({ start(c) { ctl = c; } }, { highWaterMark: 10, size: (c) => c.length });
    ctl.enqueue("abcd");
    a.eq(ctl.desiredSize, 6);
    ctl.enqueue("abcdefgh");
    a.eq(ctl.desiredSize, -2);
});

t.test("start rejection errors the stream; pull rejection errors the stream", async (a) => {
    const e1 = new Error("start");
    await a.rejects(new ReadableStream({ start() { return Promise.reject(e1); } }).getReader().closed, Error);
    const e2 = new Error("pull");
    const r = new ReadableStream({ pull() { throw e2; } }).getReader();
    a.eq(await r.read().then(() => null, (e) => e), e2);
});

t.test("cancel calls the source's cancel with the reason, resolves undefined, and closes", async (a) => {
    let got;
    const rs = new ReadableStream({ start(c) { c.enqueue(1); }, cancel(r) { got = r; return "ignored"; } });
    a.eq(await rs.cancel("why"), undefined);
    a.eq(got, "why");
    const r = rs.getReader();
    a.deep(await r.read(), { value: undefined, done: true }, "queued chunks are discarded");
});

t.test("cancel on a locked stream rejects with TypeError; reader.cancel works", async (a) => {
    const rs = new ReadableStream();
    const r = rs.getReader();
    await a.rejects(rs.cancel(), TypeError);
    await r.cancel();
    a.deep(await r.read(), { value: undefined, done: true });
});

t.test("cancel's rejection propagates", async (a) => {
    const e = new Error("c");
    await a.rejects(new ReadableStream({ cancel() { throw e; } }).cancel(), Error);
});

t.test("getReader locks; second getReader throws; releaseLock unlocks and rejects closed", async (a) => {
    const rs = new ReadableStream();
    const r = rs.getReader();
    a.throws(() => rs.getReader(), TypeError);
    a.throws(() => new ReadableStreamDefaultReader(rs), TypeError);
    const pending = r.read();
    r.releaseLock();
    a.ok(!rs.locked);
    await a.rejects(pending, TypeError, "pending read rejects on release");
    await a.rejects(r.closed, TypeError);
    await a.rejects(r.read(), TypeError, "read on a released reader");
    r.releaseLock();
    a.throws(() => rs.getReader({ mode: "bogus" }), TypeError);
    a.throws(() => rs.getReader({ mode: "byob" }), TypeError, "byob reader on a default stream");
});

t.test("reader.closed of a reader acquired after close/error", async (a) => {
    const closed = new ReadableStream({ start(c) { c.close(); } });
    await closed.getReader().closed;
    const e = new Error("x");
    const errored = new ReadableStream({ start(c) { c.error(e); } });
    a.eq(await errored.getReader().closed.then(() => null, (x) => x), e);
});

t.test("close then enqueue is a TypeError; double close is a TypeError", (a) => {
    let ctl;
    new ReadableStream({ start(c) { ctl = c; } });
    ctl.close();
    a.throws(() => ctl.enqueue(1), TypeError);
    a.throws(() => ctl.close(), TypeError);
});

t.test("close with queued chunks drains before closing", async (a) => {
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } });
    a.deep(await collect(rs), [1, 2]);
});

t.test("tee: both branches see every chunk; cancel needs both branches", async (a) => {
    let reason;
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); }, cancel(r) { reason = r; } });
    const [b1, b2] = rs.tee();
    a.ok(rs.locked);
    a.deep(await collect(b1), [1, 2]);
    a.deep(await collect(b2), [1, 2]);
    const src = new ReadableStream({ cancel(r) { reason = r; } });
    const [c1, c2] = src.tee();
    const p1 = c1.cancel("r1");
    await flush();
    a.eq(reason, undefined, "one branch canceled does not cancel the source (and its promise stays pending)");
    await c2.cancel("r2");
    await p1;
    a.deep(reason, ["r1", "r2"], "composite reason");
    const back = new ReadableStream({ cancel(x) { reason = x; } });
    const [d1, d2] = back.tee();
    const q2 = d2.cancel("s2");
    await d1.cancel("s1");
    await q2;
    a.deep(reason, ["s1", "s2"], "composite reason keeps branch order when branch 2 cancels first");
});

t.test("tee: chunk objects are shared, not cloned", async (a) => {
    const obj = { x: 1 };
    const [b1, b2] = new ReadableStream({ start(c) { c.enqueue(obj); c.close(); } }).tee();
    a.eq((await b1.getReader().read()).value, obj);
    a.eq((await b2.getReader().read()).value, obj);
});

t.test("tee: an error reaches both branches", async (a) => {
    let ctl;
    const [b1, b2] = new ReadableStream({ start(c) { ctl = c; } }).tee();
    const e = new Error("t");
    ctl.error(e);
    a.eq(await b1.getReader().closed.then(() => null, (x) => x), e);
    a.eq(await b2.getReader().closed.then(() => null, (x) => x), e);
});

t.test("async iteration: values, break cancels, preventCancel keeps the stream", async (a) => {
    let canceled = false;
    const mk = () => new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.enqueue(3); }, cancel() { canceled = true; } });
    const rs = mk();
    for await (const x of rs) { if (x === 2) break; }
    a.ok(canceled, "break cancels");
    a.ok(!rs.locked, "the lock is released");
    canceled = false;
    const rs2 = mk();
    for await (const x of rs2.values({ preventCancel: true })) { if (x === 1) break; }
    a.ok(!canceled, "preventCancel");
    a.eq((await rs2.getReader().read()).value, 2, "the rest is still readable");
});

t.test("async iterator: next() after finishing is done; return() is idempotent; brand checks", async (a) => {
    const it = new ReadableStream({ start(c) { c.enqueue(1); c.close(); } })[Symbol.asyncIterator]();
    a.deep(await it.next(), { value: 1, done: false });
    a.deep(await it.next(), { value: undefined, done: true });
    a.deep(await it.next(), { value: undefined, done: true });
    a.deep(await it.return("v"), { value: "v", done: true });
    await a.rejects(it.next.call({}), TypeError);
    a.eq(Object.prototype.toString.call(it), "[object ReadableStream AsyncIterator]");
    a.eq(Object.getPrototypeOf(Object.getPrototypeOf(it)), Object.getPrototypeOf(Object.getPrototypeOf((async function* () {}).prototype)), "inherits %AsyncIteratorPrototype%");
});

t.test("async iterator: concurrent next() calls resolve in order", async (a) => {
    const rs = ReadableStream.from([1, 2, 3]);
    const it = rs.values();
    const rs3 = await Promise.all([it.next(), it.next(), it.next(), it.next()]);
    a.deep(rs3.map((r) => r.done ? "done" : r.value), [1, 2, 3, "done"]);
});

t.test("async iterator: an errored stream rejects next() and finishes", async (a) => {
    const e = new Error("i");
    const it = new ReadableStream({ start(c) { c.error(e); } }).values();
    a.eq(await it.next().then(() => null, (x) => x), e);
    a.deep(await it.next(), { value: undefined, done: true });
});

t.test("ReadableStream.from: arrays, generators, async generators; a string is refused (WPT from.any.js: not an object)", async (a) => {
    a.deep(await collect(ReadableStream.from([1, 2])), [1, 2]);
    a.throws(() => ReadableStream.from("ab"), TypeError, "a primitive string is not an iterable object");
    a.deep(await collect(ReadableStream.from(new String("ab"))), ["a", "b"], "a String object iterates");
    a.deep(await collect(ReadableStream.from((function* () { yield 1; yield 2; })())), [1, 2]);
    a.deep(await collect(ReadableStream.from((async function* () { yield 1; await tick(); yield 2; })())), [1, 2]);
    a.deep(await collect(ReadableStream.from([Promise.resolve(7)])), [7], "a sync iterator's promise values are awaited");
    a.throws(() => ReadableStream.from(5), TypeError);
    a.throws(() => ReadableStream.from({ [Symbol.asyncIterator]: 1 }), TypeError);
    a.throws(() => ReadableStream.from({ [Symbol.asyncIterator]() { return 1; } }), TypeError);
});

t.test("ReadableStream.from: pulls lazily (highWaterMark 0) and cancel calls return(reason)", async (a) => {
    let pulled = 0, returned;
    const it = { [Symbol.asyncIterator]() { return this; }, next() { pulled++; return Promise.resolve({ value: pulled, done: false }); }, return(r) { returned = r; return Promise.resolve({ done: true }); } };
    const rs = ReadableStream.from(it);
    await flush();
    a.eq(pulled, 0, "no pull before a read");
    const r = rs.getReader();
    a.eq((await r.read()).value, 1);
    await r.cancel("stop");
    a.eq(returned, "stop");
});

t.test("ReadableStream.from: a non-object iterator result errors the stream", async (a) => {
    const it = { [Symbol.asyncIterator]() { return { next() { return Promise.resolve(5); } }; } };
    await a.rejects(ReadableStream.from(it).getReader().read(), TypeError);
});

t.test("a patched Promise.prototype.then is not observed by internal reads (tee, iteration)", async (a) => {
    const orig = Promise.prototype.then;
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.close(); } });
    const [b1, b2] = rs.tee();
    let hit = 0;
    Promise.prototype.then = function (...args) { hit++; return orig.apply(this, args); };
    try { await tick(); } finally { Promise.prototype.then = orig; }
    a.eq(hit, 0, "teeing and pulling did not call the patched then");
    a.deep(await collect(b1), [1]);
    a.deep(await collect(b2), [1]);
});

t.test("brand checks on getters and methods", async (a) => {
    a.throws(() => Object.getOwnPropertyDescriptor(ReadableStream.prototype, "locked").get.call({}), TypeError);
    await a.rejects(ReadableStream.prototype.cancel.call({}), TypeError);
    await a.rejects(ReadableStream.prototype.pipeTo.call({}, new WritableStream()), TypeError);
    a.throws(() => ReadableStream.prototype.tee.call({}), TypeError);
    await a.rejects(ReadableStreamDefaultReader.prototype.read.call({}), TypeError);
});

t.test("pipeTo: a chunk read in the same turn as abort, close or error is still written (the write runs one microtask after the read)", async (a) => {
    const probe = async (opts, act) => {
        let con; const seen = [];
        const rs = new ReadableStream({ start(c) { con = c; } }, { highWaterMark: 0 });
        const ac = new AbortController();
        const p = rs.pipeTo(new WritableStream({ write(c) { seen.push(c); } }), { signal: ac.signal, ...opts }).then(() => "ok", (e) => e.name);
        await Promise.resolve(); await Promise.resolve();
        act(con, ac);
        const r = await p;
        return [seen.join(), r];
    };
    a.deep(await probe({ preventAbort: true }, (c, ac) => { c.enqueue("a"); ac.abort(); }), ["a", "AbortError"], "abort with preventAbort");
    a.deep(await probe({}, (c, ac) => { c.enqueue("a"); ac.abort(); }), ["a", "AbortError"], "abort");
    a.deep(await probe({}, (c) => { c.enqueue("a"); c.close(); }), ["a", "ok"], "close");
    a.deep(await probe({}, (c) => { c.enqueue("a"); c.error(new RangeError("x")); }), ["a", "RangeError"], "error");
});

t.test("read results are CreateIterResultObject: Object.prototype, own value then done, data properties C/W/E", async (a) => {
    const shape = (o) => [Object.getPrototypeOf(o) === Object.prototype, Reflect.ownKeys(o).join(),
        ...["value", "done"].map((k) => { const d = Object.getOwnPropertyDescriptor(o, k); return [d.writable, d.enumerable, d.configurable, "get" in d].join(); })].join("|");
    const want = "true|value,done|true,true,true,false|true,true,true,false";
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.close(); } });
    const rd = rs.getReader();
    const r1 = await rd.read(), r2 = await rd.read();
    a.eq(shape(r1), want, "chunk result shape");
    a.eq(shape(r2), want, "done result shape");
    a.deep([r1.value, r1.done, r2.value, r2.done], [1, false, undefined, true], "values and done flags");
    r1.extra = 1; a.eq(Object.keys(r1).join(), "value,done,extra", "results are ordinary extensible objects");
    const r3 = await rd.read(); a.ok(r3 !== r2, "each read result is a fresh object");
    const it = new ReadableStream({ start(c) { c.enqueue("x"); c.close(); } }).values();
    const i1 = await it.next(), i2 = await it.next();
    a.eq(shape(i1) + "/" + shape(i2), want + "/" + want, "async iterator results");
    a.deep([i1.value, i1.done, i2.done], ["x", false, true], "iterator flags");
    const b = new ReadableStream({ type: "bytes", start(c) { c.enqueue(new Uint8Array([7])); c.close(); } }).getReader({ mode: "byob" });
    const b1 = await b.read(new Uint8Array(4)), b2 = await b.read(new Uint8Array(4));
    a.eq(shape(b1) + "/" + shape(b2), want + "/" + want, "byob results");
    a.deep([b1.value[0], b1.done, b2.done, b2.value.byteLength], [7, false, true, 0], "byob flags");
});

await t.run();
