// covers: integration dyna:stream.toWritableStream dyna:stream.fromFile dyna:stream.toFile -- pipeTo between dyna:stream streams (the C copy loop and pipe engine)
// v10b-19: dyna:stream pipe() is gone; pipeTo between dyna:stream streams replaces it. fromBytes/fromFile into toFile runs a C copy loop;
// a duck read(buf)/write(buf) pair bridged by toReadableStream/toWritableStream runs the C pipe engine. The old pipe() contracts that
// survive are pinned here through pipeTo: every byte arrives, a short write loops, a write accepting 0 bytes or a read count outside
// [0, buf.length] rejects, a failing source aborts the sink (close() called) and a failing sink cancels the source (close() called), with
// the original error. The byte total and onChunk are gone (count bytes in a TransformStream if needed).
import { suite } from "../lib/kit.js";
import { fromBytes, fromFile, toFile, toReadableStream, toWritableStream } from "dyna:stream";
import { Path, readBytes, writeFile, makeTempDir, removeAll } from "dyna:file";

const t = suite("stream_pipe");
const dir = makeTempDir();
let seq = 0;
const tmp = (n) => new Path(dir, n + "." + (++seq));
function pattern(n) { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 7 + (i >> 8)) & 255; return u; }
function same(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
async function rejection(p) { try { await p; return null; } catch (e) { return e; } }

t.test("bytes and files pipe into toFile byte-exact, empty included", async (a) => {
    for (const n of [0, 1900, 3 * 128 * 1024 + 5, 3 * 1024 * 1024 + 777]) {
        const data = pattern(n);
        const f = tmp("b");
        await fromBytes(data).pipeTo(toFile(f));
        a.ok(same(readBytes(f), data), "fromBytes " + n);
        const g = tmp("f");
        await fromFile(f).pipeTo(toFile(g));
        a.ok(same(readBytes(g), data), "fromFile " + n);
    }
});

t.test("duck sinks: every byte, short writes loop, zero-accept rejects", async (a) => {
    let received = 0, writes = 0, closed = 0;
    await fromBytes("12345").pipeTo(toWritableStream({ async write(b) { received += b.length; return b.length; }, close() { closed++; } }));
    a.eq(received, 5);
    a.eq(closed, 1, "sink closed at the end");
    received = 0;
    await fromBytes("z".repeat(30)).pipeTo(toWritableStream({ async write(b) { writes++; const n = Math.min(4, b.length); received += n; return n; } }));
    a.eq(received, 30, "short writes");
    a.ok(writes >= 8, "looped " + writes);
    a.ok(await rejection(fromBytes("data").pipeTo(toWritableStream({ async write() { return 0; } }))) instanceof TypeError, "zero accept");
});

t.test("duck sources: bad counts reject; errors propagate and close both sides", async (a) => {
    const sink = () => ({ async write(b) { return b.length; }, close() { sink.closed++; } });
    a.ok(await rejection(toReadableStream({ async read() { return -1; }, close() {} }).pipeTo(toWritableStream(sink()))) !== null, "negative count");
    a.ok(await rejection(toReadableStream({ async read(b) { return b.length + 1; }, close() {} }).pipeTo(toWritableStream(sink()))) !== null, "overlong count");
    let ca = 0, cb = 0;
    let e = await rejection(toReadableStream({ read() { throw new Error("boom"); }, close() { ca++; } }).pipeTo(toWritableStream({ async write(b) { return b.length; }, close() { cb++; } })));
    a.eq(e && e.message, "boom", "source error");
    a.ok(ca >= 1 && cb >= 1, `both closed ${ca}/${cb}`);
    ca = cb = 0;
    let reads = 0;
    e = await rejection(toReadableStream({ async read(b) { reads++; if (reads === 1) { b[0] = 1; return 1; } return 0; }, close() { ca++; } })
        .pipeTo(toWritableStream({ async write() { throw new Error("sink-down"); }, close() { cb++; } })));
    a.eq(e && e.message, "sink-down", "sink error");
    a.ok(ca >= 1 && cb >= 1, `both closed ${ca}/${cb}`);
    let err = null;
    try { toReadableStream({}); } catch (x) { err = x; }
    a.ok(err instanceof TypeError, "toReadableStream without read throws");
    err = null;
    try { toWritableStream({ write: 42 }); } catch (x) { err = x; }
    a.ok(err instanceof TypeError, "toWritableStream without a callable write throws");
});

await t.run();
removeAll(dir);
