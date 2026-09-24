// flags: --std
/* test_httpmsg_churn_leak.js -- 10k message builds/parses across the whole
 * dyna:net http message surface (ContentType, Negotiate, Range, Cookie, ETag,
 * Multipart), success and refusal paths mixed, then exit.
 *
 * This is the teardown-leak battery for the message path: the property-table
 * allocation in ContentTypeFormat used to be released only on the success
 * path, so every CTL refusal ("parameter names and values must not contain
 * control characters") leaked it at teardown (LSan: "Direct leak ... in
 * dyn_hm_ctype_format"). Under CONFIG_ASAN=y + detect_leaks=1 this suite must
 * end LSan-FLAT; with the fix reverted it goes LSan RED (16 bytes x the
 * refusal rows). The refusal-shaped rows also churn the request-hardening
 * helpers that share this file (CTL refusals, boundary bombs, header bombs),
 * which is the file's teardown-leak half.
 *
 * The assertions are exact-oracle style: every function's happy output AND
 * its full refusal matrix are pinned at the start, then re-checked at
 * intervals DURING the churn and once at the end -- a leaked, corrupted or
 * half-registered value cannot hide behind "the loop didn't crash".
 *
 * Run: dynajs --std (CONFIG_NATIVE_MODULES=y) tests/test_httpmsg_churn_leak.js
 */

import {
    ContentTypeParse, ContentTypeFormat, Negotiate, NegotiateToken,
    RangeParse, CookieParse, CookieSerialize, ETagMatch,
    MultipartParse, MultipartFormat,
} from "dyna:net";
import * as std from "std";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error(msg + " (got " + JSON.stringify(got) +
                        ", want " + JSON.stringify(want) + ")");
}
function throwsOk(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    n++;
    if (caught === null) throw new Error(msg + " (no throw)");
    return caught;
}
function gcPressure() {
    let junk = [];
    for (let i = 0; i < 16; i++)
        junk.push({ a: "z".repeat(24 + (i % 6) * 32), b: [i, i + 1, "q"] });
    junk = null;
    if (std && typeof std.gc === "function") std.gc();
}
function refuse(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    n++;
    if (caught === null) throw new Error(msg + " (a refusal was accepted)");
}

/* ---------------- one exact pass: happy + refusal matrix ---------------- */
function exactPass(round) {
    const tag = " (round " + round + ")";

    /* ContentTypeParse: the RFC 9110 shape and every way to be null */
    const p = ContentTypeParse("text/plain; charset=utf-8; x=\"a b\"");
    eq(p.type, "text", "ctype type" + tag);
    eq(p.subtype, "plain", "ctype subtype" + tag);
    eq(p.parameters["charset"], "utf-8", "ctype charset" + tag);
    eq(p.parameters["x"], "a b", "ctype quoted value loses its quotes" + tag);
    eq(ContentTypeParse("TEXT/Plain").type, "text",
        "type is case-insensitive (lowercased)" + tag);
    eq(ContentTypeParse("text/plain JUNK"), null, "bare junk is not a parameter" + tag);
    eq(ContentTypeParse("text/"), null, "empty subtype" + tag);
    eq(ContentTypeParse("/plain"), null, "empty type" + tag);
    eq(ContentTypeParse(""), null, "empty header" + tag);
    eq(ContentTypeParse("text/plain; x=\"\x01\""), null,
        "CTL in a quoted value is refused (null)" + tag);
    eq(ContentTypeParse("text/plain;; y=1").parameters["y"], "1",
        "empty elements are skipped" + tag);
    throwsOk(() => ContentTypeParse(42), "ContentTypeParse refuses non-strings" + tag);

    /* ContentTypeFormat: round-trips, and refuses CTL anywhere */
    eq(ContentTypeFormat({ type: "text", subtype: "plain",
                           parameters: { charset: "utf-8" } }),
        "text/plain; charset=utf-8", "ctype format" + tag);
    eq(ContentTypeFormat({ type: "text", subtype: "plain",
                           parameters: { x: "a b" } }),
        'text/plain; x="a b"', "ctype format quotes a spaced value" + tag);
    throwsOk(() => ContentTypeFormat({ type: "te\rxt", subtype: "p" }),
        "CRLF in type refused" + tag);
    throwsOk(() => ContentTypeFormat({ type: "t", subtype: "p\nq" }),
        "LF in subtype refused" + tag);
    /* THE regression row: a CTL in a parameter value leaves through the
       shared exit; the property table must be released there too */
    throwsOk(() => ContentTypeFormat({ type: "text", subtype: "plain",
                                       parameters: { a: "x", b: "\r\nX" } }),
        "CRLF in a parameter value refused" + tag);
    throwsOk(() => ContentTypeFormat({ type: "text", subtype: "plain",
                                       parameters: { "b\ran": "x" } }),
        "CR in a parameter name refused" + tag);
    throwsOk(() => ContentTypeFormat({ type: "", subtype: "p" }),
        "empty type refused" + tag);
    throwsOk(() => ContentTypeFormat({ type: "t" }), "missing subtype refused" + tag);
    throwsOk(() => ContentTypeFormat("text/plain"), "a string obj refused" + tag);

    /* Negotiate / NegotiateToken */
    eq(Negotiate("text/html;q=0.8,application/json", ["application/json", "text/html"]),
        "application/json", "negotiate picks the higher q" + tag);
    eq(Negotiate("", ["b", "a"]), "b", "an empty header accepts the first" + tag);
    eq(Negotiate("no/match", ["b", "a"]), null, "no match is null" + tag);
    eq(NegotiateToken("a, b;q=0", ["a", "b"]), "a", "token negotiation" + tag);
    eq(NegotiateToken("", ["b", "a"]), "b", "empty token header accepts" + tag);

    /* RangeParse: inclusive, suffix, unsatisfiable */
    const r = RangeParse("bytes=0-99,-5", 1000);
    eq(r.length, 2, "two ranges parsed" + tag);
    eq(r[0].start + "-" + r[0].end, "0-99", "first range" + tag);
    eq(r[1].start + "-" + r[1].end, "995-999", "suffix range" + tag);
    eq(RangeParse("bytes=2000-3000", 1000), "unsatisfiable",
        "an out-of-range selection is 416-shaped" + tag);
    eq(RangeParse("items=0-1", 10), null, "a non-bytes unit is ignored" + tag);
    throwsOk(() => RangeParse("bytes=0-1", -1), "negative size refused" + tag);

    /* CookieParse / CookieSerialize */
    const ck = CookieParse('a=1; b="two"; __proto__=x');
    eq(ck["a"], "1", "cookie a" + tag);
    eq(ck["b"], "two", "quoted cookie keeps quotes off" + tag);
    eq(ck["__proto__"], "x", "__proto__ is data, not the prototype" + tag);
    eq(Object.getPrototypeOf(ck), Object.prototype, "cookie prototype untouched" + tag);
    eq(CookieSerialize("n", "v", { httpOnly: true, maxAge: 5 }),
        "n=v; Max-Age=5; HttpOnly", "cookie serialize" + tag);
    throwsOk(() => CookieSerialize("n;x", "v"), "cookie name token check" + tag);
    throwsOk(() => CookieSerialize("n", "v;x"), "cookie value delimiter check" + tag);
    throwsOk(() => CookieSerialize("n", "v", { sameSite: "Nope" }),
        "sameSite enum check" + tag);
    throwsOk(() => CookieSerialize("n", "v", { dontexist: 1 }),
        "strict cookie options" + tag);

    /* ETagMatch: weak comparison per If-None-Match */
    eq(ETagMatch('*', '"abc"'), true, "star matches" + tag);
    eq(ETagMatch('W/"abc"', '"abc"'), true, "weak comparison" + tag);
    eq(ETagMatch('"abc"', '"abcd"'), false, "different etags" + tag);

    /* MultipartFormat/Parse round trip */
    const m = MultipartFormat([
        { name: "field", value: "hello" },
        { name: "file", filename: "a.txt", contentType: "text/plain",
          body: new TextEncoder().encode("BODY") },
    ]);
    assert(/multipart\/form-data; boundary=/.test(m.contentType),
        "multipart content type" + tag);
    const back = MultipartParse(m.contentType, m.body);
    eq(back.length, 2, "two parts" + tag);
    eq(back[0].name, "field", "part name" + tag);
    eq(new TextDecoder().decode(back[0].body), "hello", "part value body" + tag);
    eq(back[1].filename, "a.txt", "part filename" + tag);
    eq(new TextDecoder().decode(back[1].body), "BODY", "part body bytes" + tag);
    eq(back[1].contentType, "text/plain", "part contentType" + tag);
    /* and as a string body */
    const backS = MultipartParse(m.contentType,
        new TextDecoder().decode(m.body));
    eq(backS.length, 2, "string body parses identically" + tag);
    eq(new TextDecoder().decode(backS[0].body), "hello",
        "string body: same bytes" + tag);

    /* multipart refusals: the request-hardening shapes */
    throwsOk(() => MultipartParse("text/plain", m.body),
        "non-multipart content type refused" + tag);
    throwsOk(() => MultipartParse("multipart/form-data", m.body),
        "missing boundary refused" + tag);
    throwsOk(() => MultipartParse("multipart/form-data; boundary=a b", "--a b--"),
        "boundary with space refused" + tag);
    throwsOk(() => MultipartParse("multipart/form-data; boundary=XYZ",
        "--XYZ\r\nBAD-HDR\r\n\r\nv\r\n--XYZ--\r\n"),
        "malformed part header refused" + tag);
    throwsOk(() => MultipartParse("multipart/form-data; boundary=XYZ",
        "--XYZ\r\nX: 1\r\nNO-BLANK-LINE"),
        "unterminated header block refused" + tag);
    throwsOk(() => MultipartParse("multipart/form-data; boundary=XYZ", "nope"),
        "no opening boundary refused" + tag);
    throwsOk(() => MultipartFormat([{ name: "\x01", value: "x" }]),
        "CTL in a part name refused" + tag);
    throwsOk(() => MultipartFormat([{ name: "a", value: "x", body: "y" }]),
        "value XOR body refused" + tag);
    throwsOk(() => MultipartFormat([{ name: "a" }]),
        "neither value nor body refused" + tag);
    throwsOk(() => MultipartFormat([{ name: "a", value: "x", filename: "f\r\n" }]),
        "CTL in filename refused" + tag);
    throwsOk(() => MultipartFormat([{ name: "a", value: "x", contentType: "t\n" }]),
        "CTL in contentType refused" + tag);
    throwsOk(() => MultipartFormat([], "bad boundary"),
        "caller boundary rule enforced" + tag);
}

/* ---- pinned at round 0, mid-churn and at the end ----------------------- */
exactPass(0);

/* ---- the churn: 10k rounds, builds + parses + refusals, oracle between -- */
const enc = new TextEncoder();
const fixedBody = enc.encode(
    "--BND\r\nContent-Disposition: form-data; name=\"f\"; filename=\"x.bin\"\r\n" +
    "Content-Type: application/octet-stream\r\n\r\nPAYLOAD\r\n--BND--\r\n");

for (let half = 0; half < 2; half++) {
  const fmt = MultipartFormat([{ name: "f", value: "v" }, { name: "g", value: "w" }]);
  assert(fmt.body.length > 0, "fixture formats (half " + half + ")");
  for (let i = 0; i < 5000; i++) {
    /* builds */
    ContentTypeFormat({ type: "text", subtype: "plain",
                        parameters: { i: String(i), pad: "p".repeat(i % 17) } });
    refuse(() => ContentTypeFormat({ type: "text", subtype: "plain",
                        parameters: { a: "\r\nX", keep: String(i) } }),
        "churn: CRLF parameter refusal");   /* THE leak-path refusal */
    refuse(() => ContentTypeFormat({ type: "t\x01", subtype: "p" }),
        "churn: CTL type refusal");
    CookieSerialize("k" + (i % 5), "v".repeat(1 + (i % 7)),
        { maxAge: i % 100, httpOnly: (i & 1) === 0, secure: (i & 2) === 0,
          sameSite: ["Strict", "Lax", "None"][i % 3] });
    refuse(() => CookieSerialize("bad;name", "v"), "churn: cookie name refusal");
    const m = MultipartFormat([{ name: "n" + (i % 3), value: "x".repeat(1 + (i % 40)) },
                               { name: "f", filename: "d" + i + ".dat",
                                 contentType: "text/plain", body: enc.encode("B" + i) }]);
    refuse(() => MultipartFormat([{ name: "\r\n", value: "x" }]),
        "churn: multipart CTL name refusal");
    /* parses */
    ContentTypeParse("text/plain; charset=utf-8; n=" + (i % 9));
    eq(ContentTypeParse("text/plain; x=\"\x7f\""), null, "churn: CTL value parse");
    MultipartParse(m.contentType, m.body);
    MultipartParse("multipart/form-data; boundary=BND", fixedBody);
    refuse(() => MultipartParse("multipart/form-data; boundary=BND",
        "--BND\r\nD: 1\r\n\r\nx\r\n--BND\r\n\r\n"),
        "churn: no-disposition refusal");   /* refusal: no disposition */
    Negotiate("text/html;q=0.8,application/json;q=" + ((i % 10) / 10),
        ["application/json", "text/html", "x/y"]);
    NegotiateToken("t1, t2;q=0.5", ["t1", "t2", "t3"]);
    RangeParse("bytes=0-" + (i % 100) + ",-" + (i % 7), 1000);
    CookieParse('a=' + i + '; b="q w"; __proto__=x; c');
    ETagMatch('W/"e' + (i % 4) + '"', '"e' + (i % 4) + '"');
    if (i % 1000 === 0) gcPressure();
  }
  /* mid-churn oracle between the halves */
  exactPass(5000 + half);
}

/* ---- end-state oracle -------------------------------------------------- */
gcPressure();
exactPass("end");

/* The churn must not have registered or corrupted anything observable: the
   exact pass above is the oracle, and the fixed multipart body still parses
   byte-exact after 10k rounds. */
{
    const back = MultipartParse("multipart/form-data; boundary=BND", fixedBody);
    eq(back.length, 1, "fixed body: one part after the churn");
    eq(back[0].filename, "x.bin", "fixed body: filename intact");
    eq(String.fromCharCode(...back[0].body), "PAYLOAD", "fixed body: payload intact");
}

print("test_httpmsg_churn_leak: all tests passed (" + n + " assertions)");
