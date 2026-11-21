// covers: dyna:bytes.Text
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_text_handle");
import { Bytes, Text, compare, equal, isValidUtf8, countUtf8 } from "dyna:bytes";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function throws(fn, msg) {
    n++;
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (caught === null) throw new Error("assertion failed: " + msg + " (expected throw)");
}

__kitS.section("value is the string", () => {
    const t = new Text("héllo");
    eq(t.value, "héllo", "value is the string");
    eq(t.toString(), "héllo", "toString");
    eq(JSON.stringify({ t }), '{"t":"héllo"}', "toJSON");
    eq(t.countUtf8(), 5, "code points, not bytes");
    eq(t.isValidUtf8, true, "valid (DT-8: getter)");
    eq(t.toBytes().length, 6, "é is two bytes in UTF-8");
    assert(Bytes.isBytes(t.toBytes()), "toBytes yields a Bytes handle");

    eq(t.isWide, false, "Latin-1 is not wide");
    eq(new Text("héllo→").isWide, true, "U+2192 is wide");
    eq(new Text("abc").isWide, false, "ASCII is not wide");

    eq(new Text("").countUtf8(), 0, "the empty text");
    eq(new Text(42).value, "42", "a non-string argument is coerced once");
    throws(() => new Text(), "the argument is required");

    for (const m of ["isValidUtf16", "countUtf8", "countUtf16",
                     "latin1ToUtf8", "utf8ToLatin1", "utf8ToUtf16", "utf16ToUtf8",
                     "toBytes", "toString", "toJSON"])
        assert(typeof new Text("x")[m] === "function", "Text has " + m);
    {
        const d = Object.getOwnPropertyDescriptor(
            Object.getPrototypeOf(new Text("x")), "isValidUtf8");
        assert(d && typeof d.get === "function", "Text has isValidUtf8 (getter)");
    }

    assert(isValidUtf8("ok") === true, "isValidUtf8 survived the fold");
    eq(countUtf8("héllo"), 5, "countUtf8 survived the fold");
});
__kitS.end();

print("test_bytes_handle: all " + n + " assertions passed");
