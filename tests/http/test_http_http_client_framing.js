// flags: --std
// timeout: 300
// covers: dyna:http.HTTPClient
// slots: 2
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD (evil.py already takes its port file as an argument) with the M01 name cases /n1-/n6 added to evil.py and the suite
import { HTTPClient } from "dyna:http";
import { spawnHelper } from "../lib/helper.js";
import * as std from "std";
import * as os from "os";
import { suite } from "../lib/kit.js";

const t = suite("test_http_client_framing");
let A = null;
const ok = (c, m) => A.ok(c, m);
let why = null, T = null, P = 0, client = null, reap = () => {};
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); if (!P) a.must(false, "fixture: the oracle never bound"); return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
const CH = (...cs) => String.fromCharCode(...cs);
const CR = CH(13), LF = CH(10);

t.before(() => {
    T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_frame.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/evil.js`, `import { listen, latin1 } from ${KIT};
const NL = "\\r\\n", CR = "\\r", LF = "\\n";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const cl = (body, declared, extra = "") => "HTTP/1.1 200 OK" + NL + "Content-Length: " + declared + NL + NL + body + extra;
const st = (line, tail) => "HTTP/1.1 " + line + NL + (tail === undefined ? "Content-Length: 2" + NL + NL + "hi" : tail);
const OK2 = "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL;
const CASES = {
    "/h1": cl("ABCDEF", 3),
    "/h2": cl("HELLO", 0),
    "/h3": cl("A", 1, "HTTP/1.1 200 OK" + NL + "Content-Length: 4" + NL + NL + "BODYTRAILING-JUNK"),
    "/h4": cl("Z".repeat(4096), 1),
    "/h5": cl("ABCDE", 4),
    "/h6": cl("ABCD", 4, NL),
    "/h7": cl("", 0, "HTTP/1.1 204 No Content" + NL + NL),
    "/h8": cl("ABCDEF", 6),
    "/h9": cl("ABC", 3),
    "/h10": cl("A", 1),
    "/h11": cl("", 0),
    "/h12": cl("AB", 3),
    "/h13": "HTTP/1.1 200 OK" + NL + "Connection: close" + NL + NL + "ABCDEF",
    "/h14": cl("ABCDEF", 6, "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi"),
    "/h15": "HTTP/1.1 200 OK" + NL + "Transfer-Encoding: chunked" + NL + NL + "6" + NL + "ABCDEF" + NL + "0" + NL + NL,
    "/h16": "HTTP/1.1 200 OK" + NL + "Transfer-Encoding: chunked" + NL + NL + "1" + NL + "A" + NL + "0" + NL + NL + "HTTP/1.1 200 OK" + NL + "Content-Length: 4" + NL + NL + "BODYJUNK",
    "/h17": cl("A", 1, "Q".repeat(65536)),
    "/m1": OK2 + "X-A: v" + CR + "Set-Cookie: p=1" + NL + NL + "hi",
    "/m2": OK2 + "X-A: a" + LF + "b" + NL + NL + "hi",
    "/m3": OK2 + "X-A: v" + CR + " " + NL + NL + "hi",
    "/m4": OK2 + "X-A: v" + CR + CR + NL + NL + "hi",
    "/m5": OK2 + "X-A: a" + NL + "X-B: b" + NL + NL + "hi",
    "/m6": OK2 + "X-A: a\\tb" + NL + NL + "hi",
    "/d1": "HTTP/1.1 302 Found" + NL + "Content-Length: 0" + NL + "Location: /a" + NL + "location: /b" + NL + NL,
    "/d2": OK2 + "Set-Cookie: a=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT" + NL + "X-A: 1" + NL + "Vary: x" + NL + "x-a: 2 " + NL + "set-cookie: b=2" + NL + "Vary: y" + NL + NL + "hi",
    "/d3": OK2 + "Content-Type: text/plain" + NL + "content-type: text/plain" + NL + NL + "hi",
    "/m7": OK2 + "X-A: caf\\xe9" + NL + NL + "hi",
    "/m8": OK2 + "X-A:" + NL + NL + "hi",
    "/m9": OK2 + "Location: http://h:1/p?q=1" + NL + NL + "hi",
    "/m10": OK2 + "X-A: a\\x00b" + NL + NL + "hi",
    "/m11": OK2 + "X-A: a\\x0bb" + NL + NL + "hi",
    "/m12": OK2 + "X-A: a\\x7fb" + NL + NL + "hi",
    "/m13": OK2 + "X-A" + CR + "B: v" + NL + NL + "hi",
    "/m14": ["HTTP/1.1 200 OK", "Content-Length: 2", "X-A: v", "", "hi"].join(LF),
    "/n1": OK2 + "X A: v" + NL + NL + "hi",
    "/n2": OK2 + "X@A: v" + NL + NL + "hi",
    "/n3": OK2 + "X\\xe9A: v" + NL + NL + "hi",
    "/n4": OK2 + ": v" + NL + NL + "hi",
    "/n5": OK2 + ":X-A: v" + NL + NL + "hi",
    "/n6": OK2 + "X-A : v" + NL + NL + "hi",
    "/s1": st("1999 Weird"),
    "/s2": st("1200 Weird"),
    "/s3": st("2000 Weird"),
    "/s4": st("99999 Weird"),
    "/s5": st("1999 Weird", NL) + "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi",
    "/s6": st("20 Weird"),
    "/s7": st("2xx Weird"),
    "/s8": st("2000X Weird"),
    "/s10": st("100 Continue", NL) + "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi",
    "/s11": st("199 Odd", NL) + "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi",
    "/s12": "HTTP/1.1 101 Switching Protocols" + NL + "Upgrade: x" + NL + "Connection: Upgrade" + NL + NL,
    "/s13": "HTTP/1.1 100" + NL + NL + "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi",
    "/s14": st("099 Odd"),
    "/s15": st("199 Odd"),
    "/s16": st("200 OK"),
    "/s17": "HTTP/1.1 204" + NL + NL,
    "/s18": st("299 Odd"),
    "/s19": st("300 Multiple Choices"),
    "/s20": st("404 Not Found"),
    "/s21": st("499 Odd"),
    "/s22": st("500 Internal Server Error"),
    "/s23": st("599 Odd"),
    "/s24": st("999 Odd"),
    "/s24b": st("598 Odd"),
    "/s25": "HTTP/1.0 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi",
    "/s26": "HTTP/2 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi",
};
listen(async (c) => {
    const h = await c.readUntil(NL + NL, { timeoutMs: 5000 });
    if (!h.found) { c.close(); return; }
    const parts = latin1(h.bytes).split(NL)[0].split(" ");
    const path = parts.length >= 2 ? parts[1].split("?")[0] : "/";
    if (!Object.hasOwn(CASES, path)) { c.close(); return; }
    await c.write(CASES[path]);
    await sleep(50);
    c.close();
}, { portFile: scriptArgs[1] });
`);

    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid`); spawnHelper(`${cmd} >${T}/${tag}.log 2>&1`, { pidFile: `${T}/${tag}.pid` });
    };
    const waitPort = (tag) => {
        for (let i = 0; i < 300; i++) {
            const p = parseInt((cat(`${T}/${tag}.port`) || "").trim(), 10) || 0;
            if (p > 0) return p;
            sh("sleep 0.05");
        }
        return 0;
    };
    reap = () => sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
        `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
        `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
        `kill $p 2>/dev/null;; esac; done; rm -rf ${T}`);

    startBg(`${EXE} --std ${T}/evil.js ${T}/evil.port`, "evil");
    P = waitPort("evil");
    if (P === 0) print("  fixture log: " + cat(`${T}/evil.log`).slice(0, 400));
    client = new HTTPClient();
});
t.after(() => reap());

t.test("fixture: the oracle bound an ephemeral port", (a) => {
    A = a;
    if (why) a.skip(why);
    ok(P > 0, "fixture: the oracle bound an ephemeral port");
});

const get = async (path) => {
    try {
        const r = await client.get(`http://127.0.0.1:${P}${path}`);
        return { ok: true, status: r.status, body: r.body,
                 len: r.bodyBytes ? r.bodyBytes.length : -1,
                 headers: r.headers };
    } catch (e) {
        return { ok: false, err: String(e && e.message || e) };
    }
};
const hdr = (headers, name) => {
    if (!headers) return undefined;
    const want = name.toLowerCase();
    for (const k of Object.keys(headers)) if (k.toLowerCase() === want) return headers[k];
    return undefined;
};
const refused = async (label, path) => {
    const r = await get(path);
    ok(!r.ok, label + " is REFUSED" + (r.ok ? " -- got status " + r.status +
                                          " body " + JSON.stringify(r.body).slice(0, 60) : ""));
    return r;
};
const served = async (label, path, wantBody, wantStatus) => {
    const r = await get(path);
    if (!r.ok) { ok(false, label + " should serve -- threw " + r.err); return null; }
    ok(r.body === wantBody, label + " body is exactly " + JSON.stringify(wantBody) +
       " -- got " + JSON.stringify(r.body).slice(0, 80));
    if (wantStatus !== undefined)
        ok(r.status === wantStatus,
           label + " status is " + wantStatus + " -- got " + r.status);
    return r;
};

test("H02: a body is cut at its declared Content-Length whatever follows it", async () => {
    await served("H02 CL:3/6 sent", "/h1", "ABC", 200);
    await served("H02 CL:0/5 sent", "/h2", "", 200);
    await served("H02 CL:1 + a whole second response", "/h3", "A", 200);
    await served("H02 CL:1/4096 sent", "/h4", "Z", 200);
    await served("H02 CL:4/5 sent", "/h5", "ABCD", 200);
    await served("H02 CL:4 + a trailing CRLF", "/h6", "ABCD", 200);
    await served("H02 CL:0 + a pipelined response", "/h7", "", 200);
    await served("H02 CL:1 + 64 KiB of junk", "/h17", "A", 200);
    await served("H02 CL:6, 6 sent, then a second response", "/h14", "ABCDEF", 200);
});

test("H02 controls: well-framed bodies are served exactly", async () => {
    await served("H02 control CL:6/6 sent", "/h8", "ABCDEF", 200);
    await served("H02 control CL:3/3 sent", "/h9", "ABC", 200);
    await served("H02 control CL:1/1 sent", "/h10", "A", 200);
    await served("H02 control CL:0/nothing sent", "/h11", "", 200);
    await served("H02 control no CL, read-to-close", "/h13", "ABCDEF", 200);
    await served("H02 control chunked, the same bytes", "/h15", "ABCDEF", 200);
    await served("H02 control chunked 1 byte + a 2nd response", "/h16", "A", 200);
});

test("H02: the header region survives the cut intact", async () => {
    {
        const r = await get("/h3");
        ok(r.ok && r.headers && hdr(r.headers, "content-length") === "1",
           "H02: the header region survives the cut intact [" +
           JSON.stringify(r.headers) + "]");
    }
});

test("H02: a body cut short of its length is refused and the error names the truncation", async () => {
    {
        const r = await refused("H02: a body cut short of its declared length", "/h12");
        if (!r.ok) ok(/truncated/.test(r.err),
                      "H02: the truncation refusal NAMES the truncation [" + r.err + "]");
    }
});

test("connection reuse: leftover bytes never leak into the next response", async () => {
    {
        const first = await get("/h3");
        const second = await get("/h3");
        ok(first.ok && first.body === "A",
           "reuse: the first request's body is its own declared byte");
        ok(second.ok && second.body === "A",
           "reuse: a SECOND identical request is unaffected by the first's leftover");
        const third = await get("/h8");
        ok(third.ok && third.body === "ABCDEF",
           "reuse: a request after a fully-consumed one is also unaffected");
    }
});

test("M01: bare CR or LF inside a response header value is refused", async () => {
    await refused("M01: a bare CR in a response header value", "/m1");
    await refused("M01: a bare LF in a response header value", "/m2");
    await refused("M01: a CR then a space at the end of a value", "/m3");
    await refused("M01: CR CR in a value", "/m4");
});

test("M01 controls: split headers, TAB, obs-text, empty values and colons parse", async () => {
    {
        const r = await get("/m5");
        ok(r.ok && r.ok && hdr(r.headers, "x-a") === "a" && hdr(r.headers, "x-b") === "b",
           "M01 control: two headers split on CRLF still parse [" +
           JSON.stringify(r.headers) + "]");
    }
    {
        const r = await get("/m6");
        ok(r.ok && hdr(r.headers, "x-a") === "a\tb",
           "M01 control: TAB in a value is legal and survives [" +
           JSON.stringify(r.headers) + "]");
    }
    {
        const r = await get("/m7");
        ok(r.ok && hdr(r.headers, "x-a") !== undefined,
           "M01 control: obs-text 0xe9 in a value is legal [" +
           JSON.stringify(r.headers) + "]");
    }
    {
        const r = await get("/m8");
        ok(r.ok && hdr(r.headers, "x-a") === "",
           "M01 control: an empty value is legal [" + JSON.stringify(r.headers) + "]");
    }
    {
        const r = await get("/m9");
        ok(r.ok && hdr(r.headers, "location") === "http://h:1/p?q=1",
           "M01 control: a colon inside a value is legal [" +
           JSON.stringify(r.headers) + "]");
    }
});

test("M01 controls: NUL, VT, DEL, a CR in a name and LF-only endings are refused", async () => {
    await refused("M01 control: NUL in a value", "/m10");
    await refused("M01 control: VT in a value", "/m11");
    await refused("M01 control: DEL in a value", "/m12");
    await refused("M01 control: CR in a header name", "/m13");
    await refused("M01 control: LF-only line endings", "/m14");
});

test("M01 names: a response header name that is not a token is refused (RFC 9110 5.1, RFC 9112 5.1)", async () => {
    await refused("M01 name: a space in a header name", "/n1");
    await refused("M01 name: @ in a header name", "/n2");
    await refused("M01 name: a non-ASCII byte in a header name", "/n3");
    await refused("M01 name: an empty header name", "/n4");
    await refused("M01 name: a line starting with a colon", "/n5");
    await refused("M01 name: whitespace between the name and the colon", "/n6");
});

test("M03: status codes that are not exactly three digits in range are refused", async () => {
    await refused("M03: a 4-digit 1xx-prefixed status (1999)", "/s1");
    await refused("M03: a 4-digit 1xx-prefixed status (1200)", "/s2");
    await refused("M03: a 4-digit 2xx-prefixed status (2000)", "/s3");
    await refused("M03: a 5-digit status (99999)", "/s4");
    await refused("M03: 1999 ahead of a real 200", "/s5");
    await refused("M03: a 2-digit status", "/s6");
    await refused("M03: a non-digit status", "/s7");
    await refused("M03: 4 digits then junk", "/s8");
    await refused("F4: 999 (three digits, out of range)", "/s24");
});

test("M03 controls: valid status codes surface unchanged", async () => {
    for (const [path, want, label] of [
        ["/s16", 200, "200"],
        ["/s17", 204, "204 with no reason and no body"],
        ["/s18", 299, "299 (2xx upper edge)"],
        ["/s19", 300, "300"],
        ["/s20", 404, "404"],
        ["/s21", 499, "499"],
        ["/s22", 500, "500"],
        ["/s23", 599, "599 (5xx upper edge)"],
        ["/s24b", 598, "598 (just under the range edge)"],
        ["/s14", 99,  "099 (leading zero is still three digits)"],
        ["/s25", 200, "HTTP/1.0"],
        ["/s26", 200, "HTTP/2"],
    ]) {
        const r = await get(path);
        ok(r.ok && r.status === want,
           "M03 control: " + label + " surfaces as " + want +
           " -- " + (r.ok ? "got " + r.status : "threw " + r.err));
    }
});

test("M03 controls: interim responses before the final 200 are skipped", async () => {
    for (const [path, label] of [
        ["/s10", "100 Continue then 200"],
        ["/s11", "199 then 200"],
        ["/s13", "100 with no reason then 200"],
    ]) {
        const r = await get(path);
        ok(r.ok && r.status === 200,
           "M03 control: " + label + " still yields the final 200 -- " +
           (r.ok ? "got " + r.status : "threw " + r.err));
    }
});

test("D10: a singleton field with two different values fails get and getStream", async () => {
    {
        const r = await get("/d1");
        ok(!r.ok && /malformed response: conflicting location values/i.test(r.err),
           "D10: a singleton field with two different values fails the request, naming it [" +
           (r.ok ? "got " + r.status : r.err) + "]");
    }
    {
        let err = "";
        try { const st = await client.getStream(`http://127.0.0.1:${P}/d1`); st.close(); } catch (e) { err = String(e.message); }
        ok(/malformed response: conflicting location values/i.test(err),
           "D10: getStream applies the same rule [" + err + "]");
    }
});

test("D10: repeated fields merge case-insensitively with list and Set-Cookie joins", async () => {
    {
        const r = await get("/d2");
        ok(r.ok && "X-A" in r.headers && !("x-a" in r.headers) && !("set-cookie" in r.headers),
           "D10: names merge case-insensitively under the first spelling [" + JSON.stringify(r.headers) + "]");
        ok(r.ok && r.headers["X-A"] === "1, 2" && r.headers["Vary"] === "x, y",
           "D10: list fields join with \", \" and lose trailing OWS [" + JSON.stringify(r.headers) + "]");
        ok(r.ok && r.headers["Set-Cookie"] === "a=1; Expires=Wed, 21 Oct 2015 07:28:00 GMT\nb=2",
           "D10: Set-Cookie lines join with \"\\n\", never \", \" [" + JSON.stringify(r.headers) + "]");
    }
});

test("D10 control: an identical singleton repeat is one value", async () => {
    {
        const r = await get("/d3");
        ok(r.ok && hdr(r.headers, "content-type") === "text/plain",
           "D10 control: an identical singleton repeat is one value [" + (r.ok ? JSON.stringify(r.headers) : r.err) + "]");
    }
});

test("M03 control: 101 is a final status, not interim", async () => {
    {
        const r = await get("/s12");
        ok(r.ok && r.status === 101,
           "M03 control: 101 is final, not interim -- " +
           (r.ok ? "got " + r.status : "threw " + r.err));
    }
});

test("M03 control: a bare interim 199 is never surfaced as a final status", async () => {
    {
        const r = await get("/s15");
        ok(!r.ok || r.status !== 199,
           "M03 control: a bare interim 199 is never surfaced as a final status");
    }
});

await t.run();
