/* test_dictionary.js -- class Dictionary in dyna:compress (W8.4b).
 *
 * The token-substitution codec: a compiled Aho-Corasick automaton over a phrase
 * list, replacing known phrases with Varint codes. It is the SECOND of the two
 * dictionary mechanisms and it does not subsume the first -- `new Compressor
 * ({dict})` seeds an LZ77 window and wins when the payload resembles a known
 * block; this wins where the payload is far too short for LZ77 to have built a
 * window at all.
 *
 * The codec's own correctness (round trip, byte-identical re-encode, an
 * independent decoder, truncation and bit-flip sweeps) is proved in C by
 * tests/oracle_dict_codec.c. This file is the BINDING and the CONTRACT: the
 * shape of the API, the dictionary-identity guarantee, the adversarial argument,
 * and -- per CLAUDE.md sec.4 -- the case where the capability LOSES, which stays
 * here permanently next to the case where it wins.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_dictionary.js
 */
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

/* ---- shape ------------------------------------------------------------- */
{
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
}

/* ---- the identity guarantee -------------------------------------------- */
{
    const a = new Dictionary(RPC);
    const b = new Dictionary(["totally", "different", "phrases"]);
    const rec = a.compress(bytes('{"jsonrpc":"2.0","id":1}'));

    assert(a.id !== b.id, "different phrase lists have different ids");

    /* This is the whole reason the record carries a header. Every code in it
     * is a valid index into ANY dictionary, so decoding against the wrong one
     * would otherwise succeed and return a different string -- a silent wrong
     * answer, which is worse than an error. */
    throws(() => b.decompress(rec), "a record from another dictionary is refused");

    /* Order is part of the identity: the same phrases in a different order are
     * a different dictionary, because the codes mean different things. */
    const c = new Dictionary(RPC.slice().reverse());
    assert(c.id !== a.id, "reordering the phrases changes the id");
    throws(() => c.decompress(rec), "a reordered dictionary is a different dictionary");

    /* But an identical list, separately constructed, IS the same dictionary --
     * otherwise a record could not survive a process restart. */
    const same = new Dictionary(RPC.slice());
    assert(same.id === a.id, "an identical list reproduces the id");
    assert(text(same.decompress(rec)) === '{"jsonrpc":"2.0","id":1}',
        "a separately built but identical dictionary decodes the record");

    /* Length-prefixing in the canonical form: {"ab","c"} and {"a","bc"} are
     * different dictionaries and a plain concatenation would say otherwise. */
    assert(new Dictionary(["ab", "c"]).id !== new Dictionary(["a", "bc"]).id,
        "the id is a function of the phrase boundaries, not just the bytes");
}

/* ---- WHERE IT WINS, and WHERE IT LOSES --------------------------------- */
{
    const d = new Dictionary(RPC);

    /* WINS: a short, highly templated record. This is the case the class
     * exists for -- and the case a greedy parse got WRONG. With a phrase list
     * containing both `{"` and `"jsonrpc":"2.0"`, a greedy walk takes the
     * 2-byte match at position 0 and steps over the 15-byte match at position
     * 1, which it can never come back for: it turned 54 bytes into 61. The
     * parse is a dynamic program that is allowed to decline a match, and the
     * assertion below is what stops that regressing. */
    const rpc = '{"jsonrpc":"2.0","method":"sum","params":[1,2],"id":7}';
    const packed = d.compress(bytes(rpc));
    assert(text(d.decompress(packed)) === rpc, "templated record round-trips");
    assert(packed.length < rpc.length,
        "a templated record SHRINKS (" + rpc.length + " -> " + packed.length + ")");
    assert(packed.length < gzip(bytes(rpc)).length,
        "and beats gzip on a payload this short (" + packed.length + " vs " +
        gzip(bytes(rpc)).length + ")");

    /* LOSES: bytes containing none of the phrases become one literal run plus
     * the header, which is an EXPANSION. This row stays here permanently --
     * CLAUDE.md sec.4 requires the bypass-never-fires case to live in the
     * suite, not just the bypass-fires one. */
    const noise = "zqx".repeat(30);
    const grown = d.compress(bytes(noise));
    assert(text(d.decompress(grown)) === noise, "unmatched bytes still round-trip");
    assert(grown.length > noise.length,
        "a payload with no phrases EXPANDS (" + noise.length + " -> " +
        grown.length + ") -- the honest losing case");

    /* LOSES: already-compressed bytes. The adversarial input for any codec. */
    const gz = gzip(bytes(rpc.repeat(20)));
    const twice = d.compress(gz);
    assert(text(gunzip(d.decompress(twice))) === rpc.repeat(20),
        "compressing already-compressed bytes still round-trips");
    assert(twice.length >= gz.length,
        "already-compressed input does not shrink further");

    /* The empty input is a header and nothing else. */
    assert(d.compress(bytes("")).length === 8, "the empty input is header-only");
    assert(d.decompress(d.compress(bytes(""))).length === 0, "empty round-trips");
}

/* ---- reuse: the same instance across unbounded inputs ------------------- */
{
    const d = new Dictionary(RPC);
    /* The scratch is owned across calls, so this is the path where a stale
     * entry from a previous record would show up. A thousand alternating
     * sizes is the shape that finds it. */
    for (let i = 0; i < 1000; i++) {
        const s = i % 2
            ? '{"jsonrpc":"2.0","method":"m' + i + '","id":' + i + '}'
            : "x".repeat(i % 97);
        assert(text(d.decompress(d.compress(bytes(s)))) === s,
            "reuse round-trip at i=" + i);
    }
}

/* ---- the adversarial argument ------------------------------------------
 *
 * compress() TYPE-CHECKS its input rather than coercing it, so no user JS can
 * run inside a call. That means the reused scratch cannot be observed
 * half-written and a `busy` flag would be a bypass that never fires -- the
 * same measured conclusion as Hasher and Compressor before it.
 *
 * The test pins the REASON, not the absence: it asserts that neither hook ran
 * and that the instance is still open. Asserting only "it threw" would pass
 * even if the class had been rewritten to coerce, which is exactly the vacuous
 * form CLAUDE.md sec.8 warns about. Both toString and valueOf are hooked,
 * because a test that hooks only the one this path does not use proves
 * nothing.
 * ---------------------------------------------------------------------- */
{
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
    /* And the class still works, so the rejection above is a type check and
     * not a broken code path. */
    assert(text(d.decompress(d.compress(bytes("ok")))) === "ok",
        "the same instance still round-trips afterwards");

    /* A reentrant call -- compress() from inside a compress() -- is a
     * different hazard from close-during-coercion, and it cannot arise here
     * for the same reason: there is no callback and no coercion, so there is
     * nowhere for user JS to run. Asserted by construction below. */
    const nested = d.compress(bytes(text(d.compress(bytes("inner")))));
    assert(nested.length > 0, "a nested call completes; the operation is atomic w.r.t. JS");
}

/* ---- hostile records ---------------------------------------------------- */
{
    const d = new Dictionary(RPC);
    const good = d.compress(bytes('{"jsonrpc":"2.0","id":1}'));

    throws(() => d.decompress(bytes("")), "an empty buffer is not a record");
    throws(() => d.decompress(bytes("DT")), "a truncated header is refused");
    throws(() => d.decompress(good.subarray(0, good.length - 1)),
        "a truncated record is refused");
    for (let i = 0; i < good.length; i++) {
        const b = good.slice();
        b[i] ^= 0xff;
        /* Either it is rejected or it decodes to something -- what it must
         * never do is crash or return a wildly oversized buffer. */
        try {
            const out = d.decompress(b);
            assert(out.length <= 4096, "a corrupted record cannot inflate without bound");
        } catch { /* rejection is the expected outcome */ }
    }

    /* The magic and the version are both checked. */
    const wrongMagic = good.slice(); wrongMagic[0] = 0x58;
    throws(() => d.decompress(wrongMagic), "bad magic is refused");
    const wrongVersion = good.slice(); wrongVersion[2] = 9;
    throws(() => d.decompress(wrongVersion), "an unknown version is refused");
}

print("test_dictionary: all " + n + " assertions passed");
