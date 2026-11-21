// covers: dyna:sys.Exec
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_exec_pathcwd");
import { Exec, chDir } from "dyna:sys";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; console.log("  FAIL  " + w); } };

const T = makeTempDir("s9cwd");
writeFile(new Path(T, "probe"), "#!/bin/sh\necho CWD-HIT\n");
Exec("/bin/chmod", ["+x", T + "/probe"]);
chDir(T);

__kitS.section("default: cwd search works (execvp parity), got:", () => {
const dflt = Exec("probe", [], { env: { PATH: ":/bin:/usr/bin" } });
ok(dflt.stdout.trim() === "CWD-HIT",
   "default: cwd search works (execvp parity), got: " + JSON.stringify(dflt.stdout.trim()));
});

__kitS.section("allowPathCwd:false refuses the cwd binary", () => {
let refused = false;
try {
    Exec("probe", [], { env: { PATH: ":/bin:/usr/bin" }, allowPathCwd: false });
} catch (e) {
    refused = /command not found/.test(String(e));
}
ok(refused, "allowPathCwd:false refuses the cwd binary");
});

__kitS.section("echo OK", () => {
const both = Exec("sh", ["-c", "echo OK"], { env: { PATH: ":/bin" }, allowPathCwd: false });
ok(both.stdout.trim() === "OK", "normal PATH elements unaffected by the flag");
});
__kitS.end();

chDir("/tmp");
removeAll(new Path(T));
console.log("test_exec_pathcwd: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_exec_pathcwd: " + fail + " failures");
