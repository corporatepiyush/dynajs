// flags: --std
/* test_lz4_interop.js -- LZ4 FRAME interop with the system `lz4` CLI, and
 * checksum strictness at the frame boundary.
 *
 * THE ORACLE IS OUTSIDE THE ENGINE: a frame this engine writes must decode
 * with the reference lz4 CLI, and a frame the CLI writes (default block
 * size, block-compressed and stored, content checksum ON -- the CLI's
 * default) must decode here, through BOTH doors (one-shot dyna:compress and
 * the streaming dyna:stream codec). A self round-trip proves nothing about
 * an interop format: encoder and decoder share every mistake.
 *
 * The strictness half pins WHAT is accepted at the checksum boundary: the
 * digest is XXH32 per the LZ4 Frame Format (xxHash spec v0.7.3 -- round
 * rotation 13), exactly, and nothing else. A frame whose content checksum
 * was computed with a different rotation constant must be REFUSED even
 * though some historical build of this engine verified it as an alternate
 * flavor: accepting a non-spec digest is how corrupted frames stop being
 * detectable.
 *
 * Each strictness row corrupts ONE byte and pins the refusal CLASS the
 * decoder must name for it (truncated, corrupt block, descriptor checksum,
 * content checksum, ...): a row that merely asserts "it threw something"
 * would keep passing if the decoder started reporting the wrong reason, or
 * none.
 *
 * Skips LOUDLY when the `lz4` CLI is absent; set DYNAJS_REQUIRE_TOOLS=1 to
 * turn the skip into a failure where the tool is supposed to be installed.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_lz4_interop.js */
import { lz4Frame, lz4Unframe } from "dyna:compress";
import { fromBytes, fromFile, toFile, inflate, deflate } from "dyna:stream";
import { Path, writeFile, readFile, makeDir, removeAll } from "dyna:file";
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + a + ", want " + b + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}
function bytesEq(a, b) {
    if (!a || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
const sh = (cmd) => os.exec(["/bin/sh", "-c", cmd]);

const TMP = std.getenv("TMPDIR") || "/tmp";
const dir = new Path(TMP, "lz4-interop-" + (Date.now() % 10000000));
removeAll(dir);
makeDir(dir, { recursive: true });

const have = sh("command -v lz4 >/dev/null 2>&1") === 0;
if (!have) {
    if (std.getenv("DYNAJS_REQUIRE_TOOLS") === "1")
        throw new Error("REQUIRED tool missing: lz4 -- the interop half of this file cannot run");
    print("test_lz4_interop: SKIPPED LOUDLY -- no `lz4` CLI on PATH; the " +
          "interop half did NOT run (DYNAJS_REQUIRE_TOOLS=1 makes this a failure)");
}

/* A second, deliberately WRONG XXH32 (round rotation 17 instead of the
   spec's 13) -- the exact digest a non-conforming implementation emits.
   Used to prove wrong-flavor checksums are refused, not merely mismatched
   by accident. */
function xxh32WrongRot(data, seed, rot) {
    const P1 = 2654435761 >>> 0, P2 = 2246822519 >>> 0, P3 = 3266489917 >>> 0,
          P4 = 668265263 >>> 0, P5 = 374761393 >>> 0;
    const rotl = (x, r) => ((x << r) | (x >>> (32 - r))) >>> 0;
    const rd = (p) => (data[p] | (data[p + 1] << 8) | (data[p + 2] << 16) | (data[p + 3] << 24)) >>> 0;
    let v1 = (seed + P1 + P2) >>> 0, v2 = (seed + P2) >>> 0, v3 = seed >>> 0, v4 = (seed - P1) >>> 0;
    let p = 0;
    const len = data.length;
    const round = (acc, input) => {
        acc = (acc + Math.imul(input, P2)) >>> 0;
        acc = rotl(acc, rot);
        return Math.imul(acc, P1) >>> 0;
    };
    let h;
    if (len >= 16) {
        while (p + 16 <= len) {
            v1 = round(v1, rd(p)); v2 = round(v2, rd(p + 4));
            v3 = round(v3, rd(p + 8)); v4 = round(v4, rd(p + 12));
            p += 16;
        }
        h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) >>> 0;
    } else {
        h = (seed + P5) >>> 0;
    }
    h = (h + len) >>> 0;
    while (p + 4 <= len) {
        h = (h + Math.imul(rd(p), P3)) >>> 0;
        h = Math.imul(rotl(h, 17), P4) >>> 0;
        p += 4;
    }
    while (p < len) {
        h = (h + Math.imul(data[p], P5)) >>> 0;
        h = Math.imul(rotl(h, 11), P1) >>> 0;
        p++;
    }
    h ^= h >>> 15; h = Math.imul(h, P2) >>> 0;
    h ^= h >>> 13; h = Math.imul(h, P3) >>> 0;
    h ^= h >>> 16;
    return h >>> 0;
}

/* content sizes straddling every interesting boundary */
const SIZES = [0, 1, 12, 15, 16, 17, 63, 64, 255, 256, 4095, 4096, 4097, 65535, 65536, 70000];
function payload(size, seed) {
    const b = new Uint8Array(size);
    for (let i = 0; i < size; i++) b[i] = (i * 31 + seed * 7 + 13) & 0xff;
    return b;
}
/* A payload with no 4-byte repeats, so the frame builder writes the block
   STORED (raw bytes): the frame's block payload is then byte-for-byte the
   content, which is what makes a content-only corruption possible. */
function incompressible(size, seed) {
    const b = new Uint8Array(size);
    let x = (Math.imul(seed, 2654435761) + 1) >>> 0;
    for (let i = 0; i < size; i++) {
        x ^= x << 13; x >>>= 0;
        x ^= x >>> 17;
        x ^= x << 5; x >>>= 0;
        b[i] = x & 0xff;
    }
    return b;
}

if (have) {
    /* ---- ours -> CLI, one-shot, checksum on and off, every size ---- */
    for (const size of SIZES) {
        for (const checksum of [false, true]) {
            const data = payload(size, 1);
            const f = new Path(dir, "ours-" + size + "-" + checksum + ".lz4");
            writeFile(f, lz4Frame(data, { checksum }));
            const rc = sh("lz4 -d -f " + f + " " + f + ".out >/dev/null 2>&1");
            eq(rc, 0, "CLI decodes our one-shot frame (size=" + size + ", checksum=" + checksum + ")");
            assert(bytesEq(readFile(new Path(String(f) + ".out"), { bytes: true }), data),
                   "CLI recovers our bytes (size=" + size + ", checksum=" + checksum + ")");
        }
    }

    /* ---- CLI -> ours, one-shot: default CLI frames carry content
            checksums, so the spec digest is verified on every one ---- */
    for (const size of SIZES) {
        const data = payload(size, 2);
        const src = new Path(dir, "src-" + size + ".bin");
        writeFile(src, data);
        for (const opt of ["", "--no-frame-crc", "--content-size", "-9", "-B4"]) {
            const f = new Path(dir, "cli-" + size + "-" + (opt || "default").replace(/^-/, "") + ".lz4");
            const rc = sh("lz4 " + opt + " -f " + src + " " + f + " >/dev/null 2>&1");
            if (rc !== 0) { assert(false, "CLI produced a frame (size=" + size + ", " + opt + ")"); continue; }
            const theirs = readFile(f, { bytes: true });
            assert(bytesEq(lz4Unframe(theirs), data),
                   "one-shot lz4Unframe decodes the CLI frame (size=" + size + ", " + opt + ")");
        }
    }

    /* ---- streaming codec <-> CLI both directions ---- */
    async function streams() {
        for (const size of [16, 4096, 70000]) {
            const data = payload(size, 3);
            /* ours -> CLI: write+finish produces the canonical frame */
            const f = new Path(dir, "s-ours-" + size + ".lz4");
            const ds = deflate(toFile(f), { codec: "lz4" });
            await ds.write(data);
            await ds.finish();
            eq(sh("lz4 -d -f " + f + " " + f + ".out >/dev/null 2>&1"), 0,
               "CLI decodes our STREAMED frame (size=" + size + ")");
            assert(bytesEq(readFile(new Path(String(f) + ".out"), { bytes: true }), data),
                   "CLI recovers our streamed bytes (size=" + size + ")");

            /* CLI -> ours: default (content checksum) and block-stored */
            for (const opt of ["", "-B4", "--no-frame-crc", "--content-size"]) {
                const src = new Path(dir, "s-src-" + size + ".bin");
                writeFile(src, data);
                const t = new Path(dir, "s-cli-" + size + "-" + (opt || "def") + ".lz4");
                sh("lz4 " + opt + " -f " + src + " " + t + " >/dev/null 2>&1");
                const chunks = [];
                const buf = new Uint8Array(64 * 1024);
                const src2 = inflate(fromFile(t), { codec: "lz4" });
                let got;
                while ((got = await src2.read(buf)) > 0) chunks.push(buf.slice(0, got));
                const total = new Uint8Array(chunks.reduce((a, c) => a + c.length, 0));
                let off = 0;
                for (const c of chunks) { total.set(c, off); off += c.length; }
                assert(bytesEq(total, data),
                       "streamed inflate decodes the CLI frame (size=" + size + ", " + (opt || "def") + ")");
            }
        }
    }
    await streams();

    /* ---- strictness: only the spec digest passes, and every refusal
            NAMES its class ----
       The wrapper reports a specific reason per refusal class (bad magic,
       version, block-max, linked blocks, descriptor checksum, block cap,
       corrupt block, block checksum, CONTENT checksum, declared content
       size). Every row below corrupts exactly one byte and pins the class
       that byte must produce -- a row that accepts any refusal would not
       notice a decoder that started reporting the wrong one. `good`/`raw`
       are the controls: the untouched frame decodes, so the flipped byte is
       the row's only delta. */
    {
        const data = payload(70000, 4);   /* >= 16: the round function runs */
        const good = lz4Frame(data, { checksum: true });
        assert(bytesEq(lz4Unframe(good), data),
               "CONTROL: the untouched frame decodes");

        /* a flipped CONTENT byte: use a frame whose block is STORED, so the
           corrupt byte is raw output and no LZ4 structural check can fire
           first -- the content checksum is the only detector that can catch
           it. (In a COMPRESSED block the same flip at the block's first
           byte is refused earlier as a corrupt block, which would leave the
           checksum untested.) Layout, from the frame builder: 4 magic bytes,
           FLG, BD, descriptor-checksum byte, then each block as a 4-byte
           little-endian size word (high bit = stored) plus its bytes. */
        const raw = incompressible(70000, 9);
        const sf = lz4Frame(raw, { checksum: true });
        const bsz = (sf[7] | (sf[8] << 8) | (sf[9] << 16) | (sf[10] << 24)) >>> 0;
        assert(bsz >>> 31 === 1 && (bsz & 0x7fffffff) === raw.length,
               "layout: the incompressible block is STORED (size word at " +
               "offset 7 sets the high bit and equals the payload length)");
        assert(bytesEq(lz4Unframe(sf), raw),
               "CONTROL: the stored-block frame decodes");
        const cbad = Uint8Array.from(sf);
        cbad[11 + (raw.length >> 1)] ^= 0x01;   /* 11 = first content byte */
        throwsMatch(() => lz4Unframe(cbad), /content checksum mismatch/i,
            "a flipped content byte is caught by the content checksum");

        /* the OTHER tail shape: a checksum-bearing frame ends with the
           4-byte end mark and then the 4-byte content checksum, so
           length - 6 is the THIRD end-mark byte, not content. Flipping it
           declares a 65536-byte block that the frame cannot supply, which
           the size check refuses before any checksum comparison. */
        const ebad = Uint8Array.from(good);
        ebad[ebad.length - 6] ^= 0x01;
        throwsMatch(() => lz4Unframe(ebad), /truncated LZ4 frame/,
            "a corrupt end mark whose declared block size cannot be " +
            "satisfied is refused as truncated");

        /* Re-stamp the frame's content checksum with the WRONG rotation
           (17): such a frame verifies under the non-conforming flavor some
           code used to accept as an alternate. It must be refused even
           though it is otherwise byte-perfect. */
        const wrong = Uint8Array.from(good);
        const wsum = xxh32WrongRot(data, 0, 17);
        const spec = xxh32WrongRot(data, 0, 13);
        assert(wsum !== spec, "the wrong-rotation digest actually differs (flavor sanity)");
        const at = wrong.length - 4;
        wrong[at] = wsum & 0xff; wrong[at + 1] = (wsum >>> 8) & 0xff;
        wrong[at + 2] = (wsum >>> 16) & 0xff; wrong[at + 3] = (wsum >>> 24) & 0xff;
        throwsMatch(() => lz4Unframe(wrong), /content checksum mismatch/i,
            "a wrong-flavor (rot-17) content checksum is refused, not honored");
        /* and the streaming decoder holds the same line */
        await (async () => {
            const chunks = [];
            const buf = new Uint8Array(64 * 1024);
            const s = inflate(fromBytes(wrong), { codec: "lz4" });
            let got, threw = "";
            try { while ((got = await s.read(buf)) > 0) chunks.push(got); }
            catch (e) { threw = String(e.message); }
            assert(/content checksum mismatch/i.test(threw),
                   "streamed inflate refuses the wrong-flavor checksum too (got: " + threw + ")");
        })();

        /* a wrong descriptor checksum byte is refused before any decode */
        const hbad = Uint8Array.from(good);
        hbad[6] ^= 0x01;
        throwsMatch(() => lz4Unframe(hbad), /descriptor checksum mismatch/i,
            "a wrong descriptor checksum is refused");
    }
}

removeAll(dir);
print("test_lz4_interop: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed") +
      (have ? "" : " (INTEROP SKIPPED: no lz4 CLI)"));
if (fails) throw new Error(fails + " failures");
