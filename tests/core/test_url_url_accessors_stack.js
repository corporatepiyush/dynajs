// covers: c:src/modules/libc/libc-url-accessors.c
// URL protocol, host, search, hash and origin getters assemble their short
// result in a stack buffer (256 bytes, 512 for origin) and fall back to a
// heap buffer only when longer, instead of a malloc'd builder per read.
// Modperf round 2026-11-16 (median of 7): hostname, pathname, search, port
// and origin read over 1K/10K/100K URLs -19..-20%. Oracle: the
// same parts cut out of href with a JS splitter (scheme up to ':', authority
// after '//' minus userinfo, port after the last ':' outside brackets, query
// up to '#', fragment), and origin as scheme://host[:port] for http(s), ws(s)
// and ftp, "null" otherwise; component lengths straddle both buffer sizes.
import { suite } from "../lib/kit.js";

const t = suite("test_url_url_accessors_stack");

function split(href) {
    const c = href.indexOf(":");
    const scheme = href.slice(0, c);
    let rest = href.slice(c + 1), host = "", port = "", auth = false;
    let frag = null, q = null;
    const h = rest.indexOf("#");
    if (h >= 0) (frag = rest.slice(h + 1)), (rest = rest.slice(0, h));
    const qi = rest.indexOf("?");
    if (qi >= 0) (q = rest.slice(qi + 1)), (rest = rest.slice(0, qi));
    if (rest.startsWith("//")) {
        auth = true;
        const end = rest.indexOf("/", 2);
        let a = end < 0 ? rest.slice(2) : rest.slice(2, end);
        const at = a.lastIndexOf("@");
        if (at >= 0) a = a.slice(at + 1);
        const pc = a.lastIndexOf(":");
        if (pc >= 0 && pc > a.lastIndexOf("]")) (host = a.slice(0, pc)), (port = a.slice(pc + 1));
        else host = a;
    }
    const special = ["http", "https", "ws", "wss", "ftp"].includes(scheme);
    return {
        protocol: scheme + ":",
        host: port ? host + ":" + port : host,
        hostname: host,
        port,
        search: q ? "?" + q : "",
        hash: frag ? "#" + frag : "",
        origin: special && auth && host ? scheme + "://" + host + (port ? ":" + port : "") : "null",
    };
}

let seed = 0x2611;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) % n;
const LENS = [0, 1, 5, 60, 240, 250, 251, 252, 253, 254, 255, 256, 257, 300, 500, 505, 510, 600, 2000];
const str = (n) => Array.from({ length: n }, (_, i) => "abcdefghij"[(i + rnd(10)) % 10]).join("");
const label = (n) => {
    const parts = [];
    while (n > 0) {
        const k = Math.min(n, 60);
        parts.push(str(k));
        n -= k + 1;
    }
    return parts.join(".") || "h";
};

t.test("getters match the parts of href across buffer sizes", ({ eq }) => {
    const schemes = ["http", "https", "ws", "wss", "ftp", "foo", "file", "mailto", "data"];
    for (let r = 0; r < 1500; r++) {
        const sc = schemes[rnd(schemes.length)];
        const qn = LENS[rnd(LENS.length)], fn = LENS[rnd(LENS.length)], hn = LENS[rnd(LENS.length)] % 240;
        const port = rnd(3) ? "" : String(1 + rnd(65000));
        let src;
        if (sc === "mailto" || sc === "data") src = sc + ":x" + str(qn % 50);
        else if (sc === "file") src = "file:///p/" + str(qn % 40);
        else src = sc + "://" + (rnd(4) ? "" : "u:p@") + (rnd(5) ? label(hn + 1) : "[2001:db8::" + (1 + rnd(9)) + "]") + (port && sc !== "file" ? ":" + port : "") + "/p";
        if (rnd(4)) src += "?" + str(qn);
        if (rnd(4)) src += "#" + str(fn);
        const u = new URL(src), want = split(u.href);
        for (const k of ["protocol", "host", "hostname", "port", "search", "hash", "origin"]) eq(u[k], want[k], k + " of " + src.slice(0, 80) + " (len " + src.length + ")");
    }
});

t.test("empty query and fragment read as empty strings", ({ eq }) => {
    for (const s of ["https://h.com/?", "https://h.com/#", "https://h.com/?#", "foo://h?", "https://h.com"]) {
        const u = new URL(s);
        eq(u.search, "", "search " + s);
        eq(u.hash, "", "hash " + s);
    }
    eq(new URL("https://H.com:8443/").host, "h.com:8443", "host with port");
    eq(new URL("http://h.com:80/").host, "h.com", "default port dropped");
    eq(new URL("blob:https://a.com:9/x").origin, "https://a.com:9", "blob origin");
});

t.test("long scheme and host: origin past 512 bytes", ({ eq }) => {
    const h = label(250) + "." + label(250);
    const u = new URL("https://" + h + ":1234/");
    eq(u.origin, "https://" + h + ":1234", "long origin");
    eq(u.host, h + ":1234", "long host");
    const s2 = "x".repeat(300);
    eq(new URL(s2 + ":/a?" + "q".repeat(400)).protocol, s2 + ":", "long protocol");
});

await t.run();
