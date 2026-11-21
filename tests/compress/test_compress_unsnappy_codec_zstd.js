// flags: --std
// covers: dyna:compress.unsnappy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_unsnappy_codec_zstd");
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

__kitS.section("snappy literal-only frame", () => {
    const hello = new Uint8Array([5, 0x10, 0x68, 0x65, 0x6c, 0x6c, 0x6f]);
    eq(b2s(unsnappy(hello)), "hello", "snappy literal-only frame");
    const copy1 = new Uint8Array([10, 0x10, 0x68, 0x65, 0x6C, 0x6C, 0x6F,
                                  0x05, 5]);
    eq(b2s(unsnappy(copy1)), "hellohello", "snappy copy1 frame");
    const rle = new Uint8Array([6, 0x00, 0x61, 0x05, 1]);
    eq(b2s(unsnappy(rle)), "aaaaaa", "snappy overlapping copy (RLE)");
});
__kitS.end();

if (fails) {
    print("test_codec_zstd: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_codec_zstd failed");
}
print("test_codec_zstd: " + n + " assertions, 0 failures");
