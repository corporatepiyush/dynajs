import { Path, File, readFile, writeFile, makeDir, removeAll, symlink, stat,
         lstat, FileReader, FileWriter } from "dyna:file";
import { pid } from "dyna:sys";
import { ok, eq, setTag, done } from "../../rh_module.js";
setTag("file.eloop_matrix");

var pidv = pid();
var scratch = "tests/agent/dyna_sys_review/out/eloop_" + pidv;
var scratchPath = new Path(scratch);
makeDir(scratchPath, { recursive: true });

var realDir = new Path(scratch, "real");
makeDir(realDir);
writeFile(new Path(scratch, "target.txt"), "payload");
symlink("real", new Path(scratch, "link"));

function codeOf(fn, label) {
  var e = null;
  try { fn(); } catch (er) { e = er; }
  ok(e !== null, label + ": throws");
  if (!e) return null;
  ok(e instanceof Error, label + ": is Error");
  ok(typeof e.code === "string", label + ": string .code (got " + e.code + ")");
  ok(typeof e.errno === "number", label + ": numeric .errno (got " + e.errno + ")");
  return e.code;
}

eq(codeOf(function () { readFile(new Path(scratch, "link", "target.txt")); },
          "readFile symlink-intermediate"), "ELOOP", "symlink-intermediate maps to ELOOP");

symlink("link", new Path(scratch, "link2"));
eq(codeOf(function () { readFile(new Path(scratch, "link2", "target.txt")); },
          "readFile symlink-chain"), "ELOOP", "symlink-chain maps to ELOOP");

eq(codeOf(function () { readFile(new Path(scratch, "no-such-dir", "f.txt")); },
          "readFile missing-intermediate"), "ENOENT", "missing-intermediate stays ENOENT");

eq(codeOf(function () { readFile(new Path(new Path(scratch, "target.txt"), "under.txt")); },
          "readFile file-as-intermediate"), "ENOTDIR", "file-as-intermediate stays ENOTDIR");

eq(codeOf(function () { readFile(realDir); }, "readFile directory"), "EISDIR",
   "readFile directory EISDIR");

eq(codeOf(function () { readFile(new Path(scratch, "absent.txt")); },
          "readFile absent-final"), "ENOENT", "absent final ENOENT");

eq(codeOf(function () { writeFile(new Path(scratch, "link", "out.txt"), "x"); },
          "writeFile symlink-intermediate"), "ELOOP", "writeFile symlink-intermediate ELOOP");

eq(codeOf(function () { writeFile(new Path(realDir, "sub", "x.txt"), "x"); },
          "writeFile missing-subdir"), "ENOENT", "writeFile missing-subdir ENOENT");

writeFile(new Path(scratch, "final.txt"), "final");
symlink("final.txt", new Path(scratch, "fl"));
eq(codeOf(function () { readFile(new Path(scratch, "fl")); },
          "readFile final-symlink"), "ELOOP", "final-symlink readFile ELOOP");
var stf = stat(new Path(scratch, "fl"));
ok(stf && stf.isFile === true, "stat follows final symlink to a file");
var stl = lstat(new Path(scratch, "fl"));
ok(stl && stl.isSymlink === true, "lstat sees the symlink");

eq(codeOf(function () { new FileReader(new Path(scratch, "link", "target.txt")); },
          "FileReader symlink-intermediate"), "ELOOP", "FileReader symlink-intermediate ELOOP");
eq(codeOf(function () { new FileWriter(new Path(scratch, "link", "out.txt")); },
          "FileWriter symlink-intermediate"), "ELOOP", "FileWriter symlink-intermediate ELOOP");

eq(codeOf(function () { new File(new Path(scratch, "nope.bin")).readBytes(); },
          "File.readBytes missing"), "ENOENT", "File.readBytes missing ENOENT");

makeDir(new Path(realDir, "d1"), { recursive: true });
makeDir(new Path(realDir, "d1", "d2"), { recursive: true });
makeDir(new Path(scratch, "real2"), { recursive: true });
symlink("../real2", new Path(realDir, "d1", "jump"));
eq(codeOf(function () { readFile(new Path(realDir, "d1", "jump", "t.txt")); },
          "deep symlink-intermediate"), "ELOOP", "deep symlink-intermediate ELOOP");

writeFile(new Path(realDir, "d1", "d2", "ok.txt"), "fine");
eq(readFile(new Path(realDir, "d1", "d2", "ok.txt")), "fine", "normal strict read works");

eq(readFile(new Path(realDir, "d1", "..", "d1", "d2", "ok.txt")), "fine", "dot-dot walk works");

removeAll(scratchPath);
done();
