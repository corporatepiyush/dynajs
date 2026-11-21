// covers: integration c:src/builtins/builtin-typed-array.c -- one engine accessor read by about 100 native call sites
// F-TA-STALE-LEN: JS_GetTypedArrayBuffer (and JS_GetArrayBufferView, which delegates to it for typed arrays) returned the
// byte length captured at construction for a length-tracking typed array (one built over a resizable ArrayBuffer with no
// explicit length). After rab.resize(n) the natives saw the old length: most re-check it against the buffer and threw a
// spurious "typed array out of bounds" after a shrink, and after a grow they silently processed only the old prefix; a
// caller without the re-check would read or write past the buffer. The accessor now reports count << log2(element size),
// the same value as the spec getter byteLength. Every case compares a native over the resized view with the same native
// over view.slice() (a plain copy made through the spec getters), and runs under ASan in the verify script.
import { suite } from "../lib/kit.js";
import { SHA256 } from "dyna:crypto";
import * as C from "dyna:compress";
import * as E from "dyna:encoding";
import * as S from "dyna:simd";
import * as B from "dyna:bytes";
import * as R from "dyna:random";
import * as M from "dyna:mathx";
import { writeFile, readBytes, makeTempDir, removeAll, Path } from "dyna:file";

const t = suite("test_typed_array_rab_natives");

function view(T, initial, resizeTo, offset, grow) {
    const rab = new ArrayBuffer(initial, { maxByteLength: 4096 });
    const fill = new Uint8Array(rab);
    for (let i = 0; i < fill.length; i++) fill[i] = (i * 37 + 11) & 0xff;
    const v = offset === undefined ? new T(rab) : new T(rab, offset);
    rab.resize(resizeTo);
    if (grow !== undefined) {
        rab.resize(grow);
        const f2 = new Uint8Array(rab);
        for (let i = resizeTo; i < grow; i++) f2[i] = (i * 13 + 5) & 0xff;
    }
    return v;
}

const SCENARIOS = [
    ["shrink 96->48", () => view(Uint8Array, 96, 48)],
    ["shrink to 1", () => view(Uint8Array, 96, 1)],
    ["shrink to 0", () => view(Uint8Array, 96, 0)],
    ["shrink with offset 8", () => view(Uint8Array, 96, 40, 8)],
    ["shrink then grow past the original", () => view(Uint8Array, 96, 48, undefined, 300)],
    ["grow only", () => view(Uint8Array, 16, 16, undefined, 200)],
];

const hex = (u) => E.HexEncode(u);

for (const [name, mk] of SCENARIOS) {
    t.test("byte natives agree with a copy: " + name, (a) => {
        const v = mk();
        const ref = v.slice();
        a.eq(v.byteLength, ref.length, "spec byteLength");
        a.eq(hex(SHA256(v)), hex(SHA256(ref)), "SHA256");
        a.eq(hex(C.gunzip(C.gzip(v))), hex(ref), "gzip round trip");
        a.eq(hex(C.inflate(C.deflate(v))), hex(ref), "deflate round trip");
        a.eq(E.HexEncode(v), E.HexEncode(ref), "HexEncode");
        a.eq(E.Base64Encode(v), E.Base64Encode(ref), "Base64Encode");
        a.eq(B.equal(v, ref), true, "bytes.equal");
        a.eq(B.count(v, ref[0] === undefined ? 0 : ref[0]), B.count(ref, ref[0] === undefined ? 0 : ref[0]), "bytes.count");
        a.eq(new TextDecoder("latin1").decode(v), new TextDecoder("latin1").decode(ref), "TextDecoder");
        const out = new Uint8Array(ref.length * 2 + 4);
        a.eq(E.hexEncodeInto(v, out), ref.length * 2, "hexEncodeInto length");
    });
}

t.test("file writeFile writes exactly the live bytes", (a) => {
    const dir = makeTempDir("ta-rab-");
    try {
        for (const [name, mk] of SCENARIOS) {
            const v = mk();
            const p = new Path(dir.toString() + "/f.bin");
            a.eq(writeFile(p, v), v.byteLength, "writeFile count: " + name);
            a.eq(hex(readBytes(p)), hex(v.slice()), "file bytes: " + name);
        }
    } finally {
        removeAll(dir);
    }
});

t.test("Float32Array natives: simd sum, NDArray shape, odd byte sizes", (a) => {
    for (const [initial, to, grow] of [[96, 48], [96, 50], [96, 3], [32, 32, 400], [96, 48, 128]]) {
        const rab = new ArrayBuffer(initial, { maxByteLength: 4096 });
        const f = new Float32Array(rab);
        for (let i = 0; i < f.length; i++) f[i] = i + 1;
        rab.resize(to);
        if (grow) { rab.resize(grow); const g = new Float32Array(rab); for (let i = Math.floor(to / 4); i < g.length; i++) g[i] = i + 1; }
        const ref = f.slice();
        const want = ref.reduce((x, y) => x + y, 0);
        a.eq(f.length, Math.floor((grow || to) / 4), "spec length " + [initial, to, grow]);
        a.eq(S.sum(f), S.sum(ref), "simd.sum " + [initial, to, grow]);
        a.near(S.sum(f), want, 1e-6, "simd.sum value " + [initial, to, grow]);
        const nd = new M.linalg.NDArray(f);
        a.eq(nd.shape[0], ref.length, "NDArray shape " + [initial, to, grow]);
    }
});

t.test("random shuffle permutes exactly the live elements", (a) => {
    for (const [name, mk] of SCENARIOS) {
        const v = mk();
        const before = Array.from(v).sort((x, y) => x - y).join();
        const rab = v.buffer;
        const tail = new Uint8Array(rab, v.byteOffset + v.byteLength).slice();
        new R.Random(7).shuffle(v);
        a.eq(Array.from(v).sort((x, y) => x - y).join(), before, "multiset kept: " + name);
        a.eq(hex(new Uint8Array(rab, v.byteOffset + v.byteLength)), hex(tail), "bytes past the view untouched: " + name);
    }
});

t.test("a view left out of bounds by a shrink is refused, never read", (a) => {
    const mkOob = () => view(Uint8Array, 96, 4, 8);
    const v = mkOob();
    a.eq(v.length, 0, "spec: out-of-bounds tracking view has length 0");
    for (const [n, f] of [["SHA256", () => SHA256(v)], ["gzip", () => C.gzip(v)], ["HexEncode", () => E.HexEncode(v)],
        ["simd.sum", () => S.sum(new Float32Array(mkOob().buffer, 8))], ["bytes.equal", () => B.equal(v, v)]]) {
        let threw = null;
        try { f(); } catch (e) { threw = e; }
        a.ok(threw instanceof TypeError || threw instanceof RangeError, n + " refuses the out-of-bounds view: " + threw);
    }
});

await t.run();
