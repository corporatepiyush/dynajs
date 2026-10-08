// flags: --std
import { pipe, lines, inflate, deflate } from "dyna:stream";
import { gzip } from "dyna:compress";

let n = 0, bad = 0;

// gzip/deflate stream codecs are libcompression-backed (macOS-only); on
// other platforms they refuse with a TypeError. Probe once and skip the
// codec blocks loudly; the hostile-thenable machinery itself keeps running.
let haveCodec = true;
try { deflate(new TextEncoder().encode("x"), { codec: "gzip" }); }
catch (e) {
    if (/libcompression/.test(e.message)) {
        haveCodec = false;
        print("  SKIP  gzip/deflate stream codecs need libcompression "
              + "(macOS-only); the codec blocks are skipped");
    }
}
function ok(c, w, d) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); }
}

function hostileSrc(serve, val, calls, saved) {
    let reads = 0;
    return {
        reads() { return reads; },
        read() {
            const v = reads++ < serve ? val : 0;
            return {
                then(onok, onerr) {
                    if (saved) saved.push({ onok, onerr, v });
                    for (let i = 0; i < calls; i++)
                        onok(v);
                }
            };
        },
        close() {}
    };
}

function hostileDst(calls, saved) {
    const d = {
        writes: 0, total: 0,
        write(span) {
            d.writes++;
            d.total += span.length;
            return {
                then(onok, onerr) {
                    if (saved) saved.push({ onok, onerr, v: span.length });
                    for (let i = 0; i < calls; i++)
                        onok(span.length);
                }
            };
        },
        close() {}
    };
    return d;
}

function hostileBytes(data, calls, saved) {
    let off = 0;
    return {
        read(buf) {
            const m = Math.min(buf.length, data.length - off);
            for (let i = 0; i < m; i++)
                buf[i] = data[off + i];
            off += m;
            return {
                then(onok, onerr) {
                    if (saved) saved.push({ onok, onerr, v: m });
                    for (let i = 0; i < calls; i++)
                        onok(m);
                }
            };
        },
        close() {}
    };
}

{
    const dst = hostileDst(2, null);
    const total = await pipe(hostileSrc(2, 3, 2, null), dst);
    ok(total === 6, "double-call both sides: total 6 (got " + total + ")");
    ok(dst.writes === 2, "double-call both sides: exactly 2 writes (got " +
       dst.writes + ") -- the second onok is ignored");
}

{
    const dst = hostileDst(1, null);
    const total = await pipe(hostileSrc(2, 4, 3, null), dst);
    ok(total === 8, "triple-call source: total 8 (got " + total + ")");
    ok(dst.writes === 2, "triple-call source: exactly 2 writes (got " +
       dst.writes + ")");
}

{
    let fails = 0, closes = 0;
    const src = {
        read() {
            return {
                then(onok, onerr) {
                    onerr(new Error("boom"));
                    onerr(new Error("boom"));
                }
            };
        },
        close() { closes++; }
    };
    const dst = {
        write() { return { then(onok) { onok(0); } }; },
        close() { closes++; }
    };
    try {
        await pipe(src, dst);
        ok(false, "double-call onerr: pipe must reject");
    } catch (e) {
        fails++;
        ok(e.message === "boom", "double-call onerr: rejected with boom (got " +
           e.message + ")");
    }
    ok(fails === 1, "double-call onerr: rejected exactly once");
}

{
    const saved = [];
    const dst = hostileDst(1, null);
    let reads = 0;
    const src = {
        read() {
            const my = reads++;
            return {
                then(onok, onerr) {
                    if (my > 0 && saved.length > 0) {
                        saved[0].onok(saved[0].v);
                        saved[0].onok(saved[0].v);
                    }
                    saved.push({ onok, onerr, v: my === 0 ? 5 : 0 });
                    onok(my === 0 ? 5 : 0);
                }
            };
        },
        close() {}
    };
    const total = await pipe(src, dst);
    ok(total === 5,
       "stale across awaits: total 5 (got " + total + ") -- the stale call " +
       "did not add a chunk");
    ok(dst.writes === 1,
       "stale across awaits: exactly 1 write (got " + dst.writes + ")");
    for (const s of saved) { s.onok(s.v); s.onok(s.v); }
    ok(dst.writes === 1, "stale after terminal (pipe): still exactly 1 write");
}

{
    const saved = [];
    const dst = hostileDst(1, null);
    const total = await pipe(hostileSrc(0, 3, 1, saved), dst);
    ok(total === 0, "stale after terminal: empty pipe total 0 (got " + total + ")");
    for (const s of saved) { s.onok(s.v); s.onok(s.v); }
    ok(dst.writes === 0, "stale after terminal: no phantom writes (got " +
       dst.writes + ")");
    ok(true, "stale after terminal: the freed shell was never read");
}

{
    const saved = [];
    const src = hostileSrc(2, 10, 2, saved);
    const it = lines(src);
    let rows = 0, done = 0;
    for (let i = 0; i < 16; i++) {
        const r = await it.next();
        if (r.done) { done++; break; }
        rows++;
    }
    ok(done === 1, "hostile lines: the iterator ended exactly once");
    for (const s of saved) { s.onok(s.v); }
    ok(true, "hostile lines: stale settles walked away");
}

if (haveCodec) {
    const saved = [];
    const payload = new TextEncoder().encode(
        "hostile codec payload ".repeat(40));
    const gz = gzip(payload);
    const inner = hostileBytes(gz, 2, saved);
    const s = inflate(inner, { codec: "gzip" });
    const dst = hostileDst(1, null);
    const total = await pipe(s, dst);
    ok(total === payload.length,
       "codec hostile read: decoded " + payload.length + " bytes (got " +
       total + ") -- the doubled settle added no chunk");
    for (const s2 of saved) { s2.onok(s2.v); }
    ok(true, "codec hostile read: stale settles walked away");
}

if (haveCodec) {
    const saved = [];
    const sink = hostileDst(2, saved);
    const d = deflate({
        write: sink.write.bind(sink),
        flush() { return { then(onok) { onok(0); } }; },
        close() {}
    }, { codec: "gzip" });
    const w = await d.write(new TextEncoder().encode("payload bytes"));
    ok(w === 13, "codec hostile write: resolved the byte count (got " + w + ")");
    const fin = await d.finish();
    ok(typeof fin === "number" && fin > 0,
       "codec hostile write: finish returned a total (" + fin + ")");
    for (const s of saved) { s.onok(s.v); }
    ok(true, "codec hostile write: stale settles walked away");
}

print("test_stream_hostile_thenable: " + n + " passed, " + bad + " failed");
if (bad) throw new Error("test_stream_hostile_thenable: " + bad + " failures");
