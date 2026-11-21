// covers: dyna:matcher.MultiMatcher.allIn
// allIn on text that holds characters above U+00FF (stored as UTF-16) returns
// UTF-16 offsets. The byte-to-unit conversion used to rescan the text from the
// start for every hit, so allIn was quadratic: 400 KB of such text took 2 s and
// a 33 MB file never finished, while countIn stayed linear. Offsets are checked
// against an indexOf oracle with overlapping and non-ASCII patterns (whose hit
// starts can move backwards in end order), and a 1 MB wide text must finish
// inside a bound that the linear scan meets about 500 times over.
import { MultiMatcher } from "dyna:matcher";
import { suite } from "../lib/kit.js";
import { cpuMs } from "../lib/cpuclock.js";
import { wideText, oracle, byAt } from "../lib/matcher_wide_text.js";

const t = suite("matcher_multi_matcher_all_in_wide");

t.test("offsets match indexOf on mixed BMP and astral text", (a) => {
    const text = wideText(20000);
    const pats = ["a", "aa", "aaa", "→", "😀", "→a", "é", "😀a", "b"];
    a.deep(byAt(new MultiMatcher(pats).allIn(text)), oracle(text, pats), "every (index, at) pair");
});

t.test("overlapping patterns whose starts go backwards in end order", (a) => {
    const text = "→" + "a".repeat(50) + "😀" + "aaaa→";
    const pats = ["aaa", "a", "aa"];
    a.deep(byAt(new MultiMatcher(pats).allIn(text)), oracle(text, pats), "dense overlaps after a wide char");
});

t.test("a wide char only at the end still gives exact offsets", (a) => {
    const text = "x add y sub ".repeat(1000) + "→";
    const pats = ["add", "sub"];
    a.deep(byAt(new MultiMatcher(pats).allIn(text)), oracle(text, pats), "late wide char");
});

t.test("1 MB of wide text finishes in linear time", (a) => {
    const text = wideText(1 << 20);
    const mm = new MultiMatcher(["a", "→"]);
    const want = mm.countIn(text);
    let hits;
    const { ms } = cpuMs(() => { hits = mm.allIn(text); });
    a.eq(hits.length, want, "allIn and countIn agree");
    a.ok(ms < 3000, `allIn took ${ms} ms on 1 MB of wide text (quadratic scan takes about 12 s)`);
});

await t.run();
