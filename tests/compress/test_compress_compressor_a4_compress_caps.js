// covers: dyna:compress.Compressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_compressor_a4_compress_caps");
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

__kitS.section("Compressor.decompress cap -> RangeError", () => {
    const gz = gzip(payload);
    const c = new Compressor({ algo: "gzip" });
    let e = errOf(() => c.decompress(gz, { maxOutputBytes: 4096 }));
    assert(e instanceof RangeError, "Compressor.decompress cap -> RangeError (got " + e + ")");
    const ok = c.decompress(gz, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "Compressor.decompress under the cap round-trips");
});
__kitS.end();

print("test_a4_compress_caps: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_compress_caps: " + fails + " failures");
