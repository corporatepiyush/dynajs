// covers: dyna:http.Negotiate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_negotiate_httpmsg");
import { ContentTypeParse, ContentTypeFormat, Negotiate, NegotiateToken, RangeParse, CookieParse, CookieSerialize, ETagMatch } from "dyna:http";

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

const OFFER = ["application/json", "text/html"];

__kitS.section("exact match", () => {
eq(Negotiate("text/html", OFFER), "text/html", "exact match");
});

__kitS.section("exact match 2", () => {
eq(Negotiate("application/json", OFFER), "application/json", "exact match 2");
});

__kitS.section("type wildcard", () => {
eq(Negotiate("text/*", OFFER), "text/html", "type wildcard");
});

__kitS.section("full wildcard takes the first offer", () => {
eq(Negotiate("*/*", OFFER), "application/json", "full wildcard takes the first offer");
});

__kitS.section("no acceptable offer -> null", () => {
eq(Negotiate("image/png", OFFER), null, "no acceptable offer -> null");
});

__kitS.section("an empty header accepts anything", () => {
eq(Negotiate("", OFFER), "application/json", "an empty header accepts anything");
});

__kitS.section("text/html;q=0.1, application/json;q=0.9", () => {
eq(Negotiate("text/html;q=0.1, application/json;q=0.9", OFFER), "application/json",
   "higher q wins");
});

__kitS.section("text/html;q=0.9, application/json;q=0.1", () => {
eq(Negotiate("text/html;q=0.9, application/json;q=0.1", OFFER), "text/html",
   "higher q wins the other way");
});

__kitS.section("text/html;q=0, application/json", () => {
eq(Negotiate("text/html;q=0, application/json", OFFER), "application/json",
   "q=0 refuses that type");
});

__kitS.section("q=0 with no alternative -> null", () => {
eq(Negotiate("text/html;q=0", ["text/html"]), null, "q=0 with no alternative -> null");
});

__kitS.section("*/*, text/html", () => {
eq(Negotiate("*/*, text/html", OFFER), "text/html",
   "an exact range beats a wildcard at the same q");
});

__kitS.section("a comma inside quotes does not split the header", () => {
eq(Negotiate('text/html;x="a,b"', OFFER), "text/html",
   "a comma inside quotes does not split the header");
});

__kitS.section("text/html ; q = 0, application/json", () => {
eq(Negotiate("text/html ; q = 0, application/json", OFFER), "application/json",
   "q = 0 with BWS refuses that type, as Accept-Encoding does");
});

__kitS.section("a malformed q is ignored, not read as 0", () => {
eq(Negotiate("text/html;q=abc", ["text/html"]), "text/html",
   "a malformed q is ignored, not read as 0");
});

__kitS.section("application/json;q=0.5, text/html;q=2", () => {
eq(Negotiate("application/json;q=0.5, text/html;q=2", OFFER), "text/html",
   "an out-of-range q is ignored: weight 1");
});

__kitS.section("not an array", () => {
throws(() => Negotiate("x", "not an array"), "Negotiate requires an array");
});
__kitS.end();

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
