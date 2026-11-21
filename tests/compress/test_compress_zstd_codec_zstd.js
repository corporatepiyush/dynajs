// flags: --std
// covers: dyna:compress.zstd
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zstd_codec_zstd");
import * as std from "std";
import { zstd, unzstd, brotli, unbrotli, snappy, unsnappy, Compressor }
    from "dyna:compress";
import { SHA256Hex } from "dyna:hash";
import { Exec } from "dyna:sys";
import { Path, writeFile, readFile } from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) { let t = false; try { fn(); } catch (e) { t = true; } assert(t, msg); }
function throwsMsg(fn, substr, msg) {
    let t = false;
    try { fn(); } catch (e) { t = String(e).indexOf(substr) >= 0; }
    assert(t, msg + " (want a throw containing: " + substr + ")");
}
const enc = new TextEncoder();
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
function b2s(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s; }
function sha(u8) { return SHA256Hex(u8); }

const CLI = {};

__kitS.section("zstd", () => {
    let r = null;
    try { r = Exec("zstd", ["--version"]); } catch (e) {}
    CLI.zstd = r && r.code === 0;
    r = null;
    try { r = Exec("brotli", ["--version"]); } catch (e) {}
    CLI.brotli = r && r.code === 0;
});

function skip(what) { print("SKIP " + what + " (CLI absent)"); }

const TEXT = "the quick brown fox jumps over the lazy dog ".repeat(64);

const TEXT_BYTES = enc.encode(TEXT);

for (const sz of [0, 1, 65535, 65536, 65537, (1 << 20) - 1, 1 << 20]) {
    const data = new Uint8Array(sz);
    for (let i = 0; i < sz; i++) data[i] = i * 31 & 0xff;
    eq(sha(unzstd(zstd(data))), sha(data), "zstd roundtrip " + sz);
    eq(sha(unbrotli(brotli(data))), sha(data), "brotli roundtrip " + sz);
    eq(sha(unsnappy(snappy(data))), sha(data), "snappy roundtrip " + sz);
}

eq(unzstd(zstd(TEXT_BYTES), { asString: true }), TEXT, "zstd asString");

if (CLI.zstd) {
    const tmp = `${std.getenv("TMPDIR") || "/tmp"}/dj_zstd.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}`;
    Exec("sh", ["-c", "rm -rf " + tmp + " && mkdir -p " + tmp]);
    writeFile(new Path(tmp + "/ours.bin"), zstd(TEXT_BYTES));
    const r = Exec("sh", ["-c", "zstd -d -c " + tmp + "/ours.bin > " + tmp + "/out.txt"]);
    eq(r.code, 0, "zstd CLI decodes our frame");
    eq(sha(readFile(new Path(tmp + "/out.txt"))), sha(TEXT_BYTES), "zstd CLI output equals the input");
    writeFile(new Path(tmp + "/plain.txt"), TEXT_BYTES);
    const r2 = Exec("sh", ["-c",
        "zstd -c " + tmp + "/plain.txt | xxd -p | tr -d '\\n'"]);
    eq(r2.code, 0, "zstd CLI compresses");
    const cliFrame = bytes(r2.stdout);
    eq(cliFrame[0], 0x28, "the CLI frame starts with the zstd magic");
    eq(sha(unzstd(cliFrame)), sha(TEXT_BYTES), "we decode the CLI's frame");
} else skip("zstd");

__kitS.section("corrupted snappy refuses", () => {
    const good = snappy(TEXT_BYTES);
    const bad = good.slice();
    bad[0] = 0xff;
    throws(() => unsnappy(bad), "corrupted snappy refuses");
    const gz = zstd(TEXT_BYTES);
    const bz = gz.slice();
    bz[gz.length - 1] ^= 0xff;
    throws(() => unzstd(bz), "corrupted zstd refuses");
    throws(() => unzstd(42), "unzstd of a number refuses");
    throws(() => zstd({}), "zstd of an object refuses");
    throws(() => unsnappy("text"), "unsnappy of a plain string refuses");
});

__kitS.section("table path bytes == one-shot path bytes", () => {
    for (const algo of ["zstd", "brotli", "snappy"]) {
        const c = new Compressor({ algo });
        const one = algo === "zstd" ? zstd(TEXT_BYTES)
                  : algo === "brotli" ? brotli(TEXT_BYTES)
                  : snappy(TEXT_BYTES);
        const via = c.compress(TEXT_BYTES);
        eq(sha(one), sha(via), algo + ": table path bytes == one-shot path bytes");
        const dec = c.decompress(via);
        eq(sha(dec), sha(TEXT_BYTES), algo + ": table decompress roundtrip");
        c.close();
    }
    throws(() => new Compressor({ algo: "zstd", dict: "x" }),
        "a dictionary is refused for zstd");
    throws(() => new Compressor({ algo: "zstd", level: 99 }),
        "zstd level out of range");
    throws(() => new Compressor({ algo: "brotli", level: -1 }),
        "brotli level out of range");
});
__kitS.end();

if (fails) {
    print("test_codec_zstd: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_codec_zstd failed");
}
print("test_codec_zstd: " + n + " assertions, 0 failures");
