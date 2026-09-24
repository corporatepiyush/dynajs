// flags: --std
/* test_stream_hostile_thenable.js -- hostile thenables must not double-settle
 * the stream pipeline (the once-only settle contract), ASan-clean.
 *
 * The defect class: the cps await attaches its settle pair DIRECTLY to the
 * source/sink's thenable (then(onok, onerr)), and user code receives those
 * callbacks. A hostile thenable can invoke onok twice -- the first call
 * runs the pipeline to terminal (the shell freed), the second used to READ
 * the freed shell (use-after-free in dyn_cps_settle) -- or invoke it MUCH
 * later, after the next await cycle began (the shell's per-await flag had
 * RESET, so a stale settle hijacked the live cycle). The once-only state
 * now lives in a per-arming guard that survives the shell (the anchor
 * object): a double, triple or stale call finds the guard consumed or
 * killed and walks away without touching the shell.
 *
 * Each row asserts the DATA-FLOW outcome too (exact write counts / totals):
 * a mutant that double-steps the pipeline while fixing nothing cannot pass
 * by exiting quietly. Run: dynajs (CONFIG_NATIVE_MODULES=y)
 * tests/test_stream_hostile_thenable.js */
import { pipe, lines, inflate, deflate } from "dyna:stream";
import { gzip } from "dyna:compress";

let n = 0, bad = 0;
function ok(c, w, d) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); }
}

/* A duck source whose read() resolves through a HOSTILE thenable: `calls`
 * invocations of onok(value) per read. Values: `serve` copies of `val`,
 * then EOF (0). Every onok also lands in `saved` for the stale rows. */
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

/* A duck sink counting writes, resolving through a hostile thenable. */
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

/* A duck source serving `data` through a hostile thenable (the fill runs
   once per read; the SETTLE is what gets doubled). */
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

/* ---- the review probe's shape: double-call on BOTH sides -------------- */
{
    const dst = hostileDst(2, null);
    const total = await pipe(hostileSrc(2, 3, 2, null), dst);
    ok(total === 6, "double-call both sides: total 6 (got " + total + ")");
    ok(dst.writes === 2, "double-call both sides: exactly 2 writes (got " +
       dst.writes + ") -- the second onok is ignored");
}

/* ---- triple-call on the source --------------------------------------- */
{
    const dst = hostileDst(1, null);
    const total = await pipe(hostileSrc(2, 4, 3, null), dst);
    ok(total === 8, "triple-call source: total 8 (got " + total + ")");
    ok(dst.writes === 2, "triple-call source: exactly 2 writes (got " +
       dst.writes + ")");
}

/* ---- double-call of onerr: the failure lands exactly once ------------- */
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

/* ---- STALE ACROSS AWAITS: cycle-1's callback fired after cycle 2 has
        begun must not hijack the live cycle (it must add no write) ------ */
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
                        /* the PREVIOUS cycle's callback, fired late */
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
    /* and the saved callbacks are inert now that the pipeline is gone */
    for (const s of saved) { s.onok(s.v); s.onok(s.v); }
    ok(dst.writes === 1, "stale after terminal (pipe): still exactly 1 write");
}

/* ---- STALE AFTER TERMINAL: callbacks fired after the shell is freed --- */
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

/* ---- the lines iterator over hostile reads ---------------------------- */
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

/* ---- codec read over a hostile inner source (doubled settles, real
        payload: the decode must see each chunk exactly once) ------------ */
{
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

/* ---- codec write over a hostile inner sink ---------------------------- */
{
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
