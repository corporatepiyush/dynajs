// tools/cc-lines.js [OBJDIR] -- print the exact compile command of every translation unit the last ./build.sh built,
// one shell line per unit: `CC FLAGS ... -MMD -MF OBJ.d -c -o OBJ SRC`. OBJDIR defaults to .obj/release (./build.sh
// release); .obj-san gives the ASan+UBSan lines. The lines come from OBJDIR/rules.mk, the makefile build.sh writes
// and runs, so they cannot drift from the real build: the per-file track includes and every probed flag are there.
// Replaces the old __cc-lines subcommand of build.sh (build.sh only builds now); the configuration is whatever the
// last build used (env: IO_URING=1 SDL3=1 RAYLIB=1 UI=1 ...). Used by tools/c17-audit.sh and tools/vecaudit.sh.
import * as std from "std";
import { compileRules, allObjects } from "./js/rules-mk.js";

const dir = (scriptArgs[1] || ".obj/release").replace(/\/+$/, "");
const mk = std.loadFile(dir + "/rules.mk");
if (mk === null) {
    std.err.puts(`cc-lines: no ${dir}/rules.mk: run ./build.sh ${dir === ".obj-san" ? "san" : "release"} first\n`);
    std.exit(2);
}
const lines = mk.split("\n"), rules = compileRules(mk), want = allObjects(mk).length;
let n = 0;
for (const r of rules) {
    std.out.puts(lines[r.line].slice(1).replace(/^@/, "").split("$@").join(r.obj).split("$<").join(r.src).split("$$").join("$") + "\n");
    n++;
}
if (want && n !== want) {
    std.err.puts(`cc-lines: ${dir}/rules.mk: ${n} compile rules for ${want} objects in all:\n`);
    std.exit(1);
}
if (n === 0) {
    std.err.puts(`cc-lines: ${dir}/rules.mk has no compile rules\n`);
    std.exit(1);
}
