// flags: --std
/* test_lz4_unframe_refusals.js -- every lz4Unframe refusal names its class.
 *
 * The old contract collapsed EVERY failure class -- bad magic, bad checksum,
 * truncated input, corrupt block -- into the one anonymous "invalid LZ4
 * frame", so a refused frame was undiagnosable from the message alone. The
 * contract now is: each class has its own message, the message is stable,
 * and the classes are distinguishable from one another (the matrix below
 * also asserts pairwise distinctness, so no two classes can drift onto one
 * phrase).
 *
 * Every crafted frame is the FORMAT'S own arithmetic -- the header checksum
 * is computed XXH32, not guessed -- and each mutation is chosen to hit
 * exactly one refusal class, with valid controls beside it proving the SHAPE
 * decodes and only the corruption is refused.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_lz4_unframe_refusals.js */
import { lz4Frame, lz4Unframe, Compressor } from "dyna:compress";

let n = 0, bad = 0;
function ok(c, w) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w); }
}
const enc = (s) => new TextEncoder().encode(s);
const dec = (u) => new TextDecoder().decode(u);
const eqArr = (a, b) => a.length === b.length &&
    Array.from(a).every((x, i) => x === b[i]);

/* XXH32 (spec v0.7.3) -- a literal reimplementation, so the header and
   block checksums in the crafted frames are computed, not guessed. */
function xxh32(bytes, seed = 0) {
    const P1 = 2654435761, P2 = 2246822519, P3 = 3266489917,
          P4 = 668265263, P5 = 374761393;
    const mul = (a, b) => Math.imul(a, b) >>> 0;
    const rotl = (x, r) => ((x << r) | (x >>> (32 - r))) >>> 0;
    const u32 = (i) => (bytes[i] | (bytes[i+1] << 8) | (bytes[i+2] << 16) |
                        (bytes[i+3] << 24)) >>> 0;
    let i = 0, h;
    if (bytes.length >= 16) {
        let h1 = (seed + P1 + P2) >>> 0, h2 = (seed + P2) >>> 0,
            h3 = seed >>> 0, h4 = (seed - P1) >>> 0;
        for (; i + 16 <= bytes.length; i += 16) {
            h1 = mul(rotl((h1 + mul(u32(i), P2)) >>> 0, 13), P1);
            h2 = mul(rotl((h2 + mul(u32(i+4), P2)) >>> 0, 13), P1);
            h3 = mul(rotl((h3 + mul(u32(i+8), P2)) >>> 0, 13), P1);
            h4 = mul(rotl((h4 + mul(u32(i+12), P2)) >>> 0, 13), P1);
        }
        h = (rotl(h1, 1) + rotl(h2, 7) + rotl(h3, 12) + rotl(h4, 18)) >>> 0;
    } else {
        h = (seed + P5) >>> 0;
    }
    h = (h + bytes.length) >>> 0;
    for (; i + 4 <= bytes.length; i += 4)
        h = mul(rotl((h + mul(u32(i), P3)) >>> 0, 17), P4);
    for (; i < bytes.length; i++)
        h = mul(rotl((h + mul(bytes[i], P5)) >>> 0, 11), P1);
    h ^= h >>> 15; h = mul(h, P2);
    h ^= h >>> 13; h = mul(h, P3);
    h ^= h >>> 16;
    return h >>> 0;
}
const le32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff,
                     (v >>> 24) & 0xff];
const le64 = (v) => { const lo = v % 4294967296, hi = Math.floor(v / 4294967296);
                      return [...le32(lo), ...le32(hi)]; };

/* A fully hand-built frame: control over every header byte (FLG/BD beyond
   what lz4Frame emits), block framing and checksums. `blocks` is a list of
   { stored: Uint8Array } or { raw: [bytes] } (raw = compressed-as-written). */
function craft(opts, blocks) {
    const flg = (opts.flg !== undefined) ? opts.flg :
                (0x40 | 0x20 | (opts.contentSize !== undefined ? 0x08 : 0) |
                 (opts.blockChecksum ? 0x10 : 0) | (opts.checksum ? 0x04 : 0));
    const bd = (opts.bd !== undefined) ? opts.bd : 0x70;
    const mid = (opts.contentSize !== undefined) ? le64(opts.contentSize) : [];
    const desc = [flg, bd, ...mid];
    const hc = (opts.hc !== undefined) ? opts.hc :
               ((xxh32(new Uint8Array(desc)) >>> 8) & 0xff);
    let body = [];
    for (const b of blocks) {
        if (b.stored) {
            body = [...body, ...le32(b.stored.length | 0x80000000),
                    ...b.stored];
            if (opts.blockChecksum)
                body = [...body, ...le32(xxh32(b.stored))];
        } else {
            body = [...body, ...le32(b.raw.length), ...b.raw];
            if (opts.blockChecksum)
                body = [...body, ...le32(xxh32(new Uint8Array(b.raw)))];
        }
    }
    const tail = [...le32(0)];                       /* end mark */
    if (opts.checksum)
        tail.push(...le32(xxh32(new Uint8Array(opts.content || []))));
    return new Uint8Array([0x04, 0x22, 0x4d, 0x18, ...desc, hc, ...body,
                           ...tail]);
}

/* Run lz4Unframe; return the thrown message or null when it decoded. */
function refusal(u) {
    try { lz4Unframe(u); return null; }
    catch (e) { return String(e.message); }
}
const PREFIX = "dyna:compress lz4Unframe: ";

const payload = enc("the refusal matrix payload: abcabcabcabcabcabcabcabc");
const framed = lz4Frame(payload);                    /* valid control frame */
const observed = new Set();   /* every message text actually produced */

/* ---- the matrix: one crafted input per refusal class ------------------- */
function expect(label, input, want /* the EXACT message suffix */) {
    const msg = refusal(input);
    ok(msg !== null, label + ": refused");
    if (msg === null) return;
    ok(msg === PREFIX + want,
       label + ": names its reason, want \"" + want + "\" [" + msg + "]");
    observed.add(msg);
}

/* 1-2. too short to read the magic at all: TRUNCATED, whatever the bytes */
expect("empty input", new Uint8Array([]), "truncated LZ4 frame");
expect("3-byte garbage", new Uint8Array([1, 2, 3]), "truncated LZ4 frame");

/* 3. 5 garbage bytes: long enough to SEE the magic is wrong -> bad magic,
      not truncation (the two must never swap names) */
{
    const msg = refusal(new Uint8Array([1, 2, 3, 4, 5]));
    ok(msg === PREFIX + "not an LZ4 frame (bad magic)",
       "5-byte garbage: names bad magic [" + msg + "]");
    ok(msg !== null && msg.indexOf("truncated") < 0,
       "5-byte garbage: is NOT called truncated [" + msg + "]");
    observed.add(msg);
}

/* 4. REAL magic but only 5 bytes: truncated, not bad magic */
{
    const msg = refusal(new Uint8Array([0x04, 0x22, 0x4d, 0x18, 0x60]));
    ok(msg === PREFIX + "truncated LZ4 frame",
       "5-byte real header: names truncated [" + msg + "]");
    ok(msg !== null && msg.indexOf("bad magic") < 0,
       "5-byte real header: is NOT called bad magic [" + msg + "]");
    observed.add(msg);
}

/* 5. FLG version bits not 01 */
expect("wrong frame version",
       craft({ flg: 0x20 }, [{ stored: payload }]), "unsupported LZ4 frame version");

/* 6. BD block-max code outside 4..7 */
expect("reserved block-max size",
       craft({ bd: 0x30 }, [{ stored: payload }]), "invalid LZ4 block-max size");

/* 7. FLG bit 5 clear: linked blocks, refused with their own name */
expect("linked blocks",
       craft({ flg: 0x40 }, [{ stored: payload }]),
       "linked LZ4 blocks are not supported");

/* 8. header (descriptor) checksum corrupted */
{
    const bad = framed.slice();
    bad[6] ^= 0xff;                                  /* the HC byte */
    expect("corrupt descriptor checksum", bad,
           "LZ4 frame descriptor checksum mismatch");
}

/* 9. input ends inside a block / before the end mark */
expect("truncated mid-block", framed.slice(0, framed.length - 4),
       "truncated LZ4 frame");

/* 10. a compressed block that does not decode: token says 270 literals
       with none present -- deterministic, not a lucky bit flip */
expect("corrupt compressed block",
       craft({}, [{ raw: [0xff, 0xff] }]), "corrupt LZ4 block");

/* 10b. byte-flip sweep over a REAL compressed block (this frame carries a
        content checksum, so a flip that decodes to garbage is caught as a
        content-checksum mismatch -- itself a named class). Whatever refuses
        must refuse in the block/content classes, never another name. */
{
    const hard = lz4Frame(enc("compress me compress me compress me compress me"));
    let sawCorrupt = 0;
    /* The sweep stays inside the COMPRESSED BLOCK's bytes (after the 4-byte
       block size field, before the end mark): a flip in a length field would
       legitimately be a cap/truncation refusal, which is a different row. */
    for (let i = 11; i < hard.length - 8; i++) {
        const bad = hard.slice();
        bad[i] ^= 0xff;
        const msg = refusal(bad);
        if (msg === null) continue;
        ok(msg === PREFIX + "corrupt LZ4 block" ||
           msg === PREFIX + "LZ4 content checksum mismatch",
           "block flip @" + i + " refuses in the block classes [" + msg + "]");
        if (msg === PREFIX + "corrupt LZ4 block") sawCorrupt++;
    }
    ok(sawCorrupt > 0, "the flip sweep finds at least one corrupt-block refusal");
}

/* 11. a block longer than the frame's OWN declared block-max (BD=4 -> 64K):
       present in full, so this is the cap, not truncation */
{
    const big = new Uint8Array(70000);
    for (let i = 0; i < big.length; i++) big[i] = i & 0x7f;
    expect("block over the declared cap",
           craft({ bd: 0x40 }, [{ stored: big }]),
           "LZ4 block exceeds the declared block-max size");
}

/* 12. per-block checksum corrupted (the checksum covers the block AS
       STORED, so this is refused even though the block decodes) */
{
    const good = craft({ blockChecksum: true }, [{ stored: payload }]);
    ok(refusal(good) === null, "valid block checksum: decodes (control)");
    const bad = good.slice();
    bad[bad.length - 5] ^= 0xff;
    expect("corrupt block checksum", bad, "LZ4 block checksum mismatch");
}

/* 13. content checksum corrupted */
{
    const good = craft({ checksum: true, content: payload },
                       [{ stored: payload }]);
    ok(refusal(good) === null, "valid content checksum: decodes (control)");
    const bad = good.slice();
    bad[bad.length - 1] ^= 0xff;
    expect("corrupt content checksum", bad, "LZ4 content checksum mismatch");
}

/* 14. a frame that DECLARES its content size and lies */
expect("declared content size lies",
       craft({ contentSize: payload.length + 1 }, [{ stored: payload }]),
       "LZ4 frame declared content size mismatch");

/* ---- the classes must be DISTINGUISHABLE ------------------------------
 * Eleven classes, eleven exact messages: no two classes may drift onto one
 * phrase, and no class may go unnamed. */
{
    const expected = [
        "truncated LZ4 frame",
        "not an LZ4 frame (bad magic)",
        "unsupported LZ4 frame version",
        "invalid LZ4 block-max size",
        "linked LZ4 blocks are not supported",
        "LZ4 frame descriptor checksum mismatch",
        "LZ4 block exceeds the declared block-max size",
        "corrupt LZ4 block",
        "LZ4 block checksum mismatch",
        "LZ4 content checksum mismatch",
        "LZ4 frame declared content size mismatch",
    ];
    ok(new Set(expected).size === expected.length,
       "the expected class messages are themselves pairwise distinct");
    let missing = 0;
    for (const e of expected)
        if (!observed.has(PREFIX + e)) {
            missing++;
            print("  FAIL  refusal class never observed: \"" + e + "\"");
        }
    ok(missing === 0,
       "all " + expected.length + " refusal classes observed with their own " +
       "message (" + observed.size + " distinct messages seen)");
    ok(observed.size >= expected.length,
       "no two classes share a message");
}

/* ---- positive controls: the shapes themselves still decode ------------ */
{
    ok(eqArr(lz4Unframe(framed), payload), "valid frame round trip");
    ok(lz4Unframe(lz4Frame(new Uint8Array(0))).length === 0,
       "empty frame decodes");
    ok(lz4Unframe(lz4Frame("hello"), { asString: true }) === "hello",
       "asString frame decodes");
    ok(eqArr(lz4Unframe(craft({ checksum: true, content: payload },
                              [{ stored: payload }])), payload),
       "hand-crafted checksum frame round trip");
}

/* ---- Compressor.decompress names the same classes --------------------- */
{
    const z = new Compressor({ algo: "lz4frame" });
    let msg = null;
    try { z.decompress(new Uint8Array([1, 2, 3, 4, 5])); }
    catch (e) { msg = String(e.message); }
    ok(msg !== null && msg.indexOf("bad magic") >= 0,
       "Compressor(lz4frame).decompress names bad magic [" + msg + "]");
    msg = null;
    try { z.decompress(framed.slice(0, framed.length - 4)); }
    catch (e) { msg = String(e.message); }
    ok(msg !== null && msg.indexOf("truncated LZ4 frame") >= 0,
       "Compressor(lz4frame).decompress names truncation [" + msg + "]");
    ok(eqArr(z.decompress(framed), payload),
       "Compressor(lz4frame).decompress still decodes valid frames");
    z.close();
}

console.log("test_lz4_unframe_refusals.js: " + n + " assertions passed" +
            (bad ? ", " + bad + " FAILED" : ""));
if (bad) throw new Error("test_lz4_unframe_refusals.js: " + bad + " failures");
