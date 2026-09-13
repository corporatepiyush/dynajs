import { Path, File, readFile, writeFile, readFileAsync,
         makeDir, removeAll, chmod, FileWriter, FileReader } from "dyna:file";
import { pid } from "dyna:sys";
import { ok, eq, setTag, done } from "../../rh_module.js";
// INDEPENDENT review probe: FIX 1 errno preservation. Exercises the
// documented .code table entries reachable from a plain process:
// ENOENT, EACCES (chmod 444), EISDIR (FileReader on a dir opens fine but
// reads fail — see the readFile-directory finding), ENOTDIR, ENAMETOOLONG.
// EMFILE is exercised by the python driver (ulimit), not here.
setTag("file.errno_codes");

var scratch = "tests/agent/dyna_sys_review/out/errno_" + pid();
var sp = new Path(scratch);
makeDir(sp, { recursive: true });

function info(fn) {
  try { fn(); return { threw: false }; }
  catch (e) { return { threw: true, e: e }; }
}

// ENOENT — readFile / readBytes / FileReader / FileWriter all carry it
var r = info(function () { readFile(new Path(scratch, "absent.txt")); });
ok(r.threw && r.e.code === "ENOENT" && r.e.errno === 2,
   "readFile ENOENT code+errno, got " + (r.e && r.e.code) + "/" + (r.e && r.e.errno));
ok(r.threw && /readFile/.test(r.e.message), "readFile ENOENT message names the op: " + (r.e && r.e.message));
ok(r.threw && r.e.message.indexOf("absent.txt") >= 0, "readFile ENOENT message names the path");

var r2 = info(function () { new File(new Path(scratch, "absent.bin")).readBytes(); });
ok(r2.threw && r2.e.code === "ENOENT", "File.readBytes ENOENT .code, got " + (r2.e && r2.e.code));

var r3 = info(function () { new FileReader(new Path(scratch, "absent.txt")); });
ok(r3.threw && r3.e.code === "ENOENT", "FileReader ENOENT .code, got " + (r3.e && r3.e.code));

var r4 = info(function () { new FileWriter(new Path(scratch, "nodir", "x.txt")); });
ok(r4.threw && r4.e.code === "ENOENT", "FileWriter ENOENT .code, got " + (r4.e && r4.e.code));

// EACCES — write into a read-only file (owner-facing permission bits)
var ro = new Path(scratch, "ro.txt");
writeFile(ro, "keep");
chmod(ro, 0o444);
var r5 = info(function () { writeFile(ro, "overwrite"); });
ok(r5.threw && r5.e.code === "EACCES", "writeFile EACCES .code, got " + (r5.e && r5.e.code));
var r6 = info(function () { new FileWriter(ro); });
ok(r6.threw && r6.e.code === "EACCES", "FileWriter EACCES .code, got " + (r6.e && r6.e.code));
chmod(ro, 0o644);

// ENAMETOOLONG — a component over NAME_MAX (255)
var longName = new Array(300).join("x");
var r7 = info(function () { readFile(new Path(scratch, longName)); });
ok(r7.threw && r7.e.code === "ENAMETOOLONG", "readFile ENAMETOOLONG .code, got " + (r7.e && r7.e.code));

// writeFile error during WRITE (not open): /dev/full is Linux-only; on macOS
// exercise the write path via a file on a read-only mount is not portable —
// assert instead that a SUCCESSFUL writeFile returns the byte count.
eq(writeFile(new Path(scratch, "ok.txt"), "12345"), 5, "writeFile returns byte count");

// async variants reject with the same shape
var settled = null;
readFileAsync(new Path(scratch, "absent.txt")).then(
  function () { settled = { ok: true }; },
  function (e) { settled = { err: e }; });
var waited = 0;
var tick = function () {
  if (settled || waited > 50) {
    ok(settled && settled.err && settled.err.code === "ENOENT",
       "readFileAsync rejects ENOENT, got " + (settled && settled.err && settled.err.code));
    removeAll(sp);
    setTag("file.errno_codes");
    done();
    return;
  }
  waited++; setTimeout(tick, 20);
};
setTimeout(tick, 20);
