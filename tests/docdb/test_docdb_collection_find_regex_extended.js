// flags: --std
// covers: dyna:docdb.Collection.find
// $regex with $options "x" (extended): dd_regex_compile strips whitespace and # comments into a
// fresh buffer and hands it to lre_compile, which reads its pattern up to a NUL. The stripped copy
// was never NUL-terminated, so the compile read one uninitialized byte past the pattern: on macOS
// a fresh small block usually held a zero and the query worked; on glibc it often held garbage and
// the query threw DocDBError 2 ("Regular expression is invalid"), and which plan threw depended on
// what the allocator had reused (the docker gcc leg's find fuzz saw the unindexed reference throw
// while the indexed copies answered, seed 20261115). Oracles: the expected match counts are
// computed by a plain JS RegExp over the same strings with the whitespace/comments removed by
// hand; the checks run in this process and again in a child whose allocator fills every fresh
// block with non-zero bytes (MallocScribble=1 on macOS, MALLOC_PERTURB_=165 on glibc), which makes
// the missing terminator fail deterministically; indexed and unindexed collections must agree.
import { Collection, Database, DocDBError } from "dyna:docdb";
import { args, Exec } from "dyna:sys";
import { suite } from "../lib/kit.js";

const N = 60;
const strs = Array.from({ length: N }, (_, i) => "s" + i);
const CASES = [
    [".", /./],
    [" s 1 ", /s1/],
    ["s # a comment\n4", /s4/],
    ["[ ]?s1", /[ ]?s1/],
    ["\\  s 2", /\ s2/],
    ["^ s \\d $", /^s\d$/],
];

function run(tag) {
    const out = [];
    for (const [k, [idxName, idx]] of [["none", null], ["a.x", { "a.x": 1 }], ["compound", { "a.x": 1, b: -1 }]].entries()) {
        const db = new Database("rxext_" + tag + "_" + k);
        const c = db.collection("t");
        if (!(c instanceof Collection)) throw new TypeError("db.collection(t) is not a Collection");
        strs.forEach((s, i) => c.insertOne({ _id: i, a: { x: s }, b: i }));
        if (idx) c.createIndex(idx);
        for (const [pat, re] of CASES) {
            const want = strs.filter((s) => re.test(s)).length;
            let got;
            try {
                got = c.find({ "a.x": { $regex: pat, $options: "x" } }).length;
            } catch (e) {
                got = e instanceof DocDBError ? "DocDBError " + e.code + ": " + e.message : String(e);
            }
            out.push({ idx: idxName, pat, got, want });
        }
    }
    return out;
}

if (scriptArgs.includes("--child")) {
    print(JSON.stringify(run("child")));
    std.exit(0);
}

const BIN = args()[0];
const SELF = scriptArgs[0];
const t = suite("docdb.find $regex $options x");

function checkAll(ok, rows, where) {
    for (const r of rows)
        ok(r.got === r.want, where + " [" + r.idx + "] " + JSON.stringify(r.pat) + " x: got " + JSON.stringify(r.got) + ", want " + r.want);
}

t.test("extended patterns match their stripped form, indexed and unindexed (this process)", ({ ok }) => {
    checkAll(ok, run("self"), "in-process");
});

t.test("and again when every fresh allocation is non-zero (scribbled malloc child)", ({ ok }) => {
    const r = Exec(BIN, ["--std", SELF, "--child"], {
        timeoutMs: 60000,
        encoding: "utf8",
        env: { MallocScribble: "1", MALLOC_PERTURB_: "165" },
        extendEnv: true,
    });
    ok(r.code === 0, "the child exits 0 (code " + r.code + ", stderr " + JSON.stringify(String(r.stderr).slice(0, 200)) + ")");
    if (r.code !== 0) return;
    checkAll(ok, JSON.parse(String(r.stdout).trim().split("\n").pop()), "scribbled");
});

await t.run();
