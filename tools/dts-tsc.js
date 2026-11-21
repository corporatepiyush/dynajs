import * as std from "std";
import * as os from "os";

const CASES = [
    { dts: "src/modules/docdb/docdb.d.ts", deps: ["src/modules/serialize/serialize.d.ts"], test: "tests/types/docdb/docdb_types.ts" },
];
const TSC = ["/opt/homebrew/bin/tsc", "/usr/local/bin/tsc", "/usr/bin/tsc"].find((p) => os.stat(p)[1] === 0);
if (!TSC) { print("dts-tsc: skipped (tsc not installed)"); std.exit(0); }
const cwd = os.getcwd()[0];
const dir = cwd + "/.obj-dts-tsc";
os.mkdir(dir, 0o755);
let bad = 0;
for (const c of CASES) {
    const cfg = { compilerOptions: { strict: true, noEmit: true, target: "es2022", module: "es2022", moduleResolution: "bundler",
        lib: ["es2023", "esnext.asynciterable", "esnext.disposable"], types: [] },
        files: ["src/types/prelude.d.ts", ...c.deps, c.dts, c.test].map((f) => cwd + "/" + f) };
    const f = std.open(dir + "/tsconfig.json", "w"); f.puts(JSON.stringify(cfg)); f.close();
    const rc = os.exec(["sh", "-c", TSC + " -p " + dir + "/tsconfig.json > " + dir + "/out.txt 2>&1"]);
    const text = std.loadFile(dir + "/out.txt") || "";
    if (rc !== 0 || /error TS/.test(text)) { bad++; print("dts-tsc: FAIL " + c.test + "\n" + text); }
    else print("dts-tsc: ok " + c.test);
}
std.exit(bad ? 1 : 0);
