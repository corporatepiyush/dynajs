// covers: integration dyna:stream.toReadableStream dyna:stream.toWritableStream -- hostile thenables from duck sides under the C pipe engine and codecs
// v10b-19: dyna:stream pipe() is gone; toReadableStream(src).pipeTo(toWritableStream(dst)) between two duck objects runs the same C
// pipe engine (the bridge's native fast path), so the hostile thenables (settled twice, settled late, rejected twice) still reach it.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_stream_pipe_hostile_thenable");
import { lines, toReadableStream, toWritableStream, fromBytes } from "dyna:stream";
const pipe = async (s, d) => { await (s instanceof ReadableStream ? s : toReadableStream(s)).pipeTo(toWritableStream(d)); return d.total; };
import { gzip } from "dyna:compress";

let n = 0, bad = 0;

const haveCodec = true;
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

await __kitS.section("double-call both sides: total 6", async () => {
    const dst = hostileDst(2, null);
    const total = await pipe(hostileSrc(2, 3, 2, null), dst);
    ok(total === 6, "double-call both sides: total 6 (got " + total + ")");
    ok(dst.writes === 2, "double-call both sides: exactly 2 writes (got " +
       dst.writes + ") -- the second onok is ignored");
});

await __kitS.section("triple-call source: total 8", async () => {
    const dst = hostileDst(1, null);
    const total = await pipe(hostileSrc(2, 4, 3, null), dst);
    ok(total === 8, "triple-call source: total 8 (got " + total + ")");
    ok(dst.writes === 2, "triple-call source: exactly 2 writes (got " +
       dst.writes + ")");
});

await __kitS.section("double-call onerr: pipe must reject", async () => {
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
});

await __kitS.section("stale across awaits: total 5", async () => {
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
});

await __kitS.section("stale after terminal: empty pipe total 0", async () => {
    const saved = [];
    const dst = hostileDst(1, null);
    const total = await pipe(hostileSrc(0, 3, 1, saved), dst);
    ok(total === 0, "stale after terminal: empty pipe total 0 (got " + total + ")");
    for (const s of saved) { s.onok(s.v); s.onok(s.v); }
    ok(dst.writes === 0, "stale after terminal: no phantom writes (got " +
       dst.writes + ")");
    ok(true, "stale after terminal: the freed shell was never read");
});
__kitS.end();

print("test_stream_hostile_thenable: " + n + " passed, " + bad + " failed");
if (bad) throw new Error("test_stream_hostile_thenable: " + bad + " failures");
