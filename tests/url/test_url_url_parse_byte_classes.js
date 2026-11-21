// covers: integration c:src/core/urlrec/urlrec-parse.c c:src/core/urlrec/urlrec-serialize.c c:src/core/urlrec/urlrec-host.c c:src/core/urlrec/urlrec.h -- the byte-class table and the allocation trims run together inside every URL parse
// URL parsing classifies bytes through one 256-entry table (a bit per WHATWG
// percent-encode set plus one for forbidden domain bytes) and copies runs of
// bytes that need no escape in one go; a component with nothing to escape is
// duplicated once instead of built in a buffer and copied; a host with no '%'
// is checked in place instead of decoded into a copy; a port without a
// leading zero is kept as typed; dot-segment removal writes straight into the
// output buffer, whose storage then becomes the path. Modperf round
// 2026-11-16 (median of 7, 1K/10K/100K): new URL -38%, relative -37%,
// URL.canParse -40..-42%, URL.join -38%, setters -35%. Oracle: JS
// encoders for the userinfo, path, query (special and not) and fragment sets
// over UTF-8 bytes, the forbidden-domain set for hosts, decimal port rules
// with default ports dropped, and a JS dot-segment remover with base merge.
// Not asserted (pre-existing, finding F13): "foo:/.." serializes as "foo:"
// where WHATWG gives "foo:/".
import { suite } from "../lib/kit.js";

const t = suite("test_url_url_parse_byte_classes");

const HEX = "0123456789ABCDEF";
const C0 = (b) => b <= 0x1f || b > 0x7e;
const SETS = {
    fragment: (b) => C0(b) || ' "<>`'.includes(String.fromCharCode(b)),
    query: (b) => C0(b) || ' "#<>'.includes(String.fromCharCode(b)),
    squery: (b) => C0(b) || ' "#<>\''.includes(String.fromCharCode(b)),
    path: (b) => C0(b) || ' "#<>?^`{}'.includes(String.fromCharCode(b)),
    userinfo: (b) => C0(b) || ' "#<>?`{}/:;=@[\\]^|'.includes(String.fromCharCode(b)),
};
const isHex = (c) => /^[0-9A-Fa-f]$/.test(c);
function enc(s, set) {
    let o = "";
    for (const b of new TextEncoder().encode(s)) o += set(b) ? "%" + HEX[b >> 4] + HEX[b & 15] : String.fromCharCode(b);
    return o;
}
function hrefPass(s, set) {
    let o = "";
    for (let i = 0; i < s.length; i++) {
        const c = s[i], b = c.charCodeAt(0);
        if (c === "%" && isHex(s[i + 1] || "") && isHex(s[i + 2] || "")) {
            o += s.slice(i, i + 3);
            i += 2;
        } else o += set(b) ? "%" + HEX[b >> 4] + HEX[b & 15] : c;
    }
    return o;
}
const FORBID_HOST = (c) => c.charCodeAt(0) <= 0x20 || c === "\x7f" || "%#/:<>?@[\\]^|".includes(c);
const isDot = (s) => s === "." || s.toLowerCase() === "%2e";
const isDot2 = (s) => [".."].includes(s) || ["%2e.", ".%2e", "%2e%2e"].includes(s.toLowerCase());
function removeDots(segs) {
    const st = [];
    segs.forEach((g, i) => {
        const last = i === segs.length - 1;
        if (isDot(g)) {
            if (last) st.push("");
        } else if (isDot2(g)) {
            st.pop();
            if (last) st.push("");
        } else st.push(g);
    });
    return "/" + st.join("/");
}

const CHARS = [];
for (let c = 0; c < 0x80; c++) CHARS.push(String.fromCharCode(c));
CHARS.push("é", "€", "😀", "\u00ff");
const STRIPPED = "\t\n\r";

t.test("every char in fragment, query, path and userinfo", ({ eq, ok }) => {
    for (const ch of CHARS) {
        if (STRIPPED.includes(ch)) continue;
        const name = JSON.stringify(ch);
        const f = "a" + ch + "b";
        eq(new URL("https://h.com/#" + f).hash, "#" + enc(f, SETS.fragment), "fragment " + name);
        if (ch !== "#") {
            eq(new URL("https://h.com/?" + f).search, "?" + enc(f, SETS.squery), "special query " + name);
            eq(new URL("foo://h/?" + f).search, "?" + enc(f, SETS.query), "query " + name);
        }
        if (!"#?/\\".includes(ch)) {
            const u = new URL("https://h.com/x/" + f + "/y");
            eq(u.pathname, "/x/" + enc(f, SETS.path) + "/y", "path " + name);
            eq(u.href, "https://h.com/x/" + enc(f, SETS.path) + "/y", "href " + name);
        }
        if (!"#?/\\@:".includes(ch)) {
            const u = new URL("https://u" + ch + "v:p" + ch + "q@h.com/");
            eq(u.username, enc("u" + ch + "v", SETS.userinfo), "username " + name);
            eq(u.password, enc("p" + ch + "q", SETS.userinfo), "password " + name);
        }
    }
    ok(true, "done");
});

t.test("a byte to escape as the first or last byte of each component", ({ eq }) => {
    for (const ch of CHARS) {
        const b = ch.charCodeAt(0);
        if (STRIPPED.includes(ch) || b <= 0x20) continue;
        const name = JSON.stringify(ch);
        for (const f of [ch, "a" + ch, ch + "a"]) {
            eq(new URL("https://h.com/#" + f).hash, "#" + enc(f, SETS.fragment), "fragment " + JSON.stringify(f));
            if (ch !== "#") eq(new URL("https://h.com/?" + f).search, "?" + enc(f, SETS.squery), "query " + JSON.stringify(f));
            if (!"#?/\\.".includes(ch)) eq(new URL("https://h.com/" + f).pathname, "/" + enc(f, SETS.path), "path " + JSON.stringify(f));
            if (!"#?/\\@:".includes(ch)) eq(new URL("https://" + f + ":" + f + "@h.com/").username, enc(f, SETS.userinfo), "username " + JSON.stringify(f));
        }
        eq(name.length > 0, true, "char " + name);
    }
});

t.test("escapes already present pass through the href serializer", ({ eq }) => {
    for (const s of ["%41", "%zz", "%", "%4", "a%2Fb%2f", "%%41", "x%C3%A9y", "%7B%7d"]) {
        const u = new URL("https://h.com/p" + s + "?q" + s + "#f" + s);
        eq(u.href, "https://h.com/p" + hrefPass(s, SETS.path) + "?q" + hrefPass(s, SETS.squery) + "#f" + hrefPass(s, SETS.fragment), "href " + s);
    }
});

t.test("hosts: forbidden bytes throw, others are lower-cased", ({ eq }) => {
    for (const ch of CHARS) {
        if (STRIPPED.includes(ch) || "/?#\\@:".includes(ch) || ch.charCodeAt(0) > 0x7f) continue;
        const name = JSON.stringify(ch);
        const h = "aB" + ch + "c.com";
        let got;
        try {
            got = new URL("https://" + h + "/").hostname;
        } catch (e) {
            got = "throws";
        }
        eq(got, FORBID_HOST(ch) ? "throws" : h.toLowerCase(), "host " + name);
        let got2;
        try {
            got2 = new URL("https://a%" + HEX[ch.charCodeAt(0) >> 4] + HEX[ch.charCodeAt(0) & 15] + "b.com/").hostname;
        } catch (e) {
            got2 = "throws";
        }
        const forb2 = FORBID_HOST(ch) || "/?#\\@:".includes(ch);
        eq(got2, forb2 ? "throws" : ("a" + ch + "b.com").toLowerCase(), "escaped host " + name);
    }
    eq(new URL("https://A%41%62.COM/").hostname, "aab.com", "escaped letters");
    eq(new URL("https://bücher.de/").hostname, "xn--bcher-kva.de", "non-ASCII host still goes through IDNA");
});

t.test("ports: digits, leading zeros, defaults, range", ({ eq }) => {
    const cases = [["http", "80", ""], ["http", "0080", ""], ["http", "080", ""], ["http", "8080", "8080"], ["http", "08080", "8080"], ["http", "0", "0"], ["http", "00", "0"], ["http", "65535", "65535"], ["http", "000000000000000081", "81"], ["https", "443", ""], ["https", "00443", ""], ["https", "80", "80"], ["ws", "80", ""], ["wss", "0443", ""], ["ftp", "021", ""], ["foo", "80", "80"], ["foo", "007", "7"], ["http", "", ""]];
    for (const [sc, p, want] of cases) eq(new URL(sc + "://h:" + p + "/").port, want, sc + ":" + p);
    for (const p of ["65536", "1a", "-1", "99999"]) {
        let threw = false;
        try {
            new URL("http://h:" + p + "/");
        } catch (e) {
            threw = true;
        }
        eq(threw, true, "bad port " + p);
    }
});

let seed = 0x2611;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) % n;
const SEGS = ["a", "b", ".", "..", "%2e", "%2E%2e", ".%2e", "%2e.", "", "x y", "é", "c^d", "...", "x".repeat(80)];

t.test("dot segments, absolute and resolved against a base", ({ eq }) => {
    const bases = [["https://h.com/b1/b2/f", ["b1", "b2"]], ["https://h.com/", []], ["https://h.com/only", []], ["http://h.com/d/", ["d"]]];
    for (let r = 0; r < 1500; r++) {
        const segs = Array.from({ length: 1 + rnd(6) }, () => SEGS[rnd(SEGS.length)]);
        const path = segs.join("/");
        const encSegs = (p) => p.split("/").map((g) => enc(g, SETS.path)).join("/");
        eq(new URL("https://h.com/" + path).pathname, encSegs(removeDots(segs)), "abs " + path);
        const [b, bd] = bases[rnd(bases.length)];
        const rs = segs[0] === "" ? ["r"].concat(segs.slice(1)) : segs;
        const rp = rs.join("/");
        eq(new URL(rp, b).pathname, encSegs(removeDots(bd.concat(rs))), "rel " + rp + " on " + b);
        eq(URL.join(b, rp), new URL(rp, b).href, "join " + rp);
    }
});

t.test("non-special and opaque paths keep their shape", ({ eq }) => {
    eq(new URL("foo://h/a/../b/./c").pathname, "/b/c", "non-special dots");
    eq(new URL("foo://h").pathname, "", "non-special empty path");
    eq(new URL("https://h.com").pathname, "/", "special empty path");
    eq(new URL("foo:/a/..").href, "foo:/", "non-special dots back to root");
    eq(new URL("mailto:a b@c.d").pathname, "a b@c.d", "opaque keeps space");
    eq(new URL("mailto:a b ?x").href, "mailto:a b%20?x", "opaque space before query");
    eq(new URL("data:text/plain,a\u00e9\u0001z").pathname, "text/plain,a%C3%A9%01z", "opaque escapes C0 and non-ASCII");
    eq(new URL("file:///C:/x/../y").pathname, "/C:/y", "file drive");
    eq(new URL("..", "file:///C:/").pathname, "/C:/", "file drive is not popped");
});

await t.run();
