// flags: --std
// covers: dyna:yaml.Parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_emitter_opts");
import { Stringify, Parse } from "dyna:yaml";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqj(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b), msg +
        " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMsg(fn, re, msg) {
    let e = null;
    try { fn(); } catch (ex) { e = ex; }
    assert(e !== null && re.test(e.message),
        msg + " (threw " + (e && e.message) + ")");
}
const LONG = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
const rt = (v, opts, msg) => {
    const text = Stringify(v, opts);
    let back;
    try { back = Parse(text); }
    catch (e) { assert(false, msg + ": the writer's output does not re-parse: " + e.message + " in " + JSON.stringify(text)); return; }
    let again;
    try { again = Stringify(back, opts); }
    catch (e) { assert(false, msg + ": the re-parsed value does not re-emit: " + e.message); return text; }
    eq(again, text, msg + " (output-stable roundtrip)");
    return text;
};

__kitS.section("key: \"alpha beta gamma delta epsilon \\", () => {
    const long = LONG;
    const text = Stringify({ key: long, short: 1 }, { width: 30 });
    const lines = text.split("\n");
    eq(lines[0], "key: \"alpha beta gamma delta epsilon \\", "first line folds");
    assert(Parse(text).key === long, "folded value round-trips exactly");
    eq(Parse(text).short, 1, "the sibling entry survives the fold");
});
__kitS.end();

if (fails === 0) print("test_yaml_emitter_opts: all " + n + " tests passed");
else { print("test_yaml_emitter_opts: " + fails + " of " + n + " FAILED"); std.exit(1); }
