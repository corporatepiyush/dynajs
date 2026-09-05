/* test_uring_disk.js — dyna:uring disk I/O (Linux, CONFIG_IO_URING only).
 * Proves the io_uring reader returns byte-identical data to a pread reference,
 * then does a crude page-cache timing. A faithful disk-throughput comparison
 * needs O_DIRECT on real storage; on a cached/virtualised fs both hit cache. */
/* WHERE THIS CAN ACTUALLY RUN, and how it is verified.
 *
 * dyna:uring exists only on Linux built with CONFIG_IO_URING, so this file was
 * for a long time in no Makefile target at all -- not NATIVE_TESTS, not the
 * amd64 container -- and therefore never executed anywhere. The Path-only
 * conversion of readFile/readFileSync/checksum went in unverified because of
 * it. `make test-uring` now builds docker/Dockerfile.uring and runs this.
 *
 * On an arm64 mac even that is only a PARTIAL verification: the container is
 * qemu-emulated and io_uring_queue_init() there returns ENOSYS ("Function not
 * implemented"), with or without seccomp=unconfined. Probed directly with
 * liburing to be sure it was the environment and not the binding. So the
 * pread reference path and the argument contract are checkable on this host
 * and the io_uring path is not; the test says which it verified rather than
 * reporting a pass that covered half of what it names. */
import * as uring from "dyna:uring";
import * as std from "std";
import { Path } from "dyna:file";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

const pathStr = "/tmp/uring_test.dat";
const path = new Path(pathStr);
let chunk = "";
for (let i = 0; i < 1024; i++) chunk += String.fromCharCode(33 + (i * 7) % 94);
let big = "";
for (let i = 0; i < 8192; i++) big += chunk; /* ~8 MB */

const f = std.open(pathStr, "w");
f.puts(big);
f.close();

/* The pread reference always works; the io_uring reader needs a real kernel.
 * Detect rather than assume, so this file is a genuine pass on a native Linux
 * host and an honest partial pass under emulation. */
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

/* The argument contract is checkable everywhere, and it is what the Path
 * conversion actually changed. */
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
    bench(true); bench(false); /* warm the cache */
    const tu = bench(true), tp = bench(false);
    print("read 8MB x30 -- io_uring: " + tu.toFixed(1) + "ms  pread: " +
          tp.toFixed(1) + "ms (page-cache; understates real-disk io_uring gain)");
} else {
    bench(false);
    const tp = bench(false);
    print("read 8MB x30 -- pread: " + tp.toFixed(1) + "ms (io_uring not available here)");
}
