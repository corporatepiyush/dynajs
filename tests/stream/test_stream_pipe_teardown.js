// flags: --std
import { pipe, fromBytes, toFile, inflate, deflate, lines } from "dyna:stream";
import { gzip, lz4Frame } from "dyna:compress";
import { makeTempDir, readFile, removeAll, Path } from "dyna:file";
import * as std from "std";

let n = 0, bad = 0;
function ok(c, w, d) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); }
}
// Every section pipes through the gzip/deflate codecs, which are
// libcompression-backed (macOS-only); elsewhere they refuse with a
// TypeError naming libcompression. Probe once and skip the file loudly.
try {
    inflate(fromBytes(new TextEncoder().encode("x")), { codec: "gzip" });
} catch (e) {
    if (/libcompression/.test(e.message)) {
        print("SKIP  test_stream_pipe_teardown: the gzip/deflate codecs "
              + "need libcompression (macOS-only); nothing here runs "
              + "without them");
        std.exit(0);
    }
    throw e;
}
const enc = (s) => new TextEncoder().encode(s);
const dec = (u) => new TextDecoder().decode(u);
const eqBytes = (a, b) => a.length === b.length &&
    Array.from(a).every((x, i) => x === b[i]);

const T = makeTempDir("pipe_teardown");
const P = (n2) => T + "/" + n2;

const raw = enc(Array.from({ length: 512 },
    (_, i) => "chunk " + i + " of the pipe teardown payload. ").join(""));

const ENCODE = {
    gzip: (b) => gzip(b),
    lz4: (b) => lz4Frame(b),
};

function nest(codecs, payload) {
    let b = payload;
    for (let i = codecs.length - 1; i >= 0; i--)
        b = ENCODE[codecs[i]](b);
    return b;
}

{
    const out = P("one.bin");
    const total = await pipe(inflate(fromBytes(nest(["gzip"], raw)),
                                     { codec: "gzip" }), toFile(out));
    ok(total === raw.length, "pipe over one inflate resolves the byte total",
       String(total));
    ok(eqBytes(readFile(new Path(out), { bytes: true }), raw),
       "pipe over one inflate delivers the exact bytes");
}

for (const chain of [["gzip"], ["lz4"], ["gzip", "lz4"],
                     ["lz4", "gzip", "lz4"], ["gzip", "lz4", "gzip", "lz4"]]) {
    let src = fromBytes(nest(chain, raw));
    for (const c of chain)
        src = inflate(src, { codec: c });
    const out = P("chain" + chain.length + chain.join("_") + ".bin");
    const total = await pipe(src, toFile(out));
    ok(total === raw.length && eqBytes(readFile(new Path(out), { bytes: true }),
                                      raw),
       "inflate chain [" + chain.join(",") + "] pipes byte-exact (" +
       total + ")");
}

{
    const out = P("defl.gz");
    const ds = deflate(toFile(out), { codec: "gzip" });
    const total = await pipe(fromBytes(raw), ds);
    ok(total === raw.length, "pipe into a deflate sink counts the input",
       String(total));
    const fin = await ds.finish();
    ok(fin === raw.length, "finish() after the pipe finalizes the stream",
       String(fin));
    const back = await pipe(inflate(
        fromBytes(readFile(new Path(out), { bytes: true })),
        { codec: "gzip" }), toFile(P("back.bin")));
    ok(back === raw.length &&
       eqBytes(readFile(new Path(P("back.bin")), { bytes: true }), raw),
       "deflate sink output round trips through inflate");
}

{
    const ds = deflate(toFile(P("trans.gz")), { codec: "gzip" });
    const total = await pipe(inflate(fromBytes(nest(["gzip"], raw)),
                                     { codec: "gzip" }), ds);
    const fin = await ds.finish();
    ok(total === raw.length && fin === raw.length,
       "inflate->deflate through pipe, finished", total + "/" + fin);
}

{
    pipe(inflate(fromBytes(nest(["gzip"], raw)), { codec: "gzip" }),
         toFile(P("un1.bin")));
    pipe(inflate(fromBytes(nest(["lz4"], raw)), { codec: "lz4" }),
         toFile(P("un2.bin")));
    ok(true, "two fire-and-forget pipelines were started");
}

{
    const jobs = [];
    for (let i = 0; i < 8; i++)
        jobs.push(pipe(inflate(inflate(fromBytes(nest(["gzip", "lz4"], raw)),
                                       { codec: "gzip" }), { codec: "lz4" }),
                       toFile(P("par" + i + ".bin"))));
    const totals = await Promise.all(jobs);
    ok(totals.every((t) => t === raw.length), "8 parallel pipelines all land",
       JSON.stringify(totals));
    let same = true;
    for (let i = 0; i < 8; i++)
        same = same && eqBytes(readFile(new Path(P("par" + i + ".bin")), { bytes: true }), raw);
    ok(same, "parallel pipelines are byte-exact");
}

{
    const bad = nest(["gzip"], raw).slice();
    bad[12] ^= 0xff;
    let msg = null;
    try {
        await pipe(inflate(fromBytes(bad), { codec: "gzip" }),
                   toFile(P("err.bin")));
    } catch (e) {
        msg = String(e && e.message);
    }
    ok(msg !== null, "a corrupt stream REJECTS the pipeline [" + msg + "]");
    ok(msg !== null && /inflate|deflate|gzip|malformed|truncated/i.test(msg),
       "the rejection names the codec failure [" + msg + "]");
}

{
    let seen = 0;
    const total = await pipe(inflate(fromBytes(nest(["gzip"], raw)),
                                     { codec: "gzip" }), toFile(P("oc.bin")),
                             { onChunk: (nb) => { seen += nb; } });
    ok(seen === raw.length && total === raw.length,
       "onChunk sees every byte exactly once", seen + "/" + total);
}

{
    const text = "one\ntwo\nthree\n";
    const got = [];
    for await (const line of lines(inflate(fromBytes(nest(["gzip"],
            enc(text))), { codec: "gzip" })))
        got.push(line);
    ok(got.length === 3 && got[2] === "three",
       "lines() over an inflate source iterates to completion",
       JSON.stringify(got));
}

{
    const small = enc("churn payload. ".repeat(8));
    const packed = nest(["gzip"], small);
    for (let i = 0; i < 1000; i++) {
        const t = await pipe(inflate(fromBytes(packed), { codec: "gzip" }),
                             toFile(P("churn.bin")));
        if (t !== small.length) {
            ok(false, "churn pipeline " + i + " total " + t);
            break;
        }
    }
    ok(eqBytes(readFile(new Path(P("churn.bin")), { bytes: true }), small),
       "1000 sequential inflate pipelines ran byte-exact");
}

{
    const small = enc("concurrent churn payload. ".repeat(8));
    const packed = nest(["lz4", "gzip"], small);
    const jobs = [];
    for (let i = 0; i < 250; i++)
        jobs.push(pipe(inflate(inflate(fromBytes(packed), { codec: "lz4" }),
                                      { codec: "gzip" }),
                       toFile(P("cchurn.bin"))));
    const totals = await Promise.all(jobs);
    ok(totals.every((t) => t === small.length),
       "250 concurrent inflate pipelines all land");
}

{
    const dst = toFile(P("midclose.bin"));
    let msg = null;
    try {
        await pipe(inflate(fromBytes(nest(["gzip"], raw)), { codec: "gzip" }),
                   dst, { onChunk: () => {
                       dst.close();
                       throw new Error("observer kills the pipe");
                   } });
    } catch (e) {
        msg = String(e && e.message);
    }
    ok(msg !== null && /observer kills/.test(msg),
       "an onChunk throw rejects the pipeline [" + msg + "]");
}

removeAll(new Path(T));
console.log("test_stream_pipe_teardown.js: " + n + " assertions passed" +
            (bad ? ", " + bad + " FAILED" : ""));
if (bad) throw new Error("test_stream_pipe_teardown.js: " + bad + " failures");
