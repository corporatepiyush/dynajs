// covers: dyna:stream.lines
// v10b-18: lines() reads a duck source through toReadableStream, so its hostile thenables are absorbed by promise resolution.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_stream_lines_hostile_thenable");
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

await __kitS.section("hostile lines: the iterator ended exactly once", async () => {
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
});
__kitS.end();

print("test_stream_hostile_thenable: " + n + " passed, " + bad + " failed");
if (bad) throw new Error("test_stream_hostile_thenable: " + bad + " failures");
