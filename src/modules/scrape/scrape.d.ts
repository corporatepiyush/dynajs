/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:scrape
/** dyna:scrape — polite crawling: robots.txt policy, per-host pacing, jittered retries, conditional GETs, body caps, field extraction, resumable Crawl, sitemaps.
 *  Works over dyna:html's element tree; Fetcher gates every redirect hop (private hosts, https downgrades). */
declare module "dyna:scrape" {
    /** robots.txt parsing (RFC 9309). */
    class Robots implements DynResource {
        /** Parses a robots.txt body (RFC 9309). */
        constructor(text: string, opts?: { agent?: string });
        /** True when the path is allowed for the configured agent. */
        allows(path: string): boolean;
        /** The crawl delay for the agent (the largest across its combined groups), or null when unset. */
        crawlDelay(): number | null;
        /** The sitemap URLs listed in the file. */
        sitemaps(): string[];
        /** The number of rules that survived parsing in the groups applying to the agent; every group naming it (or every "*" group when none does) is combined (RFC 9309 2.2.1). */
        readonly ruleCount: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Field extraction spec: a selector per field. GC-managed: no close surface. */
    class Extractor {
        /** `spec` maps field names to { sel, attr?, all?, required?, trim?, source?, default?, as? } with as: "number" | "url" | "json". */
        constructor(spec: Record<string, unknown>, opts?: { text?: (node: import("dyna:html").HTMLElement) => string });
        /** Runs against a parsed dyna:html document; options.base resolves `as: "url"` fields. */
        run(doc: import("dyna:html").HTMLElement | import("dyna:html").HTMLElement[], opts?: { base?: string }): { ok: boolean; value: Record<string, unknown>; missing: string[] };
    }

    /** Polite HTTP retrieval with robots policy, retries and backoff. Asynchronous only (2026-11-26 owner decision, a clean break: the hidden synchronous HTTPClient hook is gone): get() and getStream() return Promises and never block the JS thread, so a Fetcher can crawl an App or HTTPServer in the same process. Argument errors throw synchronously: a missing url, a closed Fetcher, a url that is not http(s) or longer than 1023 bytes, and a private/loopback/link-local host when allowPrivateHosts is off. Everything after that rejects: transport errors once retries are spent, redirect gates (too many hops, an https downgrade, a private redirect target), header validation, `ca` loading, maxBodyBytes, and closing the Fetcher while a fetch is in flight (TypeError "Fetcher: closed while get() was in flight"). Pacing is reserved up front: concurrent get()s to one host each claim the next minDelayMs slot (stats().throttledMs sums the waits), and robots.txt is fetched once per host however many requests wait on it. */
    class Fetcher implements DynResource {
        constructor(opts: {
            agent: string;
            /** Optional transport: omitted, the Fetcher builds its own HTTPClient capped at maxBodyBytes; inject one (or a mock) to drive the policy in tests. */
            client?: unknown;
            /** A proxy URL: validated, stored, and redacted in stats(); the built-in transport connects directly, so routing through it is an injected client's job. */
            proxy?: string;
            /** A CA bundle path or inline PEM. With the built-in transport it REPLACES the platform trust store for https, in get(), getStream() and Crawl alike; it is loaded at the first https request, so a missing file or unreadable PEM throws there (TypeError "Fetcher: ca: ..."). An injected `client` keeps its own trust. stats() redacts it. */
            ca?: string;
            /** Connection pool size 1..64 (default 4): validated and echoed; the built-in transport opens one connection per request. */
            poolSize?: number;
            /** A dyna:http CookieJar for the built-in transport: get(), getStream() and Crawl send its cookies and store every response's Set-Cookie (cookies follow their own Domain/Path rules, unlike a Cookie header, which is refused). With an injected `client` it is a TypeError: call client.useCookies(jar) instead. */
            cookieJar?: import("dyna:http").CookieJar;
            /** Extra request headers; User-Agent stays the crawler's own, and credential headers (Authorization/Cookie/...) are refused: they would leak to redirect targets. */
            headers?: Record<string, string>;
            /** Conditional GETs against stored validators. Default true. */
            revalidate?: boolean;
            /** robots.txt refresh interval in ms (default 24h; 0 re-fetches per request); a failed refresh keeps the last good copy. */
            robotsTtlMs?: number;
            /** Follow a redirect that moves https to http. Default false. */
            allowInsecureDowngrade?: boolean;
            /** Default true: robots.txt is fetched, cached and enforced. */
            robots?: boolean;
            /** Per-host politeness floor, ms. Default 1000. */
            minDelayMs?: number;
            /** Retries per request on 429/5xx/transport errors. Default 3. */
            retries?: number;
            /** Redirect hops chased. Default 5. Only 301/302/303/307/308 WITH a Location are redirects (fetch standard 4.4): a 304, a 300, or any 3xx without Location is returned as the response, never counted against this budget. A Location resolving back to the same url is returned too. */
            maxRedirects?: number;
            /** Response-body cap in bytes. Default 8 MiB. */
            maxBodyBytes?: number;
            /** Default false: private/loopback/link-local targets refused by the SSRF gate; true allows them. An IP literal is judged by its address alone: IPv6 ::, ::1, fe80::/10, fc00::/7, ff00::/8, 2001::/23, 2001:db8::/32, 64:ff9b:1::/48 and ::ffff:0:0/96 or 64:ff9b::/96 embedding a private IPv4 are refused, any other IPv6 address is fetched. */
            allowPrivateHosts?: boolean;
            /** retries/maxRedirects/maxBodyBytes/minDelayMs/robotsTtlMs must fit int32 (RangeError beyond, never truncated). */
        });
        /** GET a URL; resolves with the HTTP response with url/fromCache/notModified added. An injected `client` needs request(method, url, body, headers) returning the response or a Promise of it. */
        get(url: string): Promise<FetcherResponse>;
        /** The crawl statistics; `savedBytes` sums cached-body sizes served on revalidation. */
        stats(): {
            fetched: number; skippedByRobots: number; retried: number;
            throttledMs: number; bytes: number; revalidated: number; savedBytes: number;
            /** The stored simple options, redacted: proxy userinfo becomes `***@`, a PEM `ca` becomes `<pem-redacted>`. */
            proxy: string | null; ca: string | null; poolSize: number;
        };
        /** Fetches under get()'s policy (robots, pacing, retries, redirect gates, maxBodyBytes) but returns a streaming FetcherStream; no revalidation or response decorations. Shares dyna:http's non-blocking transport and parser (the stream is an HTTPClient StreamedResponse with the final url): chunk sizes are strict 1*HEXDIG (RFC 9112 7.1: " 5", "+5", "0x5" reject the read), header lines are validated as in HTTPClient, and https verifies against `ca` or the platform store. It always uses a built-in HTTPClient (an injected `client` is not used for streams) with a 15 s per-read timeout, `cookieJar` and `ca`; with allowPrivateHosts off its onConnect refuses a connection whose resolved peer address is private (a DNS-rebinding guard; it rejects). A declared Content-Length over maxBodyBytes rejects with RangeError "Fetcher: declared Content-Length ... exceeds maxBodyBytes"; a transport failure once retries are spent rejects with RangeError "Fetcher: no response (...) after N attempt(s)". A robots-refused url resolves with a status-0 stream already at its end. */
        getStream(url: string): Promise<FetcherStream>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A fetched page; on revalidation the status stays 304 with the cached body and its stored content-type. */
    interface FetcherResponse {
        status: number;
        statusText: string;
        /** status is 200..299. */
        ok: boolean;
        headers: Record<string, string>;
        contentType: string;
        /** The body decoded as text; there is no `text` field. */
        body: string;
        /** The body as received, before text decoding (an ArrayBuffer, not a view). */
        bodyBytes: ArrayBuffer;
        url: string;
        fromCache: boolean;
        notModified: boolean;
        /** X-Robots-Tag directives that apply to this fetcher's agent. */
        robotsDirectives?: string[];
        /** The `Link: rel="canonical"` target (RFC 8288), resolved against the request url. */
        canonicalUrl?: string;
    }

    /** The getStream response: head fields plus a read(buf)/close() body, consumed directly by lines() and ndjson(), or as a ReadableStream through toReadableStream(body). */
    interface FetcherStream {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        /** The final url after redirect chasing. */
        url: string;
        contentType: string;
        /** Present (true) on a robots-refused fetch: status 0, body at EOF. */
        skippedByRobots?: boolean;
        /** Fills the caller's buffer and resolves with the count; 0 = end of body. Rejects on truncation, maxBodyBytes overflow ("HTTPBodyStream: body exceeds maxBodyBytes") or malformed chunking. */
        read(buf: Uint8Array): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** One page emitted by a Crawl. */
    interface CrawlPage {
        /** The page URL (seed and relative hrefs resolved). */
        url: string;
        /** Crawl depth of this page (seed is depth 0). */
        depth: number;
        /** HTTP status of the fetch. */
        status: number;
        /** The Extractor's fields for this page. */
        value: Record<string, unknown>;
        /** Merged X-Robots-Tag + meta-robots directive list, when either exists. */
        robots?: string[];
    }

    /** Bounded traversal over a Fetcher plus an Extractor; relative links resolve against the page that emitted them. */
    class Crawl implements DynResource {
        constructor(fetcher: Fetcher, opts?: {
            /** Defaults: maxPages 100, maxDepth 2, sameHost true; all three serialize()/resume(). */
            maxPages?: number; maxDepth?: number; sameHost?: boolean;
            linkField?: string; baseField?: string;
            /** rel=canonical dedup: duplicate pages emit but queue no links. */
            canonicalField?: string;
            /** Parallel array of link rel strings; nofollow slots are skipped. */
            relField?: string;
            /** Extractor field holding the page's <meta name="robots"> content: its nofollow gates link following. */
            robotsField?: string;
            /** Pages fetched in parallel (1..16, default 1). Per-host politeness is unchanged. */
            concurrency?: number;
        });
        /** Seeds the crawl with an http(s) url; `parse` is the document parser, normally dyna:html's HTMLParse. */
        start(seed: string, extractor?: Extractor, parse?: (html: string) => import("dyna:html").HTMLElement[]): this;
        /** One page per call, always a Promise (2026-11-26: the synchronous concurrency-1 path and the sync iterator were removed with the synchronous Fetcher); drain with for await. Each page is fetched with the Fetcher's get() (an object with a get(url) method works too), so revalidation applies, and page.url is the final url after redirects, the base for its relative links. A fetch failure rejects that next(); closing the Fetcher mid-crawl rejects with TypeError "Crawl: the fetcher was closed while the crawl was in flight". */
        next(): Promise<IteratorResult<CrawlPage>>;
        /** The crawl itself as a lazy async page iterator: one fetch per next(), never collected eagerly. */
        pages(): AsyncIterableIterator<CrawlPage>;
        /** Async iteration: for await (const page of crawl). */
        [Symbol.asyncIterator](): AsyncIterator<CrawlPage>;
        /** The resumable state as versioned JSON: frontier, visited keys, bounds, counters. In-flight fetches and live objects are not part of it. */
        serialize(): string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
        /** Rebuilds a crawl from serialize() output with fresh fetcher/extractor/parse; the remaining pages and their order match the donor's. */
        static resume(fetcher: Fetcher, state: string, extractor?: Extractor,
                      parse?: (html: string) => import("dyna:html").HTMLElement[]): Crawl;
    }

    /** Sitemap reader for the URLs Robots.sitemaps hands out. */
    namespace Sitemap {
        /** Parses sitemap XML (or a <sitemapindex>) into its <loc> list in document order. Bounds: 16 MiB, 50000 entries, 2048-byte <loc>. */
        function parse(xml: string): string[];
        /** Fetch + parse + flatten, following a <sitemapindex> ONE level; asynchronous (resolves with the urls). Rejects with RangeError on non-2xx, more than 1000 child sitemaps, or more than 50000 URLs in total. `source` is a Fetcher (or any object with an async get(url)), or an options bag for a Fetcher of its own, which is closed when the list settles; a bad options bag throws synchronously. */
        function list(url: string, source?: Fetcher | {
            agent?: string; headers?: Record<string, string>;
            minDelayMs?: number; allowPrivateHosts?: boolean;
            maxBodyBytes?: number; retries?: number;
        }): Promise<string[]>;
    }
}
