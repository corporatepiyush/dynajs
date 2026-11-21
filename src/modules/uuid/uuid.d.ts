/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:uuid
/** dyna:uuid — UUID v1–v8 from the OS CSPRNG, parse/validate/compare, plus NanoID and ULID (optionally monotonic). */
declare module "dyna:uuid" {
    /** Random version-4 UUID (OS CSPRNG); unknown option keys are refused (TypeError). */
    function v4(opts?: { as?: "string" }): string;
    /** The 16 bytes a v4 string would encode, for DB-key paths. */
    function v4(opts: { as: "bytes" }): Uint8Array;
    /** Time-ordered v7 UUID (48-bit ms + RFC 9562 same-ms counter; OS CSPRNG random bytes): later ids sort after earlier ones. */
    function v7(opts?: { as?: "string" }): string;
    /** The 16 bytes a v7 string would encode (same counter logic). */
    function v7(opts: { as: "bytes" }): Uint8Array;
    /** RFC 9562 v8: the caller supplies all 16 bytes; only the version nibble and variant bits are overwritten; wrong length is a RangeError. */
    function v8(opts: { fill: ByteView }): string;
    /** -1 | 0 | 1 over the canonical 16 bytes (the order v7 ids sort in); any accepted string form works. */
    function compare(a: string, b: string): -1 | 0 | 1;
    /** MD5-based name UUID. */
    function v3(namespace: string | ByteView, name: BytesInput): string;
    /** SHA-1-based name UUID. */
    function v5(namespace: string | ByteView, name: BytesInput): string;
    /** Parses any accepted form and returns the canonical lowercase string. */
    function parse(uuid: string): string;
    /** True iff the argument is a string in an accepted form. */
    function validate(value: unknown): boolean;
    /** The version nibble; throws on a malformed string. */
    function version(uuid: string): number;
    /** "NCS", "RFC4122", "Microsoft" or "Future". */
    function variant(uuid: string): string;
    /** The 16 raw bytes of a parsed UUID. */
    function bytes(uuid: string): Uint8Array;
    /** The canonical string for exactly 16 bytes. */
    function fromBytes(bytes: ByteView): string;
    /** URL-safe ID over the default 64-symbol alphabet (OS CSPRNG); size 1..4096. */
    function NanoID(size?: number): string;
    /** The same generator over a caller-supplied alphabet of 2..256 ASCII symbols. */
    function NanoIDAlphabet(alphabet: string, size?: number): string;
    /** A 26-character Crockford base32 ULID; {monotonic: true} makes same-millisecond calls strictly ascending process-wide (default: fresh entropy per call). */
    function ULID(atMillis?: number | bigint, opts?: { monotonic?: boolean }): string;
    /** The millisecond timestamp encoded in the first 10 characters. */
    function ULIDTime(ulid: string): number;
    /** The all-zero UUID. */
    const NIL: string;
    /** The all-ones UUID. */
    const MAX: string;
    /** Predefined RFC 4122 name namespaces. */
    const NAMESPACE_DNS: string;
    const NAMESPACE_URL: string;
    const NAMESPACE_OID: string;
    const NAMESPACE_X500: string;
}
