import { Path, File, readFile, writeFile, makeDir, removeAll, symlink, stat,
         lstat, FileReader, FileWriter } from "dyna:file";
import { pid } from "dyna:sys";
import { ok, eq, setTag, done } from "../../rh_module.js";
// INDEPENDENT review probe: FIX 2 (dyn_openat_parent ELOOP mapping) and
// FIX 1 (errno preservation on readFile/FileReader/FileWriter/File.readBytes).
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

// 1. symlink intermediate component -> documented ELOOP
eq(codeOf(function () { readFile(new Path(scratch, "link", "target.txt")); },
          "readFile symlink-intermediate"), "ELOOP", "symlink-intermediate maps to ELOOP");

// 2. multi-component symlink chain link2 -> link -> real
symlink("link", new Path(scratch, "link2"));
eq(codeOf(function () { readFile(new Path(scratch, "link2", "target.txt")); },
          "readFile symlink-chain"), "ELOOP", "symlink-chain maps to ELOOP");

// 3. missing intermediate stays ENOENT (the refinement must not over-map)
eq(codeOf(function () { readFile(new Path(scratch, "no-such-dir", "f.txt")); },
          "readFile missing-intermediate"), "ENOENT", "missing-intermediate stays ENOENT");

// 4. regular file as an intermediate component stays ENOTDIR
eq(codeOf(function () { readFile(new Path(new Path(scratch, "target.txt"), "under.txt")); },
          "readFile file-as-intermediate"), "ENOTDIR", "file-as-intermediate stays ENOTDIR");

// 5. readFile of a directory -> EISDIR
eq(codeOf(function () { readFile(realDir); }, "readFile directory"), "EISDIR",
   "readFile directory EISDIR");

// 6. absent final component -> ENOENT
eq(codeOf(function () { readFile(new Path(scratch, "absent.txt")); },
          "readFile absent-final"), "ENOENT", "absent final ENOENT");

// 7. writeFile through a symlinked intermediate -> ELOOP
eq(codeOf(function () { writeFile(new Path(scratch, "link", "out.txt"), "x"); },
          "writeFile symlink-intermediate"), "ELOOP", "writeFile symlink-intermediate ELOOP");

// 8. writeFile into a missing subdir -> ENOENT
eq(codeOf(function () { writeFile(new Path(realDir, "sub", "x.txt"), "x"); },
          "writeFile missing-subdir"), "ENOENT", "writeFile missing-subdir ENOENT");

// 9. symlink at the FINAL component: strict open refuses with ELOOP...
writeFile(new Path(scratch, "final.txt"), "final");
symlink("final.txt", new Path(scratch, "fl"));
eq(codeOf(function () { readFile(new Path(scratch, "fl")); },
          "readFile final-symlink"), "ELOOP", "final-symlink readFile ELOOP");
// ...while stat (follow) still FOLLOWS the final link and sees the file
var stf = stat(new Path(scratch, "fl"));
ok(stf && stf.isFile === true, "stat follows final symlink to a file");
// ...and lstat sees the link itself
var stl = lstat(new Path(scratch, "fl"));
ok(stl && stl.isSymlink === true, "lstat sees the symlink");

// 10. FileReader / FileWriter through a symlinked intermediate
eq(codeOf(function () { new FileReader(new Path(scratch, "link", "target.txt")); },
          "FileReader symlink-intermediate"), "ELOOP", "FileReader symlink-intermediate ELOOP");
eq(codeOf(function () { new FileWriter(new Path(scratch, "link", "out.txt")); },
          "FileWriter symlink-intermediate"), "ELOOP", "FileWriter symlink-intermediate ELOOP");

// 11. File.readBytes on a missing file carries code/errno
eq(codeOf(function () { new File(new Path(scratch, "nope.bin")).readBytes(); },
          "File.readBytes missing"), "ENOENT", "File.readBytes missing ENOENT");

// 12. deep path: symlink component after two real directories
makeDir(new Path(realDir, "d1"), { recursive: true });
makeDir(new Path(realDir, "d1", "d2"), { recursive: true });
makeDir(new Path(scratch, "real2"), { recursive: true });
symlink("../real2", new Path(realDir, "d1", "jump"));
eq(codeOf(function () { readFile(new Path(realDir, "d1", "jump", "t.txt")); },
          "deep symlink-intermediate"), "ELOOP", "deep symlink-intermediate ELOOP");

// 13. positive control: strict open still reads a normal nested path
writeFile(new Path(realDir, "d1", "d2", "ok.txt"), "fine");
eq(readFile(new Path(realDir, "d1", "d2", "ok.txt")), "fine", "normal strict read works");

// 14. "." and ".." components keep working through the strict walk
eq(readFile(new Path(realDir, "d1", "..", "d1", "d2", "ok.txt")), "fine", "dot-dot walk works");

removeAll(scratchPath);
done();
