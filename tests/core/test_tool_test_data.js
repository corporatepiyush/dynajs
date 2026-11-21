// covers: c:tools/test-data.js
// tools/test-data.js keeps the git-ignored test data honest: fetch makes every missing or
// mismatched file from its manifest row, verify fails on any byte that differs from the pinned
// sha256. Each case runs the tool in a scratch tree with its own tests/test-data.tsv, so no real
// data is touched and no network is used: a gen: row (stdout becomes the file), a run: row that
// reads the gen: row's output (rows are made in manifest order, which is why the real manifest
// lists tests/corpus/isrg_root_x1.pin before tests/corpus/ca-bundle.pem), a flipped byte, a row
// whose generator drifted from its pin, and a row with no generator at all.
import { args, cwd, Exec } from "dyna:sys";
import { Path, makeTempDir, makeDir, writeFile, readFile, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const BIN = args()[0].startsWith("/") ? args()[0] : cwd() + "/" + args()[0];
const TOOL = cwd() + "/tools/test-data.js";
const HELLO = "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03";
const t = suite("tool_test_data");
const root = makeTempDir("test-data-tool-");
t.after(() => removeAll(root));

let n = 0;
function tree(rows) {
    const d = root.join("t" + n++);
    makeDir(d.join("tests"), { recursive: true });
    writeFile(d.join("tests/test-data.tsv"), "path\tsha256\tsource\n" + rows.map((r) => r.join("\t")).join("\n") + "\n");
    return d;
}
function tool(d, mode) {
    const r = Exec(BIN, ["--std", TOOL, mode], { cwd: d.toString(), timeoutMs: 30000 });
    return { code: r.code, out: String(r.stdout) + String(r.stderr) };
}
const GEN = ["data/a.txt", HELLO, "gen:echo hello"];
const RUN = ["data/b.txt", HELLO, "run:cp data/a.txt data/b.txt"];

t.test("fetch makes gen: and run: rows in manifest order, then verify passes", (a) => {
    const d = tree([GEN, RUN]);
    a.eq(tool(d, "check").code, 1, "check fails while the files are missing");
    const f = tool(d, "fetch");
    a.eq(f.code, 0, "fetch exit (" + f.out.trim() + ")");
    a.eq(readFile(d.join("data/b.txt")), "hello\n", "the run: row read the gen: row's output");
    a.eq(tool(d, "verify").code, 0, "verify after fetch");
});

t.test("verify catches one flipped byte and fetch repairs it", (a) => {
    const d = tree([GEN, RUN]);
    a.must(tool(d, "fetch").code === 0, "fetch");
    writeFile(d.join("data/b.txt"), "hellp\n");
    const v = tool(d, "verify");
    a.eq(v.code, 1, "verify exit on a flipped byte");
    a.ok(v.out.includes("FAIL data/b.txt sha"), "names the file and its sha (" + v.out.trim() + ")");
    a.eq(tool(d, "check").code, 0, "check only looks for presence");
    a.eq(tool(d, "fetch").code, 0, "fetch remakes the mismatched file");
    a.eq(tool(d, "verify").code, 0, "verify after the repair");
});

t.test("a generator that drifted from its pin warns on fetch and fails verify", (a) => {
    const d = tree([["data/c.txt", HELLO, "gen:echo goodbye"]]);
    const f = tool(d, "fetch");
    a.ok(f.out.includes("WARN data/c.txt regenerated with a new sha"), "fetch warns (" + f.out.trim() + ")");
    a.eq(tool(d, "verify").code, 1, "verify fails on the drifted file");
});

t.test("a failing generator and a row without one fail fetch", (a) => {
    const d = tree([["data/d.txt", HELLO, "gen:exit 3"], ["data/e.txt", HELLO, "seed"]]);
    const f = tool(d, "fetch");
    a.eq(f.code, 1, "fetch exit");
    a.ok(f.out.includes("FAIL data/d.txt") && f.out.includes("generator failed"), "the failing generator is named");
    a.ok(f.out.includes("FAIL data/e.txt") && f.out.includes("has no generator"), "the seed row is named");
});

await t.run();
