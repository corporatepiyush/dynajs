// flags: --std
// covers: dyna:cli.IsTTY
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_cli_is_tty");
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

__kitS.section("IsTTY returns a boolean", () => {
assert(typeof IsTTY(1) === "boolean", "IsTTY returns a boolean");
});

__kitS.section("IsTTY with no fd reads no out-of-range argv", () => {
assert(typeof IsTTY() === "boolean", "IsTTY with no fd reads no out-of-range argv");
});

__kitS.section("IsTTY(fd) refuses a non-number fd", () => {
throws(() => IsTTY("banana"), "IsTTY(fd) refuses a non-number fd");
});
__kitS.end();

if (fails) {
    print("test_cli: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_cli failed");
}
print("test_cli: " + n + " assertions, 0 failures");
