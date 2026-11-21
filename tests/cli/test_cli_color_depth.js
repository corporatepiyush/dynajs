// flags: --std
// covers: dyna:cli.ColorDepth
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_color_depth");
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

__kitS.section("ColorDepth is 0, 4, 8 or 24", () => {
    const d = ColorDepth(1);
    assert([0, 4, 8, 24].indexOf(d) >= 0, "ColorDepth is 0, 4, 8 or 24 (got " + d + ")");
    if (!IsTTY(1)) eq(d, 0, "a non-TTY reports no colour");
});

__kitS.section("FORCE_COLOR=1 forces 16-colour on a non-TTY", () => {
    const { setenv, unsetenv } = std;
    const depth = () => ColorDepth(1);
    unsetenv("NO_COLOR");
    setenv("FORCE_COLOR", "1");
    eq(depth(), 4, "FORCE_COLOR=1 forces 16-colour on a non-TTY");
    setenv("FORCE_COLOR", "true");
    eq(depth(), 4, "FORCE_COLOR=true too");
    setenv("FORCE_COLOR", "2");
    eq(depth(), 8, "FORCE_COLOR=2 is 256");
    setenv("FORCE_COLOR", "3");
    eq(depth(), 24, "FORCE_COLOR=3 is truecolor");
    setenv("NO_COLOR", "1");
    eq(depth(), 24, "and FORCE_COLOR overrides NO_COLOR");
    setenv("FORCE_COLOR", "0");
    eq(depth(), 0, "FORCE_COLOR=0 refuses");
    setenv("FORCE_COLOR", "false");
    eq(depth(), 0, "and FORCE_COLOR=false too");
    setenv("FORCE_COLOR", "9");
    eq(depth(), 4, "any other non-empty value is level 1");
    unsetenv("FORCE_COLOR");
    unsetenv("NO_COLOR");
    setenv("NO_COLOR", "1");
    if (IsTTY(1))
        eq(depth(), 0, "NO_COLOR disables colour on a real TTY");
    else
        eq(depth(), 0, "NO_COLOR set and non-empty disables (non-TTY is 0 anyway)");
    unsetenv("NO_COLOR");
});
__kitS.end();

throws(() => ColorDepth("banana"), "ColorDepth(fd) refuses a non-number fd");

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
