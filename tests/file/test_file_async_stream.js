// covers: dyna:file.async.reader dyna:file.async.writer c:src/modules/file/file-ahandle.c
// F3b: AsyncFileReader/AsyncFileWriter as a differential against the sync FileReader/FileWriter: readLine with CRLF, lone CR,
// no final newline, invalid UTF-8 and lines longer than bufferSize; read/readBytes/readInto/readAll splits on one shared buffer;
// writes of strings and views, direct writes over bufferSize, append, truncate, preallocate. for-await with break/return, flush
// visibility, ordered queued writes, EBADF after close, hostile valueOf/toString/getters/Proxies, NUL paths, 0/max/max+1 edges,
// wrong receivers, views detached or shrunk in flight, fd leaks over 200 cycles. Lines and short reads already in the buffer and writes
// that fit it settle on the JS thread (one pool trip per buffer, not per call); the plans below cross both paths. Allocation failure (a fast-path line whose string allocation fails rejects and stays buffered), GC, interrupt and teardown
// flushing of a dirty writer (idle, in flight, collected): tests/file/test_file_async_api.c.
import * as file from "dyna:file";
const A = file.async;
const MAX = 1073741824;
const J = JSON.stringify;

let checks = 0, fails = 0;
function ok(c, m) { checks++; if (!c) { fails++; print("FAIL: " + m); } }
function str(u8) { let s = ""; for (const b of new Uint8Array(u8)) s += String.fromCharCode(b); return s; }
function throwsSync(fn, ctor, re, m) {
    checks++;
    let r;
    try { r = fn(); } catch (e) {
        if (!(e instanceof ctor) || (re && !re.test(String(e.message)))) { fails++; print("FAIL(" + e + "): " + m); }
        return;
    }
    fails++;
    print("FAIL(no sync throw" + (r instanceof Promise ? ", got a Promise" : "") + "): " + m);
    if (r instanceof Promise) r.catch(() => {});
}
async function rejects(p, check, m) {
    checks++;
    if (!(p instanceof Promise)) { fails++; print("FAIL(not a Promise): " + m); return; }
    try { await p; fails++; print("FAIL(resolved): " + m); }
    catch (e) { if (!check(e)) { fails++; print("FAIL(" + e + " code=" + (e && e.code) + "): " + m); } }
}
const isCode = (c) => (e) => e instanceof Error && e.code === c;
const syncErr = (f) => { try { const r = f(); if (r instanceof Promise) r.catch(() => {}); } catch (e) { return e.constructor.name; } return "none"; };
function fdCount() { try { return file.readDir("/dev/fd").length; } catch { return -1; } }

const D = String(file.makeTempDir());
const P = (n) => D + "/" + n;

try {
    const fixtures = {
        "crlf.txt": "a\r\nb\n\r\nlast\r",
        "nonl.txt": "one\ntwo",
        "empty.txt": "",
        "nl.txt": "\n\n",
        "long.txt": "x".repeat(10000) + "\r\n" + "y".repeat(5000) + "\n" + "é".repeat(3000),
        "bad.bin": new Uint8Array([0x61, 0xff, 0x0a, 0xc3, 0x28, 0x0d, 0x0a, 0xe2, 0x82, 0x0a, 0xf0, 0x9f, 0x98]),
        "big.txt": Array.from({ length: 30000 }, (_, i) => "line " + i + (i % 7 ? "" : "\r")).join("\n"),
    };
    for (const [n, v] of Object.entries(fixtures)) file.writeFile(P(n), v);
    const names = Object.keys(fixtures);

    for (const n of names) for (const bs of [undefined, 4096]) {
        const o = bs ? { bufferSize: bs } : undefined;
        const s = new file.FileReader(P(n), o), want = [];
        for (;;) { const l = s.readLine(); want.push(l); if (l === null) break; }
        s.close();
        const r = await A.reader(P(n), o), got = [];
        for (;;) { const l = await r.readLine(); got.push(l); if (l === null) break; }
        await r.close();
        ok(J(got) === J(want), "readLine matches FileReader on " + n + " bs=" + bs + ": " + J(got).slice(0, 60));
    }

    const plan = [["read", 1], ["readLine"], ["readBytes", 3], ["read", 0], ["readInto", 5], ["read", 4096], ["readLine"], ["readBytes", 5000],
        ["readInto", 9000], ["read", 7], ["readAll"], ["read"], ["readBytes"], ["readLine"], ["readInto", 4]];
    for (const n of names) {
        const s = new file.FileReader(P(n), { bufferSize: 4096 }), r = await A.reader(P(n), { bufferSize: 4096 });
        for (const [m, k] of plan) {
            let a, b;
            if (m === "readInto") {
                const v1 = new Uint8Array(k), v2 = new Uint8Array(k), c1 = s.readInto(v1), c2 = await r.readInto(v2);
                a = c1 + ":" + str(v1.subarray(0, c1));
                b = c2 + ":" + str(v2.subarray(0, c2));
            } else if (m === "readBytes") {
                a = str(k === undefined ? s.readBytes() : s.readBytes(k));
                b = str(await (k === undefined ? r.readBytes() : r.readBytes(k)));
            } else {
                a = k === undefined ? s[m]() : s[m](k);
                b = await (k === undefined ? r[m]() : r[m](k));
            }
            ok(a === b, n + " " + m + "(" + k + ") " + J(b).slice(0, 50) + " vs FileReader " + J(a).slice(0, 50));
        }
        s.close();
        await r.close();
    }

    {
        const want = [], s = new file.FileReader(P("big.txt"));
        for (let l; (l = s.readLine()) !== null;) want.push(l);
        s.close();
        const got = [], r = await A.reader(P("big.txt"), { bufferSize: 4096 });
        ok(r[Symbol.asyncIterator]() === r && r.lines() === r, "the reader is its own async iterator");
        for await (const l of r) got.push(l);
        ok(got.length === 30000 && J(got) === J(want), "for await yields every line like readLine: " + got.length);
        ok(!r.closed, "running the iterator to the end does not close");
        ok(J(await r.next()) === J({ done: true }), "next at EOF is done");
        await r.close();
    }
    {
        const s = new file.FileReader(P("big.txt"), { bufferSize: 4096 }), r = await A.reader(P("big.txt"), { bufferSize: 4096 });
        const want = [s.readLine(), str(s.readBytes(5000)), s.readLine(), s.readLine(), s.read(3)];
        s.close();
        const a = await r.readLine(), ps = [r.readBytes(5000), r.readLine(), r.readLine(), r.read(3)];
        const got = [a, str(await ps[0]), await ps[1], await ps[2], await ps[3]];
        ok(J(got) === J(want), "calls queued behind an in-flight read keep call order: " + J(got.slice(2)));
        await r.close();
        const w = await A.writer(P("ord.txt"), { bufferSize: 4096 });
        await Promise.all([w.write("x".repeat(5000)), w.write("y"), w.write("z".repeat(10))]);
        await w.close();
        ok(file.readFile(P("ord.txt")) === "x".repeat(5000) + "y" + "z".repeat(10), "writes queued behind a direct write keep call order");
    }
    {
        const r = await A.reader(P("big.txt"));
        let first;
        for await (const l of r) { first = l; break; }
        ok(first === "line 0", "the first line has its \\r stripped: " + J(first));
        ok(r.closed, "break closes the reader through return()");
        ok(J(await r.next()) === J({ done: true }), "next after return resolves done");
        await rejects(r.readLine(), isCode("EBADF"), "readLine after return rejects EBADF");
        await rejects(r.read(1), isCode("EBADF"), "read after return rejects EBADF");
        ok(J(await r.return()) === J({ done: true }), "a second return resolves done");
        ok((await r.close()) === undefined, "close after return resolves");
        await r[Symbol.asyncDispose]();
    }

    {
        const seq = ["abc", new Uint8Array([0, 1, 255]), "é", "z".repeat(5000), "", new DataView(new Uint8Array([9, 8, 7]).buffer, 1),
            new Uint8Array([1, 2, 3, 4]).buffer, "q".repeat(4095), "tail\n"];
        const s = new file.FileWriter(P("ws.bin"), { bufferSize: 4096 }), sr = seq.map((d) => s.write(d));
        s.close();
        const w = await A.writer(P("wa.bin"), { bufferSize: 4096 }), ar = [];
        for (const d of seq) ar.push(await w.write(d));
        await w.close();
        ok(J(sr) === J(ar), "write results match FileWriter " + J(ar));
        ok(str(file.readBytes(P("wa.bin"))) === str(file.readBytes(P("ws.bin"))), "written bytes match FileWriter");
    }
    {
        const w = await A.writer(P("fl.txt"));
        await w.write("hello");
        ok(file.readFile(P("fl.txt")) === "", "buffered bytes are not in the file before flush");
        await w.flush();
        ok(file.readFile(P("fl.txt")) === "hello", "flush pushes buffered bytes");
        await w.write(" world");
        await w.sync();
        ok(file.readFile(P("fl.txt")) === "hello world", "sync flushes first");
        const ps = [];
        for (let i = 0; i < 100; i++) ps.push(w.write(String(i % 10)));
        const c = w.close();
        ok(w.closed, "closed is true as soon as close() is called");
        ok(J(await Promise.all(ps)) === J(Array(100).fill(1)), "100 queued writes each resolve 1");
        await c;
        ok(file.readFile(P("fl.txt")) === "hello world" + "0123456789".repeat(10), "queued writes land in call order before close");
        await rejects(w.write("x"), isCode("EBADF"), "write after close rejects EBADF");
        await rejects(w.flush(), isCode("EBADF"), "flush after close rejects EBADF");
        await rejects(w.sync(), isCode("EBADF"), "sync after close rejects EBADF");
        ok((await w.close()) === undefined, "a second close resolves");
        await w[Symbol.asyncDispose]();
    }
    {
        file.writeFile(P("ap.txt"), "start");
        const w = await A.writer(P("ap.txt"), { append: true });
        await w.write("+more");
        await w.close();
        ok(file.readFile(P("ap.txt")) === "start+more", "append keeps the existing bytes");
        const t = await A.writer(P("ap.txt"));
        await t.write("new");
        await t.close();
        ok(file.readFile(P("ap.txt")) === "new", "the default truncates");
        const sw = new file.FileWriter(P("pre_s.bin"), { preallocate: 1 << 20 });
        sw.write("x");
        sw.close();
        const aw = await A.writer(P("pre_a.bin"), { preallocate: 1 << 20 });
        await aw.write("x");
        await aw.close();
        ok(file.stat(P("pre_a.bin")).size === file.stat(P("pre_s.bin")).size, "preallocate matches FileWriter");
    }

    {
        throwsSync(() => A.reader(P("crlf.txt"), 5), TypeError, /options must be an object/, "non-object options");
        ok(syncErr(() => A.reader(P("crlf.txt"), { bogus: 1 })) === syncErr(() => new file.FileReader(P("crlf.txt"), { bogus: 1 })), "an unknown reader key throws like FileReader");
        ok(syncErr(() => A.writer(P("bo.txt"), { bogus: 1 })) === syncErr(() => new file.FileWriter(P("bo.txt"), { bogus: 1 })), "an unknown writer key throws like FileWriter");
        ok(syncErr(() => A.reader(P("a\0b"))) === syncErr(() => new file.FileReader(P("a\0b"))) && syncErr(() => A.reader(P("a\0b"))) !== "none", "a NUL in the path throws like FileReader");
        throwsSync(() => A.writer(P("g.txt"), { get bufferSize() { throw new RangeError("getter"); } }), RangeError, /getter/, "a throwing getter propagates synchronously");
        throwsSync(() => A.writer(P("g.txt"), { append: true, get preallocate() { throw new SyntaxError("pre"); } }), SyntaxError, /pre/, "a throwing preallocate getter propagates");
        throwsSync(() => A.reader(P("crlf.txt"), new Proxy({}, { ownKeys() { throw new EvalError("trap"); } })), EvalError, /trap/, "a throwing Proxy trap propagates");
        throwsSync(() => A.reader({ toString() { throw new URIError("path"); } }), Error, null, "a throwing path toString propagates");
        ok(!file.exists(P("g.txt")), "no file is created when the options throw");
        for (const bs of [-5, 0, NaN, 1e12, { valueOf() { return 4097; } }, null]) {
            const r = await A.reader(P("long.txt"), { bufferSize: bs });
            ok((await r.readAll()) === file.readFile(P("long.txt")), "bufferSize " + String(bs) + " is clamped and reads");
            await r.close();
        }
        await rejects(A.reader(P("nope.txt")), isCode("ENOENT"), "a missing file rejects ENOENT");
        await rejects(A.writer(P("nodir/x.txt")), isCode("ENOENT"), "a missing directory rejects ENOENT");
        await rejects(A.writer(D), isCode("EISDIR"), "a directory rejects EISDIR");
        const r = await A.reader(P("long.txt"));
        throwsSync(() => r.read(-1), RangeError, /length/, "read(-1)");
        throwsSync(() => r.read(1.5), RangeError, /length/, "read(1.5)");
        throwsSync(() => r.read(MAX + 1), RangeError, /length/, "read(2^30+1)");
        throwsSync(() => r.readBytes(NaN), RangeError, /length/, "readBytes(NaN)");
        throwsSync(() => r.read(Symbol("s")), TypeError, null, "read(symbol)");
        throwsSync(() => r.readInto(5), TypeError, /TypedArray/, "readInto needs a view");
        throwsSync(() => r.readInto(), TypeError, /TypedArray/, "readInto needs an argument");
        ok((await r.read(MAX)).length === file.readFile(P("long.txt")).length, "read(2^30) is accepted and reads to EOF");
        ok((await r.read(0)) === "" && (await r.readBytes(0)).length === 0, "read(0) resolves empty");
        await rejects(r.read({ valueOf() { r.close(); return 1; } }), isCode("EBADF"), "a valueOf that closes the reader makes that read reject EBADF");
        const w = await A.writer(P("ht.txt"));
        throwsSync(() => w.write(), TypeError, /data is required/, "write() needs data");
        let thrown = "none", p;
        try { p = w.write({ toString() { w.close(); return "x"; } }); } catch (e) { thrown = e.constructor.name; }
        if (p) await rejects(p, isCode("EBADF"), "a toString that closes the writer makes that write reject EBADF");
        else ok(thrown === "TypeError", "a plain object payload throws TypeError: " + thrown);
        await w.close();
    }
    {
        const r = await A.reader(P("long.txt"));
        const ab = new ArrayBuffer(16), v = new Uint8Array(ab), p = r.readInto(v);
        ab.transfer();
        await rejects(p, (e) => e instanceof TypeError, "a view detached in flight rejects TypeError");
        const rb = new ArrayBuffer(16, { maxByteLength: 16 }), lv = new Uint8Array(rb), q = r.readInto(lv);
        rb.resize(4);
        ok((await q) === 4 && str(lv) === "xxxx", "a view shrunk in flight gets only what fits");
        ok((await r.readInto(new Uint8Array(0))) === 0, "a zero-length view resolves 0");
        const dv = new DataView(new ArrayBuffer(8), 2, 3);
        ok((await r.readInto(dv)) === 3, "a DataView with an offset fills its own window");
        await r.close();
    }
    {
        const r = await A.reader(P("crlf.txt")), w = await A.writer(P("wr.txt"));
        const RP = Object.getPrototypeOf(r), WP = Object.getPrototypeOf(w);
        ok(String(r) === "[object AsyncFileReader]" && String(w) === "[object AsyncFileWriter]", "toStringTag");
        for (const m of ["read", "readAll", "readBytes", "readLine", "next", "return", "close", "lines", Symbol.asyncIterator, Symbol.asyncDispose])
            throwsSync(() => RP[m].call(w), TypeError, null, "reader " + String(m) + " on a writer");
        throwsSync(() => RP.readInto.call({}, new Uint8Array(1)), TypeError, null, "readInto on a plain object");
        for (const m of ["write", "flush", "sync", "close", Symbol.asyncDispose])
            throwsSync(() => WP[m].call(r, "x"), TypeError, null, "writer " + String(m) + " on a reader");
        throwsSync(() => Object.getOwnPropertyDescriptor(RP, "closed").get.call(w), TypeError, null, "reader closed getter on a writer");
        throwsSync(() => Object.getOwnPropertyDescriptor(WP, "closed").get.call({}), TypeError, null, "writer closed getter on a plain object");
        await r.close();
        await w.close();
    }
    {
        const f0 = fdCount();
        for (let i = 0; i < 200; i++) {
            const r = await A.reader(P("crlf.txt"));
            await r.readLine();
            await r.close();
            const w = await A.writer(P("leak.txt"));
            await w.write("x");
            await w.close();
            const r2 = await A.reader(P("crlf.txt"));
            for await (const l of r2) break;
        }
        ok(fdCount() === f0, "no fd leak over 200 cycles: " + f0 + " -> " + fdCount());
    }
} finally {
    file.removeAll(D);
}
print("file_async_stream: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
