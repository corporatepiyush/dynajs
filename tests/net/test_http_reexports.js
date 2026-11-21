// covers: integration dyna:net.TCPServer c:src/modules/http/http-message.c -- the clean break: dyna:net keeps its sockets and exports no dyna:http name, while the dyna:http names stay one object per runtime
// B1e clean break: dyna:http is its own module and dyna:net re-exports none of it. The dyna:http names stay one object per runtime, including the
// message helpers (ContentTypeParse, Negotiate, ...) that are plain functions rather than classes, so one per-runtime holder
// creates them once. The fetch names come from one list shared by the export declaration and the binding; each is the global.
// The WHATWG Streams globals are deliberately not re-exported: dyna:http re-exports only the fetch surface.
import { suite } from "../lib/kit.js";
import * as http from "dyna:http";
import * as net from "dyna:net";
import * as httpMod from "dyna:http";

const t = suite("http_reexports");
const FETCH = ["fetch", "Request", "Response", "Headers", "AbortSignal", "AbortController", "FormData"];

t.test("dyna:net re-exports no dyna:http name (clean break)", (a) => {
    const keys = Object.keys(http);
    a.ok(keys.length >= 23, "dyna:http export count " + keys.length);
    for (const k of keys) a.ok(!(k in net), "dyna:net still exports " + k);
    for (const k of ["TCPServer", "UDPSocket", "RateLimiter", "connectHappy"]) a.ok(k in net, "dyna:net keeps " + k);
});

t.test("message helpers are one object per runtime and still work", (a) => {
    a.eq(typeof http.ContentTypeParse, "function");
    a.ok(http.ContentTypeParse === httpMod.ContentTypeParse, "ContentTypeParse identity");
    a.deep(http.ContentTypeParse("text/plain; charset=utf-8"), { type: "text", subtype: "plain", parameters: { charset: "utf-8" } });
    a.eq(httpMod.NegotiateToken("gzip, br;q=0.5", ["br", "gzip"]), "gzip");
});

t.test("the fetch names are exported and are the globals", (a) => {
    for (const n of FETCH) {
        a.eq(typeof http[n], "function", "dyna:http " + n);
        a.ok(http[n] === globalThis[n], n + " is not the global");
    }
});

t.test("streams globals are not part of the re-export surface", (a) => {
    for (const n of ["ReadableStream", "WritableStream", "TransformStream", "TextDecoderStream", "CompressionStream"]) {
        a.ok(!(n in http), "dyna:http exports " + n);
        a.eq(typeof globalThis[n], "function", n + " global");
    }
});

await t.run();
