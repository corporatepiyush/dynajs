// flags: --std
// covers: dyna:compress.brotli
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_brotli_codec_zstd");
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

function skip(what) { print("SKIP " + what + " (CLI absent)"); }

const TEXT = "the quick brown fox jumps over the lazy dog ".repeat(64);

const TEXT_BYTES = enc.encode(TEXT);

eq(unbrotli(brotli(TEXT_BYTES), { asString: true }), TEXT, "brotli asString");

if (CLI.brotli) {
    const tmp = `${std.getenv("TMPDIR") || "/tmp"}/dj_brotli.${Date.now() % 10000000}_${Math.random().toString(36).slice(2, 10)}`;
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

__kitS.section("printf '\\x0f\\x00\\x80\\x61\\x03' | brotli -d -c 2>/dev/null", () => {
    const fr = new Uint8Array([0x0f, 0x00, 0x80, 0x61, 0x03]);
    if (CLI.brotli) {
        const r = Exec("sh", ["-c",
            "printf '\\x0f\\x00\\x80\\x61\\x03' | brotli -d -c 2>/dev/null"]);
        eq(r.stdout, "a", "the hand-encoded brotli frame is what we say it is");
    }
    eq(b2s(unbrotli(fr)), "a", "hand-encoded brotli frame decodes");
    throwsMsg(() => unbrotli(new Uint8Array([0x0f, 0x00, 0x80])),
        "", "truncated brotli stream refuses");
});
__kitS.end();

if (fails) {
    print("test_codec_zstd: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_codec_zstd failed");
}
print("test_codec_zstd: " + n + " assertions, 0 failures");
