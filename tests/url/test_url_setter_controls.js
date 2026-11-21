// covers: integration c:src/modules/libc/libc-url-accessors.c c:src/modules/libc/libc-url.c -- setters live in the accessors, parsing in the core URL class
// WHATWG URL setters and control characters (WPT url-setters-stripping, pinned here so the
// behaviour holds without the third_party/wpt checkout). Every setter but username/password
// removes U+0009/000A/000D anywhere in the value (the basic URL parser does); username and
// password percent-encode them with the userinfo set instead. A NUL is percent-encoded by
// path, query, fragment and userinfo, fails host/hostname (forbidden host code point) and
// ends a port (port state with a state override keeps the digits before it). An opaque host
// (non-special scheme) percent-encodes other C0 controls; a special host refuses them.
// protocol parses scheme-start with a state override: stops at ':', fails on any other
// non-scheme byte, refuses special<->non-special switches, drops the new default port.
// The constructor and statics run non-string input through ToString; a missing one is a TypeError.
import { suite } from "../lib/kit.js";

const t = suite("test_url_setter_controls");
const base = (s) => new URL(s + "://username:password@host:8000/path?query#fragment");

t.test("tab and newline are stripped, other C0 kept", ({ eq }) => {
    for (const c of ["\t", "\n", "\r"]) {
        const u = base("https");
        u.protocol = c + "http";
        u.host = "te" + c + "st";
        u.port = "90" + c + "00";
        u.pathname = "te" + c + "st";
        u.search = "te" + c + "st";
        u.hash = "te" + c + "st";
        eq(u.href, "http://username:password@test:9000/test?test#test", "stripped " + JSON.stringify(c));
    }
    const u = base("https");
    u.username = "te\tst";
    u.password = "te\x1fst";
    eq(u.href, "https://te%09st:te%1Fst@host:8000/path?query#fragment", "userinfo encodes instead of stripping");
});

t.test("NUL per component", ({ eq }) => {
    const u = base("https");
    u.pathname = "te\0st";
    u.search = "te\0st";
    u.hash = "te\0st";
    u.username = "\0";
    u.host = "te\0st";
    eq(u.href, "https://%00:password@host:8000/te%00st?te%00st#te%00st", "NUL encoded, host refused");
    eq([...u.searchParams.keys()][0], "te\0st", "searchParams sees the decoded NUL");
    u.port = "90\x0000";
    eq(u.port, "90", "port keeps the digits before NUL");
    u.port = "\x009000";
    eq(u.port, "90", "port with no leading digit is ignored");
});

t.test("opaque vs special host C0", ({ eq }) => {
    const o = base("wpt++");
    o.hostname = "te\x1fst";
    eq(o.hostname, "te%1Fst", "opaque host percent-encodes C0");
    const s = base("https");
    s.hostname = "te\x1fst";
    eq(s.hostname, "host", "special host refuses C0");
});

t.test("protocol state override", ({ eq }) => {
    const u = base("https");
    u.protocol = "http\0";
    eq(u.protocol, "https:", "non-scheme byte before the inserted colon fails");
    u.protocol = "wpt++";
    eq(u.protocol, "https:", "special to non-special refused");
    const f = new URL("http://h:8000/");
    f.protocol = "file";
    eq(f.protocol, "http:", "file refused while a port is set");
});

t.test("ToString input", ({ eq }) => {
    eq(new URL({ toString: () => "https://x.test/a" }).href, "https://x.test/a", "object input");
    eq(URL.canParse(undefined), false, "undefined -> \"undefined\"");
    eq(URL.parse(42), null, "number input parses as \"42\"");
    for (const fn of [() => URL.parse(), () => URL.canParse(), () => new URL()]) {
        let name = "";
        try { fn(); } catch (e) { name = e.name; }
        eq(name, "TypeError", "a missing input is a TypeError (WebIDL required argument)");
    }
});

await t.run();
