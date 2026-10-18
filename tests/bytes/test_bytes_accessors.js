import { Bytes, Text } from "dyna:bytes";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
function throws(fn, w) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e.message || e); }
    ok(t, w, t ? undefined : "did NOT throw");
    return m;
}

print("--- Text: transcoders, against TextEncoder as the reference ---");
{
    const need = ["isValidUtf8", "isValidUtf16", "countUtf8", "countUtf16",
                  "toBytes", "latin1ToUtf8", "utf8ToLatin1",
                  "utf8ToUtf16", "utf16ToUtf8"];
    const proto = Object.getPrototypeOf(new Text("x"));
    ok(!("toUtf8" in proto), "Text.prototype.toUtf8 is gone (TextEncoder is the API)");
    for (const m of need) {
        const d = Object.getOwnPropertyDescriptor(proto, m);
        ok(!!d && (typeof d.value === "function" || typeof d.get === "function"),
           "Text.prototype." + m + " exists");
    }

    for (const str of ["a", "ab", "abc", "héllo", "a→b", "😀", ""]) {
        const want = Array.from(new TextEncoder().encode(str)).join(",");
        let got;
        try { got = Array.from(new Text(str).toBytes().array).join(","); }
        catch (e) { got = "THREW " + String(e.message).slice(0, 30); }
        ok(got === want, "Text(" + JSON.stringify(str) + ").toBytes() matches " +
           "TextEncoder", got);
    }
    ok(Array.from(new Text("abc").toBytes().array).length === 3,
       "an ODD-length string does not throw (it did: byte length must be even)");

    const t = new Text("héllo");
    ok(t.isValidUtf8 === true, "isValidUtf8 getter accepts valid text");
    {
        let threw = "";
        try { t.isValidUtf8(); } catch (e) { threw = String(e.message).slice(0, 30); }
        ok(threw.includes("not a function"),
           "the method spelling is no longer a call (one property per name)", threw);
    }
    ok(t.countUtf8() === 5, "countUtf8 counts code points", t.countUtf8());
    ok(new Text("a😀").countUtf16() === 2,
       "countUtf16 counts CODE POINTS (a surrogate pair counts once)",
       new Text("a😀").countUtf16());
    for (const str of ["a😀", "héllo", "a→b", ""])
        ok(new Text(str).countUtf8() === new Text(str).countUtf16(),
           "countUtf8 and countUtf16 agree on " + JSON.stringify(str) +
           " (both are code-point counts)");
    ok(new Text("a😀").countUtf16() !== "a😀".length,
       "and neither equals String.length, which IS units (3)");
    ok(new Text("abc").isValidUtf16() === true,
       "isValidUtf16 accepts plain ASCII (it answered FALSE)");
    ok(new Text("a\uD800b").isValidUtf16() === false, "a lone HIGH surrogate is invalid");
    ok(new Text("a\uDFFFb").isValidUtf16() === false, "a lone LOW surrogate is invalid");
    ok(new Text("abc").countUtf16() === 3,
       "countUtf16 of ASCII is 3 (it answered 1)", new Text("abc").countUtf16());
    ok(Array.from(new Text("abc").utf16ToUtf8()).join(",") === "97,98,99",
       "utf16ToUtf8 on a Text is its UTF-8 encoding (it threw)");
    ok(new Text("a\uD800b").isValidUtf16() === false,
       "isValidUtf16 REJECTS a lone high surrogate");
    ok(new Text("a😀b").isValidUtf16() === true,
       "and accepts a well-formed pair");
    ok(t.toBytes().length === 6, "toBytes is the UTF-8 encoding", t.toBytes().length);
}

ok((() => { try { new Text("\uD800").utf16ToUtf8(); return false; }
            catch (e) { return e instanceof RangeError; } })(),
   "Text(utf16ToUtf8) throws RangeError on a lone HIGH surrogate");
ok((() => { try { new Text("a\uDFFFb").utf16ToUtf8(); return false; }
            catch (e) { return e instanceof RangeError; } })(),
   "Text(utf16ToUtf8) throws RangeError on a lone LOW surrogate");
ok((() => { const r = new Text("\uD83D\uDE00").utf16ToUtf8();
            return Array.from(r).join(",") === "240,159,152,128"; })(),
   "Text(utf16ToUtf8) encodes a PAIRED surrogate as standard UTF-8");
ok((() => { const r = new Text("\uD800\uDC00").utf16ToUtf8();
            return Array.from(r).join(",") === "240,144,128,128"; })(),
   "Text(utf16ToUtf8) D800 DC00 is U+10000 = F0 90 80 80");
ok((() => { const r = new Text("h\u00E9llo").utf16ToUtf8();
            return r instanceof Uint8Array && r.length === 6; })(),
   "Text(utf16ToUtf8) well-formed unchanged");
ok(new Text("h\u00E9llo \uD83D\uDE00").countUtf16() === 7,
   "Text(countUtf16) pairs count once (7 code points)");
ok(new Text("a\uD800b").countUtf16() === 3,
   "Text(countUtf16) lone high still counts once per unit (3 code points)");

print("test_bytes_accessors: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_bytes_accessors: " + fail + " failures");
