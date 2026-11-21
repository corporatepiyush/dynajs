// covers: c:src/parser/parser.inc.c
// tests/core/test_parse_template_div.js -- FZ1: the arrow/destructuring lookahead
// (js_parse_skip_parens_token) read "/" after a complete template literal as a
// regexp, so `((a = \`x\`/1) => a)` was a SyntaxError, and a failed lookahead
// regexp or token left its SyntaxError pending, which kept the whole realm alive
// after the real parse succeeded (found by the fuzz_eval memory-return oracle).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_parse_template_div");
let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const ev = (src) => { try { return (0, eval)(src); } catch (e) { return e; } };

__kitS.section("template then division", () => {
    const ok = [["(`x`/1)", NaN], ["(`x`/2/1)", NaN], ["(`${4}`/2)", 2], ["(`${8}`/2/2)", 2], ["(`a${1}b`/{})", NaN],
        ["((a = `x`/1) => a)()", NaN], ["((a = `${6}`/3) => a)()", 2], ["((a = `${/x/.source}`) => a)()", "x"],
        ["(`${/a/g.flags}`)", "g"], ["((a, b = `${a}`/2) => b)(8)", 4], ["(({ a = `4`/2 }) => a)({})", 2],
        ["[`6`/3][0]", 2], ["(() => `x`)()/1", NaN], ["(`x`\n/1)", NaN], ["((a = `${`${2}`/2}`) => a)()", "1"]];
    for (const [src, want] of ok) {
        const got = ev(src);
        assert(Object.is(got, want), src + " => " + String(got) + ", want " + String(want));
    }
});
__kitS.section("lookahead errors do not leak into a good parse", () => {
    const fine = ["(a => a)(/[/]/.source)", "(function (of) { return (of / 2); })(8)", "(function (yield) { return (yield / 2); })(8)"];
    const want = ["[/]", 4, 4];
    fine.forEach((src, i) => assert(Object.is(ev(src), want[i]), src));
    const bad = ["(`${/)/.source}`)", "((a = `${/)/.source}`) => a)()", "(/[/)"];
    for (const src of bad)
        assert(ev(src) instanceof SyntaxError, src + " is a SyntaxError");
});
__kitS.end();

print("test_parse_template_div: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_parse_template_div: " + fails + " failures");
