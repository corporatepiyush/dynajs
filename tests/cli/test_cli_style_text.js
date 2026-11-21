// flags: --std
// covers: dyna:cli.StyleText
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_style_text");
import { Command, StyleText, Styles, IsTTY, Columns, ColorDepth } from "dyna:cli";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const base = () => new Command("tool")
    .option("-v, --verbose", "chatty", { type: "boolean" })
    .option("-o, --out <path>", "output file", { type: "string" })
    .option("-n, --count <n>", "how many", { type: "number", default: 1 });

__kitS.section("a single style", () => {
    const had = std.getenv("FORCE_COLOR");
    std.setenv("FORCE_COLOR", "1");
    eq(StyleText("red", "x"), "\u001B[31mx\u001B[39m", "a single style");
    eq(StyleText("bold", "x"), "\u001B[1mx\u001B[22m", "bold has its own reset");
    eq(StyleText(["red", "bold"], "x"),
       "\u001B[31m\u001B[1mx\u001B[22m\u001B[39m",
       "an array composes, and closes in reverse order");
    eq(StyleText("red", ""), "\u001B[31m\u001B[39m", "empty text still brackets");
    if (had === undefined) std.unsetenv("FORCE_COLOR"); else std.setenv("FORCE_COLOR", had);
});

eq(StyleText(["green", "underline"], "hi").stripAnsi(), "hi", "stripAnsi recovers the text");

eq(StyleText(["green", "underline"], "hi").displayWidth(), 2, "styling adds no width");

throws(() => StyleText("chartreuse", "x"), "an unknown style is REFUSED, not ignored");

throws(() => StyleText("red"), "StyleText needs both arguments");

__kitS.section("reset closes with a second 0m like Node", () => {
    const had = std.getenv("FORCE_COLOR");
    std.setenv("FORCE_COLOR", "1");
    eq(StyleText("reset", "x"), "\u001B[0mx\u001B[0m", "reset closes with a second 0m like Node");
    if (had === undefined) std.unsetenv("FORCE_COLOR"); else std.setenv("FORCE_COLOR", had);
});

throws(() => StyleText("red", 5), "StyleText refuses a non-string text like Node");

__kitS.section("an unknown style is a TypeError like Node", () => {
    let e = null;
    try { StyleText("chartreuse", "x"); } catch (ex) { e = ex; }
    assert(e !== null && e.constructor.name === "TypeError",
           "an unknown style is a TypeError like Node");
});
__kitS.end();

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
