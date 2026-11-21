// flags: --std
// The worker side of test_web_streams_transfer.js: each op reports back on the Worker channel.
import * as os from "os";
const parent = os.Worker.parent;
const ops = {
    async sum(d) { let n = 0, k = 0; for await (const c of d.rs) { n += c.length; k++; } return { n, k }; },
    async take(d) { const r = d.rs.getReader(); const got = []; for (let i = 0; i < d.count; i++) got.push((await r.read()).value); return { got }; },
    async cancel(d) { await d.rs.cancel(new TypeError("stop")); return { ok: true }; },
    async readErr(d) { const r = d.rs.getReader(); try { for (;;) { const x = await r.read(); if (x.done) return { done: true }; } } catch (e) { return { name: e.constructor.name, message: e.message }; } },
    async produce(d) { const w = d.ws.getWriter(); for (let i = 0; i < d.count; i++) await w.write("line " + i); await w.close(); return { ok: true }; },
    async abort(d) { await d.ws.abort(new RangeError("nope")); return { ok: true }; },
    async upper() { const ts = new TransformStream({ transform(c, ctl) { ctl.enqueue(String(c).toUpperCase()); } }); return { ts, transfer: [ts] }; },
    async nested(d) { return { same: d.a.s === d.b.s, kind: d.a.s.constructor.name, n: (await ops.sum({ rs: d.a.s })).n, arr: Array.isArray(d.list) && d.list[1] === 7 }; },
    async ignore() { return { ok: true }; },
    async gz(d) { let n = 0; for await (const x of ReadableStream.from([d.text]).pipeThrough(new TextEncoderStream()).pipeThrough(new CompressionStream("gzip"))) n += x.length; return { n }; },
};
parent.onmessage = async (e) => {
    const d = e.data;
    if (d.op === "bye") { parent.onmessage = null; return; }
    const r = await ops[d.op](d);
    const transfer = r.transfer; delete r.transfer;
    parent.postMessage({ op: d.op, ...r }, transfer);
    if (d.op === "ignore") parent.onmessage = null;
};
