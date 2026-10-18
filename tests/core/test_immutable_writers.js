// flags: --std
import * as std from "std";
import * as os from "os";

// Natives that write into a caller's buffer must refuse one backed by an immutable ArrayBuffer
// (JS_CheckWritableBuffer). The dyna:* writers are swept by sigfuzz; these globals are not.
let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function frozen(n) {
    const src = new Uint8Array(n);
    for (let i = 0; i < n; i++) src[i] = (i * 37 + 11) & 255;
    return src.buffer.transferToImmutable();
}
function sum(ab) { let h = 0; for (const b of new Uint8Array(ab)) h = (h * 31 + b) >>> 0; return h; }
function refuses(label, ab, fn) {
    const before = sum(ab);
    let err = null;
    try { fn(); } catch (e) { err = e; }
    ok(err instanceof TypeError && /immutable ArrayBuffer/.test(err.message), label + " refuses (got " + err + ")");
    ok(sum(ab) === before, label + " left the bytes unchanged");
}

{
    const ab = frozen(16);
    refuses("TextEncoder.encodeInto", ab, () => new TextEncoder().encodeInto("hello", new Uint8Array(ab)));
    const r = new TextEncoder().encodeInto("hi", new Uint8Array(4));
    ok(r.written === 2, "control: encodeInto into a mutable buffer writes");
}
{
    const path = os.getcwd()[0] + "/.imm_writers_" + os.getpid() + ".tmp";
    const w = std.open(path, "w");
    w.puts("0123456789abcdef");
    w.close();
    const ab = frozen(16);
    const f = std.open(path, "r");
    refuses("std FILE.read", ab, () => f.read(ab, 0, 16));
    const ok_buf = new ArrayBuffer(16);
    ok(f.read(ok_buf, 0, 4) === 4, "control: FILE.read into a mutable buffer reads");
    f.close();
    const g = std.open(path, "r");
    const out = std.open(path + ".w", "w");
    ok(out.write(ab, 0, 16) === 16, "FILE.write from an immutable buffer is a read and is allowed");
    out.close();
    g.close();
    const fd = os.open(path, os.O_RDONLY);
    refuses("os.read", ab, () => os.read(fd, ab, 0, 16));
    ok(os.read(fd, new ArrayBuffer(8), 0, 8) === 8, "control: os.read into a mutable buffer reads");
    os.close(fd);
    os.remove(path);
    os.remove(path + ".w");
}

print("test_immutable_writers: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_immutable_writers: " + fail + " failures");
