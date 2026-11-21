// covers: dyna:compress.inflate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_inflate_a4_compress_caps");
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

__kitS.section("inflate cap -> RangeError", () => {
    const df = deflate(payload);
    let e = errOf(() => inflate(df, { maxOutputBytes: 1024 }));
    assert(e instanceof RangeError, "inflate cap -> RangeError (got " + e + ")");
    const ok = inflate(df, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "inflate under the cap round-trips");
});
__kitS.end();

print("test_a4_compress_caps: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_compress_caps: " + fails + " failures");
