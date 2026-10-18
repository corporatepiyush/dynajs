// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_3.js: the file sections.
import * as os from "os";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, makeTempFile, tempDir, writeFile, removeAll, remove } from "dyna:file";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const TMP = makeTempDir("w3n3-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: name === "budget2.mjs" ? 8000 : 60000, encoding: "utf8" });
}
// ---- P1-14 --------------------------------------------------------------
// ORACLE: POSIX. read(2) of a file that shrinks returns fewer bytes; touching
// a mapped page past the new end of file is SIGBUS. A reader may therefore
// see a short or empty result or an error, but must not die of a signal.
// The writer rewrites and truncates an 8 MiB file in a loop while the child
// reads it 300 times.
{
    const target = P("shrink.bin").toString();
    writeFile(P("shrink.bin"), new Uint8Array(8 << 20));
    writeFile(P("writer.sh"), "while :; do head -c 8388608 /dev/zero > '" + target + "'; : > '" + target + "'; done\n");
    const wpid = os.exec(["/bin/sh", P("writer.sh").toString()], { block: false });
    const r = child("reader.mjs", `
import { Path, readBytes } from "dyna:file";
let n = 0, short = 0;
for (let i = 0; i < 300; i++) { try { const b = readBytes(new Path(${JSON.stringify(target)})); n++; if (b.length !== 8388608) short++; } catch (e) { n++; short++; } }
print("reads=" + n, "shortOrError=" + short);
`);
    os.kill(wpid, os.SIGTERM);
    for (let i = 0; i < 200 && os.waitpid(wpid, os.WNOHANG)[0] !== wpid; i++) os.sleep(10);
    ok(r.signal === null && /reads=300/.test(r.stdout), "[red, probabilistic] P1-14: 300 reads of a shrinking file, no signal (signal " + r.signal + ", " + r.stdout.trim() + ")");
}
// ---- P1-19 --------------------------------------------------------------
// ORACLE: path semantics. A prefix containing a separator or starting with a
// dot names something outside (or hidden inside) the temp directory.
// CONTROL: a plain prefix creates an entry directly under tempDir().
{
    const base = tempDir().toString().replace(/\/+$/, "");
    const f = makeTempFile("w3ok-");
    const fs = f.toString();
    ok(fs.startsWith(base + "/w3ok-") && fs.indexOf("/", base.length + 1) < 0, "control: a plain prefix lands directly in the temp dir (" + fs + ")");
    remove(f);
    for (const bad of ["../../w3esc-", "a/b", ".hidden", "x\\y"]) {
        let made = null, err = "none";
        try { made = makeTempFile(bad); } catch (e) { err = e.constructor.name; }
        if (made) { try { remove(made); } catch (e) {} }
        ok(err === "RangeError", "[red] P1-19: makeTempFile(" + JSON.stringify(bad) + ") is a RangeError (" + err + ")");
        err = "none"; made = null;
        try { made = makeTempDir(bad); } catch (e) { err = e.constructor.name; }
        if (made) { try { removeAll(made); } catch (e) {} }
        ok(err === "RangeError", "[red] P1-19: makeTempDir(" + JSON.stringify(bad) + ") is a RangeError (" + err + ")");
    }
}
try { removeAll(TMP); } catch (e) {}
print("test_file_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_file_audit_w3_native_3: " + failures + " failures");
