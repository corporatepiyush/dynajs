// covers: dyna:http.RangeParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_range_parse_httpmsg");
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

__kitS.section("a simple range", () => {
eq(JSON.stringify(RangeParse("bytes=0-499", 1000)), '[{"start":0,"end":499}]',
   "a simple range");
});

__kitS.section("an open-ended range runs to the last byte", () => {
eq(JSON.stringify(RangeParse("bytes=500-", 1000)), '[{"start":500,"end":999}]',
   "an open-ended range runs to the last byte");
});

__kitS.section("a suffix range counts from the end", () => {
eq(JSON.stringify(RangeParse("bytes=-500", 1000)), '[{"start":500,"end":999}]',
   "a suffix range counts from the end");
});

__kitS.section("a one-byte range -- ranges are INCLUSIVE", () => {
eq(JSON.stringify(RangeParse("bytes=0-0", 1000)), '[{"start":0,"end":0}]',
   "a one-byte range -- ranges are INCLUSIVE");
});

__kitS.section("multiple ranges", () => {
eq(RangeParse("bytes=0-499,600-699", 1000).length, 2, "multiple ranges");
});

__kitS.section("an over-long end is clamped", () => {
eq(JSON.stringify(RangeParse("bytes=900-2000", 1000)), '[{"start":900,"end":999}]',
   "an over-long end is clamped");
});

__kitS.section("a suffix longer than the resource is the whole resource", () => {
eq(JSON.stringify(RangeParse("bytes=-2000", 1000)), '[{"start":0,"end":999}]',
   "a suffix longer than the resource is the whole resource");
});

__kitS.section("start past the end", () => {
eq(RangeParse("bytes=1000-2000", 1000), "unsatisfiable", "start past the end");
});

__kitS.section("a zero-length suffix", () => {
eq(RangeParse("bytes=-0", 1000), "unsatisfiable", "a zero-length suffix");
});

__kitS.section("any range on an empty resource", () => {
eq(RangeParse("bytes=0-499", 0), "unsatisfiable", "any range on an empty resource");
});

__kitS.section("a non-bytes unit is ignored -> null", () => {
eq(RangeParse("items=0-499", 1000), null, "a non-bytes unit is ignored -> null");
});

__kitS.section("junk -> null", () => {
eq(RangeParse("nonsense", 1000), null, "junk -> null");
});

__kitS.section("non-numeric bounds select nothing", () => {
eq(RangeParse("bytes=abc-def", 1000), "unsatisfiable", "non-numeric bounds select nothing");
});

__kitS.section("a reversed range is unsatisfiable", () => {
eq(RangeParse("bytes=499-0", 1000), "unsatisfiable", "a reversed range is unsatisfiable");
});

__kitS.section("a negative size is refused", () => {
throws(() => RangeParse("bytes=0-1", -1), "a negative size is refused");
});
__kitS.end();

if (fails) {
    print("test_httpmsg: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_httpmsg failed");
}
print("test_httpmsg: " + n + " assertions, 0 failures");
