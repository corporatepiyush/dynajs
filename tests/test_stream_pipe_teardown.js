// flags: --std
/* test_stream_pipe_teardown.js -- pipe-over-codec chains exit CLEAN.
 *
 * The defect this pins: `pipe(inflate(...))` retained 1-2 fulfilled
 * promises per pending await -- dyn_cps_await's pending branch never
 * released its refs on the awaited promise and on the .then() result
 * promise -- so every pipe over an ASYNC source (a codec wrapper; plain
 * fromBytes/toFile sources settle inline and take the leak-free fast path)
 * left the runtime asserting (list_empty(&rt->gc_obj_list)) at teardown.
 *
 * The contract now: a pipeline over codec chains exits clean at ANY chain
 * length, in every drive shape (awaited, unawaited, parallel, erroring),
 * and survives a 1000-pipeline churn. The teardown itself is judged by the
 * process exit code -- the assertion fires in JS_FreeRuntime AFTER the
 * script's last line, so an rc of 0 at the harness is the proof. Every row
 * also asserts the BYTES, so a mutation that breaks data flow while fixing
 * nothing cannot pass by exiting quietly.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_stream_pipe_teardown.js */
import { pipe, fromBytes, toFile, inflate, deflate, lines } from "dyna:stream";
import { gzip, lz4Frame } from "dyna:compress";
import { makeTempDir, readFile, removeAll, Path } from "dyna:file";

let n = 0, bad = 0;
function ok(c, w, d) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); }
}
const enc = (s) => new TextEncoder().encode(s);
const dec = (u) => new TextDecoder().decode(u);
const eqBytes = (a, b) => a.length === b.length &&
    Array.from(a).every((x, i) => x === b[i]);

const T = makeTempDir("pipe_teardown");
const P = (n2) => T + "/" + n2;

/* payload with enough structure that every codec produces real compressed
   blocks (a zero-filled payload STORES and skips the decoder paths) */
const raw = enc(Array.from({ length: 512 },
    (_, i) => "chunk " + i + " of the pipe teardown payload. ").join(""));

/* codec set used to build nested chains; encoders applied outermost-last */
const ENCODE = {
    gzip: (b) => gzip(b),
    lz4: (b) => lz4Frame(b),
};

/* Build `codecs`-deep nesting: codecs[0] is the OUTERMOST wrapper. */
function nest(codecs, payload) {
    let b = payload;
    for (let i = codecs.length - 1; i >= 0; i--)
        b = ENCODE[codecs[i]](b);
    return b;
}

/* ---- shape 1: a single inflate through pipe, bytes proven ------------- */
{
    const out = P("one.bin");
    const total = await pipe(inflate(fromBytes(nest(["gzip"], raw)),
                                     { codec: "gzip" }), toFile(out));
    ok(total === raw.length, "pipe over one inflate resolves the byte total",
       String(total));
    ok(eqBytes(readFile(new Path(out), { bytes: true }), raw),
       "pipe over one inflate delivers the exact bytes");
}

/* ---- shape 2: chain lengths 1..4, exit clean at each ------------------ */
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

/* ---- shape 3: pipe INTO a deflate sink (the mirrored shape) ----------
 * The sink's finish() is CALLER-owned (pipe leaves both sides open and
 * usable on success), so the compressor is finalized explicitly -- the
 * same protocol test_stream.js pins for direct writes. */
{
    const out = P("defl.gz");
    const ds = deflate(toFile(out), { codec: "gzip" });
    const total = await pipe(fromBytes(raw), ds);
    ok(total === raw.length, "pipe into a deflate sink counts the input",
       String(total));
    const fin = await ds.finish();
    ok(fin === raw.length, "finish() after the pipe finalizes the stream",
       String(fin));
    /* round trip through the plain decompressor proves the bytes */
    const back = await pipe(inflate(
        fromBytes(readFile(new Path(out), { bytes: true })),
        { codec: "gzip" }), toFile(P("back.bin")));
    ok(back === raw.length &&
       eqBytes(readFile(new Path(P("back.bin")), { bytes: true }), raw),
       "deflate sink output round trips through inflate");
}

/* ---- shape 4: inflate -> deflate THROUGH pipe (two codecs live) ------ */
{
    const ds = deflate(toFile(P("trans.gz")), { codec: "gzip" });
    const total = await pipe(inflate(fromBytes(nest(["gzip"], raw)),
                                     { codec: "gzip" }), ds);
    const fin = await ds.finish();
    ok(total === raw.length && fin === raw.length,
       "inflate->deflate through pipe, finished", total + "/" + fin);
}

/* ---- shape 5: UNAWAITED pipelines (the promise is dropped) ----------- */
{
    pipe(inflate(fromBytes(nest(["gzip"], raw)), { codec: "gzip" }),
         toFile(P("un1.bin")));
    pipe(inflate(fromBytes(nest(["lz4"], raw)), { codec: "lz4" }),
         toFile(P("un2.bin")));
    ok(true, "two fire-and-forget pipelines were started");
}

/* ---- shape 6: parallel pipelines ------------------------------------- */
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

/* ---- shape 7: the ERROR path through a codec pipe -------------------- */
{
    const bad = nest(["gzip"], raw).slice();
    bad[12] ^= 0xff;                       /* corrupt the deflate stream */
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

/* ---- shape 8: onChunk (a JS callback per chunk through the loop) ----- */
{
    let seen = 0;
    const total = await pipe(inflate(fromBytes(nest(["gzip"], raw)),
                                     { codec: "gzip" }), toFile(P("oc.bin")),
                             { onChunk: (nb) => { seen += nb; } });
    ok(seen === raw.length && total === raw.length,
       "onChunk sees every byte exactly once", seen + "/" + total);
}

/* ---- shape 9: lines() driven over an inflate source ------------------ */
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

/* ---- shape 10: the CHURN -- 1000 sequential pipelines ---------------- */
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

/* ---- shape 11: the CHURN -- 250 concurrent pipelines ----------------- */
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

/* ---- shape 12: mid-flight close of the SINK (a failed pipe) ---------- */
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
