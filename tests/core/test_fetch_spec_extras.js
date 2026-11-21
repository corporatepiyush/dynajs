// covers: c:src/modules/libc/libc-fetch.c
// Fetch spec rules that tools/wpt.js (fetch/api) does not pin in this checkout, each with the spec step it checks:
// Request constructor "inputBody" step (a GET/HEAD request cannot inherit a body from an input Request), "extract a body"
// (an ArrayBufferView body is a copy of exactly the view's bytes), Response.redirect (only 301/302/303/307/308), and the
// Response constructor reason-phrase check (HTAB, SP, VCHAR, obs-text; nothing else).
import { suite } from "../lib/kit.js";

const t = suite("test_fetch_spec_extras");
function throws(fn, C) { try { fn(); } catch (e) { return e instanceof C; } return false; }

t.test("Request(input, {method: GET}) cannot inherit the input's body; an unbodied input is fine", (a) => {
    const src = new Request("http://h/", { method: "POST", body: "x" });
    a.ok(throws(() => new Request(src, { method: "GET" }), TypeError));
    a.ok(throws(() => new Request(src, { method: "HEAD" }), TypeError));
    a.eq(new Request(new Request("http://h/"), { method: "HEAD" }).method, "HEAD");
    a.eq(src.bodyUsed, false);
});

t.test("an ArrayBufferView body is a copy of exactly the view's bytes", async (a) => {
    const big = new Uint8Array([9, 9, 1, 2, 3, 9]);
    const r = new Response(big.subarray(2, 5));
    big[2] = 7;
    a.eq(Array.from(await r.bytes()).join(","), "1,2,3");
    const dv = new DataView(big.buffer, 3, 2);
    a.eq(Array.from(await new Request("http://h/", { method: "POST", body: dv }).bytes()).join(","), "2,3");
});

t.test("Response.redirect accepts only the five redirect statuses", (a) => {
    for (const s of [301, 302, 303, 307, 308]) a.eq(Response.redirect("http://h/", s).status, s);
    for (const s of [300, 304, 200, 399]) a.ok(throws(() => Response.redirect("http://h/", s), RangeError));
    a.eq(Response.redirect("http://h/").status, 302);
});

t.test("statusText must be a reason-phrase", (a) => {
    a.eq(new Response("", { statusText: "A\tb ÿ~" }).statusText, "A\tb ÿ~");
    for (const s of ["\n", "a\rb", "\u0000", "Ā", "\u007f"]) a.ok(throws(() => new Response("", { statusText: s }), TypeError));
});

await t.run();
