// flags: --std
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

{
    const gz = gzip(payload);
    let e = errOf(() => gunzip(gz, { maxOutputBytes: 1024 }));
    assert(e instanceof RangeError, "gunzip cap -> RangeError (got " + e + ")");
    const ok = gunzip(gz, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "gunzip under the cap round-trips");
    e = errOf(() => gunzip(gz, { maxOutputBytes: 0 }));
    assert(e instanceof RangeError, "maxOutputBytes 0 refused (got " + e + ")");
    e = errOf(() => gunzip(gz, { maxOutputBytes: -5 }));
    assert(e instanceof RangeError, "negative maxOutputBytes refused (got " + e + ")");
}

{
    const df = deflate(payload);
    let e = errOf(() => inflate(df, { maxOutputBytes: 1024 }));
    assert(e instanceof RangeError, "inflate cap -> RangeError (got " + e + ")");
    const ok = inflate(df, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "inflate under the cap round-trips");
}

{
    const lf = lz4Frame(payload);
    let e = errOf(() => lz4Unframe(lf, { maxOutputBytes: 1024 }));
    assert(e instanceof RangeError, "lz4Unframe cap -> RangeError (got " + e + ")");
    const ok = lz4Unframe(lf, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "lz4Unframe under the cap round-trips");
}

{
    const lb = lz4Compress(payload);
    let e = errOf(() => lz4Decompress(lb, { maxOutputBytes: 512 }));
    assert(e instanceof RangeError, "lz4Decompress cap -> RangeError (got " + e + ")");
}

{
    const gz = gzip(payload);
    const c = new Compressor({ algo: "gzip" });
    let e = errOf(() => c.decompress(gz, { maxOutputBytes: 4096 }));
    assert(e instanceof RangeError, "Compressor.decompress cap -> RangeError (got " + e + ")");
    const ok = c.decompress(gz, { maxOutputBytes: 1 << 20 });
    assert(ok.length === payload.length, "Compressor.decompress under the cap round-trips");
}

print("test_a4_compress_caps: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_compress_caps: " + fails + " failures");
