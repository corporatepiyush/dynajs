// flags: --std
// covers: dyna:compress.unzstd
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_unzstd_codec_zstd");
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

__kitS.section("hand-encoded zstd frame", () => {
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
});
__kitS.end();

if (fails) {
    print("test_codec_zstd: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_codec_zstd failed");
}
print("test_codec_zstd: " + n + " assertions, 0 failures");
