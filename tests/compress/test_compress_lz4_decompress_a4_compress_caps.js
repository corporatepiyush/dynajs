// covers: dyna:compress.lz4Decompress
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_decompress_a4_compress_caps");
import { gzip, gunzip, deflate, inflate, lz4Frame, lz4Unframe, lz4Compress,
         lz4Decompress, Compressor } from "dyna:compress";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}
function errOf(fn) {
    try { fn(); return null; } catch (e) { return e; }
}

const payload = "A".repeat(200000);

__kitS.section("lz4Decompress cap -> RangeError", () => {
    const lb = lz4Compress(payload);
    let e = errOf(() => lz4Decompress(lb, { maxOutputBytes: 512 }));
    assert(e instanceof RangeError, "lz4Decompress cap -> RangeError (got " + e + ")");
});
__kitS.end();

print("test_a4_compress_caps: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_compress_caps: " + fails + " failures");
