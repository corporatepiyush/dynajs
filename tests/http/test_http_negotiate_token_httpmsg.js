// covers: dyna:http.NegotiateToken
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_negotiate_token_httpmsg");
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

__kitS.section("en;q=0.0001, fr;q=0", () => {
eq(NegotiateToken("en;q=0.0001, fr;q=0", ["en", "fr"]), "en",
   "digits past the third keep a non-zero weight non-zero");
});

__kitS.section("exact language", () => {
eq(NegotiateToken("en", ["en", "fr"]), "en", "exact language");
});

__kitS.section("second language", () => {
eq(NegotiateToken("fr", ["en", "fr"]), "fr", "second language");
});

__kitS.section("language wildcard", () => {
eq(NegotiateToken("*", ["en", "fr"]), "en", "language wildcard");
});

__kitS.section("no acceptable language", () => {
eq(NegotiateToken("de", ["en", "fr"]), null, "no acceptable language");
});

__kitS.section("en matches en-GB by prefix", () => {
eq(NegotiateToken("en", ["en-GB"]), "en-GB", "en matches en-GB by prefix");
});

__kitS.section("but NOT `end` -- the dash is required", () => {
eq(NegotiateToken("en", ["end"]), null, "but NOT `end` -- the dash is required");
});

__kitS.section("token matching is case-insensitive", () => {
eq(NegotiateToken("EN", ["en"]), "en", "token matching is case-insensitive");
});

__kitS.section("gzip;q=0.5, br;q=1.0", () => {
eq(NegotiateToken("gzip;q=0.5, br;q=1.0", ["gzip", "br"]), "br", "encoding by q");
});
__kitS.end();

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
