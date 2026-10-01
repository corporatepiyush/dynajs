// flags: --std
import * as std from "std";
import { Path } from "dyna:file";

const t0 = Date.now();
let mod;
try {
    mod = await import("dyna:uring");
} catch (e) {
    print("test_uring_bytes: SKIP -- dyna:uring not built here (" +
          String(e.message || e).slice(0, 60) + ")");
    std.exit(0);
}
const { readFile, readFileBytes, readFileSync, checksum } = mod;

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ")"); }

const pathStr = `${std.getenv("TMPDIR") || "/tmp"}/dj_uring_bytes.${Date.now() % 10000000}.bin`;
const path = new Path(pathStr);

let content = "";
for (let i = 0; i < 4096; i++) content += String.fromCharCode((i * 37 + 11) % 256);
content += "\x00\xFF\xFE\xED\xA0\x80";
const bytesIn = new Uint8Array(content.length);
for (let i = 0; i < content.length; i++) bytesIn[i] = content.charCodeAt(i) & 0xFF;

const f = std.open(pathStr, "wb");
f.write(bytesIn);
f.close();

const viaBytes = readFileBytes(path);
ok(viaBytes instanceof Uint8Array, "readFileBytes returns a Uint8Array");
eq(viaBytes.length, bytesIn.length, "byte length matches the file");
let same = viaBytes.length === bytesIn.length;
for (let i = 0; i < bytesIn.length && same; i++)
    if (viaBytes[i] !== bytesIn[i]) same = false;
ok(same, "readFileBytes returns the exact bytes (binary corpus intact)");

const ref = readFileSync(path);
eq(ref.length, bytesIn.length, "pread reference reads the same byte count");
const cs = checksum(path, false);
eq(cs.bytes, bytesIn.length, "checksum(pread) byte count");
if (ref !== content)
    ok(false, "NOTE: string form differs from the raw bytes (expected for " +
              "this binary corpus); the byte form is the exact one");

let uringWorks = true;
try { readFile(path); } catch (e) { uringWorks = false; }
if (uringWorks) {
    const csU = checksum(path, true);
    eq(csU.sum, cs.sum, "io_uring checksum == pread checksum");
    const viaU = readFileBytes(path);
    eq(viaU.length, bytesIn.length, "io_uring readFileBytes length");
    let uSame = true;
    for (let i = 0; i < bytesIn.length && uSame; i++)
        if (viaU[i] !== bytesIn[i]) uSame = false;
    ok(uSame, "io_uring readFileBytes returns the exact bytes");
} else {
    print("  io_uring unavailable here; byte contract verified via pread");
}

{
    let threw = false;
    try { readFileBytes(pathStr); } catch { threw = true; }
    ok(threw, "a string path is refused, same as readFile/readFileSync");
}

{
    const emptyStr = pathStr + ".empty";
    const ef = std.open(emptyStr, "wb");
    ef.close();
    const eb = readFileBytes(new Path(emptyStr));
    ok(eb instanceof Uint8Array && eb.length === 0,
       "an empty file reads as a zero-length Uint8Array");
    std.remove(emptyStr);
}

std.remove(pathStr);
if (fails) {
    print("test_uring_bytes: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_uring_bytes failed");
}
print("test_uring_bytes: " + n + " assertions, 0 failures (" +
      (uringWorks ? "io_uring path exercised" : "pread path only") + ")");
