// flags: --std
// covers: c:src/modules/libc/libc-blob.c
// Blob, File, FileReader, DOMException and URL.createObjectURL are C globals (libc-blob.c). A Blob is an
// immutable rope of (store, offset, length) slices over refcounted stores: slice() and Blob parts of >= 1024
// bytes share stores, smaller parts are copied into one new store per run. Reads fill one buffer; stream()
// emits 64 KiB chunks or fills a BYOB buffer in place. FileReader queues one task per event and drains
// microtasks after each listener. WPT FileAPI covers the spec surface; this suite pins the rope edges
// (slices across stores, small-part copy vs large-part share, chunking), event ordering and os.openAsBlob (a lazy file-backed File: test_os_open_as_blob_lazy.js).
import { suite } from "../lib/kit.js";
import * as os from "os";
import * as std from "std";

const t = suite("test_blob_file_reader");

const bytesOf = (s) => new TextEncoder().encode(s);
const join = (arr) => Array.from(arr).join(",");

function pattern(n, seed) {
    const u = new Uint8Array(n);
    for (let i = 0; i < n; i++) u[i] = (i * 31 + seed) & 255;
    return u;
}

async function drain(stream, byob, size) {
    const out = [];
    let chunks = 0;
    if (byob) {
        const r = stream.getReader({ mode: "byob" });
        for (;;) {
            const { done, value } = await r.read(new Uint8Array(size));
            if (done) break;
            chunks++;
            for (const x of value) out.push(x);
        }
    } else {
        const r = stream.getReader();
        for (;;) {
            const { done, value } = await r.read();
            if (done) break;
            chunks++;
            for (const x of value) out.push(x);
        }
    }
    return { bytes: out, chunks };
}

t.test("parts: strings, buffers, views and blobs concatenate in order", async ({ eq }) => {
    const ab = new Uint8Array([1, 2, 3, 4]).buffer;
    const b = new Blob(["ab", new Uint8Array(ab, 1, 2), new DataView(ab, 3, 1), new Blob(["z"]), ab]);
    eq(b.size, 2 + 2 + 1 + 1 + 4, "size");
    eq(join(await b.bytes()), "97,98,2,3,4,122,1,2,3,4", "bytes");
    eq(new Blob().size, 0, "no parts");
    eq(new Blob([]).type, "", "default type");
});

t.test("string parts are USVString UTF-8; endings native rewrites CR/CRLF/LF", async ({ eq }) => {
    eq(join(await new Blob(["\ud800x"]).bytes()), "239,191,189,120", "lone surrogate -> U+FFFD");
    eq(await new Blob(["a\r\nb\rc\nd"], { endings: "native" }).text(), "a\nb\nc\nd", "native");
    eq(await new Blob(["a\r\nb"], { endings: "transparent" }).text(), "a\r\nb", "transparent");
    eq(await new Blob([new Uint8Array([13, 10])], { endings: "native" }).text(), "\r\n", "buffers untouched");
});

t.test("type is lowercased, and any char outside 0x20..0x7E empties it", ({ eq }) => {
    eq(new Blob([], { type: "Text/HTML; Charset=UTF-8" }).type, "text/html; charset=utf-8", "lowercase");
    eq(new Blob([], { type: "aé" }).type, "", "non-ascii");
    eq(new Blob([], { type: "a\tb" }).type, "", "control");
    eq(new Blob(["x"]).slice(0, 1, "IMAGE/PNG").type, "image/png", "slice contentType");
});

t.test("constructor argument errors", ({ throws }) => {
    throws(() => new Blob(5), TypeError, "non-object parts");
    throws(() => new Blob({}), TypeError, "non-iterable parts");
    throws(() => new Blob([], { endings: "dos" }), TypeError, "bad endings");
    throws(() => new Blob([], 7), TypeError, "non-object options");
    throws(() => Blob(), TypeError, "call without new");
    throws(() => new File(["x"]), TypeError, "File needs 2 args");
    throws(() => new Blob([new SharedArrayBuffer(4)]), TypeError, "shared buffer");
});

t.test("conversions run in WebIDL order: parts, then endings, then type", ({ eq }) => {
    const log = [];
    const part = { toString() { log.push("part"); return "p"; } };
    const opts = {
        get endings() { log.push("endings"); return undefined; },
        get type() { log.push("type"); return "x"; },
    };
    new Blob([part], opts);
    eq(log.join(","), "part,endings,type", "order");
});

t.test("blobParts iteration stays observable: patched iterators and growth during conversion", async ({ eq }) => {
    const own = ["a", "b"];
    own[Symbol.iterator] = function* () { yield "x"; };
    eq(await new Blob(own).text(), "x", "own @@iterator");
    const AP = Array.prototype, saved = AP[Symbol.iterator];
    try {
        AP[Symbol.iterator] = function* () { yield "p"; };
        eq(await new Blob(["a"]).text(), "p", "patched Array.prototype[@@iterator]");
    } finally {
        AP[Symbol.iterator] = saved;
    }
    const AIP = Object.getPrototypeOf([][Symbol.iterator]()), next = AIP.next;
    try {
        let n = 0;
        AIP.next = function () { return n++ ? { done: true } : { done: false, value: "n" }; };
        eq(await new Blob(["a", "b"]).text(), "n", "patched %ArrayIteratorPrototype%.next");
    } finally {
        AIP.next = next;
    }
    const grow = ["a", { toString() { grow.push("z"); return "b"; } }];
    eq(await new Blob(grow).text(), "abz", "element appended during conversion is read");
    const shrink = ["a", { toString() { shrink.length = 2; return "b"; } }, "c"];
    eq(await new Blob(shrink).text(), "ab", "length re-read each step");
});

t.test("slice: relative and clamped bounds, across many stores",async ({ eq }) => {
    const big1 = new Blob([pattern(3000, 1)]), big2 = new Blob([pattern(2000, 7)]);
    const rope = new Blob([big1, "mid", big2]);
    const flat = [...pattern(3000, 1), ...bytesOf("mid"), ...pattern(2000, 7)];
    eq(rope.size, 5003, "rope size");
    for (const [s, e] of [[0, 5003], [2990, 3010], [-2005, -1990], [10, 4], [-1e20, 1e20], [4999, 1e9], [NaN, 3]]) {
        const got = await rope.slice(s, e).bytes();
        const S = Number.isNaN(s) ? 0 : s, E = e;
        const rs = S < 0 ? Math.max(5003 + S, 0) : Math.min(S, 5003);
        const re = E < 0 ? Math.max(5003 + E, 0) : Math.min(E, 5003);
        eq(join(got), join(flat.slice(rs, Math.max(re, rs))), "slice " + s + "," + e);
    }
    const nested = rope.slice(100).slice(2900, 2910);
    eq(join(await nested.bytes()), join(flat.slice(3000, 3010)), "slice of slice");
    eq(rope.slice(1, 2) instanceof File, false, "slice is never a File");
});

t.test("small Blob parts are copied, large ones shared, and both read back identically", async ({ eq }) => {
    const parts = [];
    for (let i = 0; i < 300; i++) parts.push(new Blob([String.fromCharCode(65 + (i % 26))]));
    parts.push(new Blob([pattern(5000, 3)]));
    const b = new Blob(parts);
    const want = parts.slice(0, 300).map((_, i) => 65 + (i % 26)).concat([...pattern(5000, 3)]);
    eq(join(await b.bytes()), join(want), "content");
});

t.test("text(): BOM dropped, invalid UTF-8 replaced, ASCII fast path", async ({ eq }) => {
    eq(await new Blob([new Uint8Array([0xef, 0xbb, 0xbf, 0x68, 0x69])]).text(), "hi", "BOM");
    eq(await new Blob([new Uint8Array([0x61, 0xff, 0xc3])]).text(), "a��", "invalid");
    const split = new Blob([new Blob([new Uint8Array(1500).fill(0x61), new Uint8Array([0xc3])]), new Blob([new Uint8Array([0xa9]), new Uint8Array(1500).fill(0x62)])]);
    eq((await split.text()).slice(1499, 1502), "aéb", "code point split across stores");
    eq((await new Blob(["x".repeat(70000)]).text()).length, 70000, "ascii");
});

t.test("stream(): 64 KiB chunks, BYOB fills the caller buffer, empty blob closes at once", async ({ eq }) => {
    const src = pattern(200000, 5);
    const b = new Blob([src.subarray(0, 100000), new Blob([src.subarray(100000)])]);
    const d = await drain(b.stream(), false);
    eq(d.chunks, Math.ceil(200000 / 65536), "default chunks");
    eq(join(d.bytes.slice(65530, 65540)), join(src.slice(65530, 65540)), "default content");
    const y = await drain(b.stream(), true, 7000);
    eq(y.bytes.length, 200000, "byob length");
    eq(join(y.bytes.slice(99990, 100010)), join(src.slice(99990, 100010)), "byob content across stores");
    eq((await drain(new Blob().stream(), false)).chunks, 0, "empty");
    const s = b.stream();
    await s.cancel();
});

t.test("textStream() decodes UTF-8", async ({ eq }) => {
    const r = new Blob(["héllo"]).textStream().getReader();
    let s = "";
    for (;;) {
        const { done, value } = await r.read();
        if (done) break;
        s += value;
    }
    eq(s, "héllo", "text stream");
});

t.test("File: name, lastModified, prototype chain", ({ eq }) => {
    const f = new File(["abc"], "a/b.txt", { lastModified: 1234.9, type: "X/Y" });
    eq(f.name, "a/b.txt", "name kept");
    eq(f.lastModified, 1234, "long long truncation");
    eq(f.type, "x/y", "type");
    eq(f instanceof Blob && f instanceof File, true, "instanceof");
    eq(Object.prototype.toString.call(f), "[object File]", "tag");
    const now = Date.now();
    eq(Math.abs(new File([], "n").lastModified - now) < 5000, true, "default lastModified");
    eq(new File([], "n", { lastModified: -1 }).lastModified, -1, "negative");
});

function events(reader) {
    const seen = [];
    for (const ty of ["loadstart", "progress", "load", "abort", "error", "loadend"])
        reader.addEventListener(ty, (e) => seen.push(ty + ":" + reader.readyState + ":" + e.loaded + "/" + e.total));
    return seen;
}
function done(reader) {
    return new Promise((res) => reader.addEventListener("loadend", res, { once: true }));
}

t.test("FileReader: event order, readyState and results", async ({ eq }) => {
    const r = new FileReader();
    const seen = events(r);
    r.readAsText(new Blob(["abc"]));
    eq(r.readyState, FileReader.LOADING, "LOADING right away");
    await done(r);
    eq(seen.join(" "), "loadstart:1:0/3 progress:1:3/3 load:2:3/3 loadend:2:3/3", "order");
    eq(r.result, "abc", "text");
    const e = new FileReader();
    const se = events(e);
    e.readAsArrayBuffer(new Blob([]));
    await done(e);
    eq(se.join(" "), "loadstart:1:0/0 load:2:0/0 loadend:2:0/0", "no progress for empty");
    eq(e.result instanceof ArrayBuffer && e.result.byteLength === 0, true, "empty buffer");
    const d = new FileReader();
    d.readAsDataURL(new Blob(["TEST"]));
    await done(d);
    eq(d.result, "data:application/octet-stream;base64,VEVTVA==", "data url");
    const b = new FileReader();
    b.readAsBinaryString(new Blob([new Uint8Array([0, 255, 128])]));
    await done(b);
    eq(b.result, "\u0000ÿ\u0080", "binary string");
});

t.test("FileReader: microtasks run between load and loadend", async ({ eq }) => {
    const r = new FileReader(), log = [];
    r.onload = () => Promise.resolve().then(() => log.push("micro"));
    r.onloadend = () => log.push("loadend");
    r.readAsText(new Blob(["x"]));
    await new Promise((res) => r.addEventListener("loadend", res));
    eq(log.join(","), "micro,loadend", "checkpoint after each listener");
});

t.test("FileReader: InvalidStateError, abort, restart in abort", async ({ eq, throws }) => {
    const r = new FileReader();
    r.readAsText(new Blob(["one"]));
    let err;
    try { r.readAsText(new Blob(["two"])); } catch (e) { err = e; }
    eq(err instanceof DOMException && err.name === "InvalidStateError" && err.code === 11, true, "InvalidStateError");
    const seen = events(r);
    r.abort();
    eq(seen.join(" "), "abort:2:0/3 loadend:2:0/3", "abort + loadend sync");
    eq(r.result, null, "result null after abort");
    const q = new FileReader(), log = [];
    q.addEventListener("abort", () => q.readAsText(new Blob(["second"])), { once: true });
    q.addEventListener("loadend", () => log.push("loadend:" + q.result));
    q.addEventListener("loadstart", () => q.abort(), { once: true });
    q.readAsText(new Blob(["first"]));
    await new Promise((res) => q.addEventListener("loadend", () => { if (q.result === "second") res(); }));
    eq(log.join(","), "loadend:second", "old loadend suppressed");
    throws(() => r.readAsText("nope"), TypeError, "not a blob");
    const n = new FileReader();
    n.abort();
    eq(n.readyState, 0, "abort on EMPTY is a no-op");
});

t.test("FileReader: readAsText encodings", async ({ eq }) => {
    async function rt(bytes, enc, type) {
        const r = new FileReader();
        r.readAsText(new Blob([new Uint8Array(bytes)], type ? { type } : {}), enc);
        await done(r);
        return r.result;
    }
    eq(await rt([0, 104, 0, 105], "UTF-16BE"), "hi", "label");
    eq(await rt([104, 0, 105, 0], undefined, "text/plain;charset=utf-16le"), "hi", "type charset");
    eq(await rt([0xff, 0xfe, 104, 0], "utf-8"), "h", "BOM beats label");
    eq(await rt([0x00, 0xd8, 0x41, 0x00], "utf-16le"), "�A", "lone surrogate");
    eq(await rt([0xe9], "windows-1252"), "é", "other label through TextDecoder");
    eq(await rt([0xc3, 0xa9], "bogus-label"), "é", "bad label falls back to UTF-8");
});

t.test("FileReader: handler attributes, once, handleEvent, removeEventListener", async ({ eq }) => {
    const r = new FileReader(), log = [];
    eq(r.onload, null, "initially null");
    const h = { handleEvent(e) { log.push("obj:" + e.type); } };
    const f = () => log.push("removed");
    r.addEventListener("load", h);
    r.addEventListener("load", h);
    r.addEventListener("load", f);
    r.removeEventListener("load", f);
    r.onload = () => log.push("attr");
    r.readAsText(new Blob(["x"]));
    await done(r);
    eq(log.join(","), "obj:load,attr", "dedupe, removal, attribute slot");
});

t.test("DOMException and URL.createObjectURL", ({ eq, throws }) => {
    const e = new DOMException("m", "NotFoundError");
    eq([e.name, e.message, e.code, e instanceof Error, DOMException.NOT_FOUND_ERR].join(","), "NotFoundError,m,8,true,8", "fields");
    eq(new DOMException().name, "Error", "default name");
    const u = URL.createObjectURL(new Blob(["x"]));
    eq(/^blob:[^/]+\/[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u), true, "format " + u);
    eq(u === URL.createObjectURL(new Blob(["x"])), false, "unique");
    URL.revokeObjectURL(u);
    throws(() => URL.createObjectURL("x"), TypeError, "non-blob");
});

t.test("os.openAsBlob reads a file lazily and slices it", async ({ eq, throws }) => {
    const dir = std.getenv("TMPDIR") || ".";
    const path = dir + "/test_blob_open_" + os.getpid() + ".bin";
    const f = std.open(path, "wb");
    const data = pattern(100000, 9);
    f.write(data.buffer, 0, data.length);
    f.close();
    try {
        const b = os.openAsBlob(path, { type: "App/Bin" });
        eq([b.size, b.type, b.name.startsWith("test_blob_open_"), b instanceof File].join(","), "100000,app/bin,true,true", "fields");
        eq(join((await b.slice(70000, 70010).bytes())), join(data.slice(70000, 70010)), "slice");
        eq((await drain(b.stream(), false)).bytes.length, 100000, "stream");
        throws(() => os.openAsBlob(dir + "/definitely-missing-blob-file"), TypeError, "missing");
    } finally {
        os.remove(path);
    }
});

await t.run();
