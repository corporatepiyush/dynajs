// flags: --std
// covers: c:src/modules/stream/stream-lines.c
// v10b-18: lines()/ndjson() over an object with read(buf) (a dyna:sys child pipe, an HTTP getStream body, any duck source) read it through
// toReadableStream, so there is one C feeder (the ReadableStream reader) and no per-iterator 128 KiB read buffer: a live iterator over such
// a source costs a few KB (it cost about 154 KB). The source sees the toReadableStream contract: reads of at most 65536 bytes, close() once
// at EOF, close() on return(), a rejected read or a byte count outside [0, buf.length] rejects next().
// The peak-RSS budget is skipped when the process starts above 24 MB (sanitizer builds: shadow memory and the free quarantine inflate RSS).
import { suite } from "../lib/kit.js";
import { lines, ndjson } from "dyna:stream";
import { memoryUsage } from "dyna:sys";

const t = suite("stream_lines_source");
const enc = new TextEncoder();
function duck(text, log = {}) {
    const data = enc.encode(text);
    let off = 0;
    log.closes = 0; log.reads = [];
    return { read(buf) { log.reads.push(buf.length); const n = Math.min(buf.length, data.length - off); buf.set(data.subarray(off, off + n)); off += n; return Promise.resolve(n); }, close() { log.closes++; } };
}
async function all(it) { const out = []; for await (const v of it) out.push(v); return out; }

t.test("lines over a duck source: every line, CRLF stripped, close() once at EOF, reads of at most 64 KiB", async (a) => {
    const log = {};
    const body = Array.from({ length: 30000 }, (_, i) => "line " + i).join("\r\n") + "\nlast";
    const got = await all(lines(duck(body, log)));
    a.eq(got.length, 30001);
    a.eq(got[29999], "line 29999");
    a.eq(got[30000], "last");
    a.eq(log.closes, 1, "closed once at EOF");
    a.ok(log.reads.length > 3 && log.reads.every((n) => n > 0 && n <= 65536), "read sizes " + log.reads.slice(0, 4).join(","));
});

t.test("ndjson and batch mode over a duck source", async (a) => {
    a.eq(JSON.stringify(await all(ndjson(duck('{"a":1}\n\n[2]\n"x"\n')))), '[{"a":1},[2],"x"]');
    const batches = await all(lines(duck("a\nb\nc\n"), { batch: true }));
    a.eq(JSON.stringify(batches.flat()), '["a","b","c"]');
});

t.test("return() closes the source; a rejected read and a bad count reject next()", async (a) => {
    const log = {};
    const it = lines(duck("x\ny\nz\n", log));
    a.eq((await it.next()).value, "x");
    await it.return();
    a.eq(log.closes, 1, "return closes");
    const boom = new Error("boom");
    let err = null;
    try { await lines({ read: () => Promise.reject(boom), close() {} }).next(); } catch (e) { err = e; }
    a.eq(err, boom, "rejected read");
    for (const bad of [-1, 2 ** 20, "x"]) {
        err = null;
        try { await lines({ read: () => Promise.resolve(bad), close() {} }).next(); } catch (e) { err = e; }
        a.ok(err instanceof TypeError || err instanceof RangeError, `count ${bad}: ${err}`);
    }
    err = null;
    try { lines({ close() {} }); } catch (e) { err = e; }
    a.ok(err instanceof TypeError, "no read(): TypeError");
});

t.test("a live iterator over a duck source costs a few KB, not a 128 KiB read buffer", async (a) => {
    const N = 2000;
    const mk = () => { const it = lines(duck("first\nsecond\n")); return it.next().then(() => it); };
    await mk();
    std.gc();
    const m0 = memoryUsage();
    const keep = [];
    for (let i = 0; i < N; i++) keep.push(await mk());
    await new Promise((r) => setTimeout(r, 0));
    std.gc();
    const m1 = memoryUsage();
    const per = (m1.mallocSize - m0.mallocSize) / N, rss = (m1.peakRss - m0.peakRss) / N;
    a.ok(per < 24576, `${per.toFixed(0)} engine B per live iterator`);
    if (m0.peakRss < 24 << 20)
        a.ok(rss < 24576, `${rss.toFixed(0)} peak-RSS B per live iterator`);
    keep.length = 0;
});

await t.run();
