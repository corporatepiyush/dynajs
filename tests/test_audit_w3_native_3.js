// flags: --std
// timeout: 300
// tests/test_audit_w3_native_3.js -- audit wave 3, process / file / log / SQLite.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   N3-23 SQLite work obeys --timeout-ms and the native memory limit
//   P1-09 a timed-out os.exec leaves no zombie and no grandchild
//   P1-14 readBytes of a file being truncated never dies of SIGBUS
//   P1-17 text log lines carry no raw C1 control, DEL or line separator
//   P1-19 temp-name prefixes cannot leave the temp directory
//   D3-10/D2-05 crypto and template memory obey the native limit; D3-12 scrypt
//   work budget; D2-12 template tag cap is named
// By reading only (needs root): P1-06 setgroups result checked in setUid/setGid.
// build-note: needs the native modules (dyna:net with SQLite)
import * as os from "os";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, makeTempFile, tempDir, writeFile, removeAll, remove, exists } from "dyna:file";
import { Logger } from "dyna:log";
import * as net from "dyna:net";

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

// ---- N3-23 --------------------------------------------------------------
if (typeof net.SQLite === "function") {
    // ORACLE: the documented exit shapes. A script stopped by --timeout-ms dies
    // of an "interrupted" InternalError; exit status 113 is the hard-stop
    // watchdog, which exists for native loops that never poll. A 60M-step
    // recursive CTE inside sqlite3_step must take the first route.
    // CONTROL: a 1000-step CTE under the same flag returns its count.
    const r = child("sq_time.mjs", `
import { SQLite } from "dyna:net";
const db = new SQLite(":memory:");
print("small=" + db.query("with recursive c(x) as (select 1 union all select x+1 from c where x < 1000) select count(*) as n from c")[0].n);
db.query("with recursive c(x) as (select 1 union all select x+1 from c where x < 60000000) select count(*) as n from c");
print("finished");
`, ["--timeout-ms", "400"]);
    ok(/small=1000/.test(r.stdout), "control: a short query runs under --timeout-ms (" + (r.stdout + r.stderr).slice(0, 100) + ")");
    ok(!/finished/.test(r.stdout) && r.code !== 0, "the long query did not run to completion");
    ok(/interrupted/.test(r.stderr) && r.code !== 113, "[red] N3-23: SQLite is interrupted by the deadline, not by the hard stop (code " + r.code + ", " + r.stderr.slice(0, 80) + ")");

    // ORACLE: arithmetic. 8 x 50 MB of blobs cannot fit a 16 MB native budget.
    // CONTROL: 8 x 50 KB under the same budget is stored and counted.
    const m = child("sq_mem.mjs", `
import { SQLite } from "dyna:net";
const db = new SQLite(":memory:");
db.exec("create table t(b)");
for (let i = 0; i < 8; i++) db.exec("insert into t values (zeroblob(50000))");
print("small=" + db.query("select sum(length(b)) as n from t")[0].n);
let held = 0, err = "none";
try { for (let i = 0; i < 8; i++) { db.exec("insert into t values (zeroblob(50000000))"); held++; } } catch (e) { err = e.message; }
print("heldBig=" + held, "err=" + err);
`, ["--native-memory-limit", "16000000"]);
    ok(/small=400000/.test(m.stdout), "control: small rows fit the budget (" + (m.stdout + m.stderr).slice(0, 100) + ")");
    ok(/heldBig=0 err=.*(memory|full)/i.test(m.stdout), "[red] N3-23: 50 MB blobs are refused under a 16 MB native limit (" + m.stdout.slice(-80).trim() + ")");
} else {
    print("  SKIP: this build has no SQLite; N3-23 not exercised");
}

// ---- P1-09 --------------------------------------------------------------
// ORACLE: the process table. After os.exec returns from a timeout nothing it
// started may remain: not the child as a zombie, not the child's own child.
// The grandchild sleeps for a duration unique to this run so pgrep cannot
// match anything else (the [s] keeps the pattern from matching the shell that
// runs pgrep, which Linux pgrep does not exclude). CONTROL: os.exec without a timeout returns the status.
{
    const tag = "97." + (os.getpid() % 100000);
    ok(os.exec(["/bin/sh", "-c", "exit 7"]) === 7, "control: os.exec returns the exit status");
    const rc = os.exec(["/bin/sh", "-c", "trap '' TERM; sleep " + tag + " & sleep " + tag], { timeout: 200 });
    ok(rc === -15, "timed-out os.exec reports -SIGTERM (" + rc + ")");
    os.sleep(200);
    const left = Exec("/bin/sh", ["-c", "pgrep -f '[s]leep " + tag + "' | wc -l"], { encoding: "utf8" });
    ok(+left.stdout.trim() === 0, "[red] P1-09: no process of the timed-out command survives (" + left.stdout.trim() + " left)");
    const z = Exec("/bin/sh", ["-c", "ps -o ppid=,stat= -ax | awk -v me=" + os.getpid() + " '$1==me && $2 ~ /^Z/' | wc -l"], { encoding: "utf8" });
    ok(+z.stdout.trim() === 0, "[red] P1-09: no zombie child is left behind (" + z.stdout.trim() + ")");
    Exec("/bin/sh", ["-c", "pkill -f 'sleep " + tag + "'; true"]);
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

// ---- P1-17 --------------------------------------------------------------
// ORACLE: ECMA-48. U+009B is the 8-bit CSI, U+007F is DEL; with C0 already
// escaped these are the remaining ways a logged value drives a terminal.
// U+2028 is a line terminator for line-oriented consumers.
// CONTROL: printable non-ASCII text (U+00E9, U+4E2D) passes through unescaped.
{
    const lines = [];
    const L = new Logger({ format: "text", timestamp: false, dest: (l) => lines.push(l) });
    L.info("a\u009b31mb del\u007f sep end café 中");
    const out = lines.join("");
    ok(out.length > 0, "fixture: the text logger produced a line");
    ok(!out.includes("\u009b"), "[red] P1-17: no raw U+009B in a text line");
    ok(!out.includes("\u007f"), "[red] P1-17: no raw DEL in a text line");
    ok(!out.includes(" "), "[red] P1-17: no raw U+2028 in a text line");
    ok(out.includes("\\u009b") && out.includes("\\u007f") && out.includes("\\u2028"), "the three are visible as escapes (" + JSON.stringify(out).slice(0, 120) + ")");
    ok(out.includes("café 中"), "control: printable non-ASCII text is untouched");
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

// ---- D3-10 / D2-05 / D3-12 / D2-12 --------------------------------------
// ORACLE: arithmetic against a 32 MiB native budget. Argon2id with m = 512 MiB
// and a template that renders 300 MB cannot fit; their small forms do.
// Scrypt: RFC 7914 cost is N*r*p block mixes; N=2^19, r=8, p=32 is 2^27,
// above the 2^26 work budget while its memory (512 MiB) is inside the 1 GiB
// memory budget, so only the work bound can refuse it. The child is given
// 8 s: a build without the bound computes for minutes and is cut off.
{
    const r = child("budget.mjs", `
import { Argon2id, Scrypt } from "dyna:crypto";
import { Template } from "dyna:html";
const t = (n, f) => { try { const v = f(); print(n + "=ok:" + (v && v.length)); } catch (e) { print(n + "=" + e.constructor.name + ":" + String(e.message).slice(0, 60)); } };
t("argonSmall", () => Argon2id.hash("pw", new Uint8Array(16), { memory: 1024, iterations: 1, encoded: false }));
t("argonBig", () => Argon2id.hash("pw", new Uint8Array(16), { memory: 524288, iterations: 1, encoded: false }));
t("tplSmall", () => new Template("{{#a}}x{{/a}}").render({ a: [1, 2, 3] }));
t("tplBig", () => new Template("{{#a}}" + "x".repeat(1000) + "{{/a}}").render({ a: new Array(300000).fill(1) }));
t("tplTags", () => new Template("{{a}}".repeat(70000)));
t("scryptSmall", () => Scrypt("pw", new Uint8Array(16), { N: 1024, r: 8, p: 16, keyLen: 64 }));
`, ["--native-memory-limit", "33554432"]);
    const w = child("budget2.mjs", `
import { Scrypt } from "dyna:crypto";
try { Scrypt("pw", new Uint8Array(16), { N: 1 << 19, r: 8, p: 32, keyLen: 32 }); print("scryptWork=ok"); }
catch (e) { print("scryptWork=" + e.constructor.name + ":" + String(e.message).slice(0, 60)); }
`);
    const o = r.stdout;
    ok(/argonSmall=ok:32/.test(o) && /tplSmall=ok:3/.test(o) && /scryptSmall=ok:64/.test(o), "control: small Argon2id / Template / Scrypt run under a 32 MiB native limit (" + (o + r.stderr).slice(0, 140) + ")");
    ok(/argonBig=InternalError:out of memory/.test(o), "D3-10: Argon2id m=512 MiB is refused under the native limit");
    ok(/tplBig=InternalError:out of memory/.test(o), "D2-05: a 300 MB render is refused under the native limit");
    ok(/tplTags=SyntaxError:.*more than 65536/.test(o), "[red] D2-12: the tag cap is named, not reported as out of memory (" + (/tplTags=[^\n]*/.exec(o) || [""])[0] + ")");
    ok(/scryptWork=RangeError:.*work budget/.test(w.stdout), "[red] D3-12: Scrypt N*r*p above 2^26 is refused (" + (w.stdout.trim() || "no row: child cut off after 8 s") + ")");
}

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_3: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_3: " + failures + " failures");
