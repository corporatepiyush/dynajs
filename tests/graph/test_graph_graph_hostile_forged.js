// covers: dyna:graph.Graph
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_hostile_forged");
import * as S from "dyna:structures";
import * as GraphMod from "dyna:graph";
import { CRC32C } from "dyna:hash";

let n = 0, refused = 0;
const returned = [];

function u16(v) { return [v & 0xff, (v >>> 8) & 0xff]; }
function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
function u64(v) { return [...u32(v >>> 0), ...u32(Math.floor(v / 4294967296))]; }
function f64(v) { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v, true); return [...b]; }
function uvar(v) { const out = []; do { let b = v & 0x7f; v = Math.floor(v / 128); if (v) b |= 0x80; out.push(b); } while (v); return out; }

function record(tid, payload) {
    const head = [0x44, 0x59, 0x4e, 0x53, ...u16(1), ...u16(tid), ...u32(0), ...u64(payload.length)];
    const body = [...head, ...payload];
    const crc = CRC32C(new Uint8Array(body)) >>> 0;
    return new Uint8Array([...body, ...u32(crc)]);
}

__kitS.section("CRC32C self-test failed:", () => {
    const g = new GraphMod.Graph(); g.addNode(); g.addNode(); g.addEdge(0, 1, 2.5);
    const real = g.serialize();
    const mine = CRC32C(real.subarray(0, real.length - 4)) >>> 0;
    const theirs = (real[real.length - 4] | (real[real.length - 3] << 8) |
                    (real[real.length - 2] << 16) | (real[real.length - 1] << 24)) >>> 0;
    if (mine !== theirs) throw new Error("CRC32C self-test failed: " + mine + " vs " + theirs);
});
__kitS.end();
n++;
try { const h = new S.Heap(); h.push(1); S.Heap.deserialize(h.serialize(), 42); returned.push("heap opts non-fn (DECODED)"); }
catch (e) { refused++; }

if (returned.length === 0)
    print("test_structures_hostile_forged: all " + n + " hostile records refused cleanly");
else {
    print("test_structures_hostile_forged: " + (n - refused) + " of " + n + " were NOT refused:");
    for (const r of returned) print("  " + r);
    throw new Error("hostile records decoded");
}
