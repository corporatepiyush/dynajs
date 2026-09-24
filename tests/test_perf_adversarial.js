// flags: --std
/* tests/test_perf_adversarial.js — codec/perf adversarial probes.
 * Boundaries, error paths mid-operation, reentrancy, weird-syntax shapes for
 * raw DEFLATE/inflate, the zip surface (ZipPack/ZipList/ZipRead/ZipReadAt/
 * ZipExtractAll), CSVFile.rows() windows, dyna:simd windows, dyna:bench,
 * the WebSocket alias, and Worker.postMessage transfer lists. Every loop is
 * bounded; every async wait has a timeout. Runs on slim-bench (http/net
 * probes skip where those modules are absent).
 */
import * as std from "std";
import * as os from "os";
import { ZipPack, ZipList, ZipRead, ZipReadAt, ZipExtractAll, deflate, inflate } from "dyna:compress";
import { CSVFile } from "dyna:csv";
import * as simd from "dyna:simd";
import { bench } from "dyna:bench";
import { Path } from "dyna:file";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("  FAIL " + m + ": got |" + a + "| want |" + b + "|"); } }
function throws(fn, re, m) {
  n++;
  try { fn(); fails++; print("  FAIL (no throw) " + m); }
  catch (e) {
    if (!re.test(String(e.message || e))) { fails++; print("  FAIL (wrong throw) " + m + ": " + e.message); }
  }
}
async function rejects(fn, re, m) {
  n++;
  try { await fn(); fails++; print("  FAIL (no reject) " + m); }
  catch (e) {
    if (!re.test(String(e.message || e))) { fails++; print("  FAIL (wrong reject) " + m + ": " + e.message); }
  }
}

const enc = new TextEncoder(), dec = new TextDecoder();
const T = (std.getenv("TMPDIR") || "/tmp") + "/perf_adv_" + os.getpid();
os.mkdir(T, 0o755);

/* deterministic pseudo-random (LCG) — no RNG module dependency */
function lcgBytes(len, seed) {
  const b = new Uint8Array(len);
  let s = seed >>> 0;
  for (let i = 0; i < len; i++) { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; b[i] = s >>> 24; }
  return b;
}
function exists(p) { const r = os.stat(p); return r[0] !== null && r[0] !== undefined; }
/* CRC-32 (IEEE) in JS — keeps the probe free of cross-module deps */
function CRC32(b) {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) {
    c ^= b[i];
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return (c ^ 0xffffffff) >>> 0;
}
/* minimal hand-rolled STORED zip with one entry — for name-gate probes only */
function buildStoredZip(name, data) {
  const nb = enc.encode(name);
  const crc = CRC32(data);
  const loff = 0;
  const lhSize = 30 + nb.length;
  const cSize = 46 + nb.length;
  const total = lhSize + data.length + cSize + 22;
  const z = new Uint8Array(total);
  const dv = new DataView(z.buffer);
  let o = 0;
  const u16 = (v) => { dv.setUint16(o, v, true); o += 2; };
  const u32 = (v) => { dv.setUint32(o, v >>> 0, true); o += 4; };
  u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0);
  u32(crc); u32(data.length); u32(data.length); u16(nb.length); u16(0);
  z.set(nb, o); o += nb.length;
  z.set(data, o); o += data.length;
  const cdOff = o;
  u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0);
  u32(crc); u32(data.length); u32(data.length); u16(nb.length); u16(0); u16(0);
  u16(0); u16(0); u32(0); u32(loff);
  z.set(nb, o); o += nb.length;
  u32(0x06054b50); u16(0); u16(0); u16(1); u16(1);
  u32(cSize); u32(cdOff); u16(0);
  return z;
}
function bytesEq(a, b, m) {
  n++;
  if (a.length !== b.length) { fails++; print("  FAIL " + m + ": len " + a.length + " != " + b.length); return; }
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { fails++; print("  FAIL " + m + ": byte " + i); return; }
}

/* ================= raw DEFLATE ================= */
{
  // empty roundtrip
  const z0 = deflate(new Uint8Array(0));
  ok(z0 instanceof Uint8Array && z0.length > 0, "deflate empty emits stream");
  eq(inflate(z0).length, 0, "inflate empty");
  // single byte
  const z1 = deflate(new Uint8Array([42]));
  bytesEq(inflate(z1), new Uint8Array([42]), "1-byte roundtrip");
  // incompressible
  const rnd = lcgBytes(4096, 1234);
  bytesEq(inflate(deflate(rnd)), rnd, "random 4k roundtrip");
  // highly repetitive (dynamic huffman / stored ceiling paths)
  const rep = new Uint8Array(1 << 20);
  rep.fill(65);
  const zr = deflate(rep);
  ok(zr.length < rep.length / 16, "1MB zeros compresses hard (got " + zr.length + ")");
  bytesEq(inflate(zr), rep, "1MB repetitive roundtrip");
  // same codec + level as the ZipPack member path (compressedSize parity)
  const body = enc.encode("hello hello hello hello hello hello hello");
  const zp = ZipPack([{ name: "h.txt", data: body }]);           // pack default
  eq(ZipList(zp)[0].compressedSize, deflate(body).length, "deflate size == ZipPack member (default)");
  eq(ZipList(zp)[0].method, "deflate", "zip member deflated");
  // pack-level opts: strict bag is {method} (no level knob on the pack)
  throws(() => ZipPack([{ name: "h.txt", data: body }], { level: 9 }), /unknown option/, "pack opts strict: no level");
  ok(ZipPack([{ name: "h.txt", data: body }], { method: "deflate" }) instanceof Uint8Array, "pack method deflate ok");
  // level bounds
  throws(() => deflate(body, { level: 0 }), /level/, "level 0 refused");
  throws(() => deflate(body, { level: 13 }), /level/, "level 13 refused");
  throws(() => deflate(body, { level: -1 }), /level/, "level -1 refused");
  ok(inflate(deflate(body, { level: 12 })) instanceof Uint8Array, "level 12 ok");
  // inflate adversarial inputs
  const good = deflate(body);
  throws(() => inflate(good.subarray(0, good.length - 3)), /malformed|unexpected|truncated|deflate/, "truncated stream refused");
  const plus = new Uint8Array(good.length + 4);
  plus.set(good); plus.fill(7, good.length);
  throws(() => inflate(plus), /trailing|malformed|extra/, "trailing bytes refused");
  throws(() => inflate(enc.encode("\x1f\x8b\x08xxxxxxxx")), /malformed|deflate/, "gzip framing refused by raw inflate");
  // double-stream concat refused (whole input must be ONE stream)
  const two = new Uint8Array(good.length * 2);
  two.set(good, 0); two.set(good, good.length);
  throws(() => inflate(two), /trailing|malformed|extra/, "concatenated streams refused");
  // asString on binary payload still decodes (lossy utf8 is the contract)
  eq(typeof inflate(good, { asString: true }), "string", "asString returns string");
}

/* ================= zip adversarial ================= */
{
  const a = enc.encode("data-a");
  const zip = ZipPack([{ name: "a.txt", data: a }]);
  throws(() => ZipReadAt(zip, -1), /out of range|range|index/i, "readat -1");
  throws(() => ZipReadAt(zip, 1), /out of range|range|index/i, "readat len");
  eq(ZipReadAt(zip, 0.5).name, "a.txt", "readat fractional truncates (ToInt64)");
  // unsafe names refused AT PACK TIME too (defense in depth)
  throws(() => ZipPack([{ name: "../evil.txt", data: a }]), /unsafe|safe|refus|invalid/i, "pack refuses ../ name");
  throws(() => ZipPack([{ name: "/etc/zevil", data: a }]), /unsafe|safe|refus|invalid/i, "pack refuses absolute name");
  // hand-rolled malicious zip: extract-side name gate must still refuse it
  const evilBytes = buildStoredZip("../evil.txt", enc.encode("pwn"));
  throws(() => ZipExtractAll(evilBytes), /safe|refus|invalid/i, "extractall refuses ../ without dir");
  const dir = T + "/zdst";
  throws(() => ZipExtractAll(evilBytes, dir), /safe|refus|invalid/i, "extractall refuses ../ with dir");
  ok(!exists(T + "/evil.txt"), "no file escaped the destination dir");
  const evilAbs = buildStoredZip("/etc/zevil", enc.encode("pwn"));
  throws(() => ZipExtractAll(evilAbs, dir), /safe|refus|invalid/i, "extractall refuses absolute name");
  ok(!exists("/etc/zevil"), "no absolute write");
  // nested + directory entries
  const nested = ZipPack([
    { name: "sub/", data: new Uint8Array(0), method: "store" },
    { name: "sub/f.txt", data: enc.encode("inner") },
  ]);
  const got = ZipExtractAll(nested, dir);
  eq(got.length, 2, "nested extract count");
  ok(exists(dir + "/sub"), "sub dir created");
  const f = std.open(dir + "/sub/f.txt", "r");
  ok(!!f, "nested file written");
  if (f) { const txt = f.readAsString(); f.close(); eq(txt, "inner", "nested file content"); }
  // comment truncation at 1024
  const big = "c".repeat(2000);
  const zc = ZipPack([{ name: "a.txt", data: a, comment: big }]);
  eq(ZipList(zc)[0].comment.length, 1024, "comment truncated at 1024");
  // mtime extremes: DOS-time clamp contract (probe + pin actual)
  const zm = ZipPack([{ name: "a.txt", data: a, mtime: 0 }]);
  eq(ZipList(zm)[0].mtime, 0, "mtime 0 = none");
  const zf = ZipPack([{ name: "a.txt", data: a, mtime: 4102444800 }]);  // 2100-01-01
  ok(Math.abs(ZipList(zf)[0].mtime - 4102444800) < 86400 * 2, "far mtime clamped near actual");
  // empty + store + duplicate names
  const zd = ZipPack([
    { name: "e.txt", data: new Uint8Array(0), method: "store" },
    { name: "d.txt", data: a },
    { name: "d.txt", data: enc.encode("second") },
  ]);
  eq(ZipRead(zd, "e.txt").length, 0, "empty member");
  ok(ZipRead(zd, "d.txt") !== null, "duplicate name readable");
  // strict per-entry bag
  throws(() => ZipPack([{ name: "x", data: a, bogus: 1 }]), /unknown option/, "per-entry strict bag");
  throws(() => ZipPack([{ name: "x", data: a, method: "lzma" }]), /method/, "per-entry method enum");

  // ---- write-failure paths must free the record they were building (a
  // leaked one trips the gc assertion at teardown: this run exiting cleanly
  // is part of the assertion) ----
  {
    // the reviewer's repro: dir is an existing FILE path
    const fileAt = T + "/notadir";
    const wf = std.open(fileAt, "w");
    wf.puts("x");
    wf.close();
    throws(() => ZipExtractAll(zip, fileAt), /cannot write/, "dir is a file -> cannot write");
    // same failure one step later: the leaf slot for the member is a directory
    const blk = T + "/zblk";
    os.mkdir(blk, 0o755);
    os.mkdir(blk + "/f.txt", 0o755);
    throws(() => ZipExtractAll(ZipPack([{ name: "f.txt", data: a }]), blk),
            /cannot write/, "leaf slot is a directory -> cannot write");
  }

  // ---- strict options bags (ZipReadAt/ZipExtractAll and friends): an
  // unknown key throws naming the key AND the valid set ----
  throws(() => ZipReadAt(zip, 0, { bogus: 1 }), /unknown option "bogus".*valid/, "ZipReadAt bag strict");
  throws(() => ZipExtractAll(zip, { bogus: 1 }), /unknown option "bogus".*valid/, "ZipExtractAll bag strict");
  throws(() => ZipExtractAll(zip, T + "/zdst", { bogus: 1 }), /unknown option "bogus"/, "trailing bag strict");
  throws(() => ZipList(zip, { bogus: 1 }), /unknown option "bogus"/, "ZipList bag strict");

  // ---- dir vs options bag discrimination: an object is the bag (never a
  // coerced directory), a string/Path is the destination, ambiguity is
  // refused, and the name gate is relaxed only by the explicit flag ---- */
  {
    const cwd0 = os.getcwd();
    os.chdir(T);
    const recs = ZipExtractAll(zip, { allowUnsafeNames: true });   // bag: records only
    os.chdir(cwd0);
    eq(recs.length, 1, "bag form returns records");
    ok(!exists(T + "/[object Object]"), "no coerced [object Object] directory");
    throws(() => ZipExtractAll(evilBytes, { dir: T + "/zbag" }), /safe|refus|invalid/i,
            "bag dir: name gate still on");
    throws(() => ZipExtractAll(zip, 42), /string|Path|object/, "number arg refused");
    throws(() => ZipExtractAll(zip, T + "/zbag2", T + "/zbag3"), /dir given twice/, "two dirs refused");
    throws(() => ZipExtractAll(zip, T + "/zbag2", { dir: T + "/zbag3" }), /dir given twice/,
            "positional + bag dir refused");
    throws(() => ZipExtractAll(zip, { dir: 5 }), /string|Path/, "bag dir type-checked");
    throws(() => ZipExtractAll(zip, [], 5), /string|Path|object/, "array arg refused");
    // a Path is a destination both positional and in the bag
    eq(ZipExtractAll(zip, new Path(T + "/zpath")).length, 1, "Path destination");
    eq(ZipExtractAll(zip, { dir: new Path(T + "/zpath2") }).length, 1, "bag Path destination");
    ok(exists(T + "/zpath/a.txt") && exists(T + "/zpath2/a.txt"), "Path destinations written");
    // the documented opt-in escape: allowUnsafeNames + dir writes `../` OUTSIDE dir
    const esc = buildStoredZip("../escaped.txt", enc.encode("out"));
    throws(() => ZipExtractAll(esc, { dir: T + "/zesc" }), /safe|refus|invalid/i,
            "escape refused without opt-in");
    eq(ZipExtractAll(esc, { dir: T + "/zesc", allowUnsafeNames: true }).length, 1,
            "opt-in escape extracts");
    ok(exists(T + "/escaped.txt"), "opt-in escape wrote outside dir (documented)");
  }

  // ---- zip-slip through PRE-EXISTING symlinks inside dir must be refused
  // in both shapes, and the outside sentinel stays byte-identical ---- */
  {
    const outside = T + "/outside";
    os.mkdir(outside, 0o755);
    const sf = std.open(outside + "/target.txt", "w");
    sf.puts("SENTINEL");
    sf.close();
    const readAll = (p) => { const f = std.open(p, "r"); const s = f.readAsString(); f.close(); return s; };
    const before = readAll(outside + "/target.txt");
    const dA = T + "/zsA";
    os.mkdir(dA, 0o755);
    // shape A: a symlinked DIRECTORY inside dir
    os.symlink(outside, dA + "/link");
    const zipA = buildStoredZip("link/pwn.txt", enc.encode("pwn"));
    throws(() => ZipExtractAll(zipA, dA), /cannot write/, "symlinked dir component refused");
    ok(!exists(outside + "/pwn.txt"), "nothing written through the dir symlink");
    eq(readAll(outside + "/target.txt"), before, "outside sentinel byte-identical (shape A)");
    // shape B: a symlinked FILE inside dir
    os.symlink(outside + "/target.txt", dA + "/lnk.txt");
    const zipB = buildStoredZip("lnk.txt", enc.encode("pwn"));
    throws(() => ZipExtractAll(zipB, dA), /cannot write/, "symlinked leaf refused");
    eq(readAll(outside + "/target.txt"), before, "outside sentinel byte-identical (shape B)");
    // same shapes through the bag form
    throws(() => ZipExtractAll(zipA, { dir: dA }), /cannot write/, "bag dir: symlinked dir refused");
    throws(() => ZipExtractAll(zipB, { dir: dA }), /cannot write/, "bag dir: symlinked leaf refused");
    eq(readAll(outside + "/target.txt"), before, "outside sentinel byte-identical (bag form)");
  }
}

/* ================= rows() adversarial ================= */
{
  const p = new Path(T + "/rows.csv");
  const f = new CSVFile(p);
  f.create({ headers: ["a", "b"], rows: [["1", "x"], ["2", "y"], ["3", "z"]], overwrite: true });
  // batch bounds
  throws(() => f.rows({ batch: 0 }), /batch/, "batch 0");
  throws(() => f.rows({ batch: 100001 }), /batch/, "batch 100001");
  throws(() => f.rows({ batch: -5 }), /batch/, "batch -5");
  throws(() => f.rows({ batch: "abc" }), /batch|number|type/i, "batch non-number");
  eq(f.rows({ batch: 100000 }) !== null, true, "batch 100000 accepted");
  // early break marks done, never yields again
  const it = f.rows({ batch: 1 });
  const first = await it.next();
  eq(first.done, false, "first batch");
  await it.return();
  const after = await it.next();
  eq(after.done, true, "next after return is done");
  // for-await break terminates (return() called by the loop)
  let seen = 0;
  for await (const b of f.rows({ batch: 1 })) { seen += b.rows.length; break; }
  eq(seen, 1, "break after one row");
  // two iterators are independent
  const i1 = f.rows({ batch: 2 }), i2 = f.rows({ batch: 1 });
  const [r1, r2] = [await i1.next(), await i2.next()];
  eq(r1.value.rows.length, 2, "independent iter1 batch");
  eq(r2.value.rows.length, 1, "independent iter2 batch");
  // [Symbol.asyncIterator] identity
  eq(it[Symbol.asyncIterator](), it, "asyncIterator returns self");
  // strict malformed CSV rejects (promise, not sync throw)
  const badp = new Path(T + "/bad.csv");
  const bf = std.open(T + "/bad.csv", "w");
  bf.puts('a,b\r\n"unterminated,x\r\n'); bf.close();
  const bfile = new CSVFile(badp);
  await rejects(() => bfile.rows({ strict: true }).next(), /./, "strict malformed rejects");
  // missing file rejects
  const mp = new Path(T + "/nope.csv");
  const mf = new CSVFile(mp);
  await rejects(() => mf.rows({}).next(), /./, "missing file rejects");
  // headers-only file: zero batches, terminates
  const hp = new Path(T + "/hdr.csv");
  const hf = new CSVFile(hp);
  hf.create({ headers: ["a"], rows: [], overwrite: true });
  let hcount = 0;
  for await (const b of hf.rows({ batch: 10 })) hcount += b.rows.length;
  eq(hcount, 0, "headers-only yields zero rows");
  // rows() survives close() (path-snapshot contract)
  f.close();
  const jc = new CSVFile(p);
  let late = 0;
  for await (const b of jc.rows({ batch: 10 })) late += b.rows.length;
  eq(late, 3, "rows after close");
  jc.close();
}

/* ================= window (offset,length) adversarial ================= */
{
  const a = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
  throws(() => simd.sum(a, 5), /length|window|args/, "bare offset refused");
  throws(() => simd.sum(a, { offset: 2 }), /length|window/, "bag without length refused");
  eq(simd.sum(a, { length: 3 }), 6, "bag length-only = offset 0");
  throws(() => simd.sum(a, -1, 2), /out of range|range|>= 0/, "negative offset");
  throws(() => simd.sum(a, 0, -1), /out of range|range|length|>= 0/, "negative length");
  eq(simd.sum(a, 8, 0), 0, "zero-length window at end");
  throws(() => simd.sum(a, 9, 0), /out of range|range/, "offset past end");
  throws(() => simd.sum(a, 6, 3), /out of range|range/, "offset+length overflow");
  throws(() => simd.sum(a, { offset: 2, length: 1, nope: 3 }), /unknown option "nope"/, "bag strict names the key");
  throws(() => simd.sum(a, { offset: 2, length: 1, limit: 1 }), /aliases/, "length+limit both");
  // multi-arg: window validated against EVERY array arg
  const b = new Float32Array([10, 20, 30, 40, 50, 60, 70, 80]);
  const out = new Float32Array(8);
  simd.add(out, a, b, 4, 4);   // fits all
  eq(out[7], 8 + 80, "add window last");
  const short = new Float32Array([1, 2]);
  const out2 = new Float32Array(8);
  throws(() => simd.add(out2, a, short, 0, 4), /out of range|length|mismatch/, "window checked per-arg");
  throws(() => simd.add(out2, a, short, 2, 4), /out of range|length|mismatch/, "window checked per-arg 2");
  // untouched outside the window
  const out3 = new Float32Array(8);
  out3.fill(9);
  simd.add(out3, a, b, 2, 2);
  eq(out3[0], 9, "outside window untouched");
  eq(out3[4], 9, "outside window untouched 2");
  // f64/i32 families + reduction shapes
  const f64v = new Float64Array([1, 2, 3, 4]);
  eq(simd.f64Dot(f64v, f64v, 1, 2), 4 + 9, "f64Dot window");
  const i32v = new Int32Array([1, 2, 3, 4]);
  eq(simd.i32Max(i32v, 2, 2), 4, "i32Max window");
  // fractional offsets: pin the coercion contract (ToInt64 truncation)
  eq(simd.sum(a, 1.9, 2.9), 2 + 3, "fractional window truncates");
  // cumsum is in-place, window-local prefix, outside untouched (do last: mutates a)
  const cs = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
  eq(simd.cumsum(cs, 1, 3), cs, "cumsum returns its array");
  eq(cs[0], 1, "cumsum window outside untouched");
  eq(cs[1], 2, "cumsum window local prefix");
  eq(cs[3], 9, "cumsum window end = 2+3+4");
  eq(cs[4], 5, "cumsum window outside untouched 2");
}

/* ================= bench adversarial ================= */
{
  throws(() => bench("x", 5), /function/, "fn must be function");
  throws(() => bench(5, () => {}), /string/, "name must be string");
  throws(() => bench("x", () => {}, { timeMs: 0 }), /timeMs/, "timeMs 0");
  throws(() => bench("x", () => {}, { timeMs: 60001 }), /timeMs/, "timeMs 60001");
  throws(() => bench("x", () => {}, { warmup: -1 }), /warmup/, "warmup -1");
  throws(() => bench("x", () => {}, { warmupMs: 60001 }), /warmupMs/, "warmupMs 60001");
  throws(() => bench("x", () => {}, "t"), /object/, "opts must be object");
  // contract: opsPerSec == 1000/meanMs
  const r = bench("adv-side", () => { let x = 0; for (let i = 0; i < 100; i++) x += i; }, { timeMs: 30, warmupMs: 5 });
  ok(Math.abs(r.opsPerSec - 1000 / r.meanMs) / r.opsPerSec < 0.01, "opsPerSec == 1000/meanMs");
  ok(Number.isFinite(r.p50Ms) && Number.isFinite(r.p99Ms) && r.p99Ms >= r.p50Ms, "percentiles ordered");
  ok(r.iters >= 1 && Number.isInteger(r.iters), "iters integer");
  // cross-check against an independent clock (factor-2 envelope)
  const N = 20000;
  const t0 = performance.now();
  for (let i = 0; i < N; i++) { let x = 0; for (let j = 0; j < 100; j++) x += j; }
  const indep = N / ((performance.now() - t0) / 1000);
  const r2 = bench("adv-side2", () => { let x = 0; for (let i = 0; i < 100; i++) x += i; }, { timeMs: 60, warmupMs: 10 });
  ok(r2.opsPerSec > indep / 2 && r2.opsPerSec < indep * 2, "bench ops/sec within 2x of raw clock (" + r2.opsPerSec.toFixed(0) + " vs " + indep.toFixed(0) + ")");
  // zero-arg call contract
  let badArgs = 0;
  bench("adv-args", function () { if (arguments.length !== 0) badArgs++; }, { timeMs: 5, warmupMs: 0 });
  eq(badArgs, 0, "fn called with zero args");
  // table(): bounded at 256 rows, oldest dropped
  for (let i = 0; i < 270; i++) bench("zz" + i, () => {}, { timeMs: 1, warmupMs: 0 });
  const tbl = (await import("dyna:bench")).table();
  ok(!/zz0(?!\d)/.test(tbl), "table bounded (zz0 dropped)");
  ok(tbl.includes("zz269"), "table has newest");
}

/* ================= WebSocket alias ================= */
{
  ok(typeof WebSocket === "function" || typeof WebSocket === "object", "WebSocket global present");
  let http = null, net = null;
  try { http = await import("dyna:http"); } catch (e) { }
  try { net = await import("dyna:net"); } catch (e) { }
  if (http) eq(WebSocket, http.WsClient, "WebSocket === dyna:http WsClient");
  if (net) ok(net.WsClient === (http ? http.WsClient : net.WsClient), "net.WsClient same ctor");
}

/* ================= transfer lists (live workers) ================= */
function workerWait(w, ms, send) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { try { w.onmessage = null; } catch (e) { } reject(new Error("worker timeout")); }, ms);
    w.onmessage = (e) => { clearTimeout(t); try { w.onmessage = null; } catch (err) { } resolve(e.data); };
    send();
  });
}
{
  let WorkerCtor = null;
  try { WorkerCtor = os.Worker; } catch (e) { }
  if (!WorkerCtor) {
    print("  (transfer-list probes skipped: no os.Worker)");
  } else {
    const wf = T + "/adv_worker.js";
    const fh = std.open(wf, "w");
    fh.puts('// flags: --std\nimport * as os from "os";\nvar parent = os.Worker.parent;\nparent.onmessage = function (e) { parent.postMessage({ echo: e.data }); };\n');
    fh.close();
    const w = new WorkerCtor(wf);

    // 1. plain AB transfer: sender detached, receiver gets full bytes
    {
      const ab = new ArrayBuffer(8);
      const uv = new Uint8Array(ab);
      for (let i = 0; i < 8; i++) uv[i] = 30 + i;
      const rep = await workerWait(w, 10000, () => w.postMessage({ uv }, [ab]));
      eq(ab.byteLength, 0, "AB detached after transfer");
      bytesEq(new Uint8Array(rep.echo.uv.buffer ?? rep.echo.uv), new Uint8Array([30, 31, 32, 33, 34, 35, 36, 37]), "receiver bytes intact");
    }
    // 2. TypedArray view transfer: view's buffer detached
    {
      const ab = new ArrayBuffer(16);
      const uv = new Uint8Array(ab, 4, 8);   // offset view
      for (let i = 0; i < 8; i++) uv[i] = 40 + i;
      const rep = await workerWait(w, 10000, () => w.postMessage({ uv }, [uv]));
      eq(ab.byteLength, 0, "view transfer detaches underlying buffer");
      eq(rep.echo.uv.length, 8, "view content length kept");
    }
    // 3. DataView transfer entry (ArrayBufferView per d.ts). Content is sent as
    //    a Uint8Array view: the engine's serializer cannot CLONE a DataView
    //    (pre-existing, unrelated to transfer); the TRANSFER entry is the DataView.
    {
      const ab = new ArrayBuffer(8);
      const dv = new DataView(ab);
      dv.setUint32(0, 0xdeadbeef);
      let dvErr = null, rep = null;
      try { rep = await workerWait(w, 10000, () => w.postMessage({ u8: new Uint8Array(ab) }, [dv])); }
      catch (e) { dvErr = e; }
      if (dvErr) {
        ok(false, "DataView in transfer list: " + dvErr.message);
      } else {
        eq(ab.byteLength, 0, "DataView transfer detaches buffer");
        ok(rep.echo.u8.length === 8 && rep.echo.u8[0] === 0xde && rep.echo.u8[1] === 0xad, "content transferred intact");
      }
    }
    // 4. SAB refused (cannot detach shared memory) — sync TypeError, no detach
    {
      const sab = new SharedArrayBuffer(8);
      let err = null;
      try { w.postMessage({ sab }, [sab]); } catch (e) { err = e; }
      ok(err !== null, "SAB in transfer throws");
      if (err) ok(/SharedArrayBuffer|not an ArrayBuffer|transfer/.test(err.message), "SAB refusal message: " + err.message);
      eq(sab.byteLength, 8, "SAB not detached");
      new Uint8Array(sab)[0] = 7;   // still usable
    }
    // 5. bad entries refused, nothing detached on failure
    {
      const ab = new ArrayBuffer(4);
      throws(() => w.postMessage({ x: 1 }, [123]), /not an ArrayBuffer/, "number entry");
      throws(() => w.postMessage({ x: 1 }, [null]), /not an ArrayBuffer/, "null entry");
      throws(() => w.postMessage({ x: 1 }, ["ab"]), /not an ArrayBuffer/, "string entry");
      throws(() => w.postMessage({ x: 1 }, [{}]), /not an ArrayBuffer/, "plain object entry");
      throws(() => w.postMessage({ x: 1 }, [[1, 2]]), /not an ArrayBuffer/, "array entry");
      throws(() => w.postMessage({ x: 1 }, "nope"), /transfer must be an array/, "non-array transfer");
      throws(() => w.postMessage({ x: 1 }, { length: 1, 0: ab }), /transfer must be an array/, "array-like transfer");
      eq(ab.byteLength, 4, "buffer survives failed validation");
    }
    // 6. >1024 entries refused
    {
      const many = new Array(1025);
      many.fill(new ArrayBuffer(1));
      throws(() => w.postMessage(1, many), /too large/, "1025 entries");
    }
    // 7. throwing element getter: nothing detached
    {
      const ab = new ArrayBuffer(4);
      const arr = [ab];
      Object.defineProperty(arr, 0, { get() { throw new Error("boom-elem"); } });
      throws(() => w.postMessage(1, arr), /boom-elem/, "element getter throw propagates");
      eq(ab.byteLength, 4, "no detach on element failure");
    }
    // 8. clone failure (function value): nothing detached
    {
      const ab = new ArrayBuffer(4);
      let err = null;
      try { w.postMessage(() => {}, [ab]); } catch (e) { err = e; }
      ok(err !== null, "function value refused");
      eq(ab.byteLength, 4, "no detach on clone failure");
    }
    // 9. detached AB in transfer: refused
    {
      const ab = new ArrayBuffer(4);
      await workerWait(w, 10000, () => w.postMessage({}, [ab]));  // detaches; await own echo
      eq(ab.byteLength, 0, "sanity detached");
      throws(() => w.postMessage(1, [ab]), /detached|not an ArrayBuffer|transfer/, "detached AB refused");
    }
    // 10. duplicates + unreferenced buffer: legal, detaches
    {
      const ab = new ArrayBuffer(4);
      const rep1 = await workerWait(w, 10000, () => w.postMessage({ n: 0 }, [ab, ab]));  // duplicate entry
      eq(ab.byteLength, 0, "duplicate entries detach once");
      eq(rep1.echo.n, 0, "duplicate-entry message delivered");
      const orphan = new ArrayBuffer(4);
      const rep = await workerWait(w, 10000, () => w.postMessage({ n: 1 }, [orphan]));
      eq(orphan.byteLength, 0, "unreferenced buffer still detached");
      eq(rep.echo.n, 1, "message still delivered");
    }
    // 11. undefined/null transfer arg behaves as absent (no detach)
    {
      const ab = new ArrayBuffer(4);
      const rep = await workerWait(w, 10000, () => w.postMessage({ n: 2 }, undefined));
      eq(ab.byteLength, 4, "undefined transfer copies");
      eq(rep.echo.n, 2, "copy delivered");
    }
    // terminate the worker
    try { w.postMessage({ bye: 1 }); } catch (e) { }
  }
}

os.remove(T + "/adv_worker.js");
os.remove(T + "/rows.csv");
os.remove(T + "/bad.csv");
os.remove(T + "/hdr.csv");
os.remove(T + "/zdst/sub/f.txt");
os.remove(T + "/zdst/sub");
os.remove(T + "/zdst");
os.remove(T);

if (fails) {
  print("test_perf_adversarial: " + fails + " FAILED of " + n + " assertions");
  throw new Error("test_perf_adversarial failed");
}
print("test_perf_adversarial: " + n + " assertions, 0 failures");
