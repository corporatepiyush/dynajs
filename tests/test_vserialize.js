/* test_vserialize.js -- MessagePack, CBOR, ValueHash and structuredClone
 * (design 08).
 *
 * THE ORACLE IS THE SPECIFICATION'S OWN VECTORS: RFC 8949 Appendix A prints
 * the exact bytes for a list of CBOR values, and the MessagePack spec fixes
 * its type bytes. Byte-level expectations catch what a round trip cannot --
 * a self-consistent encoder that no other implementation can read.
 *
 * The second oracle is the round trip over a generated corpus, and the third
 * is that decode REFUSES a declared length larger than the input, which is the
 * whole attack surface of a length-prefixed format.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_vserialize.js
 */
import { MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode,
         CBORCanonical, ValueHash, structuredClone } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

/* ------------------------------- RFC 8949 Appendix A, byte for byte */

{
    const vectors = [
        [0, "00"], [1, "01"], [10, "0a"], [23, "17"], [24, "1818"], [25, "1819"],
        [100, "1864"], [1000, "1903e8"], [1000000, "1a000f4240"],
        [-1, "20"], [-10, "29"], [-100, "3863"], [-1000, "3903e7"],
        [1.1, "fb3ff199999999999a"], [1.5, "fb3ff8000000000000"],
        [-4.1, "fbc010666666666666"],
        [false, "f4"], [true, "f5"], [null, "f6"], [undefined, "f7"],
        ["", "60"], ["a", "6161"], ["IETF", "6449455446"],
        ["ü", "62c3bc"], ["水", "63e6b0b4"],
        [[], "80"], [[1, 2, 3], "83010203"],
        [[1, [2, 3], [4, 5]], "8301820203820405"],
        [{}, "a0"],
        [{ a: 1, b: [2, 3] }, "a26161016162820203"],
        [["a", { b: "c" }], "826161a161626163"],
    ];
    let bad = 0;
    for (const [v, want] of vectors) {
        const got = hex(CBOREncode(v));
        if (got !== want) {
            bad++;
            if (bad < 5) print("  CBOR(" + j(v) + ") = " + got + ", want " + want);
        }
    }
    assert(bad === 0, "every RFC 8949 vector encodes to the specified bytes ("
                      + (vectors.length - bad) + "/" + vectors.length + ")");
    let bad2 = 0;
    for (const [v, enc] of vectors)
        if (j(CBORDecode(bytes(enc))) !== j(v)) bad2++;
    assert(bad2 === 0, "and every one of them decodes back (" +
                       (vectors.length - bad2) + "/" + vectors.length + ")");
    assert(vectors.length === 31, "the vector list is the one from the RFC");
}
/* The RFC's own examples of forms this decoder accepts but does not write. */
eq(CBORDecode(bytes("f93c00")), 1, "a half-precision float decodes");
eq(CBORDecode(bytes("fa47c35000")), 100000, "and a single-precision one");
eq(j(CBORDecode(bytes("c074323031332d30332d32315432303a30343a30305a"))),
   '"2013-03-21T20:04:00Z"', "a tag is transparent: the value is what matters");
throws(() => CBORDecode(bytes("9f018202039f0405ffff")),
       "an indefinite length is REFUSED -- every length here is declared");

/* ---------------------------------- MessagePack, by its specified type bytes */

{
    const vectors = [
        [0, "00"], [127, "7f"], [128, "cc80"], [255, "ccff"], [256, "cd0100"],
        [-1, "ff"], [-32, "e0"], [-33, "d0df"], [-128, "d080"], [-129, "d1ff7f"],
        [true, "c3"], [false, "c2"], [null, "c0"],
        ["", "a0"], ["a", "a161"], ["abc", "a3616263"],
        [[], "90"], [[1, 2, 3], "9301 0203".replace(/ /g, "")],
        [{}, "80"], [{ a: 1 }, "81a16101"],
        [1.5, "cb3ff8000000000000"],
    ];
    let bad = 0;
    for (const [v, want] of vectors) {
        const got = hex(MsgPackEncode(v));
        if (got !== want) {
            bad++;
            if (bad < 5) print("  msgpack(" + j(v) + ") = " + got + ", want " + want);
        }
    }
    assert(bad === 0, "every MessagePack vector matches the spec's type bytes ("
                      + (vectors.length - bad) + "/" + vectors.length + ")");
}
eq(hex(MsgPackEncode(new Uint8Array([1, 2, 3]))), "c403010203", "bin8 for bytes");
eq(hex(CBOREncode(new Uint8Array([1, 2, 3]))), "43010203", "and a CBOR byte string");

/* ------------------------------------------ round trips over a corpus */

{
    const corpus = [
        null, true, false, 0, 1, -1, 127, 128, -128, 65535, -65536,
        2147483647, -2147483648, 4294967296, -4294967296,
        0.5, -0.5, 3.141592653589793, 1e100, -1e-100,
        "", "a", "hello world", "é你\u{1F600}", "x".repeat(1000),
        [], [1], [1, "two", null, [3, [4]]],
        {}, { a: 1 }, { a: { b: { c: [1, 2, { d: null }] } } },
        { "": "empty key" }, { "é": 1 },
        new Uint8Array([]), new Uint8Array([0, 255, 128]),
        [[[[[[[[[[1]]]]]]]]]],
    ];
    let bad = 0, checked = 0;
    for (const v of corpus) {
        for (const [enc, dec, name] of [[MsgPackEncode, MsgPackDecode, "msgpack"],
                                        [CBOREncode, CBORDecode, "cbor"]]) {
            checked++;
            const back = dec(enc(v));
            /* Uint8Array compares by content, everything else by JSON. */
            const same = v instanceof Uint8Array
                ? (back instanceof Uint8Array && hex(back) === hex(v))
                : j(back) === j(v);
            if (!same) {
                bad++;
                if (bad < 5) print("  " + name + " round trip differs for " + j(v)
                                   + " -> " + j(back));
            }
        }
    }
    assert(bad === 0, "every value round-trips through both formats (" +
                      (checked - bad) + "/" + checked + ")");
    assert(checked === corpus.length * 2, "the corpus ran through both");
}
{
    /* Large collections cross the 16-bit length prefix, which is its own path. */
    const arr = [], obj = {};
    for (let i = 0; i < 70000; i++) arr.push(i % 250);
    for (let i = 0; i < 100; i++) obj["k" + i] = i;
    eq(MsgPackDecode(MsgPackEncode(arr)).length, 70000, "a 70k array (32-bit prefix)");
    eq(CBORDecode(CBOREncode(arr)).length, 70000, "in CBOR too");
    eq(Object.keys(MsgPackDecode(MsgPackEncode(obj))).length, 100, "a 100-key map");
    eq(MsgPackDecode(MsgPackEncode("y".repeat(70000))).length, 70000, "a 70k string");
}

/* ---------------------------------------------- refusals and defences */

throws(() => MsgPackEncode(), "a value is required");
throws(() => MsgPackDecode(), "and bytes to decode");
throws(() => MsgPackDecode("not bytes"), "which must be bytes");
throws(() => MsgPackEncode(() => 1), "a function has no encoding");
throws(() => MsgPackEncode(Symbol("x")), "nor a symbol");
{
    const cyc = { a: 1 };
    cyc.self = cyc;
    throws(() => MsgPackEncode(cyc), "a cycle is refused: no wire format has one");
    throws(() => CBOREncode(cyc), "in CBOR too");
    /* But a value REPEATED in two branches is not a cycle. */
    const shared = { x: 1 };
    eq(j(MsgPackDecode(MsgPackEncode({ a: shared, b: shared }))),
       '{"a":{"x":1},"b":{"x":1}}', "a repeated node is not a cycle");
}
{
    /* THE ATTACK ON A LENGTH-PREFIXED FORMAT: a declared length far larger
     * than the input. It must be refused before anything is allocated. */
    const lies = [
        "dd7fffffff",           /* msgpack array32 of 2 billion elements */
        "df7fffffff",           /* map32 */
        "db7fffffff",           /* str32 */
        "c6ffffffff",           /* bin32 */
        "9a7fffffff",           /* cbor array of 2 billion */
        "bb7fffffffffffffff",   /* cbor map, 64-bit count */
        "5bffffffffffffffff",   /* cbor byte string, 64-bit length */
    ];
    let caught = 0;
    for (const h of lies) {
        try {
            (h[0] === "9" || h[0] === "b" || h[0] === "5" ? CBORDecode : MsgPackDecode)(bytes(h));
        } catch (e) {
            /* The MESSAGE, not merely that it threw: without the bound check
             * these still fail, but as "truncated" after the loop has already
             * started -- two guards against one symptom, and only this
             * distinguishes them. */
            if (String(e.message).indexOf("declared length") >= 0) caught++;
            else print("  " + h + " threw the wrong way: " + e.message);
        }
    }
    eq(caught, lies.length,
       "every lied-about length is refused BEFORE anything is allocated");
}
throws(() => MsgPackDecode(bytes("9301")), "a truncated array is refused");
throws(() => MsgPackDecode(bytes("a5ab")), "and a truncated string");
throws(() => MsgPackDecode(bytes("0101")), "trailing bytes after the value");
throws(() => CBORDecode(bytes("0101")), "in CBOR too");
throws(() => MsgPackDecode(bytes("c1")), "an unassigned type byte");
{
    /* Nesting is bounded, so a deep document is a RangeError not a crash. */
    let deep = "";
    for (let i = 0; i < 400; i++) deep += "91";
    deep += "01";
    throws(() => MsgPackDecode(bytes(deep)), "400-deep nesting is refused");
    let ok = "";
    for (let i = 0; i < 100; i++) ok += "91";
    ok += "01";
    eq(MsgPackDecode(bytes(ok))[0][0].length, 1, "and 100 deep still decodes");
}
{
    /* A key called __proto__ becomes an own property, not a prototype. */
    const o = MsgPackDecode(MsgPackEncode({ ["__proto__"]: "x", b: 1 }));
    eq(Object.keys(o).length, 2, "__proto__ is an own key in the result");
    eq(o.__proto__, "x", "with its value");
}

/* --------------------------------------------------------- ValueHash */

{
    eq(ValueHash({ a: 1, b: 2 }), ValueHash({ b: 2, a: 1 }),
       "key ORDER does not change the hash -- the encoding is canonical");
    assert(ValueHash({ a: 1 }) !== ValueHash({ a: 2 }), "but a value does");
    assert(ValueHash([1, 2]) !== ValueHash([2, 1]), "and array order does");
    assert(ValueHash("1") !== ValueHash(1), "a string is not its number");
    eq(ValueHash(null), ValueHash(null), "the same value hashes the same");
    eq(ValueHash({ a: { b: [1, "x"] } }).length, 16, "the digest is 16 hex digits");
    assert(/^[0-9a-f]{16}$/.test(ValueHash(42)), "and only hex");
    throws(() => ValueHash(), "a value is required");
    const cyc = {}; cyc.me = cyc;
    throws(() => ValueHash(cyc), "a cycle has no canonical encoding");
}
{
    /* The canonical encoding is the one the hash is defined over, so it must
     * be observable and stable. RFC 8949 orders keys by length, then bytes. */
    eq(hex(CBORCanonical({ bb: 1, a: 2 })), "a2616102626262" + "01",
       "canonical CBOR sorts short keys first");
    eq(hex(CBORCanonical({ a: 2, bb: 1 })), hex(CBORCanonical({ bb: 1, a: 2 })),
       "whatever order they were written in");
    assert(hex(CBOREncode({ bb: 1, a: 2 })) !== hex(CBORCanonical({ bb: 1, a: 2 })),
       "while the ordinary encoder preserves insertion order");
}

/* --------------------------------------------------- structuredClone */

{
    const src = { a: 1, b: [1, 2, { c: "three" }], d: new Uint8Array([1, 2]) };
    const c = structuredClone(src);
    eq(j({ a: c.a, b: c.b }), j({ a: src.a, b: src.b }), "a deep clone matches");
    assert(c !== src, "and is a different object");
    assert(c.b !== src.b, "at every level");
    assert(c.b[2] !== src.b[2], "including the leaves");
    assert(c.d instanceof Uint8Array && c.d[1] === 2, "bytes are copied");
    c.b[0] = 99;
    eq(src.b[0], 1, "so mutating the clone does not touch the original");
}
{
    /* THE CASE Object.clone DOES NOT HANDLE, which is why this exists. */
    const a = { name: "a" };
    a.self = a;
    a.list = [a, a];
    const c = structuredClone(a);
    assert(c.self === c, "a self-reference points at the CLONE, not the original");
    assert(c.list[0] === c, "and so does one through an array");
    assert(c.list[0] === c.list[1], "shared identity inside the graph survives");
    assert(c !== a && c.self !== a, "nothing points back at the source");
}
{
    const shared = { v: 1 };
    const c = structuredClone({ x: shared, y: shared });
    assert(c.x === c.y, "a node reached twice is cloned ONCE");
    assert(c.x !== shared, "and it is a clone");
}
eq(structuredClone(42), 42, "a primitive clones to itself");
eq(structuredClone("s"), "s", "a string too");
eq(structuredClone(null), null, "and null");
throws(() => structuredClone(() => 1), "a function cannot be cloned");
throws(() => structuredClone(), "a value is required");

/* The key walk has a fast path that skips the general enumerator's sort and
   allocation. It must REFUSE wherever that reordering matters, and the cases
   below are the refusals -- each one produced identical bytes to the general
   path, and dropping the enumerable filter in the fast path breaks case 4. */
eq(hex(MsgPackEncode({ 1: "a", 0: "b", z: "c" })), "83a130a162a131a161a17aa163",
   "integer keys still sort BEFORE string keys");
{
    const big = {};
    for (let i = 0; i < 300; i++) big["k" + i] = i;
    const b = MsgPackEncode(big);
    eq(b[0], 0xde, "300 keys is a map16 header");
    eq(hex(b.slice(0, 3)) + hex(b.slice(3, 6)), "de012ca2" + "6b30", "and starts at k0");
}
{
    const sym = { a: 1 };
    sym[Symbol("s")] = 2;
    eq(hex(MsgPackEncode(sym)), "81a16101", "a symbol key is not a string key");
}
eq(hex(MsgPackEncode(Object.defineProperty({ v: 1 }, "hidden",
    { value: 2, enumerable: false }))), "81a17601", "a non-enumerable key is skipped");
eq(hex(MsgPackEncode(new Proxy({ p: 1 }, {}))), "81a17001",
   "a proxy is exotic: the general path handles it");
eq(hex(MsgPackEncode({ "\u00e9": 1 })), "81a2c3a901",
   "a non-ASCII key is converted to UTF-8, not borrowed as Latin-1");

if (fails) {
    print("test_vserialize: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_vserialize failed");
}
print("test_vserialize: " + n + " assertions, 0 failures");
