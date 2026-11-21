// covers: dyna:html.HTMLParse
// Element names are kept whole. HTMLParse used to cut a tag name to 63 bytes (and the open-element stack to
// 62), so two long custom-element names that differ only after byte 63 parsed as the same element: the
// second opened inside the first, a closing tag matched the wrong element, and the name came back cut
// (fix-DC campaign note F-DC5; attribute names had already been fixed). Lengths around the old limits are
// checked, plus raw-text elements whose closing tag is found by name.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_html_parse_long_tag_names");
import { HTMLParse, HTMLStringify } from "dyna:html";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const S = (h) => HTMLStringify(HTMLParse(h));

for (const len of [62, 63, 64, 65, 70, 200, 4000]) {
    const a = "x-" + "a".repeat(len - 3) + "1", b = "x-" + "a".repeat(len - 3) + "2";
    const doc = HTMLParse(`<${a}>one</${a}><${b}>two</${b}>`);
    check(doc.length === 2, len + ": two sibling elements, not one nested in the other (" + doc.length + ")");
    check(doc[0] && doc[0].name === a && doc[0].name.length === len, len + ": the first name is whole");
    check(doc[1] && doc[1].name === b, len + ": the second name is whole and distinct");
    check(S(`<${a}><${b}>in</${b}>tail</${a}>`) === `<${a}><${b}>in</${b}>tail</${a}>`, len + ": nesting round-trips");
    const up = a.toUpperCase();
    check(HTMLParse(`<${up}>z</${a}>`)[0].name === a, len + ": names are lower-cased whole, and close case-insensitively");
}
check(S("<span>x</spa>y</span>") === "<span>xy</span>", "a closing tag that is a prefix of the open name closes nothing");
check(S("<sp>x</span>y</sp>") === "<sp>xy</sp>", "nor one the open name is a prefix of");
__kitS.section("a long tag inside raw text does not close the script", () => {
    const t = "x-" + "s".repeat(80);
    check(S(`<script>if (a</${t}>b) {}</script><p>x</p>`) === `<script>if (a</${t}>b) {}</script><p>x</p>`,
        "a long tag inside raw text does not close the script");
});
__kitS.end();
print("test_html_long_tag_names: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_html_long_tag_names: " + fails + " failures");
