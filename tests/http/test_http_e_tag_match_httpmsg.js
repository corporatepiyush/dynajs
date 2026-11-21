// covers: dyna:http.ETagMatch
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_e_tag_match_httpmsg");
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

__kitS.section("an exact etag matches", () => {
assert(ETagMatch('"abc"', '"abc"'), "an exact etag matches");
});

__kitS.section("a different etag does not match", () => {
assert(!ETagMatch('"abc"', '"xyz"'), "a different etag does not match");
});

__kitS.section("* matches anything", () => {
assert(ETagMatch("*", '"anything"'), "* matches anything");
});

__kitS.section("\"a\", \"b\", \"c\"", () => {
assert(ETagMatch('"a", "b", "c"', '"b"'), "a list matches any member");
});

__kitS.section("\"a\", \"b\"", () => {
assert(!ETagMatch('"a", "b"', '"c"'), "a list that does not contain it");
});

__kitS.section("a weak header matches a strong etag", () => {
assert(ETagMatch('W/"abc"', '"abc"'), "a weak header matches a strong etag");
});

__kitS.section("a strong header matches a weak etag", () => {
assert(ETagMatch('"abc"', 'W/"abc"'), "a strong header matches a weak etag");
});

__kitS.section("weak matches weak", () => {
assert(ETagMatch('W/"abc"', 'W/"abc"'), "weak matches weak");
});

__kitS.section("an empty header matches nothing", () => {
assert(!ETagMatch("", '"abc"'), "an empty header matches nothing");
});

__kitS.section("\"a\" , \"b\"", () => {
assert(ETagMatch('  "a" ,  "b"  ', '"b"'), "spacing in the list is tolerated");
});

__kitS.section("ETagMatch refuses a non-string etag", () => {
throws(() => ETagMatch('"a"', 42), "ETagMatch refuses a non-string etag");
});

__kitS.section("an empty etag matches nothing", () => {
assert(ETagMatch(",", "") === false, "an empty etag matches nothing");
});

__kitS.section("an empty etag argument matches nothing", () => {
assert(ETagMatch('"a"', "") === false, "an empty etag argument matches nothing");
});
__kitS.end();

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
