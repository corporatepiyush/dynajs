// covers: c:src/core/yamlscalar/yamlscalar-scalar.c
// yml_key_colon (is this line `key: value`, and where is the colon?) skips runs
// of ordinary bytes with a 256-entry class table (only " ' [ { ] } # : stop
// it) and jumps through quoted keys with memchr / a quote-or-backslash scan,
// instead of testing every byte against each case. It runs on every block
// line, twice. Modperf round 2026-11-15: YAML Parse 1K records 2.20 ms ->
// 2.07 ms (-5.8%). Oracle: documents generated from tokens whose parse is
// known (plain, single- and double-quoted keys holding ': ', '#', brackets;
// values with comments, flow collections, colons without a following space).
import { suite } from "../lib/kit.js";
import * as yaml from "dyna:yaml";

const t = suite("test_yaml_parse_key_colon_scan");

const KEYS = [
    ["alpha", "alpha"],
    ["'it''s: x'", "it's: x"],
    ['"q\\"x: y"', 'q"x: y'],
    ["a:b", "a:b"],
    ["'[not: flow]'", "[not: flow]"],
    ['"has # hash"', "has # hash"],
    ["k#1", "k#1"],
    ["'{x}'", "{x}"],
    ['"back\\\\slash: z"', "back\\slash: z"],
    ["sp ace", "sp ace"],
];
const VALS = [
    ["plain words", "plain words"],
    ["'q: v'", "q: v"],
    ['"d: v"', "d: v"],
    ["[1, 2]", [1, 2]],
    ["{a: 1, b: [x]}", { a: 1, b: ["x"] }],
    ["v # comment: here", "v"],
    ["v#notcomment", "v#notcomment"],
    ["123", 123],
    ["'it''s'", "it's"],
    ['"esc\\"q"', 'esc"q'],
];

let seed = 31;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);

t.test("block mappings with tricky keys and values parse to the known objects", ({ eq }) => {
    for (let doc = 0; doc < 300; doc++) {
        const want = {};
        const lines = [];
        const used = new Set();
        const n = 1 + (rnd() % 8);
        for (let i = 0; i < n; i++) {
            const [ks, kv] = KEYS[rnd() % KEYS.length];
            if (used.has(kv)) continue;
            used.add(kv);
            const [vs, vv] = VALS[rnd() % VALS.length];
            lines.push(ks + ":" + " ".repeat(1 + (rnd() % 3)) + vs);
            want[kv] = vv;
        }
        eq(JSON.stringify(yaml.Parse(lines.join("\n") + "\n")), JSON.stringify(want), "doc " + doc + ": " + JSON.stringify(lines));
    }
});

t.test("nested maps and long flow lines", ({ eq }) => {
    const recs = [];
    for (let i = 0; i < 400; i++) recs.push({ id: i, "k: q": "v: " + i, tags: ["a", "b#c"], sub: { "x y": i % 3 } });
    eq(JSON.stringify(yaml.Parse(yaml.Stringify(recs))), JSON.stringify(recs), "block round trip");
    eq(JSON.stringify(yaml.Parse(yaml.Stringify(recs, { flow: true }))), JSON.stringify(recs), "flow round trip");
    eq(JSON.stringify(yaml.Parse("outer:\n  'in: ner':\n    deep: \"x: y\" # c\n")), JSON.stringify({ outer: { "in: ner": { deep: "x: y" } } }), "nested quoted keys");
});

await t.run();
