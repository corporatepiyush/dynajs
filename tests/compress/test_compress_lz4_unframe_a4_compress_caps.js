// covers: dyna:compress.lz4Unframe
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_unframe_a4_compress_caps");
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

__kitS.section("lz4Unframe cap -> RangeError", () => {
    const lf = lz4Frame(payload);
    let e = errOf(() => lz4Unframe(lf, { maxOutputBytes: 1024 }));
    assert(e instanceof RangeError, "lz4Unframe cap -> RangeError (got " + e + ")");
    const ok = lz4Unframe(lf, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "lz4Unframe under the cap round-trips");
});
__kitS.end();

print("test_a4_compress_caps: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_compress_caps: " + fails + " failures");
