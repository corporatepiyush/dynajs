// covers: dyna:url.URL
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url");
import { URL, URLSearchParams, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";

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

__kitS.section("protocol keeps its colon", () => {
    const u = new URL("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
    eq(u.protocol, "https:", "protocol keeps its colon");
    eq(u.username, "user", "username");
    eq(u.password, "pw", "password");
    eq(u.hostname, "example.com", "hostname excludes the port");
    eq(u.port, "8443", "port");
    eq(u.host, "example.com:8443", "host includes the port");
    eq(u.pathname, "/a/b", "pathname");
    eq(u.search, "?x=1&y=2", "search keeps its question mark");
    eq(u.hash, "#frag", "hash keeps its octothorpe");
    eq(u.origin, "https://example.com:8443", "origin");
    eq(u.href, "https://user:pw@example.com:8443/a/b?x=1&y=2#frag", "href round-trips");
    eq(String(u), u.href, "toString is href");
    eq(JSON.stringify({ u }), '{"u":"' + u.href + '"}', "toJSON is href");
});

eq(new URL("http://a.com:80/").port, "", "http port 80 is dropped");

eq(new URL("https://a.com:443/").port, "", "https port 443 is dropped");

eq(new URL("http://a.com:8080/").port, "8080", "a non-default port is kept");

eq(new URL("http://a.com:80/").href, "http://a.com/", "the dropped port is gone from href");

eq(new URL("http://a.com/").origin, new URL("http://a.com:80/").origin,
   "the two spellings of one origin agree");

eq(new URL("HTTP://a.com/").protocol, "http:", "scheme is lowercased");

eq(new URL("hTtPs://a.com/").protocol, "https:", "mixed-case scheme");

__kitS.section("no query -> empty search", () => {
    const u = new URL("http://a.com/p");
    eq(u.search, "", "no query -> empty search");
    eq(u.hash, "", "no fragment -> empty hash");
    eq(u.username, "", "no userinfo -> empty username");
    eq(u.port, "", "no port -> empty port");
});

eq(new URL("http://a.com/?").search, "", "an empty query serialises as empty");

eq(new URL("http://a.com/#").hash, "", "an empty fragment serialises as empty");

eq(new URL("http://[::1]/x").hostname, "[::1]", "IPv6 hostname keeps brackets");

eq(new URL("http://[::1]:8080/x").port, "8080", "IPv6 with a port");

eq(new URL("http://[::1]:8080/x").host, "[::1]:8080", "IPv6 host");

eq(new URL("http://[2001:db8::1]/").hostname, "[2001:db8::1]", "full IPv6 literal");

eq(new URL("http:/example.com/").hostname, "example.com", "http:/ one slash still names its authority");

eq(new URL("http:/example.com/").pathname, "/", "http:/ path");

eq(new URL("http:example.com").hostname, "example.com", "http: with no slash still names its authority");

eq(new URL("http:example.com").href, new URL("http://example.com").href, "the spellings are one URL (an empty path stays empty)");

eq(new URL("https:\\\\evil.test/x").hostname, "evil.test", "backslashes join the slash run");

eq(new URL("ws:example.com/").href, "ws://example.com/", "ws: too");

eq(new URL("ftp:example.com/").href, "ftp://example.com/", "ftp: too");

eq(new URL("file:example.com").pathname, "/example.com", "file: no-slash is a fresh path over the empty host (the file state never goes opaque)");

throws(() => new URL("http:/"), "http:/ has an empty host: refused");

throws(() => new URL("http:"), "http: alone has an empty host: refused");

throws(() => new URL("http:?q"), "http:? has an empty host: refused");

__kitS.section("relative refs keep relative semantics", () => {
    const b = new URL("http://a/b/c/d;p?q");
    eq(new URL("g", b).href, "http://a/b/c/g", "relative refs keep relative semantics");
    eq(new URL("//g", b).href, "http://g/", "a network-path ref parses its authority and, like every special URL with an empty path, carries one empty segment (WHATWG, not RFC 3986)");
});

__kitS.section("mailto protocol", () => {
    const u = new URL("mailto:ada@example.com");
    eq(u.protocol, "mailto:", "mailto protocol");
    eq(u.pathname, "ada@example.com", "mailto path is opaque");
    eq(u.origin, "null", "a scheme with no authority has origin null");
});

eq(new URL("data:text/plain,hi").pathname, "text/plain,hi", "data: opaque path");

eq(new URL("file:///x").href, "file:///x", "file:///x parses (file: may have an empty host)");

eq(new URL("file:///x").origin, "null", "a file: URL has an opaque origin");

eq(new URL("file://host/x").origin, "null", "file: origin is null even with a host");

eq(new URL("http://x.test/").origin, "http://x.test", "http origin is untouched");

eq(new URL("https://a.com/p{x}").href, "https://a.com/p%7Bx%7D", "path braces are percent-encoded");

eq(new URL("https://a.com/p`x").href, "https://a.com/p%60x", "a path backtick is percent-encoded");

eq(new URL("https://a.com/p|q^r").href, "https://a.com/p|q%5Er", "path ^ is percent-encoded (U+005E is in the standard's path set), | stays literal");

eq(new URL("https://a.com/p%2fq%2fr").href, "https://a.com/p%2fq%2fr", "existing escapes keep their hex case verbatim ('%' is in no percent-encode set)");

eq(new URL("https://a.com/p?q{x}`y").href, "https://a.com/p?q{x}`y", "the query set keeps { } ` literal");

eq(new URL("https://a.com/p#f`x{").href, "https://a.com/p#f%60x{", "the fragment set encodes `, not {");

__kitS.section("a query may contain ? and /", () => {
    const u = new URL("http://a.com/p?q=a?b/c#f#g?h");
    eq(u.search, "?q=a?b/c", "a query may contain ? and /");
    eq(u.hash, "#f#g?h", "a fragment may contain # and ?");
    eq(u.pathname, "/p", "the path stops at the first ?");
});

const BASE = "http://a/b/c/d;p?q";

const NORMAL = [
    ["g:h", "g:h"], ["g", "http://a/b/c/g"], ["./g", "http://a/b/c/g"],
    ["g/", "http://a/b/c/g/"], ["/g", "http://a/g"], ["//g", "http://g/"],
    ["?y", "http://a/b/c/d;p?y"], ["g?y", "http://a/b/c/g?y"],
    ["#s", "http://a/b/c/d;p?q#s"], ["g#s", "http://a/b/c/g#s"],
    ["g?y#s", "http://a/b/c/g?y#s"], [";x", "http://a/b/c/;x"],
    ["g;x", "http://a/b/c/g;x"], ["g;x?y#s", "http://a/b/c/g;x?y#s"],
    ["", "http://a/b/c/d;p?q"], [".", "http://a/b/c/"], ["./", "http://a/b/c/"],
    ["..", "http://a/b/"], ["../", "http://a/b/"], ["../g", "http://a/b/g"],
    ["../..", "http://a/"], ["../../", "http://a/"], ["../../g", "http://a/g"],
];

let normalOk = 0;

for (const [ref, want] of NORMAL) {
    let got;
    try { got = new URL(ref, BASE).href; } catch (e) { got = "THREW: " + e.message; }
    if (got === want) normalOk++;
    else assert(false, "RFC 3986 5.4.1 <" + ref + "> -> " + JSON.stringify(got)
                       + " want " + JSON.stringify(want));
}

eq(normalOk, NORMAL.length, "all " + NORMAL.length + " RFC 3986 normal examples");

const ABNORMAL = [
    ["../../../g", "http://a/g"], ["../../../../g", "http://a/g"],
    ["/./g", "http://a/g"], ["/../g", "http://a/g"],
    ["g.", "http://a/b/c/g."], [".g", "http://a/b/c/.g"],
    ["g..", "http://a/b/c/g.."], ["..g", "http://a/b/c/..g"],
    ["./../g", "http://a/b/g"], ["./g/.", "http://a/b/c/g/"],
    ["g/./h", "http://a/b/c/g/h"], ["g/../h", "http://a/b/c/h"],
    ["g;x=1/./y", "http://a/b/c/g;x=1/y"], ["g;x=1/../y", "http://a/b/c/y"],
];

let abnormalOk = 0;

for (const [ref, want] of ABNORMAL) {
    let got;
    try { got = new URL(ref, BASE).href; } catch (e) { got = "THREW: " + e.message; }
    if (got === want) abnormalOk++;
    else assert(false, "RFC 3986 5.4.2 <" + ref + "> -> " + JSON.stringify(got)
                       + " want " + JSON.stringify(want));
}

eq(abnormalOk, ABNORMAL.length, "all " + ABNORMAL.length + " RFC 3986 abnormal examples");

assert("http://a/b/c/" + "../../../g" !== "http://a/g",
       "fault injection: concatenation really does differ from resolution");

throws(() => new URL("not a url"), "a bare string with no scheme and no base");

throws(() => new URL("/relative/only"), "a relative reference with no base");

throws(() => new URL(42), "a non-string input");

throws(() => new URL("http://a\x01b/"), "a non-strippable C0 byte in the authority is refused");

eq(new URL("http://a\r\nb/").hostname, "ab", "interior CR/LF is stripped input-wide");

eq(new URL("http://a\tb/").hostname, "ab", "interior TAB is stripped input-wide");

eq(new URL("http://a.com/x\ty\nz").pathname, "/xyz", "interior tab/LF never reach .pathname");

eq(new URL("http://a.com/p", "http://a\tb.com/").href, "http://a.com/p", "the BASE is stripped too");

throws(() => new URL("http://a.com", "also not a url"), "an invalid base");

throws(() => new URL("http://a.com:99999/"), "a port above 65535");

throws(() => new URL("http://a.com:abc/"), "a non-numeric port");

throws(() => new URL("http://[::1/"), "an unterminated IPv6 literal");

throws(() => new URL("http://a.com/" + "x".repeat(70000)), "an over-long URL");

eq(new URL("  http://a.com/  ").href, "http://a.com/", "surrounding space is stripped");

eq(new URL("\thttp://a.com/\n").href, "http://a.com/", "tabs and newlines are stripped");

__kitS.section("still has no setter", () => {
    const u = new URL("http://a.com/p?q#f");
    for (const k of ["href", "origin"])
        throws(() => { u[k] = "zz"; }, k + " still has no setter");
    const c = new URL("http://a.com:443/p");
    c.protocol = "https:x";
    c.username = "u @";
    c.password = "p:w";
    eq(c.href, "https://u%20%40:p%3Aw@a.com/p", "protocol/username/password setters (WHATWG; default port dropped)");
    c.protocol = "foo";
    eq(c.protocol, "https:", "a special scheme cannot become non-special");
    u.host = "b.com";
    eq(u.href, "http://b.com/p?q#f", "host is settable (UL-1)");
    u.hostname = "c.com";
    eq(u.href, "http://c.com/p?q#f", "hostname is settable (UL-1)");
    u.port = "8080";
    eq(u.href, "http://c.com:8080/p?q#f", "port is settable (UL-1)");
    u.pathname = "/z";
    eq(u.href, "http://c.com:8080/z?q#f", "pathname is settable (UL-1)");
    u.search = "s=1";
    eq(u.href, "http://c.com:8080/z?s=1#f", "search is settable (UL-1)");
    u.searchParams = "u=3";
    eq(u.href, "http://c.com:8080/z?u=3#f", "searchParams is replaceable (UL-1)");
    u.hash = "#g";
    eq(u.href, "http://c.com:8080/z?u=3#g", "hash is settable (UL-1)");
});

{
    const OVER = "https://a.test/" + "x".repeat(70000);
    const CASES = [
        ["https://a.test/p", undefined, true],
        ["https://a.test/p", "https://b.test/", true],
        ["/p?q", "https://b.test/", true],
        ["../z", "https://b.test/x/y", true],
        ["", "https://b.test/", true],
        ["", undefined, false],
        ["http://", undefined, false],
        [":", undefined, false],
        ["https://ex ample.com/", undefined, false],
        ["http://a.test/%zz", undefined, true],
        ["http://[::1", undefined, false],
        ["::::", undefined, false],
        ["/p", "not a url", false],
        ["https://a.test/p", "::::", false],
        [OVER, undefined, false],
        ["ht\ttp://x.test/", undefined, true],
        ["http://x.test/\n.", undefined, true],
    ];
    for (const [input, base, good] of CASES) {
        let ctorThrew = false, ctorHref = null;
        try {
            const u = base === undefined ? new URL(input) : new URL(input, base);
            ctorHref = u.href;
        } catch { ctorThrew = true; }
        assert(ctorThrew === !good, "ctor parity for " + JSON.stringify(input.slice(0, 30)));
        const p = base === undefined ? URL.parse(input) : URL.parse(input, base);
        const c = base === undefined ? URL.canParse(input) : URL.canParse(input, base);
        assert(good ? (p !== null && c === true) : (p === null && c === false),
           "parse/canParse parity for " + JSON.stringify(input.slice(0, 30)) + " base=" + base);
        if (good) assert(p.href === ctorHref, "parse href matches ctor for " + input.slice(0, 40));
        assert(URL.canParse(input, null) === (base === undefined ? c : URL.canParse(input, base)) || base !== undefined,
               "null base tolerated");
    }
    {
        class MyURL extends URL {}
        const viaSub = URL.parse("https://plain.test/");
        assert(!(viaSub instanceof MyURL), "URL.parse returns the plain class");
        assert(viaSub instanceof URL, "URL.parse result is a URL");
    }
    {
        let t1 = false, t2 = false;
        t1 = URL.parse(42) === null;
        t2 = URL.canParse({ toString: () => "https://x.test/" }) === true;
        assert(t1 && t2, "statics run non-string input through ToString like the ctor");
    }

    eq(URL.join("https://a.com/x/y", "../z"), "https://a.com/z", "join ../ past one segment");
    eq(URL.join("https://a.com/x/y", "/root"), "https://a.com/root", "join absolute path rel");
    eq(URL.join("https://a.com/x/y", "https://other.test/q"), "https://other.test/q", "join absolute rel wins");
    eq(URL.join("https://a.com/x/y?q=1#f", ""), "https://a.com/x/y?q=1", "join empty rel keeps path+query, clears fragment (RFC 3986 5.2.2)");
    eq(URL.join("https://a.com/x/y", "./p"), "https://a.com/x/p", "join ./p");
    eq(URL.join("https://a.com/x/y", "p?q=2"), "https://a.com/x/p?q=2", "join keeps rel query");
    eq(URL.join("https://a.com/x/y", "//other.test/p"), "https://other.test/p", "join scheme-relative rel");
    eq(URL.join("https://a.com/x/y", "../../../../../z"), "https://a.com/z", "join .. past root is dropped");
    throws(() => URL.join("::::", "/p"), "join throws on a bad base");
    throws(() => URL.join("https://a.com/", OVER), "join throws on an over-long rel");
    throws(() => URL.join("https://a.com/"), "join requires both arguments");
}

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");

__kitS.section("searchParams must be [SameObject]", () => {
    const u = new URL("https://example.com/?a=1");
    if (u.searchParams !== u.searchParams) throw new Error("searchParams must be [SameObject]");
    const sp = u.searchParams;
    for (let i = 0; i < 5000; i++) sp.append("k" + i, "v");
    const t0 = Date.now();
    for (let i = 0; i < 500; i++) u.searchParams.append("m" + i, "v");
    const dt = Date.now() - t0;
    if (dt > 2000) throw new Error("memoized append idiom regressed: " + dt + "ms for 500 appends");
    print("test_url: SameObject + memoized-idiom ok (" + dt + "ms/500)");
});
__kitS.end();
