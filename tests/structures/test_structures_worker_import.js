// flags: --std
// dyna:structures registers its serialize codecs in a process-wide table. Each
// context that imports the module registers them again, and the table used to
// append per import: with 37 codecs the third import overflowed the 64 slots,
// and two Workers importing at once raced on the count. Six Workers here each
// import the module and round-trip one instance of several classes.
import * as os from "os";

const N = 6;
const script = `
import * as os from "os";
const p = os.Worker.parent;
import("dyna:structures").then((S) => {
    const out = [];
    const b = new S.BitSet(8); b.set(3); out.push(S.BitSet.deserialize(b.serialize()).get(3));
    const z = new S.ZSet(); z.add("m", 2); out.push(S.ZSet.deserialize(z.serialize()).score("m") === 2);
    const r = new S.RoaringBitmap([1, 70000]); out.push(S.RoaringBitmap.deserialize(r.serialize()).has(70000));
    const h = new S.HyperLogLog(); h.add("x"); out.push(S.HyperLogLog.deserialize(h.serialize()).count() > 0);
    p.postMessage({ ok: out.every(Boolean), out });
}, (e) => p.postMessage({ ok: false, err: String(e && e.message || e) }));
`;
const std = await import("std");
const path = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/dyna_worker_import_" + Date.now() + "_" + Math.floor(Math.random() * 1e9) + ".js";
const f = std.open(path, "w"); f.puts(script); f.close();
const { BitSet } = await import("dyna:structures");
void BitSet;
let got = 0, okc = 0, errs = [];
await new Promise((resolve) => {
    for (let i = 0; i < N; i++) {
        const w = new os.Worker(path);
        w.onmessage = (e) => {
            got++;
            if (e.data.ok) okc++; else errs.push(JSON.stringify(e.data));
            w.onmessage = null;
            if (got === N) resolve();
        };
    }
});
os.remove(path);
print(`test_structures_worker_import: ${okc} of ${N} workers imported and round-tripped`);
if (okc !== N) throw new Error("worker import failures: " + errs.join("; "));
