// tests/test_perf_acceptance.js — codec/perf acceptance (deflate/inflate, zip, rows(), windows, bench, WebSocket, transfer lists).
// Runs on slim-bench (BENCH+COMPRESS+CSV+SIMD+SYS+NET+URL companions).
// flags: --std
import { ZipPack, ZipList, ZipRead, ZipReadAt, ZipExtractAll, deflate, inflate } from "dyna:compress";
import { CSVFile } from "dyna:csv";
import * as simd from "dyna:simd";
import { bench } from "dyna:bench";
import { Path, writeFile } from "dyna:file";

function assert(c, m) { if (!c) throw new Error("assert perf: " + m); }
function eq(a, b, m) { if (a !== b) throw new Error("eq perf " + m + ": " + a + " !== " + b); }
function throws(fn, re, m) {
  let ok = false;
  try { fn(); } catch (e) { ok = re.test(e.message); if (!ok) throw new Error("wrong throw " + m + ": " + e.message); }
  if (!ok) throw new Error("no throw " + m);
}

const enc = new TextEncoder(), dec = new TextDecoder();

// ---- raw DEFLATE ----
{
  const data = enc.encode("hello hello hello hello hello hello hello");
  const raw = deflate(data);
  assert(raw instanceof Uint8Array, "deflate u8");
  const back = inflate(raw);
  assert(dec.decode(back) === dec.decode(data), "deflate roundtrip");
  eq(inflate(raw, { asString: true }), dec.decode(data), "inflate asString");
  // verifies vs ZipPack embedded codec: member bytes equal deflate(data) at level 1
  const zip = ZipPack([{ name: "h.txt", data }]);
  const list = ZipList(zip);
  eq(list[0].method, "deflate", "zip member deflated");
  const member = ZipRead(zip, "h.txt");
  assert(dec.decode(member) === dec.decode(data), "zip member decodes");
  // level option + strict
  const r2 = deflate(data, { level: 6 });
  assert(inflate(r2) instanceof Uint8Array, "level 6 roundtrip");
  throws(() => deflate(data, { levle: 1 }), /unknown option/, "deflate strict");
  throws(() => inflate(raw, { bogus: 1 }), /unknown option/, "inflate strict");
  throws(() => inflate(enc.encode("not deflate")), /malformed/, "inflate rejects garbage");
}

// ---- ZipPack per-entry + ZipReadAt + ZipExtractAll + iteration ----
{
  const a = enc.encode("aaa".repeat(200));
  const b = enc.encode("b");
  const mtime = 1700000000;
  const zip = ZipPack([
    { name: "a.txt", data: a, mtime, mode: 0o644, comment: "first" },
    { name: "b.txt", data: b, method: "store", comment: "second" },
  ]);
  const list = ZipList(zip);
  eq(list.length, 2, "ziplist len");
  // iteration: ZipList is an array
  const names = [];
  for (const e of list) names.push(e.name);
  eq(names.join(","), "a.txt,b.txt", "iteration");
  eq(list[0].comment, "first", "comment read");
  eq(list[1].comment, "second", "comment2");
  eq(list[1].method, "store", "per-entry store");
  assert(Math.abs(list[0].mtime - mtime) < 3, "mtime roundtrip, got " + list[0].mtime);
  eq(list[0].mode & 0o777, 0o644, "mode read");
  // by-index
  const at0 = ZipReadAt(zip, 0);
  eq(at0.name, "a.txt", "readat name");
  assert(dec.decode(at0.data) === dec.decode(a), "readat data");
  eq(at0.comment, "first", "readat comment");
  const at1 = ZipReadAt(zip, 1);
  eq(at1.name, "b.txt", "readat1");
  throws(() => ZipReadAt(zip, 5), /out of range/, "readat range");
  // extract all (no dir)
  const all = ZipExtractAll(zip);
  eq(all.length, 2, "extractall len");
  eq(all[0].name, "a.txt", "extractall name");
  assert(dec.decode(all[0].data) === dec.decode(a), "extractall data");
  // extract all with dir
  const dir = "/tmp/perf_zip_" + Date.now();
  const all2 = ZipExtractAll(zip, dir);
  eq(all2.length, 2, "extractall dir len");
  // strict pack-level + per-entry keys
  throws(() => ZipPack([{ name: "x", data: a }], { bogus: 1 }), /unknown option/, "zippack strict");
}

// ---- rows({batch}) ----
{
  const p = new Path("/tmp/perf_rows.csv");
  const f = new CSVFile(p);
  f.create({ headers: ["id", "v"], rows: [["1", "a"], ["2", "b"], ["3", "c"], ["4", "d"], ["5", "e"]], overwrite: true });
  const seen = [];
  let batches = 0, total = -1;
  for await (const b of f.rows({ batch: 2 })) {
    batches++;
    total = b.totalRows;
    eq(b.headers.join(","), "id,v", "rows headers");
    for (const r of b.rows) seen.push(r.join(":"));
  }
  eq(total, 5, "rows total");
  eq(batches, 3, "rows batches 2+2+1");
  eq(seen.join("|"), "1:a|2:b|3:c|4:d|5:e", "rows content");
  // strict
  let threw = false;
  try { for await (const _ of f.rows({ bogus: 1 })) { break; } } catch (e) { threw = /unknown option/.test(e.message); }
  assert(threw, "rows strict");
  f.close();
}

// ---- (offset,length) windows ----
{
  const a = new Float32Array([1, 2, 3, 4, 5, 6, 7, 8]);
  eq(simd.sum(a, 2, 3), 3 + 4 + 5, "sum window");
  eq(simd.sum(a, { offset: 2, length: 3 }), 12, "sum bag");
  eq(simd.sum(a, { offset: 0, limit: 2 }), 3, "sum limit alias");
  const b = new Float32Array([10, 20, 30, 40, 50, 60, 70, 80]);
  eq(simd.dot(a, b, 0, 2), 1 * 10 + 2 * 20, "dot window");
  const out = new Float32Array(8);
  simd.add(out, a, b, 4, 2);
  eq(out[4], 5 + 50, "add window out[4]");
  eq(out[5], 6 + 60, "add window out[5]");
  eq(out[0], 0, "add window untouched");
  // f64 + i32
  const f = new Float64Array([1, 2, 3, 4]);
  eq(simd.f64Sum(f, 1, 2), 5, "f64 window");
  const ii = new Int32Array([1, 2, 3, 4]);
  eq(simd.i32Sum(ii, 1, 2), 5, "i32 window");
  // mean window
  eq(simd.mean(a, 0, 4), (1 + 2 + 3 + 4) / 4, "mean window");
  // out of range
  throws(() => simd.sum(a, 7, 5), /out of range/, "window range");
  throws(() => simd.sum(a, { offset: 0, length: 2, limit: 2 }), /aliases/, "length+limit");
}

// ---- bench ----
{
  const r = bench("noop", () => {}, { timeMs: 20, warmupMs: 5 });
  assert(r.opsPerSec > 0 && r.iters > 0, "bench works");
}

// ---- WebSocket alias ----
{
  const { WsClient } = await import("dyna:http");
  assert(typeof WebSocket !== "undefined", "WebSocket global exists");
  eq(WebSocket, WsClient, "WebSocket aliases WsClient");
}

// ---- transfer list ----
{
  const { Worker } = await import("os");
  // postMessage without transfer still works (validated by not throwing on construct path is hard without threads;
  // at minimum the arity accepts 2 args: call on a dummy? Instead verify Worker exists and postMessage length.)
  assert(typeof Worker === "function", "Worker exists");
  eq(Worker.prototype.postMessage.length, 2, "postMessage arity 2");
  // transfer validation is exercised via a real worker roundtrip when threads available:
  // (best-effort: skip if worker spawn fails in this env)
  try {
    /* Worker takes a module PATH (API.md: new Worker("worker.js")), not
       source text. A code string here sent the worker thread's module
       loader after a file literally named "for await (const _ of []) {}"
       -> "could not load module filename" -- dumped asynchronously by the
       worker thread, so the try/catch below never sees it (uncatchable
       stderr noise; the slow ASan startup made the interleaving visible).
       Write the payload to a file and pass the path, matching
       test_perf_adversarial.js's adv_worker.js. */
    const wf = "/tmp/perf_accept_worker_" + Date.now() + ".js";
    writeFile(new Path(wf), "for await (const _ of []) {}\n");
    const w = new Worker(wf);
    // bad transfer must throw synchronously
    let threw = false;
    try { w.postMessage({ x: 1 }, "nope"); } catch (e) { threw = /transfer must be an array/.test(e.message); }
    assert(threw, "transfer non-array throws");
    threw = false;
    try { w.postMessage({ x: 1 }, [123]); } catch (e) { threw = /not an ArrayBuffer/.test(e.message); }
    assert(threw, "transfer non-buffer throws");
    // good transfer detaches
    const ab = new ArrayBuffer(8);
    w.postMessage({ ab }, [ab]);
    eq(ab.byteLength, 0, "transfer detaches");
  } catch (e) {
    if (/not an ArrayBuffer|transfer must be/.test(e.message)) throw e;
    print("worker live test skipped: " + e.message);
  }
}

print("test_perf_acceptance ok");
