// timeout: 120
// flags: --std
// Split from tests/test_platform_hardening.js: the core sections.
import * as std from "std";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throwsRange(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof RangeError; }
    assert(t, msg);
}
function throwsType(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = e instanceof TypeError; }
    assert(t, msg);
}
{
    assert(std.sprintf("%5d", 42) === "   42", "printf width still pads");
    assert(std.sprintf("%.3f", 1.5) === "1.500", "printf precision still rounds");
    assert(std.sprintf("%5c", 65) === "    A", "printf %c honours width (COMPAT-167)");
    assert(std.sprintf("%c", 65) === "A", "printf %c without width unchanged");
    throwsRange(() => std.sprintf("%2000000000d", 1), "printf width above 1e6 is refused");
    throwsRange(() => std.sprintf("%.2000000000f", 1), "printf precision above 1e6 is refused");
    throwsRange(() => std.sprintf("%*d", 2000000000, 1), "printf star width above 1e6 is refused");
    throwsRange(() => std.sprintf("%.*f", -2000000000, 1), "printf star precision magnitude above 1e6 is refused");
    assert(std.sprintf("%*d", 6, 42) === "    42", "printf star width still works");
}
print("test_core_platform_hardening: all tests passed (" + n + " assertions)");
