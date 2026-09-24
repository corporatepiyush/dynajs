// converted from an escaping-throw probe: the original source is
// re-executed (output silenced) and its per-engine behavior —
// throw + emitted lines — asserted against baked expectations
__EXP = null;
test("h03_spread_getter_len.js per-engine behavior contract", function () {
    var __SRC = "// H: spread where the source array's length property is a GETTER (must be called;\n// number of calls must match node \u2014 the array iterator checks length per step)\nlet calls = 0;\nconst arr5 = [1, 2, 3, 4];\nObject.defineProperty(arr5, \"length\", { get() { calls++; return 2; }, configurable: true });\nconst r5 = [...arr5];\nconsole.log(\"g1\", calls, JSON.stringify(r5));\nlet calls2 = 0;\nconst arr6 = [1, 2, 3, 4];\nObject.defineProperty(arr6, \"length\", { get() { calls2++; return 2; }, configurable: true });\nfunction idf() { return Array.from(arguments); }\nconst r6 = idf(...arr6);\nconsole.log(\"g2\", calls2, JSON.stringify(r6));\n// length getter on the SOURCE read during for-of directly (no spread)\nlet calls3 = 0;\nconst arr7 = [9, 8, 7];\nObject.defineProperty(arr7, \"length\", { get() { calls3++; return 2; }, configurable: true });\nconst got = [];\nfor (const v of arr7) got.push(v);\nconsole.log(\"g3\", calls3, got.join(\",\"));\n// length getter that returns increasing values\nlet counter = 0;\nconst arr8 = [1, 2, 3, 4, 5];\nObject.defineProperty(arr8, \"length\", { get() { return ++counter > 3 ? 5 : counter; }, configurable: true });\nconst got8 = [];\nfor (const v of arr8) got8.push(v);\nconsole.log(\"g4\", counter, got8.join(\",\"), [...arr8].length);\n";
    var __lines = __NP(); __lines.n = 0; var __e = null;
    var __sl = console.log; var __sp = (typeof print === 'function') ? print : null;
    console.log = function () { var a = __NP(); var i; for (i = 0; i < arguments.length; i++) a[i] = arguments[i]; a.n = arguments.length; __lines[__lines.n] = __fmtLine(a); __lines.n++; };
    if (__sp) { try { print = function (s) { console.log(String(s)); }; } catch (__e9) {} }
    try { eval(__SRC); } catch (err) { __e = err; }
    console.log = __sl;
    if (__sp) { try { print = __sp; } catch (__e8) {} }
    if (__ENGINE === 'dynajs') {
        assert_eq(__e !== null, true, "dynajs threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "TypeError", "dynajs error ctor");
        assert_eq(__e && __e.message, "property is not configurable", "dynajs error message");
        assert_eq(__lines.n, 0, "dynajs line count");
    }
    if (__ENGINE !== 'dynajs') {
        assert_eq(__e !== null, true, "node threw");
        assert_eq(__e && __e.constructor && __e.constructor.name, "TypeError", "node error ctor");
        assert_eq(__e && __e.message, "Cannot redefine property: length", "node error message");
        assert_eq(__lines.n, 0, "node line count");
    }
});
summary("bbreview");
