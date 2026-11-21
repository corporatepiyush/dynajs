// covers: integration dyna:stream.toReadableStream dyna:stream.toWritableStream -- hostile thenables from duck sides under the C pipe engine and codecs
// v10b-19: dyna:stream inflate() is gone; a hostile duck source wrapped by toReadableStream feeds DecompressionStream("gzip").
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_stream_inflate_hostile_thenable");
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

await __kitS.section("hostile codec payload", async () => {
if (haveCodec) {
    const saved = [];
    const payload = new TextEncoder().encode(
        "hostile codec payload ".repeat(40));
    const gz = gzip(payload);
    const inner = hostileBytes(gz, 2, saved);
    const s = toReadableStream(inner).pipeThrough(new DecompressionStream("gzip"));
    const dst = hostileDst(1, null);
    const total = await pipe(s, dst);
    ok(total === payload.length,
       "codec hostile read: decoded " + payload.length + " bytes (got " +
       total + ") -- the doubled settle added no chunk");
    for (const s2 of saved) { s2.onok(s2.v); }
    ok(true, "codec hostile read: stale settles walked away");
}
});
__kitS.end();

print("test_stream_hostile_thenable: " + n + " passed, " + bad + " failed");
if (bad) throw new Error("test_stream_hostile_thenable: " + bad + " failures");
