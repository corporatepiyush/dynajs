// covers: dyna:http.MultipartFormat
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_multipart_format");
import { MultipartParse, MultipartFormat } from "dyna:http";

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
function throwsMsg(fn, substr, msg) {
    let t = false;
    try { fn(); } catch (e) { t = String(e).indexOf(substr) >= 0; }
    assert(t, msg + " (want a throw containing: " + substr + ")");
}
function b2s(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}
const CR = "\r\n";
const enc = new TextEncoder();

__kitS.section("hello multipart", () => {
    const fmt = MultipartFormat([
        { name: "user", value: "Xavier" },
        { name: "file", body: enc.encode("hello multipart"), filename: "hello.txt", contentType: "text/plain" },
    ]);
    eq(fmt.contentType, "multipart/form-data; boundary=" + fmt.boundary, "contentType built from boundary");
    eq(fmt.boundary.length, 48, "generated boundary is 48 chars (curl-shaped)");
    assert(/^[-0-9A-Za-z]+$/.test(fmt.boundary), "generated boundary is bcharsnospace");
    const p = MultipartParse(fmt.contentType, fmt.body);
    eq(p.length, 2, "roundtrip: two parts");
    eq(p[0].name, "user", "roundtrip: field name");
    eq(b2s(p[0].body), "Xavier", "roundtrip: field value");
    eq(p[1].filename, "hello.txt", "roundtrip: filename");
    eq(p[1].contentType, "text/plain", "roundtrip: contentType");
    eq(b2s(p[1].body), "hello multipart", "roundtrip: file body");
});

__kitS.section("line2 --abc123 world", () => {
    const fmt = MultipartFormat([
        { name: "a b", value: "line1" + CR + "line2 --abc123 world", filename: 'f"g\\h.txt', contentType: "text/plain" },
    ], "abc123");
    eq(fmt.contentType, "multipart/form-data; boundary=abc123", "explicit boundary in contentType");
    const p = MultipartParse(fmt.contentType, fmt.body);
    eq(p[0].name, "a b", "space in name survives the escape roundtrip");
    eq(p[0].filename, 'f"g\\h.txt', "quotes/backslash in filename survive the escape roundtrip");
    eq(b2s(p[0].body), "line1" + CR + "line2 --abc123 world", "payload CRLF and mid-line boundary survive");
});

__kitS.section("empty multipart roundtrips", () => {
    const fmt = MultipartFormat([]);
    eq(MultipartParse(fmt.contentType, fmt.body).length, 0, "empty multipart roundtrips");
});
__kitS.end();

throwsMsg(() => MultipartFormat([{ name: "a\nb", value: "x" }]),
    "no control characters", "CR/LF in a field name refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", value: "x", body: enc.encode("y") }]),
    "value OR body", "value+body together refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a" }]),
    "needs value or body", "part with neither value nor body refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", value: "x", filename: "f\rg" }]),
    "filename must not contain control characters", "CR/LF in a filename refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", value: "x", contentType: "t\np" }]),
    "contentType must not contain control characters", "CR/LF in contentType refused on encode");

throwsMsg(() => MultipartFormat([{ value: "x" }]),
    "needs a string name", "part without a name refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", value: 42 }]),
    "value must be a string", "non-string value refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", body: 42 }]),
    "must be a string or a byte view", "non-string/non-view body refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", value: "x", filename: 42 }]),
    "filename must be a string", "non-string filename refused on encode");

throwsMsg(() => MultipartFormat([{ name: "a", value: "x", contentType: 42 }]),
    "contentType must be a string", "non-string contentType refused on encode");

throwsMsg(() => MultipartFormat([], "bad boundary with space"),
    "1-70 bcharsnospace", "invalid caller boundary refused on encode");

throwsMsg(() => MultipartFormat([], 42),
    "boundary must be a string", "non-string boundary refused on encode");

throws(() => MultipartFormat("nope"),
    "parts must be an array", "non-array parts refused on encode");

if (fails) {
    print("test_multipart: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_multipart failed");
}
print("test_multipart: " + n + " assertions, 0 failures");
