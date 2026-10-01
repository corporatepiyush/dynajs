// flags: --std
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

const SIZES = [0, 1, 12, 15, 16, 17, 63, 64, 255, 256, 4095, 4096, 4097, 65535, 65536, 70000];
function payload(size, seed) {
    const b = new Uint8Array(size);
    for (let i = 0; i < size; i++) b[i] = (i * 31 + seed * 7 + 13) & 0xff;
    return b;
}
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

    async function streams() {
        for (const size of [16, 4096, 70000]) {
            const data = payload(size, 3);
            const f = new Path(dir, "s-ours-" + size + ".lz4");
            const ds = deflate(toFile(f), { codec: "lz4" });
            await ds.write(data);
            await ds.finish();
            eq(sh("lz4 -d -f " + f + " " + f + ".out >/dev/null 2>&1"), 0,
               "CLI decodes our STREAMED frame (size=" + size + ")");
            assert(bytesEq(readFile(new Path(String(f) + ".out"), { bytes: true }), data),
                   "CLI recovers our streamed bytes (size=" + size + ")");

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

    {
        const data = payload(70000, 4);
        const good = lz4Frame(data, { checksum: true });
        assert(bytesEq(lz4Unframe(good), data),
               "CONTROL: the untouched frame decodes");

        const raw = incompressible(70000, 9);
        const sf = lz4Frame(raw, { checksum: true });
        const bsz = (sf[7] | (sf[8] << 8) | (sf[9] << 16) | (sf[10] << 24)) >>> 0;
        assert(bsz >>> 31 === 1 && (bsz & 0x7fffffff) === raw.length,
               "layout: the incompressible block is STORED (size word at " +
               "offset 7 sets the high bit and equals the payload length)");
        assert(bytesEq(lz4Unframe(sf), raw),
               "CONTROL: the stored-block frame decodes");
        const cbad = Uint8Array.from(sf);
        cbad[11 + (raw.length >> 1)] ^= 0x01;
        throwsMatch(() => lz4Unframe(cbad), /content checksum mismatch/i,
            "a flipped content byte is caught by the content checksum");

        const ebad = Uint8Array.from(good);
        ebad[ebad.length - 6] ^= 0x01;
        throwsMatch(() => lz4Unframe(ebad), /truncated LZ4 frame/,
            "a corrupt end mark whose declared block size cannot be " +
            "satisfied is refused as truncated");

        const wrong = Uint8Array.from(good);
        const wsum = xxh32WrongRot(data, 0, 17);
        const spec = xxh32WrongRot(data, 0, 13);
        assert(wsum !== spec, "the wrong-rotation digest actually differs (flavor sanity)");
        const at = wrong.length - 4;
        wrong[at] = wsum & 0xff; wrong[at + 1] = (wsum >>> 8) & 0xff;
        wrong[at + 2] = (wsum >>> 16) & 0xff; wrong[at + 3] = (wsum >>> 24) & 0xff;
        throwsMatch(() => lz4Unframe(wrong), /content checksum mismatch/i,
            "a wrong-flavor (rot-17) content checksum is refused, not honored");
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
