// covers: dyna:yaml.Parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_upgrade");
import { Parse, ParseAll } from "dyna:yaml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg, needle) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor,
        msg + " (" + ctor.name + ", got " + caught + ")");
    if (needle)
        assert(caught.message.indexOf(needle) >= 0,
            msg + " (message mentions " + needle + "): " + caught.message);
}

__kitS.section("a: [1, 2]", () => {
assert(JSON.stringify(Parse("a: [1, 2]")) === '{"a":[1,2]}',
    "Parse without options unchanged");
});

__kitS.section("a: 1", () => {
assert(Parse("a: 1", {}).a === 1, "empty options object ok");
});

__kitS.section("a: 1 (L24)", () => {
assert(Parse("a: 1", { schema: "core" }).a === 1, "schema core explicit ok");
});

__kitS.section("a: {b: {c: 3}}", () => {
assert(Parse("a: {b: {c: 3}}").a.b.c === 3, "nesting below the default cap");
});

const deep = (d) => "k: " + "[".repeat(d) + "]".repeat(d);

__kitS.section("100 levels under default cap", () => {
assert(Parse(deep(100)).k[0][0][0] !== undefined, "100 levels under default cap");
});

__kitS.section("maxDepth at the ceiling accepted", () => {
assert(Parse(deep(100), { maxDepth: 128 }).k !== undefined,
    "maxDepth at the ceiling accepted");
});

__kitS.section("lowered cap fires", () => {
throws(() => Parse(deep(100), { maxDepth: 50 }), Error, "lowered cap fires",
    "depth limit");
});

__kitS.section("input within a lowered cap passes", () => {
assert(Parse(deep(40), { maxDepth: 50 }) !== undefined,
    "input within a lowered cap passes");
});

__kitS.section("a: 1 (L41)", () => {
throws(() => Parse("a: 1", { maxDepth: 0 }), TypeError, "maxDepth 0 refused");
});

__kitS.section("a: 1 (L43)", () => {
throws(() => Parse("a: 1", { maxDepth: -1 }), TypeError, "maxDepth negative refused");
});

__kitS.section("a: 1 (L45)", () => {
throws(() => Parse("a: 1", { maxDepth: 129 }), TypeError,
    "maxDepth above the ceiling refused");
});

__kitS.section("a: 1 (L48)", () => {
throws(() => Parse("a: 1", { maxDepth: 2.5 }), TypeError,
    "maxDepth non-integer refused");
});

__kitS.section("a: 1 (L51)", () => {
throws(() => Parse("a: 1", { maxDepth: "8" }), TypeError,
    "maxDepth string refused");
});

__kitS.section("a: 1 (L54)", () => {
assert(Parse("a: 1", { maxDepth: null }).a === 1,
    "maxDepth null is treated as absent (bag convention)");
});

__kitS.section("a: 1 (L57)", () => {
throws(() => Parse("a: 1", { schema: "full" }), RangeError,
    "schema full refused by name", "full schema is not implemented");
});

__kitS.section("a: 1 (L60)", () => {
throws(() => Parse("a: 1", { schema: "bogus" }), TypeError,
    "unknown schema refused");
});

__kitS.section("a: 1 (L63)", () => {
throws(() => Parse("a: 1", { schema: 42 }), TypeError, "schema number refused");
});

__kitS.section("&anchor v: 1", () => {
throws(() => Parse("&anchor v: 1"), Error, "anchors refused",
    "anchor");
});

__kitS.section("k: *alias", () => {
throws(() => Parse("k: *alias"), Error, "aliases refused", "alias");
});

__kitS.section("k: *alias (L70)", () => {
throws(() => Parse("k: *alias", { maxDepth: 8 }), Error,
    "alias refusal is independent of depth");
});
__kitS.end();

print("test_yaml_upgrade: all tests passed (" + n + " assertions)");
