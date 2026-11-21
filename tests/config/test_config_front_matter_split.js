// covers: dyna:config.FrontMatter.split
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_front_matter_split");
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

__kitS.section("title: hi body text", () => {
    const r = FrontMatter.split("---\ntitle: hi\n---\nbody text\n");
    eq(r.lang, "yaml", "front matter --- is yaml");
    eq(r.data, "title: hi", "front matter data");
    eq(r.body, "body text\n", "front matter body");
});

__kitS.section("+++ title = \"hi\" +++ body", () => {
    const r = FrontMatter.split("+++\ntitle = \"hi\"\n+++\nbody\n");
    eq(r.lang, "toml", "front matter +++ is toml");
    eq(r.data, 'title = "hi"', "toml front matter data");
});

__kitS.section("front matter ;;; is json", () => {
    const r = FrontMatter.split(';;;\n{"a":1}\n;;;\nbody\n');
    eq(r.lang, "json", "front matter ;;; is json");
    eq(r.data, '{"a":1}', "json front matter data");
});

__kitS.section("just a document", () => {
    const r = FrontMatter.split("just a document\n");
    eq(r.data, null, "no front matter -> data null");
    eq(r.lang, null, "no front matter -> lang null");
    eq(r.body, "just a document\n", "no front matter -> body is the whole input");
});

__kitS.section("title: hi body with no close", () => {
    const r = FrontMatter.split("---\ntitle: hi\nbody with no close\n");
    eq(r.data, null, "unclosed fence is not front matter");
    eq(r.body, "---\ntitle: hi\nbody with no close\n", "unclosed fence keeps the body whole");
});

__kitS.section("--- a: 1 ---", () => {
    const r = FrontMatter.split("\n---\na: 1\n---\n");
    eq(r.data, null, "a fence on line 2 is not front matter");
});

__kitS.section("empty front matter data is the empty string, not null", () => {
    const r = FrontMatter.split("---\n---\nbody\n");
    eq(r.data, "", "empty front matter data is the empty string, not null");
    eq(r.body, "body\n", "empty front matter body");
});

__kitS.section("r a: 1r r bodyr", () => {
    const r = FrontMatter.split("---\r\na: 1\r\n---\r\nbody\r\n");
    eq(r.lang, "yaml", "CRLF front matter detected");
    eq(r.data, "a: 1", "CRLF front matter data has no stray \\r");
});

__kitS.section("a: 1 before after", () => {
    const r = FrontMatter.split("---\na: 1\n---\nbefore\n---\nafter\n");
    eq(r.data, "a: 1", "only the first block is front matter");
    eq(r.body, "before\n---\nafter\n", "a later --- stays in the body");
});

__kitS.section("uFEFF a: 1 body", () => {
    const r = FrontMatter.split("\uFEFF---\na: 1\n---\nbody\n");
    eq(r.lang, "yaml", "BOM before the fence does not block front matter");
    eq(r.data, "a: 1", "BOM front matter data");
    eq(r.body, "body\n", "BOM front matter body");
});
__kitS.end();

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
