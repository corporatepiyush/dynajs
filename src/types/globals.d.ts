//@dts-block @globals-1

/** dyna:mathx — the math toolbox: constants, special functions, combinatorics, number theory, bit ops, linear algebra, and `stats` reductions.
 *  Statistics come in three layers: Array.prototype conveniences, mathx.stats (compensated sums, Float64Array fast path), DataFrame verbs (columnar, masked, histograms). */

//@dts-block @globals-2
// ---- std / os compatibility modules, WHATWG globals, and core prototype extensions ----

//@dts-block @globals-3

// ---- WHATWG globals (Fetch, DOM AbortSignal, XHR FormData). Oracle: WPT via tools/wpt.js (fetch/api/{headers,request,response,body}, xhr/formdata) with tests/wpt-expectations.tsv; AbortSignal by tests/net/test_fetch_abort_signal_spec.js.
// Spec first (owner rule 12). Known omissions (missing surface, not deviations): Event/EventTarget, Response/Request textStream (WPT body/textstream, a proposal). AbortSignal default reasons are DOMException AbortError/TimeoutError.
// Deviations kept on purpose (server runtime, matching Deno/undici): no browser header guards (forbidden request/response header names, no-cors safelist filtering, Set-Cookie on Response headers is allowed);
// the init dictionaries are STRICT (an unknown key throws TypeError naming the valid set; WebIDL would ignore it); fetch() adds timeout/onConnect/cookieJar; ResponseInit adds url; a FormData value may also be a byte buffer
// (Uint8Array/ArrayBuffer, sent as a file part, the filename reaches the part); redirect "manual" returns the 3xx response itself (type "basic", not "opaqueredirect"); a cross-origin redirect strips Authorization, Cookie and Proxy-Authorization (the spec strips Authorization only).

/** Identity for `using` disposal. */
interface SymbolConstructor {
    readonly dispose: symbol;
    readonly asyncDispose: symbol;
}

/** WHATWG AbortSignal (DOM "Interface AbortSignal"). Not constructible (TypeError); get one from AbortController, AbortSignal.abort/timeout/any or Request.signal.
 *  Listeners: only "abort" is stored; a listener is a function or an object with handleEvent; duplicates (same callback and capture) are ignored; options once, capture, passive and signal (an aborted options.signal removes it) are honoured; a listener added after the abort is never called.
 *  Abort order (spec "signal abort"): reason set, dependents marked aborted, this signal's listeners (onabort in its first-assignment position), then each dependent's listeners. A listener that throws is reported (rethrown in a microtask) and dispatch continues.
 *  Default reasons are DOMException instances: AbortError "signal is aborted without reason" (abort) and TimeoutError "signal timed out" (timeout). The event is a plain { type: "abort", target, currentTarget } object (no Event class). */
interface AbortSignal {
    readonly aborted: boolean;
    readonly reason: unknown;
    onabort: ((this: AbortSignal, ev: { type: string; target: AbortSignal; currentTarget: AbortSignal }) => void) | null;
    addEventListener(type: "abort", listener: ((ev: { type: string; target: AbortSignal; currentTarget: AbortSignal }) => void) | { handleEvent(ev: { type: string; target: AbortSignal }): void } | null, options?: boolean | { capture?: boolean; once?: boolean; passive?: boolean; signal?: AbortSignal }): void;
    removeEventListener(type: "abort", listener: ((ev: { type: string; target: AbortSignal }) => void) | { handleEvent(ev: { type: string; target: AbortSignal }): void } | null, options?: boolean | { capture?: boolean }): void;
    throwIfAborted(): void;
}
interface AbortSignalConstructor {
    abort(reason?: unknown): AbortSignal;
    /** Aborts with a TimeoutError after `delayMs` (a negative or non-finite delay is a TypeError). fetch() clears a timeout signal's timer when the fetch settles. */
    timeout(delayMs: number): AbortSignal;
    /** A dependent signal: aborted now if any source already is (with the first such reason), else when the first source aborts. Every element must be an AbortSignal (TypeError). */
    any(signals: Iterable<AbortSignal>): AbortSignal;
}
declare const AbortSignal: AbortSignalConstructor;

/** WHATWG AbortController: abort() fires its `signal`. */
interface AbortController {
    readonly signal: AbortSignal;
    abort(reason?: unknown): void;
}
interface AbortControllerConstructor {
    new (): AbortController;
}
declare const AbortController: AbortControllerConstructor;

type HeadersInit = Headers | [string, string][] | Iterable<Iterable<string>> | Record<string, string>;
/** WHATWG Headers. Names must be HTTP tokens and values may not contain NUL/CR/LF or code points above U+00FF (TypeError); values are trimmed of HTTP whitespace (tab, LF, CR, space).
 *  A repeated name combines with ", " (get() returns the combined value) except set-cookie, whose values stay separate (getSetCookie(), and one iteration entry each).
 *  Iteration (keys/values/entries/forEach/for-of) is the spec's "sort and combine": names lower-cased and sorted, and it is live: each next() re-reads the current list at the iterator's index.
 *  init: a sequence of [name, value] pairs (each exactly 2 items, else TypeError) or a record (own enumerable string keys; a Symbol key is a TypeError), all converted before any is added.
 *  Headers of a fetched response, of Response.error() and of Response.redirect() are immutable (TypeError on append/set/delete). */
interface Headers {
    append(name: string, value: string): void;
    delete(name: string): void;
    get(name: string): string | null;
    getSetCookie(): string[];
    has(name: string): boolean;
    set(name: string, value: string): void;
    forEach(cb: (value: string, key: string, parent: Headers) => void, thisArg?: unknown): void;
    keys(): IterableIterator<string>;
    values(): IterableIterator<string>;
    entries(): IterableIterator<[string, string]>;
    [Symbol.iterator](): IterableIterator<[string, string]>;
}
interface HeadersConstructor {
    new (init?: HeadersInit): Headers;
}
declare const Headers: HeadersConstructor;

/** WHATWG FormData (XHR spec): an ordered list of [name, value] entries. Names and string values are USVStrings (lone surrogates become U+FFFD); a non-buffer value is String()-converted (undefined -> "undefined").
 *  Iteration is live (each next() reads the current list at the iterator's index). Extension: a value may be a Uint8Array or ArrayBuffer, sent as a file part; a filename is allowed only with a Blob or such a value (a string value with a filename is a TypeError, as WebIDL overload resolution picks the Blob overload for three arguments). */
interface FormData {
    /** A Blob value is stored as a File (named filename, or "blob" / the File name when omitted) and sent as a file part with its type (else application/octet-stream); Uint8Array/ArrayBuffer stay bytes; anything else becomes a USVString. */
    append(name: string, value: string | Uint8Array | ArrayBuffer | Blob, filename?: string): void;
    delete(name: string): void;
    get(name: string): string | File | Uint8Array | ArrayBuffer | null;
    getAll(name: string): (string | File | Uint8Array | ArrayBuffer)[];
    has(name: string): boolean;
    set(name: string, value: string | Uint8Array | ArrayBuffer | Blob, filename?: string): void;
    forEach(cb: (value: string | File | Uint8Array | ArrayBuffer, key: string, parent: FormData) => void, thisArg?: unknown): void;
    keys(): IterableIterator<string>;
    values(): IterableIterator<string | File | Uint8Array | ArrayBuffer>;
    entries(): IterableIterator<[string, string | File | Uint8Array | ArrayBuffer]>;
    [Symbol.iterator](): IterableIterator<[string, string | File | Uint8Array | ArrayBuffer]>;
}
interface FormDataConstructor {
    /** Takes no form: any argument other than undefined is a TypeError (there is no HTMLFormElement). */
    new (): FormData;
}
declare const FormData: FormDataConstructor;

/** Global WebSocket: thin alias over dyna:http WsClient (same constructor). Present only with the "http" module; without the optional "tls" module a wss:// URL throws a TypeError naming "tls". */
declare const WebSocket: typeof import("dyna:http").WsClient;

/** Global WebRTC classes: the same objects dyna:webrtc exports, installed on first use (TLS builds only). */
declare const RTCPeerConnection: typeof import("dyna:webrtc").RTCPeerConnection;
declare const RTCDataChannel: typeof import("dyna:webrtc").RTCDataChannel;
declare const RTCIceCandidate: typeof import("dyna:webrtc").RTCIceCandidate;
declare const RTCSessionDescription: typeof import("dyna:webrtc").RTCSessionDescription;
declare const RTCCertificate: typeof import("dyna:webrtc").RTCCertificate;
declare const RTCRtpTransceiver: typeof import("dyna:webrtc").RTCRtpTransceiver;
declare const RTCRtpSender: typeof import("dyna:webrtc").RTCRtpSender;
declare const RTCRtpReceiver: typeof import("dyna:webrtc").RTCRtpReceiver;
declare const MediaStream: typeof import("dyna:webrtc").MediaStream;
declare const MediaStreamTrack: typeof import("dyna:webrtc").MediaStreamTrack;

/** Options for fetch() and new Request() (WebIDL RequestInit; members are read in lexicographic order; enum members are validated with a TypeError). */
interface RequestInit {
    /** Extracted at construction (Fetch "extract a body"): string -> UTF-8 bytes with content-type "text/plain;charset=UTF-8"; ArrayBuffer/any ArrayBufferView -> a copy of its bytes; a Blob -> its bytes, with its type as the content-type when non-empty; URLSearchParams -> urlencoded bytes with
     *  "application/x-www-form-urlencoded;charset=UTF-8"; FormData -> the multipart bytes, serialized now, with "multipart/form-data; boundary=..." (later changes to the FormData do not affect the body);
     *  a ReadableStream (requires duplex: "half"; TypeError if locked or disturbed; fetch() reads it whole before sending); anything else -> String(body). The content-type is added only when headers lack one.
     *  A GET or HEAD request with a body (given or inherited from an input Request) is a TypeError. */
    body?: unknown;
    cache?: "default" | "no-store" | "reload" | "no-cache" | "force-cache" | "only-if-cached";
    credentials?: "omit" | "same-origin" | "include";
    /** Only "half"; required when body is a ReadableStream. */
    duplex?: "half";
    headers?: HeadersInit;
    integrity?: string;
    keepalive?: boolean;
    /** An HTTP token (TypeError otherwise); CONNECT, TRACE and TRACK are forbidden (TypeError); DELETE, GET, HEAD, OPTIONS, POST and PUT are upper-cased case-insensitively, other methods keep their case. */
    method?: string;
    /** "navigate" is a TypeError. "no-cors" allows only GET, HEAD and POST. */
    mode?: "cors" | "same-origin" | "no-cors" | "navigate";
    priority?: "high" | "low" | "auto";
    /** "follow" (default), "manual" (return the 3xx itself) or "error" (reject on a redirect). */
    redirect?: "follow" | "manual" | "error";
    /** "" means no referrer; otherwise a URL (TypeError if it does not parse). Recorded only: fetch() sends no Referer header from it. */
    referrer?: string;
    referrerPolicy?: "" | "no-referrer" | "no-referrer-when-downgrade" | "same-origin" | "origin" | "strict-origin" | "origin-when-cross-origin" | "strict-origin-when-cross-origin" | "unsafe-url";
    signal?: AbortSignal | null;
    /** Must be null or undefined (TypeError otherwise). */
    window?: null;
    /** Milliseconds until the fetch fails; bounds the WHOLE exchange (connect + body). fetch() only. */
    timeout?: number;
    /** Connect hook as HTTPClient.onConnect: resolved IP; return false or throw to refuse. fetch() only. */
    onConnect?: (ip: string) => unknown;
    /** A dyna:http CookieJar: sent and filled on every hop (fetch() only; an explicit Cookie header wins over it). */
    cookieJar?: import("dyna:http").CookieJar;
    /** dyna meta configuration (fetch() only): see HTTPClientMetaOptions; held for every redirect hop. An invalid bag rejects (TypeError or RangeError). */
    dyna?: import("dyna:http").HTTPClientMetaOptions;
}
/** WHATWG Request. The URL is parsed with URL (relative URLs resolve against globalThis.location.href when it exists); an invalid URL or one with a username/password is a TypeError.
 *  new Request(request, init) copies the input; when the body comes from the input Request, the input becomes used (bodyUsed true) and must not already be unusable (TypeError). */
interface Request {
    readonly method: string;
    readonly url: string;
    readonly headers: Headers;
    readonly destination: "";
    /** "about:client" by default, "" for no referrer, else the parsed URL. */
    readonly referrer: string;
    readonly referrerPolicy: string;
    readonly mode: "cors" | "same-origin" | "no-cors";
    readonly credentials: "omit" | "same-origin" | "include";
    readonly cache: string;
    readonly redirect: "follow" | "manual" | "error";
    readonly integrity: string;
    readonly keepalive: boolean;
    readonly isReloadNavigation: false;
    readonly isHistoryNavigation: false;
    /** Always an AbortSignal: a new signal that follows init.signal (or the input Request's signal), created at first access. */
    readonly signal: AbortSignal;
    readonly duplex: "half";
    /** The body as a byte ReadableStream, or null. */
    readonly body: ReadableStream<Uint8Array> | null;
    readonly bodyUsed: boolean;
    /** TypeError when the body is unusable; a stream body is tee()d. */
    clone(): Request;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
    json(): Promise<unknown>;
    text(): Promise<string>;
    /** The body bytes as a Blob whose type is the Content-Type header ("" when absent). */
    blob(): Promise<Blob>;
    /** Fetch formData(): multipart/form-data (fields UTF-8 decoded without BOM; file parts become Files typed by their Content-Type, else text/plain) or application/x-www-form-urlencoded (bytes UTF-8 decoded whatever the charset; a leading "?" is kept). Any other MIME type, a missing boundary or a malformed body rejects with TypeError. */
    formData(): Promise<FormData>;
}
interface RequestConstructor {
    new (input: string | URL | Request, init?: RequestInit): Request;
}
declare const Request: RequestConstructor;

/** Options for new Response() and Response.json(). */
interface ResponseInit {
    /** WebIDL unsigned short, then 200..599 or RangeError. */
    status?: number;
    /** Default "" (Fetch spec). Must be a reason-phrase (tab, space, visible ASCII or U+0080..U+00FF) or TypeError. */
    statusText?: string;
    headers?: HeadersInit;
    /** Extension: the value of Response.url. */
    url?: string;
}
/** WHATWG Response. A body with a null-body status (101, 103, 204, 205, 304) is a TypeError. Body readers on a null body resolve as empty ("" / empty bytes; json() rejects with SyntaxError) and never mark it used. */
interface Response {
    /** "default" (constructed), "basic" (from fetch()), "error" (Response.error()). */
    readonly type: "default" | "basic" | "error" | "opaqueredirect";
    readonly url: string;
    /** True when fetch() followed at least one redirect. */
    readonly redirected: boolean;
    readonly status: number;
    readonly ok: boolean;
    readonly statusText: string;
    readonly headers: Headers;
    /** The body as a WHATWG byte ReadableStream (null for a null body), created at first access and the same object after. Once body exists, text()/json()/bytes()/arrayBuffer()
     *  read through it: they reject (TypeError) if it is locked or was read from, and a stream body's chunks must be Uint8Array. fetch() buffers the whole response first, so this streams the buffered bytes. */
    readonly body: ReadableStream<Uint8Array> | null;
    /** True after a body method ran or once the body stream was read from or canceled. */
    readonly bodyUsed: boolean;
    /** The body bytes as a Blob whose type is the Content-Type header ("" when absent). */
    blob(): Promise<Blob>;
    /** Fetch formData(): multipart/form-data (fields UTF-8 decoded without BOM; file parts become Files typed by their Content-Type, else text/plain) or application/x-www-form-urlencoded (bytes UTF-8 decoded whatever the charset; a leading "?" is kept). Any other MIME type, a missing boundary or a malformed body rejects with TypeError. */
    formData(): Promise<FormData>;
    /** A stream body is tee()d: this response keeps one branch, the clone gets the other. TypeError once used or while the body stream is locked. Headers keep their guard. */
    clone(): Response;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
    json(): Promise<unknown>;
    /** UTF-8 decode of the bytes (a leading BOM is dropped; invalid sequences become U+FFFD). */
    text(): Promise<string>;
}
interface ResponseConstructor {
    /** body: see RequestInit.body (no duplex needed for a ReadableStream). */
    new (body?: unknown, init?: ResponseInit): Response;
    /** A network-error response: type "error", status 0, statusText "", immutable empty headers, null body. */
    error(): Response;
    /** A redirect to the parsed URL (TypeError if invalid) with immutable headers holding Location; status 301, 302 (default), 303, 307 or 308, else RangeError. */
    redirect(url: string | URL, status?: number): Response;
    /** JSON.stringify(data) as UTF-8 with content-type "application/json" (unless init.headers sets one); TypeError when data does not serialize. */
    json(data: unknown, init?: ResponseInit): Response;
}
declare const Response: ResponseConstructor;

/** W3C File API Blob (libc-blob.c). An immutable rope: a list of (store, offset, length) slices over refcounted byte stores; one slice is inline, more grow 4, 8, 16... slots.
 *  Construction copies string/BufferSource parts once into one new store per run of adjacent parts; a Blob part of >= 1024 bytes is shared by reference (no copy), a smaller one is copied into the run.
 *  slice() is O(number of slices) and never copies bytes. Reads (text/arrayBuffer/bytes) allocate the result once and fill it from the slices; stream() enqueues fresh 64 KiB chunks, or fills a BYOB reader's buffer in place.
 *  A string part is converted as a USVString (lone surrogate -> U+FFFD), encoded as UTF-8; a SharedArrayBuffer part throws TypeError. type: any char outside U+0020..U+007E makes it "", else ASCII-lowercased.
 *  endings: "native" turns CRLF/CR/LF in string parts into "\n" (POSIX) or "\r\n" (Windows); "transparent" (default) keeps them; anything else throws TypeError.
 *  Memory: an arrayBuffer()/bytes() result and every stream() chunk move off the native ledger when returned (they count toward the engine heap), so repeated reads never leave nativeSize high or hit a false out-of-memory under setNativeMemoryLimit. */
interface Blob {
    readonly size: number;
    readonly type: string;
    /** start/end are clamped long long, negative values count from the end; contentType is normalized like type. Always returns a Blob (never a File). */
    slice(start?: number, end?: number, contentType?: string): Blob;
    /** A byte ReadableStream (type "bytes") that holds its own references to the stores, so the Blob may be collected while it is read. */
    stream(): ReadableStream<Uint8Array>;
    /** stream() piped through a TextDecoderStream (UTF-8, ignoring any charset in type). */
    textStream(): ReadableStream<string>;
    /** UTF-8 decode (a leading BOM is dropped, invalid bytes become U+FFFD); an all-ASCII blob is copied straight into a one-byte string. */
    text(): Promise<string>;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
}
interface BlobPropertyBag {
    type?: string;
    endings?: "transparent" | "native";
}
interface BlobConstructor {
    readonly prototype: Blob;
    new (blobParts?: Iterable<ArrayBuffer | ArrayBufferView | Blob | string>, options?: BlobPropertyBag): Blob;
}
declare const Blob: BlobConstructor;

/** W3C File: a Blob with a name and lastModified (ms since epoch; default the construction time). The name is kept as given. */
interface File extends Blob {
    readonly name: string;
    readonly lastModified: number;
    readonly webkitRelativePath: string;
}
interface FilePropertyBag extends BlobPropertyBag {
    lastModified?: number;
}
interface FileConstructor {
    readonly prototype: File;
    new (fileBits: Iterable<ArrayBuffer | ArrayBufferView | Blob | string>, fileName: string, options?: FilePropertyBag): File;
}
declare const File: FileConstructor;

/** W3C FileReader. Each readAs* sets readyState LOADING and queues one task per event: loadstart, progress (only for a non-empty blob), then load (or error) and loadend in one task;
 *  pending microtasks run after each listener, as in a browser. A readAs* call while LOADING throws DOMException InvalidStateError. abort() fires abort and loadend synchronously; a read started
 *  in an abort or load listener suppresses the old read's loadend. readAsText(blob, label): a BOM (UTF-8, UTF-16BE/LE) wins, then the label, then the charset in blob.type, then UTF-8;
 *  UTF-8 and UTF-16 are decoded in C, other labels through the global TextDecoder. Events are plain objects { type, target, currentTarget, loaded, total, lengthComputable, isTrusted }
 *  (there is no Event/EventTarget global); addEventListener supports the capture and once options and handleEvent objects, and only the six FileReader event types. */
interface FileReader {
    readonly readyState: 0 | 1 | 2;
    readonly result: string | ArrayBuffer | null;
    readonly error: DOMException | null;
    readonly EMPTY: 0;
    readonly LOADING: 1;
    readonly DONE: 2;
    onloadstart: ((ev: ProgressEventLike) => unknown) | null;
    onprogress: ((ev: ProgressEventLike) => unknown) | null;
    onload: ((ev: ProgressEventLike) => unknown) | null;
    onabort: ((ev: ProgressEventLike) => unknown) | null;
    onerror: ((ev: ProgressEventLike) => unknown) | null;
    onloadend: ((ev: ProgressEventLike) => unknown) | null;
    readAsArrayBuffer(blob: Blob): void;
    readAsBinaryString(blob: Blob): void;
    readAsText(blob: Blob, encoding?: string): void;
    /** "data:<type or application/octet-stream>;base64,<base64>". */
    readAsDataURL(blob: Blob): void;
    abort(): void;
    addEventListener(type: string, listener: unknown, options?: boolean | { capture?: boolean; once?: boolean }): void;
    removeEventListener(type: string, listener: unknown, options?: boolean | { capture?: boolean }): void;
}
interface ProgressEventLike {
    readonly type: string;
    readonly target: FileReader;
    readonly currentTarget: FileReader;
    readonly loaded: number;
    readonly total: number;
    readonly lengthComputable: boolean;
    readonly isTrusted: boolean;
}
interface FileReaderConstructor {
    readonly prototype: FileReader;
    new (): FileReader;
    readonly EMPTY: 0;
    readonly LOADING: 1;
    readonly DONE: 2;
}
declare const FileReader: FileReaderConstructor;

/** WebIDL DOMException: prototype chain reaches Error.prototype; code is the legacy code for the name (0 for names without one); the 25 legacy *_ERR constants sit on the constructor and prototype. */
interface DOMException extends Error {
    readonly name: string;
    readonly message: string;
    readonly code: number;
}
interface DOMExceptionConstructor {
    readonly prototype: DOMException;
    new (message?: string, name?: string): DOMException;
}
declare const DOMException: DOMExceptionConstructor;

/** The WHATWG URL class (with parse, canParse and join statics). It is a web global, so it is core (libc-url.c, libc-url-accessors.c, B1d): present in every build, with or without dyna:url; dyna:url re-exports the core constructor, so the global and the export are one object in every context and Worker. */
declare const URL: typeof import("dyna:url").URL & {
    /** "blob:<location.origin or null>/<uuid v4>"; the Blob is kept in a per-context Map until revokeObjectURL (fetch does not resolve blob: URLs). TypeError for a non-Blob. */
    createObjectURL(blob: Blob): string;
    revokeObjectURL(url: string): void;
};
/** WHATWG query list, THE same class the dyna:url module exports. It is core (libc-search-params.c, B1c): present in every build, with or without dyna:url; dyna:url re-exports the core constructor, so the global and the export are one object in every context and Worker. */
declare const URLSearchParams: typeof import("dyna:url").URLSearchParams;

/** The global fetch: the SAME function as dyna:http's — see its notes on redirects and dropped credentials. Request, Response, FormData and the multipart and urlencoded bodies are core (no dyna:url or dyna:http needed; URLs parse with the core WHATWG parser, never through globalThis.URL); only the transport is optional: a build without the "http" module rejects every network fetch with a TypeError naming "http", and a build without the optional "tls" module rejects https (also a redirect to https) with a TypeError naming "tls". Headers, FormData, AbortSignal/AbortController, Request, Response, the redirect policy and the body readers are C classes installed with no compile (libc-fetch.c); only the per-request await loop is JS, compiled once per context on the first fetch() call. A body reader always returns a promise (a bad body rejects, never throws). */
declare const fetch: typeof import("dyna:http").fetch;

/** WHATWG UTF-8 encoder: a lone surrogate becomes U+FFFD (EF BF BD); dyna:bytes' Text#toBytes keeps it as WTF-8. */
declare class TextEncoder {
    constructor();
    readonly encoding: string;
    encode(input?: string): Uint8Array;
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number };
}
declare class TextDecoder {
    /** Every WHATWG Encoding label (default "utf-8"; trimmed, ASCII case-insensitive; .encoding is the canonical name): utf-8, utf-16le/utf-16be (label "utf-16" is utf-16le), the 28 single-byte encodings (windows-1252 for latin1/iso-8859-1/ascii/us-ascii, iso-8859-2..16, koi8-r/u, windows-874/1250..1258, ibm866, macintosh, x-mac-cyrillic), gbk, gb18030, big5, euc-jp, iso-2022-jp, shift_jis, euc-kr and x-user-defined. The replacement labels (replacement, iso-2022-kr, hz-gb-2312, iso-2022-cn, ...) and unknown labels are a RangeError.
     *  Legacy decoders run in C (src/core/dyn-textdec.c) over index tables generated from the WHATWG/WPT index data by tools/js/gen-textdec-index.js (src/core/dyn-textdec-index.h, checked by check-generated): gb18030 is the GB18030-2022 index (Go x/text pre-2022 table + the 18 WPT 2022 code points + PUA from iconv, asserted to land in U+E000..F8FF). The full jis0208/jis0212/euc-kr/big5 tables are checked against the WPT legacy-mb-* fixtures (tests/wpt/test_wpt_legacy_decode.js).
     *  A BOM is stripped only for utf-8 and utf-16 (unless ignoreBOM). Output is a Latin-1 (8-bit) string when every code unit is below U+0100, a UTF-16 string otherwise; the input view is read in place (no copy), one pass into a code-unit buffer of input+16 units. */
    constructor(encoding?: string, opts?: { fatal?: boolean; ignoreBOM?: boolean });
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    /** Decodes with the decoder's encoding. {stream: true} continues the decoder's stream: an incomplete trailing sequence (and iso-2022-jp mode, utf-16 lead byte/surrogate) is carried into the next call; a call without it is FINAL (an unterminated tail, e.g. an odd utf-16 byte or a lone lead surrogate, becomes U+FFFD, or a TypeError when fatal). */
    decode(buffer?: Uint8Array | ArrayBuffer, options?: { stream?: boolean }): string;
}

/** WHATWG Streams (https://streams.spec.whatwg.org), Encoding streams and Compression streams as globals.
 *  The 17 globals are lazy: the first access to ANY of them registers the native (C) classes once per realm (well under a millisecond, no JS is evaluated), so programs that never touch them pay nothing.
 *  Every algorithm runs in C; promise reactions the spec takes with no derived promise ("upon") are native reactions, so microtask order matches the spec job for job. Records (streams, readers, writers, controllers, pipes, tees) are native objects whose held values are GC-marked, so a stream cycle is collectable.
 *  Callback and start results are converted as WebIDL requires (a new promise adopting the value), so a callback that returns a promise settles two microtasks after it, as in browsers.
 *  Spec algorithms are followed step for step (queue-with-sizes, pull coalescing, BYOB pull-into descriptors, tee, pipeTo shutdown order); WebIDL conversions read dictionary members in lexicographic order, operations are enumerable, constructors throw on illegal construction.
 *  pipeTo's chunk write is queued as a microtask after each read (spec "queue a microtask to perform the remaining steps"); AbortError/DataCloneError are DOMExceptions. Streams are transferable through os.Worker postMessage transfer lists (see there); there is no MessageChannel. */
type QueuingStrategySize<T = unknown> = (chunk: T) => number;
interface QueuingStrategy<T = unknown> {
    /** Non-negative, not NaN; default 1 (0 for byte streams' readable side). */
    highWaterMark?: number;
    /** Chunk weight; must return a finite non-negative number or the stream errors. Not allowed on byte streams. */
    size?: QueuingStrategySize<T>;
}
interface UnderlyingSource<R = unknown> {
    /** Runs once, synchronously in the constructor; a returned promise delays the first pull. */
    start?(controller: ReadableStreamDefaultController<R> | ReadableByteStreamController): unknown;
    /** Called while desiredSize > 0 and again after each returned promise settles, never concurrently. */
    pull?(controller: ReadableStreamDefaultController<R> | ReadableByteStreamController): unknown;
    /** Called once on cancel with the reason; its rejection rejects cancel(). */
    cancel?(reason?: unknown): unknown;
    /** "bytes" makes a byte stream (ReadableByteStreamController, BYOB readers). */
    /** "owning" (Streams, tentative): enqueue(chunk, {transfer}) structured-clones the chunk with that transfer list; a non-empty transfer on any other stream is a TypeError. */
    type?: "bytes" | "owning";
    /** Byte streams only: a default reader's pull gets a byobRequest of this many bytes. */
    autoAllocateChunkSize?: number;
}
interface ReadableStreamReadResult<T> { done: boolean; value: T | undefined; }
interface ReadableStreamGetReaderOptions { mode?: "byob"; }
interface ReadableStreamIteratorOptions { preventCancel?: boolean; }
interface StreamPipeOptions {
    preventClose?: boolean;
    preventAbort?: boolean;
    preventCancel?: boolean;
    /** Aborting it shuts the pipe down with the signal's reason (abort/cancel the ends unless prevented). */
    signal?: AbortSignal;
}
interface ReadableWritablePair<R = unknown, W = unknown> { readable: ReadableStream<R>; writable: WritableStream<W>; }
/** WHATWG ReadableStream. Async-iterable (values()). dyna:stream fromFile/fromBytes piped to toFile, and toReadableStream piped to toWritableStream, take a native fast path (see dyna:stream). */
declare class ReadableStream<R = unknown> {
    constructor(underlyingSource?: UnderlyingSource<R>, strategy?: QueuingStrategy<R>);
    /** From an async iterable or a sync iterable (Symbol.asyncIterator first); cancel() calls the iterator's return(). */
    static from<T>(asyncIterable: AsyncIterable<T> | Iterable<T>): ReadableStream<T>;
    readonly locked: boolean;
    cancel(reason?: unknown): Promise<void>;
    getReader(): ReadableStreamDefaultReader<R>;
    /** BYOB readers exist only for byte streams (TypeError otherwise). */
    getReader(options: { mode: "byob" }): ReadableStreamBYOBReader;
    pipeThrough<T>(transform: ReadableWritablePair<T, R>, options?: StreamPipeOptions): ReadableStream<T>;
    pipeTo(destination: WritableStream<R>, options?: StreamPipeOptions): Promise<void>;
    /** Two branches; the source is canceled only when both are, with the reason [reason1, reason2]. Byte streams tee into byte streams, and branch 2 gets copies of the chunks. */
    tee(): [ReadableStream<R>, ReadableStream<R>];
    /** Locks the stream; breaking out of for-await cancels it unless preventCancel. */
    values(options?: ReadableStreamIteratorOptions): AsyncIterableIterator<R>;
    [Symbol.asyncIterator](options?: ReadableStreamIteratorOptions): AsyncIterableIterator<R>;
}
declare class ReadableStreamDefaultReader<R = unknown> {
    constructor(stream: ReadableStream<R>);
    readonly closed: Promise<void>;
    cancel(reason?: unknown): Promise<void>;
    read(): Promise<ReadableStreamReadResult<R>>;
    /** Pending reads reject with a TypeError. */
    releaseLock(): void;
}
declare class ReadableStreamBYOBReader {
    constructor(stream: ReadableStream<Uint8Array>);
    readonly closed: Promise<void>;
    cancel(reason?: unknown): Promise<void>;
    /** view's buffer is TRANSFERRED (the passed view is detached); the result is a view of the same type over the moved buffer. min (elements, default 1) waits until at least that many are filled, except at close. */
    read<T extends ArrayBufferView>(view: T, options?: { min?: number }): Promise<ReadableStreamReadResult<T>>;
    releaseLock(): void;
}
declare class ReadableStreamDefaultController<R = unknown> {
    /** Not constructible (TypeError). */
    private constructor();
    readonly desiredSize: number | null;
    close(): void;
    enqueue(chunk?: R): void;
    error(e?: unknown): void;
}
declare class ReadableByteStreamController {
    private constructor();
    /** The pending BYOB (or autoAllocateChunkSize) request, else null. */
    readonly byobRequest: ReadableStreamBYOBRequest | null;
    readonly desiredSize: number | null;
    close(): void;
    /** The chunk's buffer is transferred into the stream (detached for the caller). */
    enqueue(chunk: ArrayBufferView): void;
    error(e?: unknown): void;
}
declare class ReadableStreamBYOBRequest {
    private constructor();
    readonly view: Uint8Array | null;
    /** bytesWritten bytes were written into view (0 only after close). */
    respond(bytesWritten: number): void;
    /** Same buffer region, possibly a transferred buffer of the same capacity. */
    respondWithNewView(view: ArrayBufferView): void;
}
interface UnderlyingSink<W = unknown> {
    start?(controller: WritableStreamDefaultController): unknown;
    /** One write at a time; a rejection errors the stream. */
    write?(chunk: W, controller: WritableStreamDefaultController): unknown;
    close?(): unknown;
    abort?(reason?: unknown): unknown;
    type?: undefined;
}
/** WHATWG WritableStream. */
declare class WritableStream<W = unknown> {
    constructor(underlyingSink?: UnderlyingSink<W>, strategy?: QueuingStrategy<W>);
    readonly locked: boolean;
    abort(reason?: unknown): Promise<void>;
    close(): Promise<void>;
    getWriter(): WritableStreamDefaultWriter<W>;
}
declare class WritableStreamDefaultWriter<W = unknown> {
    constructor(stream: WritableStream<W>);
    readonly closed: Promise<void>;
    readonly desiredSize: number | null;
    /** Resolves when desiredSize > 0 (backpressure released). */
    readonly ready: Promise<void>;
    abort(reason?: unknown): Promise<void>;
    close(): Promise<void>;
    releaseLock(): void;
    write(chunk?: W): Promise<void>;
}
declare class WritableStreamDefaultController {
    private constructor();
    /** Aborted when the stream is aborted, with the abort reason. */
    readonly signal: AbortSignal;
    error(e?: unknown): void;
}
interface Transformer<I = unknown, O = unknown> {
    start?(controller: TransformStreamDefaultController<O>): unknown;
    /** Default: enqueue the chunk unchanged. */
    transform?(chunk: I, controller: TransformStreamDefaultController<O>): unknown;
    flush?(controller: TransformStreamDefaultController<O>): unknown;
    /** Called when the readable side is canceled or the writable side aborted. */
    cancel?(reason?: unknown): unknown;
    readableType?: undefined;
    writableType?: undefined;
}
/** WHATWG TransformStream: writes wait while the readable side is full (initial backpressure: the readable strategy defaults to highWaterMark 0). */
declare class TransformStream<I = unknown, O = unknown> {
    constructor(transformer?: Transformer<I, O>, writableStrategy?: QueuingStrategy<I>, readableStrategy?: QueuingStrategy<O>);
    readonly readable: ReadableStream<O>;
    readonly writable: WritableStream<I>;
}
declare class TransformStreamDefaultController<O = unknown> {
    private constructor();
    readonly desiredSize: number | null;
    enqueue(chunk?: O): void;
    error(reason?: unknown): void;
    terminate(): void;
}
/** Weighs chunks by chunk.byteLength. */
declare class ByteLengthQueuingStrategy {
    constructor(init: { highWaterMark: number });
    readonly highWaterMark: number;
    /** The same function for every instance; its name is "size". */
    readonly size: (chunk: ArrayBufferView) => number;
}
/** Weighs every chunk as 1. */
declare class CountQueuingStrategy {
    constructor(init: { highWaterMark: number });
    readonly highWaterMark: number;
    readonly size: (chunk?: unknown) => 1;
}
/** Strings in, UTF-8 Uint8Array chunks out; a surrogate pair split across chunks is joined, a lone surrogate becomes U+FFFD. */
declare class TextEncoderStream {
    constructor();
    readonly encoding: "utf-8";
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<string>;
}
/** BufferSource chunks in, strings out; multi-byte sequences split across chunks are joined. Labels as TextDecoder; fatal makes invalid input error the stream with a TypeError. */
declare class TextDecoderStream {
    constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    readonly readable: ReadableStream<string>;
    readonly writable: WritableStream<ArrayBufferView | ArrayBuffer>;
}
/** "deflate" is zlib framing (RFC 1950), "deflate-raw" raw DEFLATE (RFC 1951), "gzip" RFC 1952, "brotli" RFC 7932 (darwin only: Apple libcompression; a TypeError elsewhere); dynajs extensions: "zstd" (RFC 8878 frames with a content checksum, level 3; only when built with libzstd, else a TypeError) and "lz4" (LZ4 frame format, 64 KiB independent blocks, level 1; encoder output does not depend on how the input was chunked; decoding verifies an optional content checksum); any other string (case-sensitive) is a TypeError. Decompression of the three DEFLATE formats runs a portable resumable inflater, so data after the end is handled chunk by chunk (a TypeError, except gzip's node-compatible multi-member and NUL-padding rules; see DecompressionStream). */
type CompressionFormat = "deflate" | "deflate-raw" | "gzip" | "brotli" | "zstd" | "lz4";
/** BufferSource chunks in, compressed Uint8Array chunks out, driven synchronously by the native dyna:stream codecs (no promise per chunk inside the codec). "gzip", "deflate" and "deflate-raw" share one streaming DEFLATE encoder in C on every platform: zlib level-6 lazy matching (128-step hash chains on a 4-byte hash, lazy threshold 8) with a 32 KiB window carried across chunks, dynamic Huffman blocks per 16 KiB of input, and a hardware CRC-32 for gzip. Output depends only on the bytes, never on how they were chunked; input is held until 16 KiB plus 269 bytes of lookahead are buffered (or the stream closes), so a short write produces no chunk yet. Memory: about 1 KB per stream until data arrives, about 49 KB while under 16 KiB is buffered, then about 240 KB (64 KB hash head, 64 KB chain links, 49 KB window buffer, up to 64 KB of tokens) until close, when it is freed. "brotli" uses Apple libcompression (darwin only). */
declare class CompressionStream {
    constructor(format: CompressionFormat);
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<ArrayBufferView | ArrayBuffer>;
}
/** The inverse of CompressionStream. Corrupt input, a checksum mismatch (gzip CRC-32, zlib Adler-32) or a truncated stream errors both sides with a TypeError. "deflate", "deflate-raw", "brotli", "zstd" and "lz4": any data after the end is a TypeError; the bytes decoded before it are enqueued first (as the spec requires, the error then resets the queue, so a reader sees the part of that prefix it had already pulled). "gzip" deliberately deviates from the Compression Streams spec (which makes any data after the end a TypeError), by owner choice, to match node's zlib.gunzip (checked against node v26 gunzipSync and createGunzip, whole input and chunks of 1, 3 and 7 bytes): after a member's 8-byte trailer, a 0x00 byte ends decoding and everything after it is ignored (NUL padding, even a later valid member); any other byte starts a new member that is decoded and appended (concatenated gzip, as `cat a.gz b.gz`), so it must be a complete valid member: a bad header (garbage such as 01 02 03 04, or a lone 1f), a truncated member or a CRC-32/ISIZE mismatch is a TypeError, as node throws there. WPT cost: decompression-corrupt-input "trailing junk for 'gzip'" (one 0x00 appended) fails, and decompression-extra-input times out (its gzip case never closes the stream and waits for an error). The three DEFLATE formats are decoded in C (64-bit bit buffer, 10-bit root tables with subtables, word-wise match copy, hardware CRC-32 where available) and the stream calls the codec directly, with no JS call per chunk. One input chunk yields one output chunk per 96 KiB of decoded data at most (the spec allows any split; no chunk is empty). Memory: about 1.2 KB per stream until the first compressed byte arrives, then one 51 KB table block laid out root tables first (literal root, distance root, distance subtables, literal subtables), so a typical block touches about 6 KB at its start instead of two separate pages, plus a history/output buffer that grows 1 KiB, 2 KiB, ... up to 128 KiB. Measured on macOS (16 KiB pages), 10000 live streams each holding 1 KB of output: 256 MB peak RSS against 391 MB before v10b. */
declare class DecompressionStream {
    constructor(format: CompressionFormat);
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<ArrayBufferView | ArrayBuffer>;
}

interface Performance {
    now(): number;
}
declare const performance: Performance;

/** Each call formats, writes and flushes at once (unbuffered), after flushing any dyna:stream stdout()/stderr() bytes still buffered, so the output keeps call order. log/info/debug go to stdout; trace/warn/error/assert to stderr. */
interface Console {
    log(...args: unknown[]): void;
    info(...args: unknown[]): void;
    debug(...args: unknown[]): void;
    trace(...args: unknown[]): void;
    warn(...args: unknown[]): void;
    error(...args: unknown[]): void;
    assert(cond: unknown, ...args: unknown[]): void;
}
declare const console: Console;

/** Prints values to stdout. */
declare function print(...args: unknown[]): void;

/** The engine's own argument vector. */
declare const scriptArgs: string[];

/** Standard timers; return a numeric id. A positive setTimeout delay is a minimum: the callback never runs before `ms` full milliseconds of monotonic time have passed since the call (deadlines are kept in microseconds and the loop waits with a microsecond timeout, so it runs as soon as the delay has passed, never early). Waits under 1 s are exempt from OS timer coalescing (macOS: an EVFILT_TIMER with NOTE_CRITICAL on the reactor kqueue, or on a per-thread deadline kqueue polled with the loop fds; Linux: epoll_pwait2 / ppoll), so setTimeout(1) fires about 1.02 ms after arming and setTimeout(10) about 10.05 ms, where a plain kevent or poll timeout is stretched by the kernel (1.26 ms for 1, 6.3 ms for 5, 12.0 ms for 10 on macOS); waits of 1 s or more keep the default leeway. setInterval periods are not rounded, so they do not drift. */
declare function setTimeout(cb: (...args: unknown[]) => void, ms?: number, ...args: unknown[]): number;
declare function clearTimeout(id: number): void;
declare function setInterval(cb: (...args: unknown[]) => void, ms?: number, ...args: unknown[]): number;
declare function clearInterval(id: number): void;

/** Microtask scheduling: the callback runs before the next macrotask. */
declare function queueMicrotask(callback: () => void): void;

/** WHATWG base64 over LATIN-1 STRINGS, not UTF-8: btoa("é") is "6Q==" and chars above U+00FF throw; use dyna:encoding's Base64* for bytes. */
declare function atob(data: string): string;
declare function btoa(data: string): string;

/** The base Iterator constructor; Iterator.prototype carries the ES2025 helpers plus engine extras, so every runtime iterator chains map/filter/take/... */
interface IteratorConstructor {
    /** Wraps any iterable or iterator in the helper-equipped iterator. */
    from<T>(objects: Iterable<T> | Iterator<T>): IteratorObject<T>;
    /** Concatenates iterables into one iterator (ES2025 Iterator.concat). */
    concat<T>(...items: (Iterable<T> | Iterator<T>)[]): IteratorObject<T>;
    /** Zips iterables into arrays; mode "shortest" (default), "longest" (pads from `padding`, else undefined) or "strict" (unequal lengths throw).
     *  The result is an Iterator Helper: return() closes every open input, last to first. */
    zip<T extends readonly (Iterable<unknown> | Iterator<unknown>)[]>(iterables: T, options?: { mode?: "shortest" | "longest" | "strict"; padding?: Iterable<unknown> }): IteratorObject<{ [K in keyof T]: T[K] extends Iterable<infer U> ? U : (T[K] extends Iterator<infer U2> ? U2 : unknown) }>;
    /** Zips an object's enumerable own iterables (undefined values skipped) into null-prototype objects with the same keys; padding is keyed. */
    zipKeyed<O extends Record<string, Iterable<unknown> | Iterator<unknown>>>(iterables: O, options?: { mode?: "shortest" | "longest" | "strict"; padding?: Partial<Record<keyof O, unknown>> }): IteratorObject<{ [K in keyof O]: O[K] extends Iterable<infer U> ? U : (O[K] extends Iterator<infer U2> ? U2 : unknown) }>;
    /** start + step*i from start toward end, end exclusive unless inclusive (TC39 proposal-iterator.range); step defaults to 1 or -1; NaN, an infinite start or step, or a zero step throw RangeError. */
    range(start: number, end: number, optionOrStep?: number | { step?: number; inclusive?: boolean }): IteratorObject<number>;
    /** BigInt form of range; end may also be Infinity or -Infinity. */
    range(start: bigint, end: bigint | number, optionOrStep?: bigint | { step?: bigint; inclusive?: boolean }): IteratorObject<bigint>;
}
declare const Iterator: IteratorConstructor;

/** The helper-equipped iterator shape every runtime iterator has. */
interface IteratorObject<T, TReturn = unknown, TNext = unknown> extends Iterator<T, TReturn, TNext> {
    [Symbol.iterator](): IteratorObject<T, TReturn, TNext>;
    map<U>(fn: (value: T) => U): IteratorObject<U, TReturn, TNext>;
    filter(fn: (value: T) => boolean): IteratorObject<T, TReturn, TNext>;
    take(n: number): IteratorObject<T, TReturn, TNext>;
    drop(n: number): IteratorObject<T, TReturn, TNext>;
    flatMap<U>(fn: (value: T) => Iterable<U> | Iterator<U>): IteratorObject<U, TReturn, TNext>;
    takeWhile(pred: (value: T) => boolean): IteratorObject<T, TReturn, TNext>;
    dropWhile(pred: (value: T) => boolean): IteratorObject<T, TReturn, TNext>;
    scan<R>(fn: (acc: R, value: T) => R, seed: R): IteratorObject<R, TReturn, TNext>;
    intersperse(v: T): IteratorObject<T, TReturn, TNext>;
    compact(): IteratorObject<NonNullable<T>, TReturn, TNext>;
    dropRepeats(): IteratorObject<T, TReturn, TNext>;
    dropRepeatsWith(eq: (a: T, b: T) => boolean): IteratorObject<T, TReturn, TNext>;
    dropRepeatsBy<K>(key: (value: T) => K): IteratorObject<T, TReturn, TNext>;
    /** Full sliding windows of size n (windows(n) with a coerced size); n < 1 throws RangeError. */
    aperture(n: number): IteratorObject<T[], TReturn, TNext>;
    /** Consecutive arrays of n values, the last one shorter (chunks(n) with a coerced size); n < 1 throws RangeError. */
    splitEvery(n: number): IteratorObject<T[], TReturn, TNext>;
    /** Consecutive arrays of size values, the last one shorter (TC39 proposal-iterator-chunking); size must be an integral number in 1..2^32-1. */
    chunks(size: number): IteratorObject<T[], TReturn, TNext>;
    /** Overlapping arrays of size values; "allow-partial" yields one short window when the source has fewer values (TC39 proposal-iterator-chunking). */
    windows(size: number, undersized?: "only-full" | "allow-partial"): IteratorObject<T[], TReturn, TNext>;
    /** Yields [index, value] pairs. */
    withIndex(): IteratorObject<[number, T], TReturn, TNext>;
    /** Alias of withIndex. */
    enumerate(): IteratorObject<[number, T], TReturn, TNext>;
    /** Flattens one level of iterables; a primitive value throws TypeError (flatMap with the identity). */
    flat(): IteratorObject<T extends Iterable<infer U> ? U : never, TReturn, TNext>;
    /** Repeats the source forever, buffering one pass; an empty source yields nothing. */
    cycle(): IteratorObject<T, TReturn, TNext>;
    /** Yields the first value and then every step-th one; step < 1 or NaN throws RangeError. */
    stepBy(step: number): IteratorObject<T, TReturn, TNext>;
    /** Calls fn(value, index) for each value and yields the value unchanged. */
    tap(fn: (value: T, index: number) => void): IteratorObject<T, TReturn, TNext>;
    zipWith<U, R>(fn: (a: T, b: U) => R, other: Iterable<U>): IteratorObject<R, TReturn, TNext>;
    pluck<K extends keyof T>(key: K): IteratorObject<T[K], TReturn, TNext>;
    tee(n?: number): IteratorObject<T, TReturn, TNext>[];
    unique(): IteratorObject<T, TReturn, TNext>;
    uniq(): IteratorObject<T, TReturn, TNext>;
    uniqBy<K>(key: (value: T) => K): IteratorObject<T, TReturn, TNext>;
    /** Materializes the iterator (ES2025 Iterator.prototype.toArray). */
    toArray(): T[];
    forEach(fn: (value: T) => void): void;
    reduce<R>(fn: (acc: R, value: T) => R, seed: R): R;
    reduce(fn: (acc: T, value: T) => T): T;
    some(pred: (value: T) => boolean): boolean;
    every(pred: (value: T) => boolean): boolean;
    find(pred: (value: T) => boolean): T | undefined;
    findIndex(pred: (value: T) => boolean): number;
    includes(value: T): boolean;
    count(pred?: (value: T) => boolean): number;
    first(): T | undefined;
    head(): T | undefined;
    last(): T | undefined;
    nth(i: number): T | undefined;
    init(): IteratorObject<T, TReturn, TNext>;
    tail(): IteratorObject<T, TReturn, TNext>;
    sum(): number;
    average(): number;
    mean(): number;
    product(): number;
    min(): T | undefined;
    max(): T | undefined;
    none(pred: (value: T) => boolean): boolean;
    any(pred: (value: T) => boolean): boolean;
    all(pred: (value: T) => boolean): boolean;
    countBy<K extends string | number>(key: (value: T) => K): Record<K, number>;
    indexBy<K extends string | number>(key: (value: T) => K): Record<K, T>;
    groupBy<K extends string | number>(key: (value: T) => K): Record<K, T[]>;
    reduceBy<K extends string | number, R>(fn: (acc: R, value: T) => R, seed: R, key: (value: T) => K): Record<K, R>;
    /** [values where fn(value, index) is truthy, the rest]. */
    partition(fn: (value: T, index: number) => unknown): [T[], T[]];
    /** Collects the values into a Set. */
    toSet(): Set<T>;
    /** Collects [key, value] entries into a Map; a non-object entry throws TypeError. */
    toMap<K, V>(this: IteratorObject<readonly [K, V]>): Map<K, V>;
    /** Joins like Array.prototype.join: separator defaults to ",", undefined and null become "". */
    join(separator?: string): string;
    /** The first value whose key fn(value, index) is smallest under the < operator (strings compare as strings). */
    minBy<K>(fn: (value: T, index: number) => K): T | undefined;
    /** The first value whose key fn(value, index) is largest under the < operator. */
    maxBy<K>(fn: (value: T, index: number) => K): T | undefined;
}

/** ES2025 half-precision (binary16) typed array. */
declare const Float16Array: Float16ArrayConstructor;
interface Float16ArrayConstructor {
    readonly prototype: Float16Array;
    readonly BYTES_PER_ELEMENT: number;
    new (length?: number): Float16Array;
    new (array: ArrayLike<number> | Iterable<number>): Float16Array;
    new (buffer: ArrayBufferLike, byteOffset?: number, length?: number): Float16Array;
    of(...items: number[]): Float16Array;
    from(arrayLike: ArrayLike<number>): Float16Array;
    from<T>(arrayLike: ArrayLike<T>, mapFn: (v: T, i: number) => number): Float16Array;
    from(source: Iterable<number>, mapFn?: (v: number, i: number) => number): Float16Array;
}
interface Float16Array {
    readonly length: number;
    readonly buffer: ArrayBufferLike;
    readonly byteOffset: number;
    readonly byteLength: number;
    [index: number]: number;
    at(index: number): number | undefined;
    set(array: ArrayLike<number>, offset?: number): void;
    subarray(start?: number, end?: number): Float16Array;
    slice(start?: number, end?: number): Float16Array;
    map(fn: (value: number, index: number, array: Float16Array) => number): Float16Array;
    forEach(fn: (value: number, index: number, array: Float16Array) => void): void;
    fill(value: number, start?: number, end?: number): this;
    copyWithin(target: number, start?: number, end?: number): this;
    reverse(): this;
    sort(compareFn?: (a: number, b: number) => number): this;
    /** The element count is captured before `separator` is coerced, so a detach inside a separator toString still emits the remaining separators. */
    join(separator?: string): string;
    indexOf(value: number, fromIndex?: number): number;
    lastIndexOf(value: number, fromIndex?: number): number;
    includes(value: number, fromIndex?: number): boolean;
    every(fn: (value: number) => boolean): boolean;
    some(fn: (value: number) => boolean): boolean;
    find(fn: (value: number) => boolean): number | undefined;
    findIndex(fn: (value: number) => boolean): number;
    filter(fn: (value: number) => boolean): Float16Array;
    reduce<U>(fn: (acc: U, value: number, index: number, array: Float16Array) => U, seed: U): U;
    reduce(fn: (acc: number, value: number, index: number, array: Float16Array) => number): number;
    keys(): IterableIterator<number>;
    values(): IterableIterator<number>;
    entries(): IterableIterator<[number, number]>;
    [Symbol.iterator](): IterableIterator<number>;
}

/** Explicit resource management (TC39 proposal, test262 feature explicit-resource-management): `using x = v` and `await using x = v` (async functions, async generators and module top level) bind a const and dispose it when the enclosing block, loop iteration (`for (using x of it)`), function or module body exits by any completion: records run in reverse order, the dispose method is read once at the declaration, null/undefined are skipped, a non-object or a missing method is a TypeError at the declaration, and each disposal error wraps the previous one as SuppressedError(error = new, suppressed = previous). `await using` awaits each [Symbol.asyncDispose] (or [Symbol.dispose]) result inline, once per record; null records cost at most one tick and none after a real await in the same scope. `using` is rejected at the top level of scripts and eval code and directly in a case clause. Iterator.prototype[Symbol.dispose] and AsyncIterator.prototype[Symbol.asyncDispose] call return(). */
interface SuppressedError extends Error {
    readonly error: unknown;
    readonly suppressed: unknown;
}
declare const SuppressedError: {
    readonly prototype: SuppressedError;
    new (error?: unknown, suppressed?: unknown, message?: string): SuppressedError;
};

interface DisposableStack {
    /** Registers a resource whose [Symbol.dispose] runs at stack disposal; null/undefined pass through. */
    use<T>(value: T): T;
    /** Registers a non-resource value with an explicit disposer. */
    adopt<T>(value: T, onDispose: (value: T) => void): T;
    /** Registers a bare callback to run at disposal. */
    defer(onDispose: () => void): void;
    /** Moves the captured callbacks to a fresh stack and returns it. */
    move(): DisposableStack;
    dispose(): void;
    readonly disposed: boolean;
    [Symbol.dispose](): void;
}
declare const DisposableStack: {
    readonly prototype: DisposableStack;
    new (): DisposableStack;
};

interface AsyncDisposableStack {
    /** Registers a resource whose [Symbol.asyncDispose] (or [Symbol.dispose]) runs at stack disposal; null/undefined pass through. */
    use<T>(value: T): T;
    adopt<T>(value: T, onDispose: (value: T) => void): T;
    defer(onDispose: () => void | Promise<void>): void;
    move(): AsyncDisposableStack;
    disposeAsync(): Promise<void>;
    readonly disposed: boolean;
    [Symbol.asyncDispose](): Promise<void>;
}
declare const AsyncDisposableStack: {
    readonly prototype: AsyncDisposableStack;
    new (): AsyncDisposableStack;
};

/** Engine error for internal invariant failures and out-of-memory ("out of memory"; never null).
 * When the error object itself cannot be allocated, a per-realm preallocated instance is thrown (shared, mutable). */
interface InternalError extends Error {
    name: "InternalError";
}
declare const InternalError: {
    readonly prototype: InternalError;
    new (message?: string): InternalError;
};

/** A native optic: a reusable, composable getter/setter over one focus (property, index, path, or custom pair); view/set/over never mutate the source. */
interface Lens<S = any, A = any> {
    /** Reads the focus (undefined when absent). */
    view(source: S): A | undefined;
    /** A copy of source with the focus set. */
    set(value: A, source: S): S;
    /** A copy of source with the focus transformed. */
    over(fn: (value: A) => A, source: S): S;
}
declare const Lens: {
    readonly prototype: Lens<any, any>;
    /** Builds a custom lens from a getter and an immutable setter set(value, source) -> updated source. */
    new <S, A>(get: (source: S) => A, set: (value: A, source: S) => S): Lens<S, A>;
    <S, A>(get: (source: S) => A, set: (value: A, source: S) => S): Lens<S, A>;
    /** A lens over one property. */
    prop(name: string): Lens<any, any>;
    /** A lens over one array index (preserves the array shape). */
    index(index: number): Lens<any, any>;
    /** A lens over a nested path: "a.b.c" (dotted) or ["a", "b", "c"]. */
    path(path: string | string[]): Lens<any, any>;
    /** Builds a custom lens (same as the constructor). */
    lens<S, A>(get: (source: S) => A, set: (value: A, source: S) => S): Lens<S, A>;
    /** Reads through the lens. */
    view<S, A>(lens: Lens<S, A>, source: S): A | undefined;
    /** Sets through the lens (immutable copy). */
    set<S, A>(lens: Lens<S, A>, value: A, source: S): S;
    /** Transforms through the lens (immutable copy). */
    over<S, A>(lens: Lens<S, A>, fn: (value: A) => A, source: S): S;
};

/** @internal Loads and evaluates a script file into global scope (bootstrap helper; use import instead). */
declare function __loadScript(filename: string): unknown;


/** Resolves after `ms` milliseconds without blocking the event loop (os.sleep is the blocking form); never before `ms` full milliseconds of monotonic time have passed. */
declare function sleep(ms: number): Promise<void>;

/** Structured deep copy (HTML structuredClone) with an optional transfer list; failures are DataCloneError DOMExceptions. Same function as dyna:serialize's. */
declare function structuredClone(value: unknown, options?: { transfer?: Iterable<unknown> }): unknown;
/** WebIDL DOMException: name/message/code accessors on the prototype, the 25 legacy code constants on both, and Error.prototype as the prototype's prototype. Lazy global (created on first access). Thrown as AbortError/TimeoutError by AbortSignal, DataCloneError by structuredClone and stream transfer. */
declare class DOMException extends Error {
    constructor(message?: string, name?: string);
    readonly name: string;
    readonly message: string;
    readonly code: number;
}

/* ---- Atomics (built-in, includes one non-standard extension) ------ */

/** Atomics: read-modify-write ops take INTEGER typed arrays over a SharedArrayBuffer; BigInt64/BigUint64 use BigInt values; `index` is an element index. */
interface Atomics {
    /** `ta[index] += value`; returns the OLD value. */
    add(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] &= value`; returns the OLD value. */
    and(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] |= value`; returns the OLD value. */
    or(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] -= value`; returns the OLD value. */
    sub(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] ^= value`; returns the OLD value. */
    xor(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] = value`; returns the OLD value. */
    exchange(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** Writes `replacement` only when the current value === `expected`; returns the OLD value. */
    compareExchange(ta: AnyIntegerTypedArray, index: number,
        expectedValue: number, replacementValue: number): number;
    /** Atomic load; returns `ta[index]`. */
    load(ta: AnyIntegerTypedArray, index: number): number;
    /** Atomic store; returns `value`. */
    store(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** True for element sizes 1/2/4/8. */
    isLockFree(size: number): boolean;
    /** NON-STANDARD spin hint: one CPU pause instruction (x86 PAUSE, arm64 YIELD) per call; the argument is validated but is not a loop count. */
    pause(iterations?: number): undefined;
    /** Blocks until notified, the value changes, or `timeout` ms: "ok" | "timed-out" | "not-equal". Int32Array/BigInt64Array only; the main thread cannot block (TypeError). */
    wait(ta: Int32Array | BigInt64Array, index: number,
        value: number, timeout?: number): "ok" | "timed-out" | "not-equal";
    /** Wakes at most `count` (default all) waiters on ta[index]; returns the number woken. */
    notify(ta: AnyIntegerTypedArray, index: number, count?: number): number;
}

/* ---- core prototype extensions (project additions) --------------- */

/** String.prototype extensions (absent under --no-prototypes). */
interface String {
    /** A lazy Iterator over the string's CODE POINTS (an astral character is one entry); composes with map/filter/take/toArray. */
    lazy(): Iterator<string>;
    /** True when the string is empty. */
    isEmpty(): boolean;
    /** Strips a leading prefix when present. */
    trimPrefix(prefix: string): string;
    /** Strips a trailing suffix when present. */
    trimSuffix(suffix: string): string;
    /** Strips every character in `chars` from both ends. */
    trimChars(chars: string): string;
    /** True when any code unit of `set` occurs. */
    containsAny(set: string): boolean;
    /** The first position of any code unit of `set`, or -1. */
    indexOfAny(set: string): number;
    /** Every position where `sub` occurs, ascending, counting overlaps. */
    indexOfAll(sub: string): number[];
    /** ASCII-only case-insensitive equality (only 'A'..'Z' fold; non-ASCII pairs compare unequal). */
    equalsIgnoreCase(other: string): boolean;
    /** Code-point comparison after NFC normalization; `locales`/`options` accepted and ignored (no ICU collation). */
    localeCompare(that: string, locales?: string | string[], options?: Intl.CollatorOptions): number;
    /** Byte-wise comparison, -1, 0, or 1. */
    compareBytes(other: string): number;
    /** Splits into at most n pieces. */
    splitN(sep: string, n: number): string[];
    /** True when the string is empty or all whitespace. */
    isBlank(): boolean;
    /** The first n characters, or "". */
    first(n?: number): string;
    /** The last n characters, or "". */
    last(n?: number): string;
    /** Characters from `from` (inclusive) to `to` (exclusive). */
    from(from: number, to?: number): string;
    /** Characters up to `to` (exclusive). */
    to(to: number): string;
    /** The code points as an array. */
    chars(): string[];
    /** The UTF-8 byte values as an array. */
    codes(): number[];
    /** The reversed string. */
    reverse(): string;
    /** A new string with `text` inserted at index `i` (default end). */
    insert(text: string, i?: number): string;
    /** Removes the first occurrence of `text`. */
    remove(text: string): string;
    /** Removes every occurrence of `text`. */
    removeAll(text: string): string;
    /** Collapses internal whitespace runs and trims. */
    compact(): string;
    /** Caesar-shifts each ASCII letter by n (default 0). */
    shift(n?: number): string;
    /** Center-pads to `len` with `padding`. */
    pad(len: number, padding?: string): string;
    /** Uppercases the first character (each word when `all`); lowercases the rest when `lower`. */
    capitalize(lower?: boolean, all?: boolean): string;
    /** CamelCase to snake_case. */
    underscore(): string;
    /** CamelCase to dash-separated. */
    dasherize(): string;
    /** Underscores/dashes to spaces. */
    spacify(): string;
    /** snake_case/dash-case to UpperCamelCase (`upper` true, default) or lowerCamelCase. */
    camelize(upper?: boolean): string;
    /** Truncates to `len` characters from `from` ("left" | "middle" | "right", default right) with `ellipsis`. */
    truncate(len: number, from?: "left" | "middle" | "right", ellipsis?: string): string;
    /** Truncates at a word boundary within `len`; same `from`/`ellipsis` options as truncate. */
    truncateOnWord(len: number, from?: "left" | "middle" | "right", ellipsis?: string): string;
    /** Escapes the five HTML-significant characters & < > " ' (as &amp; &lt; &gt; &quot; &#39;), so the result is safe in element content and inside a quoted attribute value. */
    escapeHTML(): string;
    /** Unescapes HTML entities. */
    unescapeHTML(): string;
    /** Strips HTML tags. */
    stripTags(): string;
    /** The number of occurrences of `sub`. */
    count(sub: string): number;
    /** C strtod/strtoll semantics (leading whitespace, inf/nan, 0x; `base` 2..36 = integer form), NOT JS Number(); NaN when nothing parses. */
    toNumber(base?: number): number;
    /** snake_case to a human label. */
    humanize(): string;
    /** Title-cases words. */
    titleize(): string;
    /** Converts to a URL-friendly slug. */
    parameterize(): string;
    /** A naive plural form. */
    pluralize(): string;
    /** A naive singular form. */
    singularize(): string;
    /** Removes every element with tag `tagName` (all elements when omitted), content included. */
    removeTags(tagName?: string): string;
    /** Calls fn for each character. */
    forEach(fn: (ch: string) => void): void;
    /** Brace formatting: {0}/{name} from args; {{ }} escape; a {name} reads OWN properties only, and an unknown name renders empty. */
    format(...args: unknown[]): string;
    /** The words of the string. */
    words(): string[];
    /** The lines of the string. */
    lines(): string[];
    /** Base64 encodes the string's UTF-8 bytes. */
    encodeBase64(): string;
    /** Base64 decodes to a string. */
    decodeBase64(): string;
    /** Percent-encodes (encodeURI; encodeURIComponent when `param`). */
    escapeURL(param?: boolean): string;
    /** Percent-decodes: `param` selects decodeURIComponent, absence decodeURI. */
    unescapeURL(param?: boolean): string;
    /** Strips ANSI escape sequences. */
    stripAnsi(): string;
    /** The display width of the string (wide characters count twice). */
    displayWidth(options?: { ambiguousAsWide?: boolean }): number;
    /** Wraps the string to `width` columns with ANSI. */
    wrapAnsi(width: number, options?: { hard?: boolean; trim?: boolean }): string;
    /** The grapheme clusters as an array. */
    graphemes(): string[];
    /** True when the string's UTF-8 encoding is well-formed. */
    isWellFormed(): boolean;
    /** Replaces lone surrogates with U+FFFD. */
    toWellFormed(): string;
}

/** Array.prototype extensions (absent under --no-prototypes). */
interface Array<T> {
    /** True when the array is empty. */
    isEmpty(): boolean;
    /** The first element, or undefined. */
    first(): T | undefined;
    /** The last element, or undefined. */
    last(): T | undefined;
    /** The sum of numeric elements. */
    sum(): number;
    /** The mean of numeric elements -- the canonical spelling of the pair. */
    average(): number;
    /** LEGACY alias of average; prefer average. */
    mean(): number;
    /** Removes null/undefined elements. */
    compact(): NonNullable<T>[];
    /** The number of elements matching `value`, a predicate, or a RegExp; all elements when omitted. */
    count(matcher?: T | ((v: T) => boolean) | RegExp): number;
    /** True when no element satisfies the predicate. */
    none(pred: (v: T) => boolean): boolean;
    /** True when any element satisfies the predicate. */
    any(pred: (v: T) => boolean): boolean;
    /** True when every element satisfies the predicate. */
    all(pred: (v: T) => boolean): boolean;
    /** The minimum element, by mapper or property key (default identity). */
    min(map?: ((v: T) => number) | keyof T): T;
    /** The maximum element, by mapper or property key (default identity). */
    max(map?: ((v: T) => number) | keyof T): T;
    /** The first n elements. */
    take(n: number): T[];
    /** Everything after the first n elements. Fixed-size groups have no Array method: `arr.values().chunks(n).toArray()` (and `windows(n)` for sliding ones). */
    drop(n: number): T[];
    /** The last n elements. */
    takeLast(n: number): T[];
    /** Everything except the last n elements. */
    dropLast(n: number): T[];
    /** Returns a NEW array sorted stably by a key function (the receiver is not changed, unlike sort()). Numeric keys compare numerically (NaN last); strings by UTF-8 byte order; numbers before strings. */
    sortBy<K>(key: (v: T) => K): T[];
    /** The insertion index for a sorted array; `comparator` must match the array's sort order. */
    sortedIndexOf(value: T, comparator?: (a: T, b: T) => number): number;
    /** Groups elements by a key function into a Record. */
    groupBy<K extends string | number>(key: (v: T) => K): Record<K, T[]>;
    /** A fresh array in random order from the GLOBAL RNG; use a dyna:random Random for reproducible shuffles. */
    shuffle(): T[];
    /** n random elements without replacement (global RNG, as shuffle). */
    sample(n?: number): T[];
    /** The distinct elements, first occurrence kept; `map` picks the dedup key. */
    unique(map?: ((v: T) => unknown) | keyof T): T[];
    /** LEGACY alias of unique; prefer unique. */
    uniq(map?: ((v: T) => unknown) | keyof T): T[];
    /** The distinct elements by a key function. */
    uniqBy<K>(key: (v: T) => K): T[];
    /** The elements present in both arrays. Canonical spelling of the pair. */
    intersect(other: T[]): T[];
    /** LEGACY alias of intersect; prefer intersect. */
    intersection(other: T[]): T[];
    /** The elements of this array not in `other`. */
    difference(other: T[]): T[];
    /** A copy without elements present in `other`. */
    without(other: T[]): T[];
    /** The union of this array with `other`. */
    union(other: T[]): T[];
    /** [passing, failing] by predicate. */
    partition(pred: (v: T) => boolean): [T[], T[]];
    /** The values of a property per element. */
    pluck<K extends keyof T>(key: K): T[K][];
    /** Zips with another array into pairs. */
    zip<U>(other: U[]): [T, U][];
    zipWith<U, R>(fn: (a: T, b: U) => R, other: U[]): R[];
    /** Inserts `v` between every pair of elements. */
    intersperse(v: T): T[];
    /** Recursive flatten, guarded to a depth of 512 (deeper nesting is emitted as-is). */
    flatten(): T extends unknown[] ? T[number][] : T[];
    /** The matrix transpose (array of arrays). */
    transpose(): T[][];
    /** The Cartesian product with another array. */
    xprod<U>(other: U[]): [T, U][];
    /** Sliding windows of size n. n === 0 yields len+1 empty windows; n < 0 throws RangeError. */
    aperture(n: number): T[][];
    /** Chunks of exactly n elements (last may be short). */
    splitEvery(n: number): T[][];
    /** Splits at the given index into [left, right]. */
    splitAt(i: number): [T[], T[]];
    /** A copy with index i set to fn(a[i]). */
    adjust(i: number, fn: (v: T) => T): T[];
    /** A copy with index i set to `v`. */
    update(i: number, v: T): T[];
    /** Moves the element at from to to. */
    move(from: number, to: number): T[];
    /** Swaps two elements. */
    swap(i: number, j: number): T[];
    /** The element at i, or undefined. */
    nth(i: number): T | undefined;
    /** Everything except the last element. */
    init(): T[];
    /** Everything except the first element. */
    tail(): T[];
    /** The first element (LEGACY alias of first). */
    head(): T | undefined;
    takeWhile(pred: (v: T) => boolean): T[];
    dropWhile(pred: (v: T) => boolean): T[];
    takeLastWhile(pred: (v: T) => boolean): T[];
    dropLastWhile(pred: (v: T) => boolean): T[];
    /** Appends one element. */
    append(v: T): T[];
    /** Prepends one element. */
    prepend(v: T): T[];
    /** The elements failing the predicate. */
    reject(pred: (v: T) => boolean): T[];
    /** A copy with `v` inserted at `i`. */
    insert(i: number, v: T): T[];
    insertAll(i: number, values: T[]): T[];
    /** A copy with the element at `i` removed. */
    removeAt(i: number): T[];
    /** An object mapping this[i] (as key) to values[i]. */
    zipObj<U>(values: U[]): Record<string, U>;
    /** An object from [key, value] pairs. */
    fromPairs(): Record<string, T>;
    /** The median numeric element. */
    median(): number;
    /** The product of numeric elements. */
    product(): number;
    /** Running accumulation: [x0, f(x0,x1), ...]. */
    scan<R>(fn: (acc: R, v: T) => R, seed: R): R[];
    /** Counts elements per key. */
    countBy<K extends string | number>(key: (v: T) => K): Record<K, number>;
    /** Indexes elements by a key. */
    indexBy<K extends string | number>(key: (v: T) => K): Record<K, T>;
    /** A copy without the elements equal to `value`. */
    remove(value: T): T[];
    /** Removes every occurrence of `value`. */
    exclude(value: T): T[];
    /** Removes `count` elements starting at `from` (second argument is a COUNT, not an end index; clamped). */
    removeRange(from: number, count: number): T[];
    /** Splits at the first index where pred changes. */
    splitWhen(pred: (v: T) => boolean): [T[], T[]];
    /** Keeps the elements for which `pred(a, b)` holds for some b in `other`. */
    innerJoin(pred: (a: T, b: T) => boolean, other: T[]): T[];
    /** True when the array starts with the given prefix. */
    startsWith(prefix: T[]): boolean;
    endsWith(suffix: T[]): boolean;
    /** Flattens nested arrays exactly one level (NOT the recursive flatten). */
    unnest(): T[];
    /** Drops consecutive duplicates. */
    dropRepeats(): T[];
    dropRepeatsWith(eq: (a: T, b: T) => boolean): T[];
    dropRepeatsBy<K>(key: (v: T) => K): T[];
    /** Sorts by an array of comparators. */
    sortWith(comparators: ((a: T, b: T) => number)[]): T[];
    /** Union with a custom equality. */
    unionWith(eq: (a: T, b: T) => boolean, other: T[]): T[];
    differenceWith(eq: (a: T, b: T) => boolean, other: T[]): T[];
    /** Elements in exactly one of the arrays. */
    symmetricDifference(other: T[]): T[];
    symmetricDifferenceWith(eq: (a: T, b: T) => boolean, other: T[]): T[];
    /** Reduces per key. */
    reduceBy<K extends string | number, R>(fn: (acc: R, v: T) => R, seed: R, key: (v: T) => K): Record<K, R>;
    /** Transducer composition over the array. */
    transduce<R>(xf: unknown, fn: (acc: R, v: T) => R, seed: R): R;
    /** Converts into another structure via a transducer. */
    into(target: unknown, xf: unknown): unknown;
    /** Traverses applicatively. */
    sequence<U>(of: (v: T) => U): U;
    traverse<U>(fn: (v: T) => U, of: (v: T) => U): U;
    /** Index-aware map from `startIndex`; `loop` wraps around to index 0. */
    mapFromIndex<R>(startIndex: number, fn: (v: T, i: number) => R, context?: unknown): R[];
    mapFromIndex<R>(startIndex: number, loop: boolean, fn: (v: T, i: number) => R, context?: unknown): R[];
    forEachFromIndex(startIndex: number, fn: (v: T, i: number) => void, context?: unknown): void;
    forEachFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => void, context?: unknown): void;
    filterFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): T[];
    filterFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): T[];
    findFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): T | undefined;
    findFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): T | undefined;
    findIndexFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): number;
    findIndexFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): number;
    someFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    someFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    everyFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    everyFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    reduceFromIndex<R>(startIndex: number, fn: (acc: R, v: T, i: number) => R, seed?: R): R;
    reduceRightFromIndex<R>(startIndex: number, fn: (acc: R, v: T, i: number) => R, seed?: R): R;
    /** A lazy Iterator over the array's elements; composes with the Iterator helpers. */
    lazy(): Iterator<T>;
}

/** Array static extensions. */
interface ArrayConstructor {
    /** Repeats `value` n times. */
    repeat<T>(value: T, n: number): T[];
    /** Creates an array from an async iterable. */
    fromAsync<T>(iterable: AsyncIterable<T> | Iterable<T | PromiseLike<T>>): Promise<T[]>;
}

/** Map.prototype extensions (get-or-insert). */
interface Map<K, V> {
    /** The stored value for `key`, inserting `value` (default undefined) when absent. */
    getOrInsert(key: K, value?: V): V;
    /** The stored value for `key`, inserting `fn(key)` when absent. */
    getOrInsertComputed(key: K, fn: (key: K) => V): V;
}

/** Set static extensions. */
interface SetConstructor {
    /** Groups the iterable into a Map of key -> values array in input order. */
    groupBy<K, T>(items: Iterable<T>, fn: (value: T) => K): Map<K, T[]>;
}

/** Number.prototype extensions (absent under --no-prototypes). */
interface Number {
    abs(): number;
    sqrt(): number;
    exp(): number;
    sin(): number;
    cos(): number;
    tan(): number;
    asin(): number;
    acos(): number;
    atan(): number;
    negate(): number;
    inc(): number;
    dec(): number;
    add(other: number): number;
    subtract(other: number): number;
    multiply(other: number): number;
    divide(other: number): number;
    modulo(other: number): number;
    pow(other: number): number;
    gt(other: number): boolean;
    gte(other: number): boolean;
    lt(other: number): boolean;
    lte(other: number): boolean;
    isInteger(): boolean;
    isOdd(): boolean;
    isEven(): boolean;
    isMultipleOf(other: number): boolean;
    /** The modulo with the sign of the divisor. */
    mathMod(other: number): number;
    clamp(lo: number, hi: number): number;
    /** The logarithm in base `base` (default e). */
    log(base?: number): number;
    /** Rounds half away from zero to `precision` decimal places (negative: tens/hundreds). */
    round(precision?: number): number;
    ceil(precision?: number): number;
    floor(precision?: number): number;
    /** The character for this code point. */
    chr(): string;
    /** Zero-pads to `place` digits (above 65536 is clamped); `sign` forces the sign; `base` is 2..36. */
    pad(place?: number, sign?: boolean, base?: number): string;
    /** The number as lowercase hex, zero-padded to `place` digits (same 65536 clamp). */
    hex(place?: number): string;
    /** Locale-style grouping formatting: place, thousands separator, decimal separator. */
    format(place?: number, thousands?: string, decimal?: string): string;
    /** A compact human abbreviation (1.2k, 3.4m; lowercase k/m/b/t). */
    abbr(precision?: number): string;
    /** SI-prefixed magnitude. */
    metric(precision?: number): string;
    /** Byte-count formatting. */
    bytes(precision?: number): string;
    /** 1st, 2nd, 3rd, ... */
    ordinalize(): string;
    /** The number formatted as a duration. */
    duration(): string;
    /** Calls fn(i) n times; returns the results (fn defaults to identity). */
    times<R = number>(fn?: (i: number) => R): R[];
    /** Iterates from this number up to `end` inclusive by `step`; returns the results, mapped by `fn` when given. */
    upto<R = number>(end: number, step?: number, fn?: (value: number, index: number) => R): R[];
    /** Iterates from this number down to `end` inclusive by `step`; returns the results, mapped by `fn` when given. */
    downto<R = number>(end: number, step?: number, fn?: (value: number, index: number) => R): R[];
}

/** Number static extensions. */
interface NumberConstructor {
    /** An ARRAY of numbers [start, end), EXCLUSIVE of end; mathx.linspace is the inclusive n-point grid. */
    range(start: number, end?: number, step?: number): number[];
}

/** Object static extensions: functional helpers over plain objects (pick, omit, path, merge, ...). */
interface ObjectConstructor {
    isObject(v: unknown): v is object;
    isArray(v: unknown): v is unknown[];
    isBoolean(v: unknown): v is boolean;
    isNumber(v: unknown): v is number;
    isString(v: unknown): v is string;
    isFunction(v: unknown): v is (...args: unknown[]) => unknown;
    isDate(v: unknown): v is Date;
    isRegExp(v: unknown): v is RegExp;
    isError(v: unknown): v is Error;
    isSet(v: unknown): v is Set<unknown>;
    isMap(v: unknown): v is Map<unknown, unknown>;
    isArguments(v: unknown): boolean;
    isNil(v: unknown): v is null | undefined;
    isNotNil(v: unknown): boolean;
    /** A short type name: "String", "Number", "Object", ... */
    type(v: unknown): string;
    /** `value` when not nil, else the default. */
    defaultTo<T>(def: T, value: unknown): T;
    /** The number of own enumerable properties. */
    size(obj: object): number;
    isEmpty(obj: object): boolean;
    /** Swaps keys and values. Canonical spelling of the pair. */
    invert(obj: object): Record<string, string>;
    /** LEGACY alias of invert; prefer invert. */
    invertObj(obj: object): Record<string, string>;
    objOf<K extends string, V>(key: K, value: V): Record<K, V>;
    /** An object of the picked keys. */
    pick(keys: string[], obj: object): Record<string, unknown>;
    /** An object without the given keys. */
    omit(keys: string[], obj: object): Record<string, unknown>;
    pickBy(pred: (value: unknown, key: string) => boolean, obj: object): Record<string, unknown>;
    /** [key, value] pairs. */
    toPairs(obj: object): [string, unknown][];
    /** An object from [key, value] pairs. */
    fromPairs(pairs: [string, unknown][]): Record<string, unknown>;
    /** A copy with a property set. */
    assoc(key: string, value: unknown, obj: object): Record<string, unknown>;
    /** A copy with a property removed. */
    dissoc(obj: object, key: string): Record<string, unknown>;
    /** Calls fn with obj and returns obj. */
    tap<T>(fn: (v: T) => void, value: T): T;
    /** A deep clone (a cyclic structure throws RangeError). */
    clone<T>(value: T): T;
    /** Deep structural equality; a cyclic operand overflows the C stack with a RangeError (as clone). */
    equals(a: unknown, b: unknown): boolean;
    /** Object.is identity. */
    identical(a: unknown, b: unknown): boolean;
    /** The value at a key. */
    prop(key: string, obj: object): unknown;
    propOr(def: unknown, key: string, obj: object): unknown;
    /** The values at several keys. */
    props(keys: string[], obj: object): unknown[];
    /** The value at a nested path. */
    path(path: string[], obj: object): unknown;
    pathOr(def: unknown, path: string[], obj: object): unknown;
    /** Values at several paths. */
    paths(paths: string[][], obj: object): unknown[];
    /** A copy with a nested path set. */
    assocPath(path: string[], value: unknown, obj: object): Record<string, unknown>;
    /** A copy with a nested path removed. */
    dissocPath(path: string[], obj: object): Record<string, unknown>;
    hasPath(path: string[], obj: object): boolean;
    /** True when the key is present anywhere on the chain. */
    has(key: string, obj: object): boolean;
    hasIn(key: string, obj: object): boolean;
    keysIn(obj: object): string[];
    valuesIn(obj: object): unknown[];
    propEq(value: unknown, key: string, obj: object): boolean;
    eqProps(key: string, a: object, b: object): boolean;
    pathEq(value: unknown, path: string[], obj: object): boolean;
    /** True when obj satisfies every {key: predicate}. */
    where(spec: Record<string, (v: unknown) => boolean>, obj: object): boolean;
    /** True when obj matches the {key: value} spec. */
    whereEq(spec: Record<string, unknown>, obj: object): boolean;
    /** Shallow merge where later sources win; mergeLeft is the first-wins variant. */
    mergeRight(a: object, b: object): Record<string, unknown>;
    /** LEGACY alias of mergeRight; prefer mergeRight. */
    merge(a: object, b: object): Record<string, unknown>;
    mergeLeft(a: object, b: object): Record<string, unknown>;
    /** Deep merge. */
    mergeDeepRight(a: object, b: object): Record<string, unknown>;
    mergeDeepLeft(a: object, b: object): Record<string, unknown>;
    /** The value at a path, or undefined. */
    get(obj: object, path: string | string[]): unknown;
    /** Sets a path IN PLACE and returns `obj`; only OWN properties are walked or created, so a path can never reach a prototype. */
    set(obj: object, path: string | string[], value: unknown): Record<string, unknown>;
    /** Fills missing keys with defaults. */
    defaults(defaults: object, obj: object): Record<string, unknown>;
    /** A copy with {key: fn} applied per key. */
    evolve(transformations: Record<string, (v: unknown) => unknown>, obj: object): Record<string, unknown>;
    mapObjIndexed<R>(fn: (value: unknown, key: string) => R, obj: object): Record<string, R>;
    forEachObjIndexed(fn: (value: unknown, key: string) => void, obj: object): void;
    /** A copy with keys transformed by fn. */
    mapKeys(fn: (key: string) => string, obj: object): Record<string, unknown>;
    /** Merge with a custom value combine. */
    mergeWith(fn: (a: unknown, b: unknown) => unknown, a: object, b: object): Record<string, unknown>;
    mergeWithKey(fn: (key: string, a: unknown, b: unknown) => unknown, a: object, b: object): Record<string, unknown>;
    /** A copy with one key transformed. */
    modify(key: string, fn: (v: unknown) => unknown, obj: object): Record<string, unknown>;
    modifyPath(path: string[], fn: (v: unknown) => unknown, obj: object): Record<string, unknown>;
    /** pick including absent keys as undefined. */
    pickAll(keys: string[], obj: object): Record<string, unknown>;
    /** Projects objects onto the given keys. */
    project(keys: string[], objs: object[]): Record<string, unknown>[];
    propSatisfies(pred: (v: unknown) => boolean, key: string, obj: object): boolean;
    pathSatisfies(pred: (v: unknown) => boolean, path: string[], obj: object): boolean;
    /** True when obj satisfies any of the where spec's predicates. */
    whereAny(spec: Record<string, (v: unknown) => boolean>, obj: object): boolean;
    /** A copy with keys renamed per {old: new}. */
    renameKeys(map: Record<string, string>, obj: object): Record<string, unknown>;
    propIs(type: Function, key: string, obj: object): boolean;
    /** Groups array items by a key function. */
    groupBy<K extends string | number>(items: unknown[], key: (item: unknown) => K): Record<K, unknown[]>;
}

/* Legacy accessors live on Object.prototype, not the constructor. */
interface Object {
    __defineGetter__(property: string, getter: (this: unknown) => unknown): void;
    __defineSetter__(property: string, setter: (this: unknown, value: unknown) => void): void;
    __lookupGetter__(property: string): ((this: unknown) => unknown) | undefined;
    __lookupSetter__(property: string): ((this: unknown, value: unknown) => void) | undefined;
}

/** Date.prototype extensions (absent under --no-prototypes). */
interface Date {
    isValid(): boolean;
    isToday(): boolean;
    isYesterday(): boolean;
    isTomorrow(): boolean;
    isFuture(): boolean;
    isPast(): boolean;
    isWeekday(): boolean;
    isWeekend(): boolean;
    isLeapYear(): boolean;
    isSunday(): boolean;
    isMonday(): boolean;
    isTuesday(): boolean;
    isWednesday(): boolean;
    isThursday(): boolean;
    isFriday(): boolean;
    isSaturday(): boolean;
    isJanuary(): boolean;
    isFebruary(): boolean;
    isMarch(): boolean;
    isApril(): boolean;
    isMay(): boolean;
    isJune(): boolean;
    isJuly(): boolean;
    isAugust(): boolean;
    isSeptember(): boolean;
    isOctober(): boolean;
    isNovember(): boolean;
    isDecember(): boolean;
    getWeekday(): number;
    getISOWeek(): number;
    daysInMonth(): number;
    isBefore(other: Date): boolean;
    isAfter(other: Date): boolean;
    isBetween(a: Date, b: Date): boolean;
    millisecondsSince(other: Date): number;
    millisecondsUntil(other: Date): number;
    millisecondsAgo(): number;
    millisecondsFromNow(): number;
    secondsSince(other: Date): number;
    secondsUntil(other: Date): number;
    secondsAgo(): number;
    secondsFromNow(): number;
    minutesSince(other: Date): number;
    minutesUntil(other: Date): number;
    minutesAgo(): number;
    minutesFromNow(): number;
    hoursSince(other: Date): number;
    hoursUntil(other: Date): number;
    hoursAgo(): number;
    hoursFromNow(): number;
    daysSince(other: Date): number;
    daysUntil(other: Date): number;
    daysAgo(): number;
    daysFromNow(): number;
    weeksSince(other: Date): number;
    weeksUntil(other: Date): number;
    weeksAgo(): number;
    weeksFromNow(): number;
    monthsSince(other: Date): number;
    monthsUntil(other: Date): number;
    monthsAgo(): number;
    monthsFromNow(): number;
    yearsSince(other: Date): number;
    yearsUntil(other: Date): number;
    yearsAgo(): number;
    yearsFromNow(): number;
    addMilliseconds(n: number): Date;
    addSeconds(n: number): Date;
    addMinutes(n: number): Date;
    addHours(n: number): Date;
    addDays(n: number): Date;
    addWeeks(n: number): Date;
    addMonths(n: number): Date;
    addYears(n: number): Date;
    beginningOfDay(): Date;
    endOfDay(): Date;
    beginningOfWeek(): Date;
    endOfWeek(): Date;
    beginningOfMonth(): Date;
    endOfMonth(): Date;
    beginningOfYear(): Date;
    endOfYear(): Date;
    /** Adds a {days?, months?, ...} duration. */
    advance(delta: Record<string, number>): Date;
    /** Subtracts a {days?, months?, ...} duration. */
    rewind(delta: Record<string, number>): Date;
    clone(): Date;
    /** Formats with a layout string. */
    format(layout: string): string;
    /** A relative phrase like "3 days ago". */
    relative(): string;
    /** The ISO 8601 string. */
    iso(): string;
    /** Legacy year accessors and GMT alias. */
    getYear(): number;
    setYear(year: number): number;
    toGMTString(): string;
}

/** RegExp static extensions. */
interface RegExpConstructor {
    /** Escapes a literal string for use in a RegExp. */
    escape(text: string): string;
    /** Legacy statics of the last successful built-in match in this realm (only `input` is settable); reading them through a subclass, or after a subclass match, throws TypeError. */
    lastMatch: string; lastParen: string; leftContext: string; rightContext: string; input: string;
    /** Aliases (`$_` input, `$&` lastMatch, `$+` lastParen, `` $` `` leftContext, `$'` rightContext) and `$1`-`$9`, the first nine groups. */
    "$_": string; "$&": string; "$+": string; "$`": string; "$'": string; $1: string; $2: string; $3: string; $4: string; $5: string; $6: string; $7: string; $8: string; $9: string;
}

/** Promise static extensions (await-dictionary). */
interface PromiseConstructor {
    /** Promise.all over an object's enumerable own properties; resolves to a null-prototype object with the same keys. */
    allKeyed<T extends object>(promises: T): Promise<{ [K in keyof T]: Awaited<T[K]> }>;
    /** Promise.allSettled over an object's enumerable own properties, keyed the same way. */
    allSettledKeyed<T extends object>(promises: T): Promise<{ [K in keyof T]: PromiseSettledResult<Awaited<T[K]>> }>;
}

/** Immutable ArrayBuffers: no write through any view succeeds, and the buffer cannot be detached, resized or transferred. */
interface ArrayBuffer {
    /** Whether this buffer is immutable. */
    readonly immutable: boolean;
    /** Moves the contents (optionally resized) into a new immutable buffer and detaches this one. */
    transferToImmutable(newLength?: number): ArrayBuffer;
    /** Copies bytes [start, end) into a new immutable buffer; this buffer is left as it is. */
    sliceToImmutable(start?: number, end?: number): ArrayBuffer;
}

/** RegExp.prototype extensions. A match has no step limit of its own: catastrophic backtracking such as /(a+)+$/ on a long near-miss runs until it finishes or `--timeout-ms` interrupts it (uncatchably, nonzero exit), so untrusted patterns or subjects need that flag. The backtrack stack is capped at 256 MB; past it a match throws InternalError("out of memory in regexp execution"). A greedy `X*` or `X+` whose X is one character matcher (a literal, `.`, `[\s\S]`, `\s`, `\S`, a class, incl. /i and /u forms) takes constant stack however long the run; any other repeated piece (a group, an alternation, a lazy or bounded quantifier) keeps at least one 24-byte frame per iteration open (64 bytes for a two-way alternation), so e.g. /^(?:a|b)*X/ fails past about 4M iterations. */
interface RegExp {
    /** Whether the v flag is set. */
    readonly unicodeSets: boolean;
}