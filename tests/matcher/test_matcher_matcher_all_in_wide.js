// covers: dyna:matcher.Matcher.allIn
// Matcher.allIn shares the byte-to-UTF-16 offset conversion with MultiMatcher
// (see test_matcher_multi_matcher_all_in_wide.js): offsets must equal indexOf's
// on text holding characters above U+00FF, and 1 MB of such text must scan in
// linear time.
import { Matcher } from "dyna:matcher";
import { suite } from "../lib/kit.js";
import { cpuMs } from "../lib/cpuclock.js";
import { wideText, oracle } from "../lib/matcher_wide_text.js";

const t = suite("matcher_matcher_all_in_wide");

for (const p of ["a", "aa", "→", "😀", "a→a", "é"]) {
    t.test(`offsets of ${JSON.stringify(p)} match indexOf`, (a) => {
        const text = wideText(20000, 11);
        a.deep(new Matcher(p).allIn(text), oracle(text, [p]).map((h) => h.at), "every offset");
    });
}

t.test("1 MB of wide text finishes in linear time", (a) => {
    const text = wideText(1 << 20, 3);
    const m = new Matcher("a");
    let n;
    const { ms } = cpuMs(() => { n = m.allIn(text).length; });
    a.eq(n, m.countIn(text), "allIn and countIn agree");
    a.ok(ms < 3000, `allIn took ${ms} ms on 1 MB of wide text (quadratic scan takes about 12 s)`);
});

await t.run();
