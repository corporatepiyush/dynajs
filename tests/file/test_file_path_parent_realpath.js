// covers: dyna:file.Path -- an absolute Path resolves symlinks in its parent directory through realpath at construction (owner 11-19: kept on purpose, documented in dynajs.d.ts); the final component, relative paths and missing parents are left as written
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_file_path_parent_realpath", { chain: true });
import { Path, makeTempDir, makeDir, writeFile, symlink, realPath, removeAll } from "dyna:file";

let failures = 0, passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else { failures++; print("FAIL " + msg); }
}

const dir = makeTempDir("pathreal");
const real = String(realPath(dir));
makeDir(new Path(real + "/target"));
writeFile(new Path(real + "/target/f.txt"), "x");
symlink(new Path(real + "/target"), new Path(real + "/link"));
symlink(new Path(real + "/target/f.txt"), new Path(real + "/target/leaf"));

__kitS.section("parent symlink resolved", () => {
    ok(String(new Path(real + "/link/f.txt")) === real + "/target/f.txt", "ctor resolves symlinked parent: " + String(new Path(real + "/link/f.txt")));
    ok(String(new Path(real + "/link", "f.txt")) === real + "/target/f.txt", "multi-segment ctor resolves parent");
    ok(String(new Path(real + "/link").join("g.txt")) === real + "/link/g.txt", "join stays lexical");
    ok(String(new Path(real + "/link", "g.txt")) === real + "/target/g.txt", "ctor resolves parent of a not-yet-existing leaf");
    ok(String(new Path(real + "/link/f.txt").dirname) === real + "/target", "dirname of resolved path");
    ok(String(new Path(String(dir) + "/target/f.txt")) === real + "/target/f.txt", "temp dir prefix canonicalised");
});

__kitS.section("left as written", () => {
    ok(String(new Path(real + "/target/leaf")) === real + "/target/leaf", "final-component symlink kept");
    ok(String(new Path(real + "/nope/f.txt")) === real + "/nope/f.txt", "missing parent kept");
    ok(String(new Path("rel/link/f.txt")) === "rel/link/f.txt", "relative path untouched");
    ok(String(new Path(real + "/link/../x")) === real + "/x", "lexical .. applied before resolution");
});

removeAll(new Path(real));
__kitS.end();
print(passed + " passed, " + failures + " failed");
if (failures) throw new Error(failures + " failures");
