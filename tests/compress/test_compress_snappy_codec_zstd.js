// flags: --std
// covers: dyna:compress.snappy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_snappy_codec_zstd");
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

const TEXT = "the quick brown fox jumps over the lazy dog ".repeat(64);

const TEXT_BYTES = enc.encode(TEXT);

__kitS.section("snappy asString", () => {
eq(unsnappy(snappy(TEXT_BYTES), { asString: true }), TEXT, "snappy asString");
});
__kitS.end();

if (fails) {
    print("test_codec_zstd: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_codec_zstd failed");
}
print("test_codec_zstd: " + n + " assertions, 0 failures");
