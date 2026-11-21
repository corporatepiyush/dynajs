/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:url
declare module "dyna:url" {
    /** WHATWG URL. Every string input is a USVString: a lone surrogate becomes U+FFFD (percent-encoded %EF%BF%BD). Input over 65536 bytes throws; a non-ASCII host over 4096 mapped code points is invalid; href and origin are read-only.
 *  Setters (WHATWG basic URL parser with a state override): every setter but username/password first removes U+0009/000A/000D anywhere in the value; other C0 controls are kept and then percent-encoded (path, query, fragment, userinfo, opaque host) or refused (special host). A NUL is never truncating: it becomes %00, or makes host/hostname a no-op, or ends the port digits. */
    class URL {
        /** Parses `input` against an optional `base` URL; a bad `base` or unparsable `input` throws, and input longer than 65536 bytes throws a RangeError. Non-string input and base go through ToString (WebIDL USVString); a missing input is a TypeError. */
        constructor(input: unknown, base?: unknown);
        /** Parses without throwing: null for every failure the constructor would throw (a missing argument is still a TypeError; other values go through ToString). */
        static parse(input: unknown, base?: unknown): URL | null;
        /** parse() as a predicate. */
        static canParse(input: unknown, base?: unknown): boolean;
        /** Resolves `rel` against `base` and returns the href; THROWS TypeError when either is unparseable. */
        static join(base: string, rel: string): string;
        readonly href: string;
        /** Settable: scheme-start parse of value + ":" (stops at the first ":"; any other non-scheme character is a no-op). Special <-> non-special switches, "file" with credentials or a port, and leaving "file" with an empty host are no-ops; a port equal to the new scheme's default is dropped. */
        protocol: string;
        /** Settable unless the URL has no host, an empty host or the file scheme; percent-encoded with the userinfo set (no tab/newline stripping); "" clears it. */
        username: string;
        /** As username. */
        password: string;
        /** Settable; a `:port` suffix splits off; a forbidden host code point or an empty value makes the assignment a no-op. */
        host: string;
        /** Settable (lowercased; the port is untouched); invalid input is a no-op. */
        hostname: string;
        /** Settable; the leading ASCII digits are kept ("90x" sets 90; no leading digit is a no-op; above 65535 is a no-op); "" clears the port; no-op without a host or for file URLs. */
        port: string;
        /** Settable; parsed in path state: `?` starts the query, `#` the fragment, `.`/`..` resolve. */
        pathname: string;
        /** Settable; one leading `?` is a delimiter; "" clears the query. */
        search: string;
        /** Settable; one leading `#` is a delimiter; "" clears the fragment. */
        hash: string;
        readonly origin: string;
        /** Settable: assigning replaces the whole query with `new URLSearchParams(value)`'s serialization. */
        searchParams: URLSearchParams;
        /** The full href. */
        toJSON(): string;
        /** The full href. */
        toString(): string;
    }
    /** WHATWG query list; names and values are USVStrings (a lone surrogate becomes U+FFFD), and percent-decoded bytes that are not UTF-8 decode to U+FFFD. One obtained from url.searchParams (serialization leaves `~` unescaped, unlike the WHATWG form encoder, which writes %7E; kept deliberately) writes through to that URL. entries/keys/values are live iterators (the *Array() twins return arrays). */
    class URLSearchParams {
        constructor(init?: string | URLSearchParams | Array<[string, string]> | Record<string, string>);
        /** The number of name/value pairs. */
        readonly size: number;
        /** Appends a pair; a name that already exists keeps its other values. */
        append(name: string, value: string): void;
        /** With `value`, deletes only pairs matching BOTH name and value (WHATWG parity). */
        delete(name: string, value?: string): void;
        /** The first value for the name, or `null` when absent. */
        get(name: string): string | null;
        /** Every value for the name. */
        getAll(name: string): string[];
        /** With the optional value only pairs matching BOTH count (WHATWG has(name, value)). */
        has(name: string, value?: string): boolean;
        /** Replaces every pair with that name with a single pair. */
        set(name: string, value: string): void;
        /** Orders pairs by the DECODED key's byte order, in place. */
        sort(): void;
        /** The `name=value&...` form, re-encoded. */
        toString(): string;
        forEach(callback: (value: string, key: string, params: URLSearchParams) => void): void;
        [Symbol.iterator](): IterableIterator<[string, string]>;
        /** Iterator over [name, value] pairs, live between steps. */
        entries(): IterableIterator<[string, string]>;
        /** Iterator over names. */
        keys(): IterableIterator<string>;
        /** Iterator over values. */
        values(): IterableIterator<string>;
        /** Array forms of entries/keys/values, for code that indexes or measures the result. */
        entriesArray(): Array<[string, string]>;
        /** The names in order, as an array (the pre-iteration `keys()` form). */
        keysArray(): string[];
        /** The values in order, as an array (the pre-iteration `values()` form). */
        valuesArray(): string[];
    }
    /** IDNA 2008 / UTS #46 ToASCII (Unicode 16.0) with CONTEXTJ/CONTEXTO rules; {transitional} selects transitional processing. By default a disallowed code point (lone surrogates included) is dropped, as before policy P15 (owner decision D8); { strict: true } makes it a TypeError instead. Every other validation failure (bad A-label, hyphen placement, CONTEXTJ/CONTEXTO, bidi, DNS length) is a TypeError in both modes. A non-boolean strict and an unknown key are a TypeError. */
    function domainToASCII(domain: string, options?: { transitional?: boolean; strict?: boolean }): string;
    /** IDNA ToUnicode: decodes punycode labels for display. */
    function domainToUnicode(domain: string, options?: { transitional?: boolean }): string;
    /** RFC 3492 encoding; input over 1024 code points is refused. */
    function punycodeEncode(text: string): string;
    /** RFC 3492 decoding; capped at 1024 UTF-8 octets (RangeError beyond), the decode-side DoS bound. */
    function punycodeDecode(text: string): string;
    /** Percent-encodes the object's own enumerable string keys into a=1&b=2; `~` stays unescaped (WHATWG form encoding writes %7E; kept deliberately). */
    function formEncode(obj: Record<string, unknown>): string;
    /** Decodes `+` as space, keeps the LAST value per key. A lone surrogate in the text and percent-decoded bytes that are not UTF-8 (encoded surrogates %ED%A0%80 included) become U+FFFD per maximal subpart, as URLSearchParams does. */
    function formDecode(text: string): Record<string, string>;
    /** encodeURIComponent plus !'()~. */
    function encodeURIComponentStrict(text: string): string;
}
