// covers: c:src/modules/libc/libc-fetch.c
// The native fast paths of the Fetch globals must be observably identical to the generic WebIDL/Fetch steps
// (results checked against node 26 and bun on the same inputs; node strips two leading BOMs, the spec strips one):
// Headers(sequence) takes a pristine-array fast path only when Array.prototype[Symbol.iterator] and
// %ArrayIteratorPrototype%.next are untouched and no array carries its own iterator; header names are stored as
// atoms and sorted on demand; string bodies stay strings until bytes are needed (text()/json() strip one UTF-8 BOM,
// lone surrogates become U+FFFD); clones share the string; iterators are native iterator_next functions.
import { suite } from "../lib/kit.js";

const t = suite("test_fetch_native_paths");
function err(fn) { try { return JSON.stringify(fn()); } catch (e) { return e.constructor.name; } }

t.test("Headers(sequence) fast path matches the iterator protocol", (a) => {
    a.eq(err(() => [...new Headers([["B", "2"], ["a", "1"], ["set-cookie", "x"], ["Set-Cookie", "y"], ["a", "3"]])]), '[["a","1, 3"],["b","2"],["set-cookie","x"],["set-cookie","y"]]');
    a.eq(err(() => { const x = [["a", "1"]]; x[Symbol.iterator] = function* () { yield ["z", "9"]; }; return [...new Headers(x)]; }), '[["z","9"]]');
    a.eq(err(() => { const p = ["a", "1"]; p[Symbol.iterator] = function* () { yield "q"; yield "w"; }; return [...new Headers([p])]; }), '[["q","w"]]');
    a.eq(err(() => { const o = Array.prototype[Symbol.iterator]; Array.prototype[Symbol.iterator] = function* () { yield* o.call(this); yield ["late", "1"]; }; try { return [...new Headers([["k", "v"]])]; } finally { Array.prototype[Symbol.iterator] = o; } }), "TypeError");
    a.eq(err(() => { const AIP = Object.getPrototypeOf([][Symbol.iterator]()); const o = AIP.next; let c = 0; AIP.next = function () { c++; return o.call(this); }; try { new Headers([["k", "v"]]); return c; } finally { AIP.next = o; } }), "5");
    a.eq(err(() => new Headers([["a", "b", "c"]])), "TypeError");
    a.eq(err(() => new Headers([["a"]])), "TypeError");
    a.eq(err(() => [...new Headers([["n", 5], ["o", { toString() { return "s"; } }]])]), '[["n","5"],["o","s"]]');
    a.eq(err(() => { const x = [["a", "1"]]; x.length = 2; return [...new Headers(x)]; }), "TypeError");
    a.eq(err(() => { class A extends Array {} return [...new Headers(A.from([["a", "1"]]))]; }), '[["a","1"]]');
});

t.test("Headers store: sorted iteration, combined get, set-cookie kept apart, live iterators", (a) => {
    const h = new Headers([["x-b", "1"], ["X-A", "2"], ["set-cookie", "c1"], ["x-b", "3"], ["set-cookie", "c2"]]);
    a.eq(JSON.stringify([...h.keys()]), '["set-cookie","set-cookie","x-a","x-b"]');
    a.eq(JSON.stringify([...h.values()]), '["c1","c2","2","1, 3"]');
    a.eq(h.get("set-cookie"), "c1, c2");
    a.eq(JSON.stringify(h.getSetCookie()), '["c1","c2"]');
    h.set("set-cookie", "only");
    a.eq(JSON.stringify(h.getSetCookie()), '["only"]');
    const it = h.keys();
    a.eq(it.next().value, "set-cookie");
    h.append("a-first", "1");
    a.eq(it.next().value, "set-cookie");
    a.eq(JSON.stringify(it.next()), '{"value":"x-a","done":false}');
    h.delete("x-a");
    a.eq(JSON.stringify(it.next()), '{"done":true}');
    a.eq(JSON.stringify([...new Request("http://h/", { headers: h }).headers]), JSON.stringify([...h]));
    const seen = []; h.forEach((v, k) => seen.push(k + "=" + v));
    a.eq(seen.join(";"), "a-first=1;set-cookie=only;x-b=1, 3");
});

t.test("string bodies stay strings until bytes are needed", async (a) => {
    const rows = [["hello", "hello", "104,101,108,108,111"], ["﻿bom", "bom", "239,187,191,98,111,109"], ["lone\uD800x", "lone�x", "108,111,110,101,239,191,189,120"],
        ["ü", "ü", "195,188"], ["﻿﻿two", "﻿two", "239,187,191,239,187,191,116,119,111"]];
    for (const [src, text, bytes] of rows) {
        a.eq(await new Response(src).text(), text);
        a.eq([...new Uint8Array(await new Response(src).arrayBuffer())].join(","), bytes);
        const r = new Response(src);
        a.eq(await r.clone().text(), text);
        a.eq(await new Response(r.body).text(), text);
    }
    a.eq(JSON.stringify(await new Response('﻿{"b":2}').json()), '{"b":2}');
    a.eq(err(() => 0) === "0" && await new Response("{").json().then(() => "ok", (e) => e.constructor.name), "SyntaxError");
    a.eq(new Response("x").headers.get("content-type"), "text/plain;charset=UTF-8");
    a.eq(new Response("x", { headers: { "content-type": "a/b" } }).headers.get("content-type"), "a/b");
    const thenable = await new Response('{"then":1}').json();
    a.eq(thenable.then, 1);
});

await t.run();
