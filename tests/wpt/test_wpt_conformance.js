// flags: --std
// covers: c:tools/wpt.js
// timeout: 600
// web-platform-tests conformance of the web globals (streams, encoding, url, compression, FileAPI, xhr/formdata).
// Runs tools/wpt.js over the WPT *.any.js tests (sparse clone outside the tree: WPT_DIR, default third_party/wpt in
// the main checkout; the suite skips when it is absent) and compares every subtest with tests/wpt-expectations.tsv:
// a subtest or test expected PASS/OK that is now anything else, or missing, fails the suite; a newly passing one only
// prints (re-baseline with: ./dynajs --std tools/wpt.js --write-expect tests/wpt-expectations.tsv).
// Runner self-test: a planted WPT directory (resources linked to the real checkout) holds one passing subtest, one
// failing test(), one rejecting promise_test, one async_test that times out on its own step_timeout, and a file that
// throws at top level. Each must be reported with its own status in both the window and the dedicated-worker
// variant, and a baseline that expects the failing subtest to PASS must exit 1 naming it. A planted test that
// includes /common/subset-tests-by-key.js (DOM code only under ?get-keys) must run, not be skipped as needs-dom,
// and its ?include=a variant must run key a only. Mutation: a result hook that reports every subtest as PASS, a
// runner that drops the worker variant, or one that scans the subset helpers for DOM use, turns the self-test red.
import * as std from "std";
import * as os from "os";
import * as sys from "dyna:sys";
import { suite } from "../lib/kit.js";

const t = suite("wpt.conformance");
const BIN = std.getenv("DYNAJS_BIN") || "./dynajs";
const WPT = std.getenv("WPT_DIR") || "/Users/piyush/ai/dynascript/third_party/wpt";
const HAVE = std.loadFile(WPT + "/resources/testharness.js") !== null;

async function drain(pipe) {
    const parts = [], buf = new Uint8Array(65536);
    for (;;) { const n = await pipe.read(buf); if (n <= 0) break; parts.push(buf.slice(0, n)); }
    const all = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    parts.reduce((o, p) => (all.set(p, o), o + p.length), 0);
    return new TextDecoder().decode(all);
}
async function wpt(args) {
    const p = new sys.Spawn(BIN, ["--std", "tools/wpt.js", "--bin", BIN, ...args], { stdin: "ignore" });
    const [out, err] = await Promise.all([drain(p.stdout), drain(p.stderr)]);
    const r = await p.wait();
    p.close();
    return { code: r.code, out: out + err };
}
function writeAll(path, text) { const f = std.open(path, "w"); f.puts(text); f.close(); }

const PLANT = `// META: global=window,dedicatedworker
test(() => assert_true(true), "planted pass");
test(() => assert_equals(1, 2), "planted fail");
promise_test(() => Promise.reject(new Error("planted reject")), "planted reject");
async_test((tc) => { step_timeout(tc.step_func(() => assert_unreached("planted late")), 10); }, "planted async fail");
`;

const SUBSET = `// META: global=window
// META: script=/common/subset-tests-by-key.js
// META: variant=?include=a
subsetTestByKey("a", test, () => assert_true(true), "subset a");
subsetTestByKey("b", test, () => assert_true(true), "subset b");
`;

t.test("runner self-test: a planted failing subtest is reported, in both globals", async (a) => {
    if (!HAVE) a.skip("no WPT checkout at " + WPT);
    const base = (std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "") + "/dyna-wpt-plant-" + os.getpid();
    os.mkdir(base);
    os.mkdir(base + "/plant");
    os.symlink(WPT + "/resources", base + "/resources");
    writeAll(base + "/plant/a.any.js", PLANT);
    writeAll(base + "/plant/b.any.js", "// META: global=window\nthrow new Error('planted top-level throw');\n");
    os.mkdir(base + "/common");
    os.symlink(WPT + "/common/subset-tests-by-key.js", base + "/common/subset-tests-by-key.js");
    writeAll(base + "/plant/c.any.js", SUBSET);
    try {
        const json = base + "/out.json";
        const r = await wpt(["--wpt", base, "--dirs", "plant", "--json", json, "--write-expect", base + "/exp.tsv"]);
        a.eq(r.code, 0, "runner exits 0 without --expect: " + r.out.slice(-300));
        const res = JSON.parse(std.loadFile(json));
        const ids = res.map((x) => x.id).sort();
        a.eq(ids.join(","), "plant/a.any.html,plant/a.any.worker.html,plant/b.any.html,plant/c.any.html?include=a", "one run per global and variant");
        const c = res.find((y) => y.id === "plant/c.any.html?include=a");
        a.eq(c.skip, null, "a subset-tests-by-key include is not a needs-dom skip");
        a.eq(c.harness && c.harness.status, "OK", "subset run: harness OK");
        a.eq(c.subs.map((x) => x.name + "=" + x.status).join(","), "subset a=PASS", "?include=a runs key a only");
        for (const id of ["plant/a.any.html", "plant/a.any.worker.html"]) {
            const x = res.find((y) => y.id === id);
            const st = Object.fromEntries(x.subs.map((s) => [s.name, s.status]));
            a.eq(x.harness.status, "OK", id + ": harness OK");
            a.eq(st["planted pass"], "PASS", id + ": pass reported");
            a.eq(st["planted fail"], "FAIL", id + ": failing test() reported");
            a.eq(st["planted reject"], "FAIL", id + ": rejecting promise_test reported");
            a.eq(st["planted async fail"], "FAIL", id + ": failing async_test step reported");
        }
        a.eq(res.find((y) => y.id === "plant/b.any.html").harness.status, "ERROR", "a top-level throw is a harness ERROR");
        const exp = std.loadFile(base + "/exp.tsv").replace("planted fail\tFAIL", "planted fail\tPASS");
        writeAll(base + "/exp2.tsv", exp);
        const g = await wpt(["--wpt", base, "--dirs", "plant", "--expect", base + "/exp2.tsv"]);
        a.eq(g.code, 1, "a baseline expecting the failing subtest to pass exits 1");
        a.match(g.out, /REGRESSION: plant\/a\.any\.html :: planted fail: expected PASS, got FAIL/, "and names it");
    } finally {
        for (const f of ["plant/a.any.js", "plant/b.any.js", "plant/c.any.js", "common/subset-tests-by-key.js", "out.json", "exp.tsv", "exp2.tsv", "resources"]) os.remove(base + "/" + f);
        os.remove(base + "/common");
        os.remove(base + "/plant");
        os.remove(base);
    }
}, { timeoutMs: 120000 });

t.test("no subtest regresses against tests/wpt-expectations.tsv", async (a) => {
    if (!HAVE) a.skip("no WPT checkout at " + WPT);
    const r = await wpt(["--wpt", WPT, "--expect", "tests/wpt-expectations.tsv"]);
    print(r.out.split("\n").filter((l) => !l.startsWith("REGRESSION")).join("\n"));
    const reg = r.out.split("\n").filter((l) => l.startsWith("REGRESSION"));
    a.eq(reg.join("\n"), "", "regressions");
    a.eq(r.code, 0, "runner exit code");
}, { timeoutMs: 600000 });

await t.run();
