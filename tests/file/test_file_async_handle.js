// covers: dyna:file.async.open c:src/modules/file/file-ahandle.c
// F3a: async FileHandle on the I/O pool. Calls on one handle run one at a time in call order (per-handle queue), reads go
// through a private buffer copied into the caller's view at completion (a view detached in flight rejects TypeError, a view
// shrunk in flight gets only what fits), seek is sync and throws while a call is queued or in flight, close() runs after the
// queued calls and every later call rejects EBADF. Hostile valueOf/toString/getters/Proxies, NUL in the path, 0/max/max+1
// edges, wrong receivers and fd leaks over 200 open/close cycles. Allocation failure, GC and interrupt in flight, teardown
// with queued and in-flight calls (fd ownership passes to the in-flight job): tests/file/test_file_async_api.c.
import * as file from "dyna:file";
const A = file.async;
const MAX = 1073741824;

let checks = 0, fails = 0;
function ok(c, m) { checks++; if (!c) { fails++; print("FAIL: " + m); } }
function str(u8) { let s = ""; for (const b of u8) s += String.fromCharCode(b); return s; }
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
function fdCount() { try { return file.readDir("/dev/fd").length; } catch { return -1; } }

const D = String(file.makeTempDir());
const P = (n) => D + "/" + n;

try {
    {
        const h = await A.open(P("a.txt"), { flags: "w" });
        ok(!h.closed && Object.prototype.toString.call(h) === "[object FileHandle]", "open resolves an open FileHandle");
        ok(await h.write("hello ") === 6, "write a string resolves the byte count");
        ok(await h.write(new Uint8Array([119, 111, 114, 108, 100])) === 5, "write bytes");
        ok(await h.write("") === 0, "write of 0 bytes");
        ok(await h.sync() === undefined, "sync resolves undefined");
        const st = await h.stat();
        ok(st.size === 11 && st.isFile, "stat sees the written size");
        const c = h.close();
        ok(h.closed, "closed is true once close() is called");
        ok(await c === undefined && await h.close() === undefined, "close and a second close resolve");
        ok(file.readFile(P("a.txt")) === "hello world", "content on disk");
    }
    {
        const h = await A.open(P("a.txt"));
        ok(str(await h.read(5)) === "hello", "read(n) resolves a Uint8Array");
        const v = new Uint8Array(3);
        ok(await h.read(v) === 3 && str(v) === " wo", "read(view) fills the view and resolves the count");
        ok(str(await h.read(5, 0)) === "hello", "read at a position");
        ok(str(await h.read(100)) === "rld", "a positioned read does not move the cursor; a short read at EOF");
        ok((await h.read(10)).length === 0 && await h.read(new Uint8Array(4)) === 0, "EOF reads empty / 0");
        ok((await h.read(0)).length === 0, "read(0)");
        ok(h.seek(0) === 0 && h.seek(-3, "end") === 8 && h.seek(1, "cur") === 9, "seek set/end/cur");
        ok(str(await h.read()) === "ld", "read() defaults to 64 KiB");
        const dv = new DataView(new ArrayBuffer(4));
        ok(await h.read(dv, 6) === 4 && new Uint8Array(dv.buffer)[0] === 119, "read into a DataView");
        const off = new Uint8Array(new ArrayBuffer(8), 2, 3);
        ok(await h.read(off, 0) === 3 && str(new Uint8Array(off.buffer)).slice(2, 5) === "hel", "read into an offset view");
        ok(str(await h.read("5", 0)) === "hello", "a numeric string length");
        await rejects(h.write("x"), isCode("EBADF"), "write on a read-only handle rejects EBADF");
        throwsSync(() => h.seek(-1), Error, /EINVAL|Invalid/, "seek before 0 throws EINVAL");
        await h.close();
        await rejects(h.read(1), isCode("EBADF"), "read after close rejects EBADF");
        await rejects(h.write("x"), isCode("EBADF"), "write after close rejects EBADF");
        await rejects(h.sync(), isCode("EBADF"), "sync after close rejects EBADF");
        await rejects(h.stat(), isCode("EBADF"), "stat after close rejects EBADF");
        throwsSync(() => h.seek(0), Error, /bad file|EBADF/i, "seek after close throws EBADF");
    }
    {
        const h = await A.open(P("q.txt"), { flags: "w+" });
        const ps = [];
        for (let i = 0; i < 100; i++) ps.push(h.write(String(i % 10)));
        throwsSync(() => h.seek(0), TypeError, /queued or in flight/, "seek throws while calls are queued");
        ps.push(h.read(10, 0), h.read(10, 95));
        const r = await Promise.all(ps);
        ok(str(r[100]) === "0123456789" && str(r[101]) === "56789", "reads queued after writes see them (call order)");
        ok(h.seek(0) === 0, "seek works once the queue drains");
        const c = h.close();
        await rejects(h.read(1), isCode("EBADF"), "a call after close() rejects EBADF even while close is queued");
        await c;
        ok(file.readFile(P("q.txt")) === "0123456789".repeat(10), "100 queued writes land in call order");
    }
    {
        file.writeFile(P("f.txt"), "base");
        await rejects(A.open(P("f.txt"), { flags: "wx" }), isCode("EEXIST"), "wx on an existing file rejects EEXIST");
        await rejects(A.open(P("missing.txt")), (e) => e.code === "ENOENT" && e.message.includes("missing.txt"), "open of a missing file rejects ENOENT naming the path");
        let h = await A.open(P("f.txt"), { flags: "a" });
        await h.write("+a", 0);
        await h.close();
        ok(file.readFile(P("f.txt")) === "base+a", "a appends (the position is ignored by O_APPEND)");
        h = await A.open(P("f.txt"), { flags: "r+" });
        await h.write("B");
        await h.close();
        ok(file.readFile(P("f.txt")) === "Base+a", "r+ writes in place without truncating");
        h = await A.open(P("f.txt"), { flags: "a+" });
        ok(str(await h.read(4, 0)) === "Base", "a+ reads");
        await h.write("!");
        await h.close();
        ok(file.readFile(P("f.txt")) === "Base+a!", "a+ appends");
        h = await A.open(P("m.txt"), { flags: "wx", mode: 0o600 });
        ok(((await h.stat()).mode & 0o777) === 0o600, "mode applies to a created file");
        await h[Symbol.asyncDispose]();
        ok(h.closed, "Symbol.asyncDispose closes");
        if (typeof AsyncDisposableStack === "function") {
            const s = new AsyncDisposableStack();
            const h2 = s.use(await A.open(P("m.txt")));
            await s.disposeAsync();
            ok(h2.closed, "AsyncDisposableStack.use closes the handle");
        }
        const big = new Uint8Array(4 << 20);
        for (let i = 0; i < big.length; i++) big[i] = (i * 31) & 255;
        h = await A.open(P("big.bin"), { flags: "w+" });
        ok(await h.write(big) === big.length, "4 MiB write");
        const back = await h.read(big.length, 0);
        let same = back.length === big.length;
        for (let i = 0; same && i < big.length; i += 4093) same = back[i] === big[i];
        ok(same, "4 MiB read back matches");
        await h.close();
    }
    {
        const h = await A.open(P("a.txt"), { flags: "r+" });
        throwsSync(() => A.open(P("x"), { flags: "rw" }), TypeError, /flags/, "unknown flags throw TypeError");
        throwsSync(() => A.open(P("x"), { flags: "r", bogus: 1 }), TypeError, null, "an unknown option key throws TypeError");
        throwsSync(() => A.open(P("x"), "r"), TypeError, /object/, "non-object options throw TypeError");
        throwsSync(() => A.open(P("x"), { mode: -1 }), RangeError, /mode/, "mode -1");
        throwsSync(() => A.open(P("x"), { mode: 0o10000 }), RangeError, /mode/, "mode max+1");
        throwsSync(() => A.open(P("x"), { mode: 1.5 }), RangeError, /mode/, "fractional mode");
        const hm = await A.open(P("mx.txt"), { flags: "w", mode: 0o7777 });
        await hm.close();
        ok(file.exists(P("mx.txt")), "mode max is accepted");
        let se = null;
        try { file.readFile(P("a\0b")); } catch (e) { se = e; }
        throwsSync(() => A.open(P("a\0b")), se ? se.constructor : TypeError, null, "NUL in the path throws like the sync call");
        throwsSync(() => A.open(), TypeError, null, "a missing path throws TypeError");
        throwsSync(() => A.open(P("x"), { get flags() { throw new SyntaxError("boom"); } }), SyntaxError, /boom/, "a throwing flags getter propagates");
        const hp = await A.open(P("a.txt"), new Proxy({ flags: "r" }, {}));
        ok(str(await hp.read(5)) === "hello", "a Proxy options object");
        await hp.close();
        const ht = await A.open(P("a.txt"), { flags: { toString() { return "r"; } } });
        await ht.close();
        ok(ht.closed, "flags converts through toString");
        throwsSync(() => h.read(-1), RangeError, /length/, "read(-1)");
        throwsSync(() => h.read(1.5), RangeError, /length/, "fractional length");
        throwsSync(() => h.read(NaN), RangeError, /length/, "NaN length");
        throwsSync(() => h.read(MAX + 1), RangeError, /length/, "length max+1");
        throwsSync(() => h.read(1, -1), RangeError, /position/, "position -1");
        throwsSync(() => h.read(1, 2 ** 53), RangeError, /position/, "position 2^53");
        throwsSync(() => h.read({}), TypeError, null, "a non-buffer object");
        throwsSync(() => h.read(new Uint8Array(1), { valueOf() { throw new EvalError("pos"); } }), EvalError, /pos/, "a throwing position valueOf propagates");
        throwsSync(() => h.write(undefined), TypeError, null, "write(undefined)");
        throwsSync(() => h.write(null), TypeError, null, "write(null)");
        throwsSync(() => h.write(), TypeError, null, "write()");
        throwsSync(() => h.seek(1.5), RangeError, /offset/, "fractional seek offset");
        throwsSync(() => h.seek(0, "top"), TypeError, /whence/, "unknown whence");
        throwsSync(() => h.seek(0, { toString() { throw new URIError("wh"); } }), URIError, /wh/, "a throwing whence toString propagates");
        const proto = Object.getPrototypeOf(h);
        for (const k of ["read", "write", "seek", "sync", "stat", "close"]) throwsSync(() => proto[k].call({}, 1), TypeError, null, k + " on a wrong receiver");
        throwsSync(() => Object.getOwnPropertyDescriptor(proto, "closed").get.call({}), TypeError, null, "closed on a wrong receiver");
        const b = new Uint8Array([49, 50, 51]);
        await h.write(b, { valueOf() { b[0] = 57; return 0; } });
        ok(str(await h.read(3, 0)) === "123", "write data is copied before the position converts");
        await rejects(h.read(1, { valueOf() { h.close(); return 0; } }), isCode("EBADF"), "a position valueOf that closes the handle rejects EBADF");
        ok(h.closed, "the hostile valueOf closed the handle");
    }
    {
        const h = await A.open(P("a.txt"));
        if (ArrayBuffer.prototype.transfer) {
            const ab = new ArrayBuffer(8), v = new Uint8Array(ab);
            const p = h.read(v, 0);
            ab.transfer();
            await rejects(p, (e) => e instanceof TypeError, "a view detached in flight rejects TypeError");
        }
        try {
            const want = file.readFile(P("a.txt")).slice(0, 2);
            const rab = new ArrayBuffer(8, { maxByteLength: 16 }), v = new Uint8Array(rab);
            const p = h.read(v, 0);
            rab.resize(2);
            const n = await p;
            ok(n === 2 && str(new Uint8Array(rab)) === want, "a view shrunk in flight gets only what still fits (" + n + ")");
            const rab2 = new ArrayBuffer(8, { maxByteLength: 16 }), fixed = new Uint8Array(rab2, 0, 8);
            const p2 = h.read(fixed, 0);
            rab2.resize(4);
            await rejects(p2, (e) => e instanceof TypeError, "a fixed-length view pushed out of bounds in flight rejects TypeError");
            ok(new Uint8Array(rab2).every((b) => b === 0), "nothing is written into an out-of-bounds view");
        } catch (e) { ok(false, "resizable buffer read: " + e); }
        await h.close();
    }
    {
        const before = fdCount();
        for (let i = 0; i < 200; i++) {
            const h = await A.open(P("a.txt"));
            if (i % 2) await h.read(4);
            await h.close();
        }
        ok(before < 0 || fdCount() === before, "200 open/close cycles leak no fd (" + before + " -> " + fdCount() + ")");
    }
} finally {
    file.removeAll(D);
}
print("file_async_handle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
