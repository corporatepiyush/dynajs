// covers: dyna:compress.gunzip
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gunzip_a4_compress_caps");
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

__kitS.section("gunzip cap -> RangeError", () => {
    const gz = gzip(payload);
    let e = errOf(() => gunzip(gz, { maxOutputBytes: 1024 }));
    assert(e instanceof RangeError, "gunzip cap -> RangeError (got " + e + ")");
    const ok = gunzip(gz, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "gunzip under the cap round-trips");
    e = errOf(() => gunzip(gz, { maxOutputBytes: 0 }));
    assert(e instanceof RangeError, "maxOutputBytes 0 refused (got " + e + ")");
    e = errOf(() => gunzip(gz, { maxOutputBytes: -5 }));
    assert(e instanceof RangeError, "negative maxOutputBytes refused (got " + e + ")");
});
__kitS.end();

print("test_a4_compress_caps: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_compress_caps: " + fails + " failures");
