// tests/httpc.js -- a non-blocking HTTP client for suites that talk to a
// server running in their OWN process. The blocking HTTPClient methods would
// deadlock there (the server needs the event loop the client is holding), and
// the public promise-based client is fetch(). Suites whose subject is the
// server import this file for its side effect: it gives HTTPClient the three
// promise-returning helpers below, built on the engine entry point fetch()
// itself uses. They exist in tests only; they are not part of dyna:net.
import { HTTPClient } from "dyna:net";

const PENDING = Symbol.for("dyna.http.request");
if (typeof HTTPClient.prototype[PENDING] !== "function")
    throw new Error("tests/httpc.js: this build has no pending-request entry point on HTTPClient");

Object.defineProperties(HTTPClient.prototype, {
    requestAsync: { configurable: true, writable: true, value(...a) { return this[PENDING](...a); } },
    getAsync: { configurable: true, writable: true, value(url, headers) { return this[PENDING]("GET", url, undefined, headers); } },
    postAsync: { configurable: true, writable: true, value(url, body, headers) { return this[PENDING]("POST", url, body, headers); } },
});

export { PENDING };
