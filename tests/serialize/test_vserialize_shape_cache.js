// MsgPack/CBOR decode builds maps of up to 32 keys from the previous map's shape
// at the same depth when the keys repeat in order (JS_NewObjectFromProps). The
// source values here are built independently of the decoder, so a value landing
// under the wrong key, a lost key, a changed order or a shared mutable shape
// shows as a difference from the source.
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode } from "dyna:serialize";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; if (fail < 20) print("  FAIL: " + m); } };
function same(a, b) {
    if (a === b || (Number.isNaN(a) && Number.isNaN(b))) return true;
    if (typeof a !== "object" || typeof b !== "object" || !a || !b) return false;
    if (Array.isArray(a) !== Array.isArray(b) || Object.getPrototypeOf(b) !== (Array.isArray(a) ? Array.prototype : Object.prototype)) return false;
    const ka = Reflect.ownKeys(a), kb = Reflect.ownKeys(b);
    return ka.length === kb.length && ka.every((k, i) => k === kb[i] && same(a[k], b[k]));
}
const codecs = [["msgpack", MsgPackEncode, MsgPackDecode], ["cbor", CBOREncode, CBORDecode]];
function rt(v, what) { for (const [n, enc, dec] of codecs) ok(same(v, dec(enc(v))), n + " " + what); }

const shapes = [{ a: 1, b: 2 }, { b: 2, a: 1 }, { a: 1, c: 3 }, { a: 1 }, { a: 1, b: 2, c: 3 }, {}, { ["__proto__"]: 5, x: 1 },
    { "1": 1, "0": 0, a: 2 }, { "é": 1, "éx": 2 }, { "": 0 }, { a: { a: 1, b: 2 }, b: [{ a: 1, b: 2 }, { b: 2, a: 1 }] }];
const proto = JSON.parse('{"__proto__":5,"x":1}');
shapes[6] = proto;
for (const a of shapes) for (const b of shapes) rt([a, b, a, b], JSON.stringify(a) + " / " + JSON.stringify(b));

const wide = (n, off) => Object.fromEntries(Array.from({ length: n }, (_, i) => ["k" + ((i + off) % n), i]));
for (const n of [31, 32, 33, 40]) rt([wide(n, 0), wide(n, 0), wide(n, 1), wide(n, 0)], "wide " + n);
let deep = { v: 0 };
for (let d = 1; d < 14; d++) deep = { ["d" + (d % 3)]: deep, e: d };
rt([deep, deep, deep], "deep 14");

// rows that share a shape: a decoded row must not share mutable state with the next
const rows = MsgPackDecode(MsgPackEncode(Array.from({ length: 50 }, (_, i) => ({ id: i, name: "n" + i, t: [i] }))));
rows[3].extra = 1;
delete rows[4].name;
ok(rows[5].name === "n5" && !("extra" in rows[5]) && Object.keys(rows[3]).join() === "id,name,t,extra"
   && Object.keys(rows[4]).join() === "id,t", "rows built from one shape are independent objects");

// a map with a repeated key, hand-encoded (an encoder never emits one): the later value wins, order of first sight
const dup = new Uint8Array([0x92, 0x82, 0xa1, 0x61, 0x01, 0xa1, 0x62, 0x02, 0x83, 0xa1, 0x61, 0x01, 0xa1, 0x62, 0x02, 0xa1, 0x61, 0x03]);
const d = MsgPackDecode(dup);
ok(Object.keys(d[1]).join() === "a,b" && d[1].a === 3 && d[1].b === 2, "a repeated key keeps its first position and the last value: " + JSON.stringify(d));
// integer keys arrive through the slow key path and still match a shape
const ik = new Uint8Array([0x92, 0x81, 0x07, 0xa1, 0x78, 0x81, 0x07, 0xa1, 0x79]);
const di = MsgPackDecode(ik);
ok(di[0]["7"] === "x" && di[1]["7"] === "y", "integer map keys decode on both rows");

print("test_vserialize_shape_cache: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_vserialize_shape_cache failed");
