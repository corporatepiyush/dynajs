// flags: --std
// std FILE methods coerce their arguments BEFORE they resolve the FILE*: the
// coercion runs caller code, and that code may close the very file. Resolving
// first wrote to (or sought, read, flushed) a stream fclose() had released --
// a use-after-free glibc reports and macOS hides behind a pooled FILE, where
// it showed only as seek() returning -EBADF. Found by the resolve-then-coerce
// scan that followed tests/lib/sigfuzz.js's Robots#allows crash.
import * as std from "std";
import * as os from "os";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function err(f) { try { return "returned " + JSON.stringify(f()); } catch (e) { return e.constructor.name + ": " + e.message; } }

const path = `${(std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "")}/std-file-coerce-${os.getpid()}`;
{ const f = std.open(path, "w"); f.puts("0123456789\n"); f.close(); }
const CLOSED = /^TypeError: std\.file: invalid file handle$/;

const cases = [
    ["puts", "w", (f, a) => f.puts("ok", a), "string"],
    ["writeStr", "w", (f, a) => f.writeStr(a), "string"],
    ["printf", "w", (f, a) => f.printf("%s", a), "string"],
    ["seek", "r", (f, a) => f.seek(a, std.SEEK_SET), "number"],
    ["read", "r", (f, a) => f.read(new ArrayBuffer(8), a, 4), "number"],
    ["write", "r+", (f, a) => f.write(new ArrayBuffer(8), a, 4), "number"],
    ["readBytes", "r", (f, a) => f.readBytes(a), "number"],
    ["readAsString", "r", (f, a) => f.readAsString(a), "number"],
    ["putByte", "w", (f, a) => f.putByte(a), "number"],
];
for (const [name, mode, call, kind] of cases) {
    const f = std.open(path, mode);
    const arg = kind === "string" ? { toString() { f.close(); return "x"; } } : { valueOf() { f.close(); return 1; } };
    const m = err(() => call(f, arg));
    ok(CLOSED.test(m), `${name}: an argument that closes the file is refused as closed (${m})`);
}
for (const [name, mode, call, kind] of cases) {
    const f = std.open(path, mode === "w" ? "a" : mode);
    const m = err(() => call(f, kind === "string" ? "x" : 1));
    ok(!/^TypeError/.test(m), `${name}: an ordinary argument still works (control: ${m})`);
    f.close();
}
os.remove(path);
print("test_std_file_coerce: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
