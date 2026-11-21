// covers: dyna:url.URL
// F13: new URL("foo:/..").href was "foo:"; WHATWG path state: a ".." segment at the end of the input shortens the
// path and then appends the empty string (unless c is "/" or a special-scheme "\"), so the path is [""] and the href
// "foo:/". The double-dot branch only appended for special schemes or a non-empty path; it now always does at the
// end. F14: the host parser accepted an "xn--" label whose Punycode decodes to text that is not in NFC, which
// domainToASCII rejects; UTS #46 section 4.1 (validity criteria, step 1: the label must be in NFC) applies to the
// host parser too (WHATWG domain to ASCII runs UTS46 ToASCII), so such a host is now a failure in both.
// Oracles: the WHATWG URL path-state text and WPT urltestdata vectors (non-spec:/.. family), UTS #46 4.1.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url_dot_segments_host_nfc");
import { URL, domainToASCII, punycodeEncode } from "dyna:url";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const href = (s, b) => { try { return new URL(s, b).href; } catch (e) { return "THROW"; } };

__kitS.section("a trailing .. leaves an empty last segment (non-special schemes)", () => {
    eq(href("foo:/.."), "foo:/", "ISSUES repro");
    eq(href("foo:/a/.."), "foo:/", "WPT non-spec:/a/..");
    eq(href("foo:/a/b/.."), "foo:/a/", "two segments");
    eq(href("foo:/../.."), "foo:/", "above the root");
    eq(href("foo:/%2e%2E"), "foo:/", "percent-encoded double dot");
    eq(href("foo://h/.."), "foo://h/", "with a host");
    eq(href("foo:/.//"), "foo:/.//", "WPT non-spec:/.// keeps the /. guard");
    eq(href("foo:/..//"), "foo:/.//", "WPT non-spec:/..//");
    eq(href("http://h/.."), "http://h/", "special scheme (control)");
    eq(href("foo:/a/."), "foo:/a/", "single dot (control)");
    eq(href("..", "foo:/a/b"), "foo:/", "relative .. against a non-special base");
});

__kitS.section("an xn-- label that decodes to non-NFC text is not a valid host", () => {
    const nonNfc = "xn--" + punycodeEncode("áb");
    let d = "ok";
    try { domainToASCII(nonNfc + ".com"); } catch (e) { d = "THROW"; }
    eq(d, "THROW", "domainToASCII rejects it (control)");
    eq(href("http://" + nonNfc + ".com/"), "THROW", "the URL host parser rejects it too");
    eq(href("http://" + nonNfc.toUpperCase() + ".com/"), "THROW", "upper-case XN-- prefix");
    eq(href("http://é." + nonNfc + "/"), "THROW", "next to a non-ASCII label");
    const nfc = "xn--" + punycodeEncode("áb");
    eq(href("http://" + nfc + ".com/"), "http://" + nfc + ".com/", "the NFC spelling is accepted (control)");
    eq(href("http://áb.com/"), "http://" + nfc + ".com/", "and is what a Unicode host maps to");
    eq(href("https://xn--n3h/"), "https://xn--n3h/", "WPT xn--n3h (control)");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_url_url_dot_segments_host_nfc: " + n + " checks passed");
__kitS.end();
