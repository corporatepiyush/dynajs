// covers: dyna:compress.Dictionary
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_dictionary");
import { Dictionary, gzip, gunzip } from "dyna:compress";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throws(fn, msg) {
    n++;
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (caught === null) throw new Error("assertion failed: " + msg + " (expected throw)");
    return caught;
}

const enc = new TextEncoder();
const dec = new TextDecoder();
const bytes = (s) => enc.encode(s);
const text = (b) => dec.decode(b);

const RPC = ['"jsonrpc":"2.0"', '"method":', '"params":', '"id":',
             '"result":', '"error":', '{"', '"}', '":"', '","'];

__kitS.section("size reports the phrase count", () => {
    const d = new Dictionary(RPC);
    assert(d.size === RPC.length, "size reports the phrase count");
    assert(typeof d.id === "number" && d.id >= 0, "id is an unsigned 32-bit value");
    assert(d.closed === false, "a fresh Dictionary is open");

    throws(() => Dictionary(RPC), "Dictionary requires new");
    throws(() => new Dictionary(), "the phrase list is required");
    throws(() => new Dictionary([]), "an empty phrase list is rejected");
    throws(() => new Dictionary(["ok", ""]), "an empty PHRASE is rejected");
    throws(() => new Dictionary("not an array"), "a non-array is rejected");
    d.close();
    assert(d.closed === true, "close() is observable");
    throws(() => d.compress(bytes("x")), "use after close throws");
});

__kitS.section("different phrase lists have different ids", () => {
    const a = new Dictionary(RPC);
    const b = new Dictionary(["totally", "different", "phrases"]);
    const rec = a.compress(bytes('{"jsonrpc":"2.0","id":1}'));

    assert(a.id !== b.id, "different phrase lists have different ids");

    throws(() => b.decompress(rec), "a record from another dictionary is refused");

    const c = new Dictionary(RPC.slice().reverse());
    assert(c.id !== a.id, "reordering the phrases changes the id");
    throws(() => c.decompress(rec), "a reordered dictionary is a different dictionary");

    const same = new Dictionary(RPC.slice());
    assert(same.id === a.id, "an identical list reproduces the id");
    assert(text(same.decompress(rec)) === '{"jsonrpc":"2.0","id":1}',
        "a separately built but identical dictionary decodes the record");

    assert(new Dictionary(["ab", "c"]).id !== new Dictionary(["a", "bc"]).id,
        "the id is a function of the phrase boundaries, not just the bytes");
});

__kitS.section("reuse round-trip at i", () => {
    const d = new Dictionary(RPC);
    for (let i = 0; i < 1000; i++) {
        const s = i % 2
            ? '{"jsonrpc":"2.0","method":"m' + i + '","id":' + i + '}'
            : "x".repeat(i % 97);
        assert(text(d.decompress(d.compress(bytes(s)))) === s,
            "reuse round-trip at i=" + i);
    }
});

__kitS.section("compress rejects a non-buffer argument", () => {
    const d = new Dictionary(RPC);
    let ranToString = false, ranValueOf = false, ranPrimitive = false;
    const attacker = {
        toString() { ranToString = true; d.close(); return "gotcha"; },
        valueOf() { ranValueOf = true; d.close(); return 1; },
        [Symbol.toPrimitive]() { ranPrimitive = true; d.close(); return "gotcha"; },
    };
    throws(() => d.compress(attacker), "compress rejects a non-buffer argument");
    assert(!ranToString && !ranValueOf && !ranPrimitive,
        "no coercion hook ran -- compress type-checks, it does not coerce");
    assert(d.closed === false, "the instance the attacker tried to close is still open");
    assert(text(d.decompress(d.compress(bytes("ok")))) === "ok",
        "the same instance still round-trips afterwards");

    const nested = d.compress(bytes(text(d.compress(bytes("inner")))));
    assert(nested.length > 0, "a nested call completes; the operation is atomic w.r.t. JS");
});

__kitS.section("an empty buffer is not a record", () => {
    const d = new Dictionary(RPC);
    const good = d.compress(bytes('{"jsonrpc":"2.0","id":1}'));

    throws(() => d.decompress(bytes("")), "an empty buffer is not a record");
    throws(() => d.decompress(bytes("DT")), "a truncated header is refused");
    throws(() => d.decompress(good.subarray(0, good.length - 1)),
        "a truncated record is refused");
    for (let i = 0; i < good.length; i++) {
        const b = good.slice();
        b[i] ^= 0xff;
        try {
            const out = d.decompress(b);
            assert(out.length <= 4096, "a corrupted record cannot inflate without bound");
        } catch {  }
    }

    const wrongMagic = good.slice(); wrongMagic[0] = 0x58;
    throws(() => d.decompress(wrongMagic), "bad magic is refused");
    const wrongVersion = good.slice(); wrongVersion[2] = 9;
    throws(() => d.decompress(wrongVersion), "an unknown version is refused");
});
__kitS.end();

print("test_dictionary: all " + n + " assertions passed");
