// flags: --std
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
{
    let r = null;
    try { r = Exec("zstd", ["--version"]); } catch (e) {}
    CLI.zstd = r && r.code === 0;
    r = null;
    try { r = Exec("brotli", ["--version"]); } catch (e) {}
    CLI.brotli = r && r.code === 0;
}
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
eq(unbrotli(brotli(TEXT_BYTES), { asString: true }), TEXT, "brotli asString");
eq(unsnappy(snappy(TEXT_BYTES), { asString: true }), TEXT, "snappy asString");

if (CLI.zstd) {
    const tmp = `${std.getenv("TMPDIR") || "/tmp"}/dj_zstd.${Date.now() % 10000000}`;
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

if (CLI.brotli) {
    const tmp = `${std.getenv("TMPDIR") || "/tmp"}/dj_brotli.${Date.now() % 10000000}`;
    Exec("sh", ["-c", "rm -rf " + tmp + " && mkdir -p " + tmp]);
    writeFile(new Path(tmp + "/ours.br"), brotli(TEXT_BYTES));
    const r = Exec("sh", ["-c", "brotli -d -c " + tmp + "/ours.br > " + tmp + "/out.txt"]);
    eq(r.code, 0, "brotli CLI decodes our frame");
    eq(sha(readFile(new Path(tmp + "/out.txt"))), sha(TEXT_BYTES), "brotli CLI output equals the input");
    writeFile(new Path(tmp + "/plain.txt"), TEXT_BYTES);
    const r2 = Exec("sh", ["-c",
        "brotli -c " + tmp + "/plain.txt | xxd -p | tr -d '\\n'"]);
    eq(r2.code, 0, "brotli CLI compresses");
    eq(sha(unbrotli(bytes(r2.stdout))), sha(TEXT_BYTES), "we decode the CLI's frame");
} else skip("brotli");

{
    const hello = new Uint8Array([5, 0x10, 0x68, 0x65, 0x6c, 0x6c, 0x6f]);
    eq(b2s(unsnappy(hello)), "hello", "snappy literal-only frame");
    const copy1 = new Uint8Array([10, 0x10, 0x68, 0x65, 0x6C, 0x6C, 0x6F,
                                  0x05, 5]);
    eq(b2s(unsnappy(copy1)), "hellohello", "snappy copy1 frame");
    const rle = new Uint8Array([6, 0x00, 0x61, 0x05, 1]);
    eq(b2s(unsnappy(rle)), "aaaaaa", "snappy overlapping copy (RLE)");
}
{
    const fr = new Uint8Array([
        0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x58, 0x29, 0x00, 0x00,
        0x68, 0x65, 0x6C, 0x6C, 0x6F]);
    eq(b2s(unzstd(fr)), "hello", "hand-encoded zstd frame");
    throwsMsg(() => unzstd(new Uint8Array([
        0x28, 0xB5, 0x2F, 0xFD, 0xC0, 0x58,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])),
        "declared", "zstd frame with lying FCS refuses");
    throwsMsg(() => unzstd(new Uint8Array([
        0x28, 0xB5, 0x2F, 0xFE, 0x40, 0x21, 0x68])),
        "bad magic", "zstd bad magic refuses");
    throwsMsg(() => unzstd(new Uint8Array([
        0x28, 0xB5, 0x2F, 0xFD, 0x40])),
        "truncated", "zstd truncated frame refuses");
}
{
    const fr = new Uint8Array([0x0f, 0x00, 0x80, 0x61, 0x03]);
    if (CLI.brotli) {
        const r = Exec("sh", ["-c",
            "printf '\\x0f\\x00\\x80\\x61\\x03' | brotli -d -c 2>/dev/null"]);
        eq(r.stdout, "a", "the hand-encoded brotli frame is what we say it is");
    }
    eq(b2s(unbrotli(fr)), "a", "hand-encoded brotli frame decodes");
    throwsMsg(() => unbrotli(new Uint8Array([0x0f, 0x00, 0x80])),
        "", "truncated brotli stream refuses");
}

{
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
}

{
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
}

if (fails) {
    print("test_codec_zstd: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_codec_zstd failed");
}
print("test_codec_zstd: " + n + " assertions, 0 failures");
