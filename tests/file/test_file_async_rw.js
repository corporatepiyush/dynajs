// covers: dyna:file.async.readFile dyna:file.async.readBytes dyna:file.async.writeFile
// F1: Promise twins on the I/O pool. Differential against the sync calls (same strings, bytes, error shape), argument errors thrown
// synchronously, I/O errors rejected, hostile getters/Proxies/toString, NUL in the path, size 0, data copied before any option getter runs.
// 64 un-awaited atomic writes to one path: one whole payload wins, no temp file left (pool-thread temp-name sequence is atomic; run under --san too).
// Allocation failure, GC and a one-shot interrupt in flight, teardown with requests pending and the DYN_MAX_INPUT edges: tests/file/test_file_async_api.c.
import * as file from "dyna:file";
const { Path } = file;
const A = file.async;

let checks = 0, fails = 0;
function ok(c, m) { checks++; if (!c) { fails++; print("FAIL: " + m); } }
function eqBytes(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
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
    try { await p; fails++; print("FAIL(resolved): " + m); }
    catch (e) { if (!check(e)) { fails++; print("FAIL(" + e + "): " + m); } }
}
function syncErr(fn) { try { fn(); } catch (e) { return e; } return null; }

const dir = new Path(Path.temp(), "dj_fasync_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8));
file.makeDir(dir, { recursive: true });
const p = (n) => new Path(dir, n);

ok(typeof A === "object" && typeof A.readFile === "function" && typeof A.readBytes === "function" && typeof A.writeFile === "function", "file.async exports the three functions");

{
    const r = A.writeFile(p("a.txt"), "héllo ✓");
    ok(r instanceof Promise, "writeFile returns a Promise");
    ok(await r === new TextEncoder().encode("héllo ✓").length, "writeFile resolves to the byte count");
    ok(await A.readFile(p("a.txt")) === "héllo ✓", "readFile round-trips UTF-8");
    ok(await A.readFile(p("a.txt").toString()) === "héllo ✓", "a string path works");
    ok(await A.writeFile(p("a.txt"), "!!", { append: true }) === 2, "append resolves to 2");
    ok(file.readFile(p("a.txt")) === "héllo ✓!!", "append wrote after the old content");
    ok(await A.writeFile(p("a.txt"), new Uint8Array([65, 66]), { atomic: true }) === 2, "atomic write");
    ok(file.readFile(p("a.txt")) === "AB", "atomic replaced the content");
    const b = await A.readBytes(p("a.txt"));
    ok(b instanceof Uint8Array && eqBytes(b, [65, 66]), "readBytes returns the bytes");
    const b2 = await A.readFile(p("a.txt"), { bytes: true });
    ok(b2 instanceof Uint8Array && eqBytes(b2, [65, 66]), "readFile {bytes:true} returns the bytes");
    ok(await A.writeFile(p("ab.bin"), new Uint8Array([1, 2, 3, 4]).buffer) === 4, "ArrayBuffer data");
    ok(await A.writeFile(p("ab.bin"), new Uint16Array([1, 2]).subarray(1)) === 2, "view with an offset");
    ok(eqBytes(file.readBytes(p("ab.bin")), [2, 0]), "view offset honoured");
}

{
    ok(await A.writeFile(p("empty"), "") === 0, "size 0 write resolves 0");
    ok(await A.readFile(p("empty")) === "", "size 0 readFile is the empty string");
    const e = await A.readBytes(p("empty"));
    ok(e instanceof Uint8Array && e.length === 0, "size 0 readBytes is an empty Uint8Array");
    ok(await A.writeFile(p("empty2"), new Uint8Array(0)) === 0, "empty Uint8Array write");
}

{
    let seed = 0x9e3779b9;
    const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed; };
    const pool = [0x00, 0x41, 0x7f, 0x80, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf, 0xe0, 0xa0, 0xed, 0x9f, 0xef, 0xf0, 0x90, 0xf4, 0x8f, 0xf5, 0xff, 0xe2, 0x9c, 0x93, 0x0a];
    let bad = 0;
    for (let i = 0; i < 400; i++) {
        const n = i < 24 ? i : rnd() % 300;
        const u = new Uint8Array(n);
        for (let k = 0; k < n; k++) u[k] = (rnd() & 3) ? pool[rnd() % pool.length] : rnd() & 255;
        file.writeFile(p("d.bin"), u);
        const [s, t, by] = await Promise.all([A.readFile(p("d.bin")), Promise.resolve(file.readFile(p("d.bin"))), A.readBytes(p("d.bin"))]);
        if (s !== t || !eqBytes(by, u)) { bad++; if (bad < 4) print("diff case " + i + ": " + JSON.stringify(Array.from(u))); }
    }
    ok(bad === 0, "400 random/malformed UTF-8 files: async readFile === sync readFile and readBytes round-trips (" + bad + " differ)");
    const big = new Uint8Array(3 << 20);
    for (let k = 0; k < big.length; k++) big[k] = k % 251 === 7 ? 0xff : 0x61 + (k % 26);
    file.writeFile(p("big.bin"), big);
    ok(await A.readFile(p("big.bin")) === file.readFile(p("big.bin")), "3 MiB file with invalid bytes: async === sync");
}

{
    const missing = p("nope/missing.txt");
    const se = syncErr(() => file.readFile(missing));
    await rejects(A.readFile(missing), (e) => e instanceof Error && e.code === "ENOENT" && e.errno === se.errno && e.message === se.message, "readFile ENOENT rejects with the sync error shape");
    const sb = syncErr(() => file.readBytes(missing));
    await rejects(A.readBytes(missing), (e) => e.code === "ENOENT" && e.message === sb.message, "readBytes ENOENT rejects like sync");
    const sw = syncErr(() => file.writeFile(missing, "x"));
    await rejects(A.writeFile(missing, "x"), (e) => e.code === "ENOENT" && e.message === sw.message, "writeFile ENOENT rejects like sync");
    const sd = syncErr(() => file.readFile(dir));
    await rejects(A.readFile(dir), (e) => e.code === sd.code && e.message === sd.message, "readFile of a directory rejects like sync (" + (sd && sd.code) + ")");
    const sdw = syncErr(() => file.writeFile(dir, "x"));
    await rejects(A.writeFile(dir, "x"), (e) => e.code === sdw.code && e.message === sdw.message, "writeFile onto a directory rejects like sync");
}

{
    throwsSync(() => A.readFile(42), TypeError, /Path/, "readFile(number) throws TypeError synchronously");
    throwsSync(() => A.readFile(), TypeError, /Path/, "readFile() throws synchronously");
    throwsSync(() => A.readBytes({}), TypeError, /Path/, "readBytes({}) throws synchronously");
    throwsSync(() => A.writeFile(p("x")), TypeError, /writeFile/, "writeFile without data throws");
    throwsSync(() => A.writeFile(p("x"), undefined), TypeError, /data must be/, "writeFile(undefined data) throws");
    throwsSync(() => A.writeFile(p("x"), null), TypeError, /data must be/, "writeFile(null data) throws");
    throwsSync(() => A.writeFile(p("x"), "a", { append: true, atomic: true }), TypeError, /atomic and append/, "atomic+append throws");
    throwsSync(() => A.readFile(p("a.txt"), { byte: true }), TypeError, null, "unknown readFile option throws");
    throwsSync(() => A.writeFile(p("x"), "a", { apend: true }), TypeError, null, "unknown writeFile option throws");
    const nulPath = dir.toString() + "/a\u0000b";
    const sn = syncErr(() => file.readFile(nulPath));
    throwsSync(() => A.readFile(nulPath), TypeError, null, "NUL in a string path throws TypeError synchronously");
    ok(sn instanceof TypeError, "sync readFile also rejects NUL with TypeError");
    throwsSync(() => A.writeFile(nulPath, "x"), TypeError, null, "NUL in the writeFile path throws");
    throwsSync(() => A.readBytes("\u0000"), TypeError, null, "a lone NUL path throws");
    ok(!file.exists(p("a")), "the NUL path never created the truncated name");
}

{
    const boom = () => { throw new RangeError("hostile"); };
    throwsSync(() => A.readFile(p("a.txt"), { get bytes() { boom(); } }), RangeError, /hostile/, "throwing bytes getter propagates synchronously");
    throwsSync(() => A.writeFile(p("h"), "a", { get append() { boom(); } }), RangeError, /hostile/, "throwing append getter propagates");
    throwsSync(() => A.writeFile(p("h"), "a", { get atomic() { boom(); } }), RangeError, /hostile/, "throwing atomic getter propagates");
    throwsSync(() => A.writeFile(p("h"), { toString: boom }), RangeError, /hostile/, "throwing data toString propagates");
    throwsSync(() => A.readFile({ toString() { return dir.toString(); } }), TypeError, /Path/, "an object with toString is not a path");
    throwsSync(() => A.readFile(new Proxy(p("a.txt"), {})), TypeError, /Path/, "a Proxy over a Path is not a Path");
    throwsSync(() => A.readFile(p("a.txt"), new Proxy({}, { ownKeys: boom })), RangeError, /hostile/, "Proxy options ownKeys trap throws through");
    throwsSync(() => A.writeFile(p("h"), "a", new Proxy({}, { get: boom, ownKeys: () => ["append"], getOwnPropertyDescriptor: () => ({ value: 1, enumerable: true, configurable: true }) })), RangeError, /hostile/, "Proxy options get trap throws through");
    ok(!file.exists(p("h")), "no write happened after a hostile argument");
    ok(await A.writeFile(p("ts"), { toString() { return "custom"; } }) === 6, "object data goes through toString");
    ok(file.readFile(p("ts")) === "custom", "toString data written");
    let calls = 0;
    const o = { get bytes() { calls++; return 1; } };
    const v = await A.readFile(p("a.txt"), o);
    ok(calls === 1 && v instanceof Uint8Array, "the bytes getter runs exactly once");
    const prox = new Proxy({ append: false }, { get(t, k) { calls++; return Reflect.get(t, k); } });
    calls = 0;
    await A.writeFile(p("px"), "q", prox);
    ok(calls === 2 && file.readFile(p("px")) === "q", "a Proxy options bag is read once per key");
}

{
    const u = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const pr = A.writeFile(p("mut.bin"), u);
    u.fill(0xee);
    await pr;
    ok(eqBytes(file.readBytes(p("mut.bin")), [1, 2, 3, 4, 5, 6, 7, 8]), "mutating the data after the call returns does not change what is written");
    if (typeof ArrayBuffer.prototype.transfer === "function") {
        const d = new Uint8Array([9, 8, 7, 6]);
        let pd = null, te = null;
        try { pd = A.writeFile(p("det.bin"), d, { get append() { d.buffer.transfer(); return false; } }); } catch (e) { te = e; }
        if (pd) await pd;
        ok(te instanceof TypeError ? !file.exists(p("det.bin")) : eqBytes(file.readBytes(p("det.bin")), [9, 8, 7, 6]), "an option getter that tries to detach the data either fails synchronously with nothing written or the copied bytes land (" + te + ")");
        const d2 = new Uint8Array([5, 5]);
        const pd2 = A.writeFile(p("det2.bin"), d2);
        d2.buffer.transfer();
        await pd2;
        ok(eqBytes(file.readBytes(p("det2.bin")), [5, 5]), "detaching right after the call keeps the copied data");
    }
}

{
    file.writeFile(p("c.txt"), "concurrent");
    const many = [];
    for (let i = 0; i < 256; i++) many.push(i % 2 ? A.readFile(p("c.txt")) : A.readBytes(p("c.txt")));
    const res = await Promise.all(many);
    ok(res.every((r, i) => i % 2 ? r === "concurrent" : r.length === 10), "256 concurrent requests all settle correctly");
    const ws = [];
    for (let i = 0; i < 64; i++) ws.push(A.writeFile(p("w" + i), "n" + i, { atomic: i % 2 === 0 }));
    await Promise.all(ws);
    let wbad = 0;
    for (let i = 0; i < 64; i++) if (file.readFile(p("w" + i)) !== "n" + i) wbad++;
    ok(wbad === 0, "64 concurrent writes (half atomic) land");
    const order = [];
    const a = A.readFile(p("c.txt")).then(() => order.push("a"));
    order.push("sync");
    await a;
    ok(order[0] === "sync", "settlement is never synchronous");
}

{
    const tgt = p("race.txt"), pays = [];
    for (let i = 0; i < 64; i++) pays.push(String(i).padStart(2, "0").repeat(1 + i * 37));
    const rs = await Promise.all(pays.map((s) => A.writeFile(tgt, s, { atomic: true })));
    ok(rs.every((n, i) => n === pays[i].length), "64 un-awaited atomic writes to one path resolve to their byte counts");
    ok(pays.includes(file.readFile(tgt)), "the final content is exactly one whole payload");
    ok(file.readDir(dir).every((e) => !e.name.startsWith("race.txt.tmp-")), "no temporary files left beside the target");
    const plain = await Promise.all(pays.slice(0, 16).map((s) => A.writeFile(p("race2.txt"), s)));
    ok(plain.length === 16 && file.readFile(p("race2.txt")).length > 0, "16 un-awaited plain writes to one path all settle");
}

file.removeAll(dir);
print("test_file_async_rw: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " check(s) failed");
