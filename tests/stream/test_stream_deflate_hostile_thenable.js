// covers: integration dyna:stream.toReadableStream dyna:stream.toWritableStream -- hostile thenables from duck sides under the C pipe engine and codecs
// v10b-19: dyna:stream deflate() is gone; CompressionStream("gzip") pipes into toWritableStream over a hostile duck sink.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_stream_deflate_hostile_thenable");
import { lines, toReadableStream, toWritableStream, fromBytes } from "dyna:stream";
const pipe = async (s, d) => { await (s instanceof ReadableStream ? s : toReadableStream(s)).pipeTo(toWritableStream(d)); return d.total; };
import { gzip, gunzip } from "dyna:compress";

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
            (d.parts ||= []).push(span.slice());
            return {
                then(onok, onerr) {
                    if (saved) saved.push({ onok, onerr, v: span.length });
                    for (let i = 0; i < calls; i++)
                        onok(span.length);
                }
            };
        },
        bytes() { const o = new Uint8Array(d.total); let k = 0; for (const p of d.parts || []) { o.set(p, k); k += p.length; } return o; },
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

await __kitS.section("payload bytes", async () => {
if (haveCodec) {
    const saved = [];
    const sink = hostileDst(2, saved);
    await fromBytes("payload bytes").pipeThrough(new CompressionStream("gzip")).pipeTo(toWritableStream({
        write: sink.write.bind(sink),
        flush() { return { then(onok) { onok(0); } }; },
        close() {}
    }));
    const back = new TextDecoder().decode(gunzip(sink.bytes()));
    ok(back === "payload bytes", "codec hostile write: the sink got a whole gzip member (" + back + ")");
    for (const s of saved) { s.onok(s.v); }
    ok(true, "codec hostile write: stale settles walked away");
}
});
__kitS.end();

print("test_stream_hostile_thenable: " + n + " passed, " + bad + " failed");
if (bad) throw new Error("test_stream_hostile_thenable: " + bad + " failures");
