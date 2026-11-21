// covers: dyna:yaml.ParseAll
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_all");
import { Parse, ParseAll, Stringify } from "dyna:yaml";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
const j = (v) => JSON.stringify(v);
function same(text, want, msg) { eq(j(Parse(text)), j(want), msg); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
function refuses(text, needle, msg) {
    let m = "";
    try { Parse(text); } catch (e) { m = String(e.message); }
    assert(m.indexOf(needle) >= 0, msg + " (message: " + m + ")");
}

eq(j(ParseAll("a: 1\n---\nb: 2")), j([{ a: 1 }, { b: 2 }]), "two documents");

eq(j(ParseAll("---\na: 1\n---\nb: 2")), j([{ a: 1 }, { b: 2 }]), "a leading ---");

eq(ParseAll("a: 1").length, 1, "one document is still an array");

eq(ParseAll("").length, 0, "an empty input has no documents");

refuses("a: 1\n---\nb: 2", "ParseAll", "Parse REFUSES a multi-document input, naming ParseAll");

eq(j(ParseAll("a: 1\n...\n")), j([{ a: 1 }]),
   "`...` ends the document: exactly one, never a runaway stream of nulls");

eq(j(ParseAll("a: 1\n---\nb: 2\n...\n")), j([{ a: 1 }, { b: 2 }]),
   "two documents closed by a trailing ...");

eq(j(ParseAll("...\n---\na: 1")), j([{ a: 1 }]),
   "an end marker before any document contributes no document");

eq(ParseAll("...\n").length, 0, "a bare end marker is ZERO documents (nothing was open)");

refuses("a: 1\n...\nb: 2", "ParseAll",
        "content after `...` is still a second document, refused by name");

refuses("a: &anchor 1", "(&name)", "an anchor ALONE is refused as an anchor");
refuses("b: *anchor", "(*name)", "and an alias alone as an alias");
refuses("a: !!str 1", "tag", "a tag is refused as a tag");
refuses("a: !custom {}", "tag", "including a local tag");
refuses("%YAML 1.2\n---\na: 1", "directive", "a directive is refused as a directive");
refuses("d:\n  <<: x", "merge key", "a merge key is refused as a merge key");
refuses("? complex\n: value", "explicit key", "an explicit key is refused as one");
refuses("a:\n\tb: 1", "tab", "a tab in the indentation is refused as a tab");
__kitS.section("defaults: &d retries: 3 prod: <<: *d", () => {
    let got = "parsed";
    try { got = j(Parse("defaults: &d\n  retries: 3\nprod:\n  <<: *d\n")); }
    catch (e) { got = "refused"; }
    eq(got, "refused", "an anchored document fails rather than losing a key");
});
__kitS.end();

eq(j(ParseAll("\uFEFFa: 1\n")), j([{ a: 1 }]), "ParseAll strips it too");

eq(j(ParseAll("--- 5\n--- 6")), j([5, 6]), "ParseAll reads marker-line nodes");

if (fails) {
    print("test_yaml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_yaml failed");
}
print("test_yaml: " + n + " assertions, 0 failures");
