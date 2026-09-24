/* bench_protobuf.js -- the dynamic protobuf codec in dyna:serialize.
 *
 * WHY. There is no existing protobuf code, so nothing in the tree measures it;
 * this file pins the per-call cost of the two entry points AND the choice
 * that dominates them: a packed repeated field writes one LEN record while an
 * unpacked one writes a tag per element. If a future change speeds up packed
 * and slows unpacked by the same amount, the packed/unpacked rows diverge.
 *
 * WHAT IS MEASURED. encode and decode of a message with 24 fields spanning
 * every wire type, a nested message, a packed repeated field, an unpacked
 * repeated field and a map -- at 8 and 512 entries in the repeated fields
 * (the amortisation denominator: per-call schema parse vs per-element work).
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/bench_protobuf.js
 */
import { Proto } from "dyna:serialize";

const MIN_MS = 120;
let sink = 0;
function ms(fn) {
    fn(); fn();
    let mult = 1, dt;
    for (;;) {
        const t0 = performance.now();
        for (let m = 0; m < mult; m++) sink += fn();
        dt = performance.now() - t0;
        if (dt >= MIN_MS || mult >= 1 << 16) break;
        mult = Math.max(mult * 2, Math.ceil(mult * MIN_MS / Math.max(dt, 0.001)));
    }
    let best = Infinity;
    for (let k = 0; k < 5; k++) {
        const t0 = performance.now();
        for (let m = 0; m < mult; m++) sink += fn();
        const d = performance.now() - t0;
        if (d < best) best = d;
    }
    return best / mult;
}
const use = (v) => v.length;      /* Uint8Array.length forces the work */

const inner = { fields: [{ name: "n", number: 1, type: "int32" },
                         { name: "s", number: 2, type: "string" }] };
const schema = { fields: [
    { name: "i32", number: 1, type: "int32" },
    { name: "i64", number: 2, type: "int64" },
    { name: "u32", number: 3, type: "uint32" },
    { name: "u64", number: 4, type: "uint64" },
    { name: "s32", number: 5, type: "sint32" },
    { name: "s64", number: 6, type: "sint64" },
    { name: "f32", number: 7, type: "fixed32" },
    { name: "f64", number: 8, type: "fixed64" },
    { name: "sf32", number: 9, type: "sfixed32" },
    { name: "sf64", number: 10, type: "sfixed64" },
    { name: "fl", number: 11, type: "float" },
    { name: "db", number: 12, type: "double" },
    { name: "b", number: 13, type: "bool" },
    { name: "str", number: 14, type: "string" },
    { name: "byt", number: 15, type: "bytes" },
    { name: "inner", number: 17, type: "message", message: inner },
    { name: "rep", number: 18, type: "int32", repeated: true },
    { name: "rep_unpacked", number: 19, type: "int32", repeated: true, packed: false },
    { name: "m", number: 21, type: "message", map: true, keyType: "string", valueType: "int32" },
] };

function mkValue(nrep) {
    const rep = [], un = [];
    for (let i = 0; i < nrep; i++) { rep.push(i); un.push(-i); }
    const m = {};
    for (let i = 0; i < 8; i++) m["k" + i] = i;
    return { i32: -150, i64: -5000000000, u32: 4000000000, u64: 1234567890123456,
             s32: -123456, s64: -9876543210, f32: 3735928559, f64: 1234567890123456,
             sf32: -123456789, sf64: -1125899906842624, fl: 3.5, db: -0.25,
             b: true, str: "héllo wörld", byt: new Uint8Array([1, 2, 3, 255]),
             inner: { n: 42, s: "deep" }, rep, rep_unpacked: un, m };
}

for (const nrep of [8, 512]) {
    const v = mkValue(nrep);
    const enc = Proto.encode(v, schema);
    const dec = Proto.decode(enc, schema);
    sink += use(enc);
    const e = ms(() => use(Proto.encode(v, schema)));
    const d = ms(() => use(Proto.decode(enc, schema)));
    print(`protobuf nrep=${nrep} enc=${e.toFixed(3)}us dec=${d.toFixed(3)}us bytes=${enc.length}`);
    sink += use(dec);
}