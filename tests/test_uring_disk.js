// flags: --std
import * as uring from "dyna:uring";
import * as std from "std";
import { Path } from "dyna:file";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const pathStr = `${std.getenv("TMPDIR") || "/tmp"}/dj_uring.${Date.now() % 10000000}.dat`;
const path = new Path(pathStr);
let chunk = "";
for (let i = 0; i < 1024; i++) chunk += String.fromCharCode(33 + (i * 7) % 94);
let big = "";
for (let i = 0; i < 8192; i++) big += chunk;

const f = std.open(pathStr, "w");
f.puts(big);
f.close();

let viaUring = null, uringAvailable = true;
try { viaUring = uring.readFile(path); }
catch (e) { uringAvailable = false; print("  io_uring unavailable here (" + e.message + ")"); }
const viaPread = uring.readFileSync(path);
assert(viaPread.length === big.length, "pread length == source");
assert(viaPread === big, "pread bytes == written bytes");
if (uringAvailable) {
    assert(viaUring.length === big.length, "uring length == source");
    assert(viaUring === viaPread, "io_uring bytes == pread bytes");
    assert(viaUring === big, "io_uring bytes == written bytes");
}

{
    let threw = false;
    try { uring.readFileSync(pathStr); } catch { threw = true; }
    assert(threw, "a string path is refused: every entry point takes a Path");
    assert(uring.checksum(path, false).bytes === big.length,
           "checksum takes a Path too, and reads the whole file");
}

const cp = uring.checksum(path, false);
assert(cp.bytes === big.length, "checksum byte count (pread)");
if (uringAvailable) {
    const cu = uring.checksum(path, true);
    assert(cu.bytes === big.length, "checksum byte count (io_uring)");
    assert(cu.sum === cp.sum, "io_uring checksum == pread checksum");
}
print("test_uring_disk: " + (uringAvailable ? "correctness OK" :
      "pread + argument contract OK; io_uring path NOT exercised here") +
      " (" + cp.bytes + " bytes, sum=" + cp.sum + ")");

function bench(useUring) {
    const t0 = performance.now();
    let s = 0;
    for (let i = 0; i < 30; i++) s ^= uring.checksum(path, useUring).sum;
    return performance.now() - t0;
}
if (uringAvailable) {
    bench(true); bench(false);
    const tu = bench(true), tp = bench(false);
    print("read 8MB x30 -- io_uring: " + tu.toFixed(1) + "ms  pread: " +
          tp.toFixed(1) + "ms (page-cache; understates real-disk io_uring gain)");
} else {
    bench(false);
    const tp = bench(false);
    print("read 8MB x30 -- pread: " + tp.toFixed(1) + "ms (io_uring not available here)");
}
