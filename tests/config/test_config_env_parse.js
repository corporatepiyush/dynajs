// covers: dyna:config.Env.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env_parse");
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

__kitS.section("Env simple key", () => {
    const e = Env.parse("A=1\nB=two\n");
    eq(e.A, "1", "Env simple key");
    eq(e.B, "two", "Env second key");
});

eq(Env.parse("export A=1\n").A, "1", "Env accepts an export prefix");

eq(Env.parse("# c\nA=1\n").A, "1", "Env skips comments");

eq(Env.parse("A=1 # trailing\n").A, "1", "Env strips an unquoted trailing comment");

eq(Env.parse('A="1 # not a comment"\n').A, "1 # not a comment",
   "Env keeps # inside quotes");

eq(Env.parse('A="line\\nbreak"\n').A, "line\nbreak", "Env expands \\n in double quotes");

eq(Env.parse("A='line\\nbreak'\n").A, "line\\nbreak",
   "Env does NOT expand escapes in single quotes");

eq(Env.parse("A=  spaced  \n").A, "spaced", "Env trims an unquoted value");

eq(Env.parse('A="  spaced  "\n').A, "  spaced  ", "Env keeps padding inside quotes");

eq(Env.parse("A=\n").A, "", "Env allows an empty value");

eq(Env.parse("novalue\nA=1\n").A, "1", "Env skips a line with no =");

eq(Env.parse("A=1\r\nB=2\r\n").B, "2", "Env handles CRLF");

__kitS.section("Env writes __proto__ as an OWN property", () => {
    const e = Env.parse("__proto__=x\n");
    assert(Object.prototype.hasOwnProperty.call(e, "__proto__"),
           "Env writes __proto__ as an OWN property");
    eq(({}).polluted, undefined, "Env pollutes nothing");
});
__kitS.end();

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
