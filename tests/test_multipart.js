/* test_multipart.js -- RFC 7578 multipart/form-data parse + encode in dyna:net.
 *
 * The parser sits on the untrusted frontier (a server body), so every
 * structural fault is a REFUSAL with its own guard, and the test asserts the
 * guard's message -- a parser that accepts anything fails this suite. Vectors
 * from outside the engine: a byte-exact curl -F capture (V1), the RFC 7578
 * 4.5/4.6 example (V2), and the canonical Chromium/WebKit shape (V3).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_multipart.js
 */
import { MultipartParse, MultipartFormat } from "dyna:net";

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
/* Each refusal names its guard: the thrown message must contain `substr`. */
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

/* ---------------------------------------------- V1: real curl capture, byte
 * exact, recorded with xxd from `curl -F "user=Xavier" -F "file=@hello.txt;
 * type=text/plain;filename=hello.txt"` (curl 8.21.0). 319-byte body. */
{
    const BND = "------------------------3TnyLrkNEMgiDrVkLtyeai";
    const ct = "multipart/form-data; boundary=" + BND;
    const body = "--" + BND + CR +
        'Content-Disposition: form-data; name="user"' + CR + CR +
        "Xavier" + CR +
        "--" + BND + CR +
        'Content-Disposition: form-data; name="file"; filename="hello.txt"' + CR +
        "Content-Type: text/plain" + CR + CR +
        "hello multipart" + CR +
        "--" + BND + "--" + CR;
    const p = MultipartParse(ct, body);
    eq(p.length, 2, "curl capture: two parts");
    eq(p[0].name, "user", "curl capture: field name");
    eq(b2s(p[0].body), "Xavier", "curl capture: field value");
    eq(p[1].name, "file", "curl capture: file part name");
    eq(p[1].filename, "hello.txt", "curl capture: filename param");
    eq(p[1].contentType, "text/plain", "curl capture: part content-type");
    eq(b2s(p[1].body), "hello multipart", "curl capture: file body");
}

/* V2: RFC 7578 section 4.5/4.6 example (boundary AaB03x, lowercased header
 * names, a content-transfer-encoding header that MUST be ignored per 4.8). */
{
    const BND = "AaB03x";
    const body = "--" + BND + CR +
        'content-disposition: form-data; name="field1"' + CR +
        "content-type: text/plain;charset=UTF-8" + CR +
        "content-transfer-encoding: quoted-printable" + CR + CR +
        "Joe owes =E2=82=AC100." + CR +
        "--" + BND + CR +
        'content-disposition: form-data; name="_charset_"' + CR + CR +
        "iso-8859-1" + CR +
        "--" + BND + "--" + CR;
    const p = MultipartParse("multipart/form-data; boundary=" + BND, body);
    eq(p.length, 2, "RFC 7578 example: two parts");
    eq(p[0].name, "field1", "RFC 7578 example: lowercase header names accepted");
    eq(p[0].contentType, "text/plain;charset=UTF-8", "RFC 7578 example: part content-type");
    eq(b2s(p[0].body), "Joe owes =E2=82=AC100.", "RFC 7578 example: field1 body");
    eq(p[1].name, "_charset_", "RFC 7578 example: second part name");
}

/* V3: canonical Chromium/WebKit form shape. */
{
    const BND = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    const body = "--" + BND + CR +
        'Content-Disposition: form-data; name="field1"' + CR + CR +
        "value1" + CR +
        "--" + BND + CR +
        'Content-Disposition: form-data; name="file"; filename="a.txt"' + CR +
        "Content-Type: text/plain" + CR + CR +
        "hello" + CR +
        "--" + BND + "--" + CR;
    const p = MultipartParse("multipart/form-data; boundary=" + BND, body);
    eq(p.length, 2, "webkit shape: two parts");
    eq(p[1].filename, "a.txt", "webkit shape: filename");
}

/* body accepted as string, Uint8Array, ArrayBuffer; a plain value refused */
{
    const BND = "b1";
    const ct = "multipart/form-data; boundary=" + BND;
    const body = "--" + BND + CR + 'Content-Disposition: form-data; name="a"' + CR + CR + "x" + CR + "--" + BND + "--" + CR;
    eq(MultipartParse(ct, enc.encode(body)).length, 1, "body as Uint8Array");
    eq(MultipartParse(ct, enc.encode(body).buffer).length, 1, "body as ArrayBuffer");
    throws(() => MultipartParse(ct, 42), "body number refused");
    throws(() => MultipartParse(ct, ["a"]), "body Array refused (no coercion)");
    throws(() => MultipartParse("not a string", body), "contentType non-string refused");
}

/* ---- adversarial: the plan's list, each refusal naming its own guard ------ */
{
    const ct = (b) => "multipart/form-data; boundary=" + b;

    /* boundary in content: mid-line "--bnd" is NOT a delimiter (CRLF-prefix
       rule); a content LINE that is the delimiter splits (format property) */
    {
        const body = "--b1" + CR + 'Content-Disposition: form-data; name="note"' + CR + CR +
            "this --b1 stays content" + CR + "--b1--" + CR;
        const p = MultipartParse(ct("b1"), body);
        eq(p.length, 1, "boundary mid-line is content (CRLF-prefix rule)");
        eq(b2s(p[0].body), "this --b1 stays content", "mid-line boundary kept in body");
    }
    {
        const body = "--b1" + CR + 'Content-Disposition: form-data; name="a"' + CR + CR +
            "one" + CR + "--b1" + CR + 'Content-Disposition: form-data; name="b"' + CR + CR +
            "two" + CR + "--b1--" + CR;
        const p = MultipartParse(ct("b1"), body);
        eq(p.length, 2, "a line that is the delimiter does split");
    }
    /* longest-prefix attack: "--bndXX" with boundary "bnd" is not a delimiter */
    {
        const body = "--bnd" + CR + 'Content-Disposition: form-data; name="a"' + CR + CR +
            "before" + CR + "--bndXX" + CR + "after" + CR + "--bnd--" + CR;
        const p = MultipartParse(ct("bnd"), body);
        eq(p.length, 1, "longest-prefix: longer token is not a delimiter");
        eq(b2s(p[0].body), "before" + CR + "--bndXX" + CR + "after", "longest-prefix: content preserved");
    }
    /* empty parts */
    {
        const body = "--b1" + CR + 'Content-Disposition: form-data; name="e"' + CR + CR +
            "--b1" + CR + 'Content-Disposition: form-data; name="f"' + CR + CR + "v" + CR +
            "--b1--" + CR;
        const p = MultipartParse(ct("b1"), body);
        eq(p.length, 2, "empty part parses");
        eq(p[0].body.length, 0, "empty part has an empty body");
        eq(b2s(p[1].body), "v", "the part after the empty one parses");
    }
    /* quoted boundary in the content-type parameter */
    {
        const body = "--abc" + CR + 'Content-Disposition: form-data; name="a"' + CR + CR + "x" + CR + "--abc--" + CR;
        eq(MultipartParse('multipart/form-data; boundary="abc"', body).length, 1,
           'boundary="abc" (quoted) accepted');
        throwsMsg(() => MultipartParse('multipart/form-data; boundary="a b"', body),
            "boundary parameter is missing or invalid", "space is not a bcharsnospace boundary");
        throwsMsg(() => MultipartParse("multipart/form-data", body),
            "boundary parameter is missing or invalid", "missing boundary refused");
        throwsMsg(() => MultipartParse("multipart/form-data; boundary=", body),
            "boundary parameter is missing or invalid", "empty boundary refused");
        throwsMsg(() => MultipartParse("multipart/form-data; boundary=" + "x".repeat(71), body),
            "boundary parameter is missing or invalid", "71-char boundary refused");
    }
    /* missing final boundary -> refuse (guard M3) */
    throwsMsg(() => MultipartParse(ct("b1"), "--b1" + CR + 'Content-Disposition: form-data; name="a"' + CR + CR + "value"),
        "missing closing boundary", "missing final boundary refused");
    /* missing opening boundary -> refuse (M16) */
    throwsMsg(() => MultipartParse(ct("b1"), "no delimiter in this body at all"),
        "no opening boundary", "missing opening boundary refused");
    /* wrong media type -> refuse (M1) */
    throwsMsg(() => MultipartParse("text/plain; boundary=b1", "--b1--"),
        "must be multipart/form-data", "wrong media type refused");
    throwsMsg(() => MultipartParse("multipart/mixed; boundary=b1", "--b1--"),
        "must be multipart/form-data", "multipart/mixed refused");
    /* filename header injection: a second Content-Disposition -> refuse (M13) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name="safe"' + CR +
        'Content-Disposition: form-data; name="evil"' + CR + CR + "x" + CR + "--b1--" + CR),
        "more than one Content-Disposition", "duplicate Content-Disposition refused");
    /* a bare CR inside a header line -> refuse (M14) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name="a"; filename="evil' + "\r" + '.txt"' + CR + CR + "x" + CR + "--b1--" + CR),
        "bare CR or LF", "lone CR in a header line refused");
    /* a lone LF line ending -> refuse (M14) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name="a"' + "\n" + "X: 1" + CR + CR + "x" + CR + "--b1--" + CR),
        "bare CR or LF", "lone LF in the header block refused");
    /* header line over-length -> refuse (M7) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name="' + "n".repeat(5000) + '"' + CR + CR + "x" + CR + "--b1--" + CR),
        "exceeds 4096 bytes", "over-long header line refused");
    /* missing Content-Disposition -> refuse (M5) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + "X-Ignored: 1" + CR + CR + "x" + CR + "--b1--" + CR),
        "no Content-Disposition", "part without Content-Disposition refused");
    /* disposition type not form-data -> refuse (M18) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: attachment; name="a"' + CR + CR + "x" + CR + "--b1--" + CR),
        "type must be form-data", "attachment disposition refused");
    /* missing name parameter -> refuse (M6) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; filename="f"' + CR + CR + "x" + CR + "--b1--" + CR),
        "lacks a name parameter", "missing name refused");
    /* malformed disposition grammar -> refuse (M19) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name' + CR + CR + "x" + CR + "--b1--" + CR),
        "malformed Content-Disposition", "bare parameter refused");
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name="unterminated' + CR + CR + "x" + CR + "--b1--" + CR),
        "malformed Content-Disposition", "unterminated quoted-string refused");
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name=a b' + CR + CR + "x" + CR + "--b1--" + CR),
        "malformed Content-Disposition", "space in an unquoted value refused");
    /* duplicate name parameter -> refuse (M20) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + 'Content-Disposition: form-data; name="a"; name="b"' + CR + CR + "x" + CR + "--b1--" + CR),
        "duplicate name parameter", "duplicate name refused");
    /* malformed header line -> refuse (M17) */
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + "NotAHeader" + CR + CR + "x" + CR + "--b1--" + CR),
        "malformed part header line", "header without a colon refused");
    throwsMsg(() => MultipartParse(ct("b1"),
        "--b1" + CR + "X Y: 1" + CR + 'Content-Disposition: form-data; name="a"' + CR + CR + "x" + CR + "--b1--" + CR),
        "malformed part header name", "non-token header name refused");
    /* nested quoting and quoted-pair escaping */
    {
        const body = "--b1" + CR + 'Content-Disposition: form-data; name="a\\"b"; filename="f\\"g.txt"' + CR + CR + "x" + CR + "--b1--" + CR;
        const p = MultipartParse(ct("b1"), body);
        eq(p[0].name, 'a"b', "quoted-pair in name unescaped");
        eq(p[0].filename, 'f"g.txt', "quoted-pair in filename unescaped");
    }
    /* oversized part -> refuse (M11), cap = 16 MiB */
    {
        const part0 = "--b1" + CR + 'Content-Disposition: form-data; name="big"' + CR + CR;
        const part2 = CR + "--b1--" + CR;
        const big = new Uint8Array((16 << 20) + 16);
        big.fill(0x61);
        const e0 = enc.encode(part0), e2 = enc.encode(part2);
        const full = new Uint8Array(e0.length + big.length + e2.length);
        full.set(e0, 0);
        full.set(big, e0.length);
        full.set(e2, e0.length + big.length);
        throwsMsg(() => MultipartParse(ct("b1"), full),
            "exceeds 16 MiB", "oversized part refused (16 MiB cap)");
    }
    /* part count cap -> refuse (M10), cap = 1024 */
    {
        let parts_s = [];
        for (let i = 0; i < 1025; i++)
            parts_s.push("--b1" + CR + 'Content-Disposition: form-data; name="f' + i + '"' + CR + CR + "v" + CR);
        parts_s.push("--b1--" + CR);
        throwsMsg(() => MultipartParse(ct("b1"), parts_s.join("")),
            "more than 1024 parts", "part count cap refused (1024)");
    }
    /* boundary-like text inside a VALUE with a quoted boundary parameter */
    {
        const body = "--abc" + CR + 'Content-Disposition: form-data; name="a"' + CR + CR +
            "x --abc y" + CR + "--abc--" + CR;
        eq(b2s(MultipartParse('multipart/form-data; boundary="abc"', body)[0].body),
           "x --abc y", "quoted boundary: mid-line text stays content");
    }
}

/* ---- encode ------------------------------------------------------------ */

/* roundtrip: fields + files */
{
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
}
/* explicit boundary + escaping + CTL-free payload */
{
    const fmt = MultipartFormat([
        { name: "a b", value: "line1" + CR + "line2 --abc123 world", filename: 'f"g\\h.txt', contentType: "text/plain" },
    ], "abc123");
    eq(fmt.contentType, "multipart/form-data; boundary=abc123", "explicit boundary in contentType");
    const p = MultipartParse(fmt.contentType, fmt.body);
    eq(p[0].name, "a b", "space in name survives the escape roundtrip");
    eq(p[0].filename, 'f"g\\h.txt', "quotes/backslash in filename survive the escape roundtrip");
    eq(b2s(p[0].body), "line1" + CR + "line2 --abc123 world", "payload CRLF and mid-line boundary survive");
}
/* empty parts list roundtrips */
{
    const fmt = MultipartFormat([]);
    eq(MultipartParse(fmt.contentType, fmt.body).length, 0, "empty multipart roundtrips");
}
/* encode-side refusals, each with its own guard */
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

/* 08-17 security regression: a raw NUL inside a quoted-string parameter
   value must refuse (CTL outside a quoted-pair) -- the disposition carries
   filenames, and a NUL there truncates at the filesystem layer. */
{
    const ct = "multipart/form-data; boundary=BND";
    const body = "--BND\r\nContent-Disposition: form-data; name=\"f\";"
        + " filename=\"a\u0000b.txt\"\r\n\r\nx\r\n--BND--\r\n";
    throws(() => MultipartParse(ct, new TextEncoder().encode(body)),
        "NUL in quoted filename refused");
}

if (fails) {
    print("test_multipart: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_multipart failed");
}
print("test_multipart: " + n + " assertions, 0 failures");