// flags: --std
// covers: dyna:cli.Styles
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_styles");
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

__kitS.section("Styles() enumerates the set", () => {
    const list = Styles();
    assert(Array.isArray(list) && list.length > 10, "Styles() enumerates the set");
    for (const s of list)
        assert(typeof StyleText(s, "x") === "string",
               "every style Styles() lists actually works: " + s);
});

__kitS.section("the style set matches Node v22's 46 names", () => {
    const list = Styles();
    eq(list.length, 46, "the style set matches Node v22's 46 names");
    for (const s of ["grey", "redBright", "bgGray", "blink", "hidden", "framed",
                     "overlined", "doubleunderline", "bgRedBright", "whiteBright"])
        assert(list.indexOf(s) >= 0, "Node's style name is present: " + s);
});
__kitS.end();

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
