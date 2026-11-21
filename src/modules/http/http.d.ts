/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:http
/** dyna:http — the web platform: WHATWG fetch/Request/Response/Headers/FormData, HTTP servers (App, HTTPServer, HTTPServerAsync), WebSockets, SSE, header codecs.
 *  Its own module since B1e: dyna:net no longer re-exports it. The global fetch gets its transport from this module; a build without it rejects http(s) fetch with a TypeError naming "http". */
declare module "dyna:http" {
    /** A parsed Content-Type header. */
    interface ContentType {
        type: string;
        subtype: string;
        parameters: Record<string, string>;
    }
    /** Parses a Content-Type header into {type, subtype, parameters}; null when malformed. */
    function ContentTypeParse(header: string): ContentType | null;
    /** Formats a Content-Type back to a header string. */
    function ContentTypeFormat(ct: ContentType): string;
    /** Content negotiation: the best candidate STRING for an Accept header, or null; q=0 excludes, ties follow the header's order; every candidate must be a string (a hole included, TypeError naming the index). Weights (RFC 9110 12.4.2) are read exactly as App's compress reads Accept-Encoding: BWS around "=" is tolerated ("q = 0" excludes), a q that is not a qvalue ("abc", "2", "1.5") is ignored (weight 1), and digits past the third never turn a non-zero q into 0. */
    function Negotiate(header: string, candidates: string[]): string | null;
    /** Picks the best supported token by q-value (weights read as Negotiate reads them); "*" selects the first candidate; candidates follow Negotiate's string rule. */
    function NegotiateToken(header: string, candidates: string[]): string | null;
    /** Parses a Range header against a size: inclusive {start, end} ranges, "unsatisfiable" when none fits, null when malformed. */
    function RangeParse(header: string, size: number): { start: number; end: number }[] | "unsatisfiable" | null;
    /** Parses a Cookie header into an object; a repeated name keeps the LAST value. */
    function CookieParse(header: string): Record<string, string>;
    /** CookieSerialize options; STRICT bag (an unknown key is a TypeError naming it). */
    interface CookieOptions {
        maxAge?: number;
        domain?: string;
        path?: string;
        sameSite?: "Strict" | "Lax" | "None" | "strict" | "lax" | "none";
        secure?: boolean;
        httpOnly?: boolean;
    }
    /** Serializes a cookie; absent options emit exactly `name=value`. */
    function CookieSerialize(name: string, value: string, opts?: CookieOptions): string;
    /** True when the If-None-Match header matches the etag. */
    function ETagMatch(header: string, etag: string): boolean;
    /** Parses a multipart body against the boundary in the Content-Type header. */
    function MultipartParse(contentType: string, body: ByteView): { name: string; filename?: string; body: Uint8Array }[];
    /** Formats multipart parts; returns {body, contentType}. */
    function MultipartFormat(parts: { name: string; value?: string; body?: ByteView; filename?: string }[], boundary?: string): { body: Uint8Array; contentType: string };

    /** A non-blocking HTTP/1.1 client on the event loop's reactor (T1, 2026-11-26: the blocking get/post/request/getStream were removed, a clean break). Every exchange returns a Promise and never blocks the JS thread, so a client can call an App, HTTPServer or TCPServer in the same process. It shares fetch()'s transport and connection pool: requests to one origin reuse idle pooled connections (kept at most 4 s, 256 per process); at most 64 exchanges per origin (host, port, scheme) are in flight at once per thread by default (dyna.maxPerOrigin raises or removes the cap) and further requests wait FIFO for a freed slot (RFC 9112 9.4; it also keeps one origin from overflowing a listen backlog, 128 by default on macOS), the wait is not cut by the per-read timeout but is bounded by the total deadline (20x the timeout); a getStream response gives back its slot once its head has arrived, so open streams never starve other requests; a reused connection that died before any response byte is retried once on a fresh one only for replayable requests (GET, HEAD, OPTIONS, or an Idempotency-Key header); a non-replayable request lost that way rejects (RFC 9110 9.2.2) and the origin is remembered, so the next one opens its own connection. Choose this over fetch for explicit timeout/maxBody/onConnect control and a plain {status, headers, body} result; scrape's Fetcher runs its policy over this client. Argument errors (url, method, header names/values, a closed client, https without the tls module) throw synchronously; network, framing and size errors reject. */
    /** dyna meta configuration of HTTPClient and fetch() (an extension beyond RFC 9110/9112), a strict bag: an unknown key is a TypeError. */
    interface HTTPClientMetaOptions {
        /** The most exchanges this client keeps in flight to one origin (scheme, host, port) at once; default 64. An integer in [1, 2^31-1] (RangeError otherwise, TypeError for a non-number) or Infinity for no cap. The origin table is shared per thread: an exchange is admitted while the origin's total in flight, across every client and fetch(), is under its OWN cap, otherwise it waits FIFO; an Infinity caller never waits, and it never admits a capped caller past that caller's cap. */
        maxPerOrigin?: number;
    }

    class HTTPClient implements DynResource {
        /** maxBody caps the buffered response (default 16 MiB); a response over it throws RangeError. HTTPS verifies certificate and hostname against the platform trust store. opts is a strict bag (unknown key: TypeError) holding only the dyna meta bag, see HTTPClientMetaOptions. */
        constructor(maxBody?: number, opts?: { dyna?: HTTPClientMetaOptions });
        /** GET resolving to {status, statusText, ok, headers, body}. Rejects with RangeError when the response passes maxBody. */
        get(url: string, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** POST with an optional body; same contract as get(). */
        post(url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** Any method; same contract as get(). */
        request(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** Called once per request with the RESOLVED IP between connect and the first request byte; return false or throw to refuse the connection. */
        onConnect?: (ip: string) => unknown;
        /** Per-read/per-write socket timeout in ms (0 disables); a buffered exchange is also bounded as a whole at 20x this value. */
        setTimeout(ms: number): void;
        /** Cancels this client's in-flight exchanges (each rejects, requests still waiting for a connection slot and a getStream body's pending read included) and closes the idle pooled connections it opened; a request made afterwards runs normally. */
        disconnect(): void;
        /** Resolves once the response head arrives, with the body left on the connection: the StreamedResponse's read(buf) resolves as bytes arrive (one pending read at a time; a second rejects TypeError) and never blocks the JS thread. Bytes the server sends ahead of read() are buffered, bounded by maxBody, which counts the whole body (a chunked trailer section included); a declared Content-Length over maxBody rejects the getStream promise, a running total over it rejects read() with RangeError "HTTPBodyStream: body exceeds maxBodyBytes". Body framing and truncation errors after the head reject read(), not getStream. The per-read timeout (setTimeout) runs only while a read() is pending, so a long-lived stream can idle between reads. close() cancels the exchange and settles a pending read with 0. A stream read to its end leaves its connection in the pool like get(). Framing per RFC 9112 6: Transfer-Encoding is chunked only when chunked is its final coding; Content-Length with chunked, or two different Content-Length values, rejects "malformed response: ..."; Content-Length with a non-chunked coding frames by Content-Length, as request() does. Chunk sizes are strict 1*HEXDIG. */
        getStream(url: string, headers?: Record<string, string>): Promise<StreamedResponse>;
        /** Attaches a CookieJar (null detaches): every later get/post/request/getStream and fetch-style async exchange sends the jar's Cookie for its url unless the call passes its own Cookie header, and stores each response's Set-Cookie lines. The jar is shared, not copied. */
        useCookies(jar: CookieJar | null): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** The buffered response an HTTPClient promise resolves to. */
    interface HTTPResponse {
        status: number;
        statusText: string;
        ok: boolean;
        /** One key per field name, spelled as the server first spelled it (names match case-insensitively). RFC 9110 5.3: a repeated list field joins as "v1, v2"; Set-Cookie lines join with "\n" (a cookie's Expires holds a comma); a single-value field (Content-Length, Content-Type, Location, ETag, Last-Modified, Date, Expires, Age, Retry-After, Content-Location, Content-Range, ...) repeated with a different value fails the request ("malformed response: conflicting <name> values"), an identical repeat is one value. Values lose surrounding whitespace. getStream, Fetcher and fetch() read heads the same way. Over https an unframed body must end with close_notify, else "truncated body". */
        headers: Record<string, string>;
        body: string;
    }

    /** The getStream response: head fields plus a read(buf)/close() body: lines() and ndjson() consume it directly; toReadableStream(body) makes it a ReadableStream (pipeTo, pipeThrough(new DecompressionStream(...))). */
    interface StreamedResponse {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        /** The final url of the exchange (one hop: no redirect chasing). */
        url: string;
        contentType: string;
        /** Fills the caller's buffer and resolves with the count; 0 = end of body. Rejects on truncation, the max body, or malformed chunked framing. */
        read(buf: Uint8Array): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A static route table: path -> body or {status, contentType, body}. App runs JS handlers; HTTPServer and HTTPServerAsync serve these tables off-thread. */
    type StaticRoutes = Record<string, string | { status?: number; contentType?: string; body?: string }>;

    /** A static-route HTTP/1.1 server on its own reactor thread (any client in the same script can call it, as it can an App); binds in the constructor. Every final response (status 200 and up; not 101) carries one `Date: <IMF-fixdate>` line straight after the status line (RFC 9110 section 6.6.1, https://www.rfc-editor.org/rfc/rfc9110#section-6.6.1), taken from a per-thread line rebuilt at most once per second from the wall-clock second the thread's reactor read when it last woke (no clock read per response). */
    class HTTPServer implements DynResource {
        /** Defaults: port 0 (ephemeral, read it back from `.port`), all interfaces, requestTimeoutMs 30000; a request is capped at 16 KiB (431/413). `workers` (default 1, clamped to 1..64) is the number of reactor threads start() runs; each serves the connections it accepted whole, and stop() joins them all. On Linux each reactor accepts on its own SO_REUSEPORT listener: it helps from about 16 concurrent connections (with 4, the kernel's hash can put two on one reactor). On macOS the reactors share one listening socket: only pipelined or keep-alive loads that saturate one reactor gain, while unpipelined and connection-per-request traffic stays kernel-bound and costs more CPU per request. The 8192-connection cap applies per reactor. */
        constructor(opts?: {
            port?: number;
            host?: string;
            workers?: number;
            backlog?: number;
            requestTimeoutMs?: number;
            routes?: StaticRoutes;
        });
        /** Binds the listener and starts serving. A started server stays alive until stop() or close(), as in node, even when its handle is dropped (a GC root from start() to stop()/close()); an unstarted, stopped or closed one is collectable. */
        start(): void;
        /** Stops accepting, closes live connections, and joins every reactor thread; the server is no longer a GC root. */
        stop(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A static-route server multiplexing every connection on one background kqueue/epoll thread; a request is capped at 1 MiB. Every final response (status 200 and up; not 101) carries one `Date: <IMF-fixdate>` line straight after the status line (RFC 9110 section 6.6.1, https://www.rfc-editor.org/rfc/rfc9110#section-6.6.1), taken from a per-thread line rebuilt at most once per second from the wall-clock second the thread's reactor read when it last woke (no clock read per response). */
    class HTTPServerAsync implements DynResource {
        /** Defaults: port 0 (ephemeral), all interfaces, idleTimeoutMs 30000 (restarted by a request consumed or response bytes the peer accepts), maxConns 8192 (0 = unbounded). `workers` (default 1, clamped to 1..64) is the number of reactor threads start() runs; each serves the connections it accepted whole, and stop() joins them all. Routes are static and served in C, so workers parallelise the whole request path. On Linux each reactor accepts on its own SO_REUSEPORT listener: it helps from about 16 concurrent connections (with 4, the kernel's hash can put two on one reactor). On macOS the reactors share one listening socket: only pipelined or keep-alive loads that saturate one reactor gain, while unpipelined and connection-per-request traffic stays kernel-bound and costs more CPU per request. maxConns applies per reactor. */
        constructor(opts?: {
            port?: number;
            host?: string;
            backlog?: number;
            idleTimeoutMs?: number;
            maxConns?: number;
            routes?: StaticRoutes;
            workers?: number;
        });
        /** Binds the listener and starts serving. A started server stays alive until stop() or close(), as in node, even when its handle is dropped (a GC root from start() to stop()/close()); an unstarted, stopped or closed one is collectable. */
        start(): void;
        /** Stops accepting, closes live connections, and joins every reactor thread; the server is no longer a GC root. */
        stop(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A routed application server: handlers are plain JS on the event-loop thread through the shared reactor; binds at start(). Every final response (status 200 and up; not 101) carries one `Date: <IMF-fixdate>` line straight after the status line (RFC 9110 section 6.6.1, https://www.rfc-editor.org/rfc/rfc9110#section-6.6.1), taken from a per-thread line rebuilt at most once per second from the wall-clock second the thread's reactor read when it last woke (no clock read per response). A handler that sets its own Date header (any case) has it sent instead, once. */
    class App implements DynResource {
        /** Defaults: port 0, all interfaces, idleTimeoutMs 30000, maxConns 8192, compress true, metrics false. The idle clock restarts on a request consumed and on response bytes the peer accepts, so a download slower than idleTimeoutMs completes while a peer that reads nothing is closed. Compression is gzip for accepting clients, skipped for image/video/audio/archive/woff types, and done on the io thread pool from 8 KiB up unless its threads are held by blocking work, then on the reactor (replies stay in request order). */
        constructor(opts?: { port?: number; host?: string; idleTimeoutMs?: number; maxConns?: number; compress?: boolean; metrics?: boolean;
            /** listen(2) backlog; integer 1..65535, default 1024; the kernel caps it at somaxconn (macOS 128). */
            backlog?: number;
            /** Validated (1..64) and echoed for config parity with HTTPServer; App is single-reactor, so it sizes nothing. Default 1. */
            workers?: number; });
        /** Registers a strict JSON-RPC 2.0 endpoint (typed route: bypasses use()). */
        rpc(path: string, methods: Record<string, (...args: unknown[]) => unknown>): this;
        // An rpc handler may return a ByteSource or a ReadableStream (or { stream, contentType?, status?, headers? }; a locked ReadableStream is a 500) instead of JSON: the reply is then chunked, outside the JSON-RPC envelope.

        /** Serves a document root at a URL prefix; exact and dynamic routes win over the mount, whichever was registered first. `allow` lists extensions or media types (else 403); maxFileSize defaults to 32 MiB (413); `..` is 403, dotfiles 404, the path is not percent-decoded. Symlinks resolve only while they stay under the root: a link (or `..` inside a link target) that leaves it is 403, a dangling or looping one 404. The root is reopened by path when it is renamed or replaced, so the new directory serves at once. Each open walks the path one component at a time with O_NOFOLLOW, plus a realpath containment check. */
        static(prefix: string, root: import("dyna:file").Path, opts?: { maxFileSize?: number; allow?: string[] }): this;
        /** Proxies a URL prefix to a host/port; exact and dynamic routes win over the mount. */
        proxy(prefix: string, opts: { host?: string; port?: number }): this;
        /** Upload endpoint: the handler gets the saved path and {size, contentType}; a non-matching declared Content-Type is 415, a body over maxFileSize (default 16 MiB) is 413; a refusal is sent with Connection: close while the unread body is drained (at most 64 MiB or 5 s, whatever idleTimeoutMs is), so the client can read it.
         *  The client gets `{ok: true, id}` (id = the saved file's basename, never the server path), or the handler's returned object as JSON. */
        upload(path: string, opts: { dir?: import("dyna:file").Path; maxFileSize?: number; allow?: string[] }, handler: (savedPath: string, meta: { size: number; contentType: string }) => object | void): this;
        /** WebSocket endpoint. `upgrade` runs BEFORE the 101 with origin/host/cookie/protocol; a falsy return or throw answers 403 — check `origin` when cookies authenticate. */
        ws(path: string, handlers: { upgrade?: (req: { origin: string | null; host: string | null; cookie: string | null; protocol: string | null }) => unknown; open?: (socket: WsConn) => void; message?: (socket: WsConn, data: string | Uint8Array, isBinary: boolean) => void; close?: (socket: WsConn, code: number, reason: string) => void }): this;
        /** Server-sent events endpoint; open and close receive the stream. */
        sse(path: string, handlers: { open?: (stream: SseConn) => void; close?: (stream: SseConn) => void }): this;
        /** Dynamic route for one method. Pattern segments: literal, `:param` (one decoded segment) or a trailing `*rest`; first registered wins; a method mismatch is 405.
         *  The return is the response: string, bytes, the strict envelope {status?, body?, contentType?, headers?} (header values: string or string[]; server-owned names are a 500), or any value as JSON; thenables are awaited; a throw is a 500. */
        get(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for POST. */
        post(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PUT. */
        put(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PATCH. */
        patch(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for DELETE (the method name cannot be a keyword). */
        del(pattern: string, handler: (req: AppRequest) => unknown): this;
        /** Appends SYNC middleware run before matched dynamic handlers with the same AppRequest; returning { response } short-circuits. Typed routes (rpc/static/ws/...) bypass it. */
        use(fn: (req: AppRequest) => { response?: unknown } | void): this;
        /** Binds the listener and starts serving; the App holds the event loop from here until close(), not from construction. A started App stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        /** The ctor's workers option (default 1); echoed only. */
        get workers(): number;
        /** The effective listen(2) backlog: the ctor option, or 1024. */
        get backlog(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Server-side WebSocket connection, handed to App.ws handlers. */
    interface WsConn {
        send(data: BytesInput): void;
        close(): void;
        /** Open: bytes queued on the socket, including framing and the handshake response (as Node ws bufferSize); a send passing 4 MB closes the connection.
         *  Closed: starts at 0; each later send adds its data's byte length, never sent (WHATWG WebSocket send()). */
        readonly bufferedAmount: number;
    }

    /** Server-side SSE stream, handed to the App.sse open handler. */
    interface SseConn {
        send(data: string): void;
        close(): void;
        /** As WsConn.bufferedAmount (SSE has no standard): open, bytes queued on the socket with event framing, past 4 MB closes;
         *  closed, starts at 0 and each later send adds its data's byte length, never sent. */
        readonly bufferedAmount: number;
    }

    /** The request context handed to dynamic-route handlers and `app.use` middleware. */
    interface AppRequest {
        /** The method as sent; matching is exact and case-sensitive (HEAD is not aliased to GET). */
        method: string;
        /** The raw request path (percent-encoding as received), query stripped. */
        path: string;
        /** `:param` and `*rest` captures, percent-decoded; {} when the pattern has none. */
        params: Record<string, string>;
        /** The query string, percent-decoded (`+` is a space); a bare key is "", a repeated key keeps the LAST value. */
        query: Record<string, string>;
        /** Headers with lower-cased names; list fields join with ", ", Cookie with "; "; any other duplicate is a 400 before dispatch. */
        headers: Record<string, string>;
        /** The raw body as a string; the whole request is bounded at 1 MiB (body 413, headers 431, target 414). */
        body: string;
    }

    /** A WebSocket client. */
    class WsClient implements DynResource {
        /** Connects to the URL and dispatches open/message/close to the handlers. */
        constructor(url: string, handlers: { open?: (ws: WsClient) => void; message?: (ws: WsClient, data: string | Uint8Array, isBinary: boolean) => void; close?: (ws: WsClient, code: number, reason: string) => void });
        /** Sends a text frame (string) or binary frame (bytes). A closed connection silently drops the frame. Frames sent from an open/message handler leave together in one socket write when that handler returns (frames sent from a timer or promise go out at once); close() still sends every earlier frame first. */
        send(data: BytesInput): void;
        /** Performs polite teardown: a close frame first, then the connection. */
        close(): void;
        /** Performs polite teardown: a close frame first, then the connection. */
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** RFC 6265 cookie store (src/core/httpk), shared by HTTPClient.useCookies, fetch's `cookieJar` and Fetcher's `cookieJar`. Storage model RFC 6265 5.3: host-only unless a Domain attribute domain-matches the request host (a single-label Domain is ignored; there is no public-suffix list, so Domain=co.uk from a.b.co.uk is accepted); default path is the request path's directory; Secure cookies only from and to https; __Secure-/__Host- prefixes enforced (RFC 6265bis 4.1.3); Max-Age wins over Expires, Max-Age<=0 deletes; cookies of 4096+ name+value bytes are ignored; when full, the oldest-created cookie goes. In memory only, per jar; not thread-shared. */
    class CookieJar implements DynResource {
        /** maxCookies 1..1000000, default 3000 (RFC 6265 6.1's minimum). */
        constructor(opts?: { maxCookies?: number });
        /** Applies one Set-Cookie line as if `url` had sent it; a url that is not http(s) or holds a NUL, or a line with a CTL other than HTAB (RFC 6265bis 5.6 step 1), stores nothing ("ignored"). */
        setCookie(url: string, setCookie: string): "stored" | "deleted" | "ignored";
        /** The Cookie header value for a request to `url` ("" when none or the url holds a NUL): longer paths first, then older cookies (RFC 6265 5.4). */
        cookieHeader(url: string): string;
        /** Stores every Set-Cookie line of a response headers object (HTTPResponse.headers joins them with "\n"); returns how many were stored (0 for a url holding a NUL).
         * Throws what a getter or Proxy trap of `headers` throws, and on out of memory; lines stored before the throw stay stored. */
        storeResponse(url: string, headers: Record<string, string>): number;
        clear(): void;
        /** Live cookies (expired ones are purged first). */
        readonly size: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** WHATWG fetch subset (no credentials/integrity keys). It shares HTTPClient's transport and connection pool, so by default at most 64 exchanges per origin are in flight at once per thread and further requests wait FIFO for a slot (see HTTPClient; init.dyna.maxPerOrigin changes the cap). Follows at most 20 redirects unless `redirect` is "manual" (the 3xx is returned) or "error" (rejects); the redirect rule is httpk's (fetch standard 4.4): only 301/302/303/307/308 with a Location redirect; 301/302 turn only POST into GET and 303 turns every method but HEAD into GET, dropping the body and its Content-Type/-Encoding/-Language/-Location; authorization/cookie headers are dropped when a hop leaves the origin (scheme, host and port); with a `cookieJar` each hop sends that jar's cookies for its own url and stores the hop's Set-Cookie. An exchange runs on the shared reactor and holds no io pool thread (only the name lookup uses the pool). Connections are kept alive: up to 256 idle ones wait 4 s for the next request to the same host and are probed before reuse; a request that dies on a reused connection before any response byte is retried once on a fresh one only for GET/HEAD/OPTIONS, with an Idempotency-Key header, or when the kernel still holds every byte of it unacknowledged (the peer had closed before it arrived, so it was never read; TCP on macOS and Linux), and a host seen dropping one gets fresh connections for other methods for 60 s; idle ones never keep the process running. */
    function fetch(input: string | Request, init?: RequestInit): Promise<Response>;
    /** The web-platform classes, the same objects as the globals. */
    const Request: RequestConstructor;
    const Response: ResponseConstructor;
    const Headers: HeadersConstructor;
    const AbortController: AbortControllerConstructor;
    const AbortSignal: AbortSignalConstructor;
    const FormData: FormDataConstructor;
}
