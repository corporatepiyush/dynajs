// flags: --std
/* test_url_host_boundary.js -- the URL host/hostname setters at their
 * WHATWG boundaries (every row pinned against the reference
 * implementation), plus the allocation flatness the old candidate shape
 * broke.
 *
 * The defect class: the host setter built its candidate href with BOTH the
 * value's host:port and the OLD port (http://n:81:8080/a), the re-parse
 * refused that (legitimately), and the setter silently did NOTHING --
 * while dyn_url_adopt's failed-parse early return leaked the parser's
 * partial fields (~7 allocations per refused call: 2k setter calls stood
 * 2k leaks). WHATWG host = "n:81" must set host n port 81.
 *
 * The split rules the rows pin (each verified against the reference
 * implementation): the port is the digit run that STARTS the port text
 * (a tail is ignored: "n:81:8080" and "n:81x" both set 81), a port that
 * does not fit is ignored while the host still applies ("n:x",
 * "n:65536"), a value with no port -- bare trailing colon included --
 * keeps the old one, an empty host or a refused host (forbidden code
 * points) is a whole no-op, and hostname= naming any port is a no-op.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_url_host_boundary.js */
let n = 0, bad = 0;
function ok(c, w, d) {
    if (c) { n++; print("  ok    " + w); }
    else { bad++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); }
}

/* [base href, setter, value, expected href] -- expected hrefs verified
   against the reference implementation, row for row. */
const ROWS = [
    ["http://h:8080/a", "host", "n:81", "http://n:81/a"],
    ["http://h:8080/a", "host", "n", "http://n:8080/a"],
    ["http://h:8080/a", "host", "n:", "http://n:8080/a"],
    ["http://h:8080/a", "host", "n:81:8080", "http://n:81/a"],
    ["http://h:8080/a", "host", "n:81x", "http://n:81/a"],
    ["http://h:8080/a", "host", "n:x", "http://n:8080/a"],
    ["http://h:8080/a", "host", "n: 81", "http://n:8080/a"],
    ["http://h:8080/a", "host", "n:65536", "http://n:8080/a"],
    ["http://h:8080/a", "host", "n:65535", "http://n:65535/a"],
    ["http://h:8080/a", "host", "n:0", "http://n:0/a"],
    ["http://h:8080/a", "host", "n:080", "http://n/a"],
    ["http://h:8080/a", "host", "n:00000000081", "http://n:81/a"],
    ["http://h:8080/a", "host", "n:99999999999999999999999", "http://n:8080/a"],
    ["http://h:8080/a", "host", "n:81:", "http://n:81/a"],
    ["http://h:8080/a", "host", "n::", "http://n:8080/a"],
    ["http://h:8080/a", "host", "[::1]:9", "http://[::1]:9/a"],
    ["http://h:8080/a", "host", "[::1]", "http://[::1]:8080/a"],
    ["http://h:8080/a", "host", "[::1]j", "http://h:8080/a"],
    ["http://h:8080/a", "host", "[::1", "http://h:8080/a"],
    ["http://h:8080/a", "host", "[]", "http://h:8080/a"],
    ["http://h:8080/a", "host", "", "http://h:8080/a"],
    ["http://h:8080/a", "host", ":81", "http://h:8080/a"],
    ["http://h:8080/a", "host", ":x", "http://h:8080/a"],
    ["http://h:8080/a", "host", "n m", "http://h:8080/a"],
    ["http://h:8080/a", "host", "N m", "http://h:8080/a"],
    ["http://h:8080/a", "host", "n|m", "http://h:8080/a"],
    ["http://h:8080/a", "host", "n@x", "http://h:8080/a"],
    ["http://h:8080/a", "host", "N:81", "http://n:81/a"],
    ["http://h:8080/a", "hostname", "n:81", "http://h:8080/a"],
    ["http://h:8080/a", "hostname", "n:", "http://h:8080/a"],
    ["http://h:8080/a", "hostname", "[::1]", "http://[::1]:8080/a"],
    ["http://h:8080/a", "hostname", "n", "http://n:8080/a"],
    ["http://h:8080/a", "hostname", "", "http://h:8080/a"],
    ["foo://h:8080/a", "host", "n:81", "foo://n:81/a"],
    ["foo://h/a", "host", "n:81", "foo://n:81/a"],
    ["foo://h:8080/a", "host", "n:", "foo://n:8080/a"],
];

for (const [href, k, v, want] of ROWS) {
    const u = new URL(href);
    u[k] = v;
    ok(u.href === want,
       k + "=" + JSON.stringify(v) + " on " + href + " -> " + want +
       (u.href === want ? "" : " (got " + u.href + ")"));
}

/* the host/port getters agree with the href they just produced */
{
    const u = new URL("http://h:8080/a");
    u.host = "n:81";
    ok(u.host === "n:81", "getter: host is n:81 (got " + u.host + ")");
    ok(u.port === "81", "getter: port is 81 (got " + u.port + ")");
    u.host = "n";
    ok(u.port === "81", "getter: bare host keeps the current port (got " +
       u.port + ")");
    u.host = "m:8080";
    ok(u.port === "8080" && u.host === "m:8080",
       "getter: explicit non-default port survives (got " + u.host + ":" +
       u.port + ")");
    u.host = "m:80";
    ok(u.href === "http://m/a",
       "getter: the default port elides (got " + u.href + ")");
}

/* ---- allocation flatness over 2k setter calls (the LSan row) ---------- */
{
    const u = new URL("http://u:p@h:8080/a?b#c");
    for (let i = 0; i < 2000; i++)
        u.host = "n:81";
    ok(u.href === "http://u:p@n:81/a?b#c",
       "2k host sets: href stable (got " + u.href + ")");
    for (let i = 0; i < 2000; i++)
        u.host = "m x";          /* the REFUSING shape: leaked per call */
    ok(u.href === "http://u:p@n:81/a?b#c",
       "2k refused host sets: href unchanged (got " + u.href + ")");
    ok(true, "2k + 2k setter calls ran (LSan judges flatness)");
}

print("test_url_host_boundary: " + n + " passed, " + bad + " failed");
if (bad) throw new Error("test_url_host_boundary: " + bad + " failures");
