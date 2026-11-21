/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:encoding
/** dyna:encoding — byte<->text codecs (hex, base64/32/58/85, varints), charset detection, JSON5, stable JSON, JSONPath, QR codes.
 *  These are BYTE codecs; the global atob/btoa are latin-1 STRING codecs and give different answers, so never mix the two families. */
declare module "dyna:encoding" {

    /** Lowercase hex string, SIMD-accelerated. */
    function HexEncode(data: BytesInput): string;
    /** Hex text into a caller buffer; returns the 2*n bytes written; `out` needs 2*n bytes (RangeError, nothing written); overlap refuses. */
    function hexEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Returns a Uint8Array; throws SyntaxError on odd length or an invalid digit. */
    function HexDecode(text: string): Uint8Array;
    /** Decodes hex into a caller buffer; returns bytes written; `out` needs text.length/2 bytes or RangeError. */
    function hexDecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 base64 (`+/`, padded) over BYTES: Base64Encode(utf8 "é") is "w6k=" while the latin-1 btoa("é") is "6Q==". */
    function Base64Encode(data: BytesInput): string;
    /** Base64 text into a caller buffer; returns the byte count; `out` needs 4*ceil(n/3) bytes or RangeError. */
    function base64EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Byte-level RFC 4648 decode (not interchangeable with atob output). */
    function Base64Decode(text: string): Uint8Array;
    /** Decodes base64 into a caller buffer; returns the byte count; `out` needs 3*(text.length/4) bytes or RangeError. */
    function base64DecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 section 5 base64url (no padding). */
    function Base64URLEncode(data: BytesInput): string;
    /** Unpadded base64url text into a caller buffer; `out` needs 4*ceil(n/3) bytes or RangeError (up to 3 bytes past the count are scratch). */
    function base64UrlEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** A length of `4k+1` throws `SyntaxError` (no byte count encodes to that many characters), and a stray `+`/`/` is rejected. */
    function Base64URLDecode(text: string): Uint8Array;
    /** Decodes base64url into a caller buffer; `out` needs 3*((text.length+3)/4) bytes or RangeError. */
    function base64UrlDecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 base32, `=` padded. */
    function Base32Encode(data: BytesInput): string;
    /** Base32 text into a caller buffer; `out` needs ((n+4)/5)*8 bytes or RangeError. */
    function base32EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Throws `SyntaxError` on invalid input. */
    function Base32Decode(text: string): Uint8Array;
    /** Decodes padded base32 (length % 8 == 0) into a caller buffer; `out` needs (text.length/8)*5 bytes or RangeError. */
    function base32DecodeInto(text: string, out: Uint8Array): number;
    /** Extended-hex base32. */
    function Base32HexEncode(data: BytesInput): string;
    /** Base32hex text into a caller buffer; capacity as base32EncodeInto. */
    function base32HexEncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base32HexDecode(text: string): Uint8Array;
    /** base32hex into a caller-owned buffer; capacity as base32DecodeInto. */
    function base32HexDecodeInto(text: string, out: Uint8Array): number;
    /** Adobe-less ascii85 with the `z` shorthand. */
    function Base85Encode(data: BytesInput): string;
    /** Ascii85 text into a caller buffer; `out` needs ((n+3)/4)*5 bytes or RangeError. */
    function base85EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Skips whitespace (space/tab/CR/LF/VT/FF) for line-wrapped formats. */
    function Base85Decode(text: string): Uint8Array;
    /** Decodes ascii85 into a caller buffer; `out` needs 4*text.length bytes or RangeError. */
    function base85DecodeInto(text: string, out: Uint8Array): number;
    /** Bitcoin base58; leading zero bytes become leading `1`s. */
    function Base58Encode(data: BytesInput): string;
    /** Base58 text into a caller buffer, zero-allocation; `out` needs (n*8)/5+1 bytes; input over 4096 bytes refuses (the codec is quadratic). */
    function base58EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Throws `SyntaxError` on a character outside the alphabet. */
    function Base58Decode(text: string): Uint8Array;
    /** Decodes base58 into a caller buffer; `out` needs text.length bytes or RangeError. */
    function base58DecodeInto(text: string, out: Uint8Array): number;
    /** Base58 with a double-SHA256 checksum appended. */
    function Base58CheckEncode(data: BytesInput): string;
    /** Base58check text into a caller buffer; `out` needs ((n+4)*8)/5+1 bytes or RangeError. */
    function base58CheckEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Throws `SyntaxError` when the checksum does not match or the input is too short to carry one. */
    function Base58CheckDecode(text: string): Uint8Array;
    /** base58check into a caller-owned buffer; capacity as base58DecodeInto. */
    function base58CheckDecodeInto(text: string, out: Uint8Array): number;
    /** Encode in a caller-supplied alphabet of 2..255 distinct characters. */
    function BaseXEncode(data: BytesInput, alphabet: string): string;
    /** Decodes text written in a custom `alphabet`. */
    function BaseXDecode(text: string, alphabet: string): Uint8Array;
    /** LEB128 varint encoding of a non-negative value. */
    function PutUvarint(value: number | bigint): Uint8Array;
    /** Zigzag-encoded signed LEB128. */
    function PutVarint(value: number | bigint): Uint8Array;
    /** Decodes; returns [value, bytesRead]; magnitude is bigint when it exceeds 2^53-1. */
    function Uvarint(buf: ByteView): [number | bigint, number];
    /** Decodes a signed varint as [value, bytesRead]; uvarint is the unsigned form. */
    function Varint(buf: ByteView): [number | bigint, number];
    /** Writes the varint at `offset` and returns the NEW END offset; a buffer too small throws RangeError and writes nothing (at most 10 bytes per value). */
    function appendUvarint(buf: Uint8Array, value: number | bigint, offset?: number): number;
    /** The varint value at `offset` without the tuple: Number up to 2^53-1, BigInt above; truncated input returns 0, 64-bit overflow throws RangeError. */
    function uvarintAt(buf: ByteView, offset?: number): number | bigint;
    /** varintAt: the zigzag signed form of uvarintAt. */
    function varintAt(buf: ByteView, offset?: number): number | bigint;

    /** Charset detection options. */
    interface DetectOptions {
        fallback?: string;
        allowList?: string[];
    }
    /** Deterministic charset detection over a byte view; throws TypeError when allowList excludes the verdict. */
    function DetectEncoding(data: ByteView, opts?: DetectOptions): string;
    /** Same as DetectEncoding under its lowercase name. */
    function detectEncoding(data: ByteView, opts?: DetectOptions): string;

    /** JSON5 superset parsing; depth capped at 256 (RangeError). JSON Pointer/Patch live in dyna:json. */
    function JSON5Parse(text: string): unknown;
    /** JSON5 output with unquoted keys and NaN/Infinity literals; indent clamped to 0-10. */
    function JSON5Stringify(value: unknown, opts?: { indent?: number }): string;
    /** RFC 8785 canonical JSON; NaN/Infinity rejected. The canonical form is always compact, so `indent` is accepted and ignored. */
    function StableStringify(value: unknown, opts?: { indent?: number }): string;

    /** A compiled RFC 9535 JSONPath: the PATTERN query language (wildcards, filters, `$..x`); use dyna:json's Pointer for one exact address. */
    class JSONPath {
        /** Compiles the expression once and reuses it; a syntax error or an expression longer than 4096 bytes throws. */
        constructor(expression: string);
        /** Every match. */
        all(value: unknown): unknown[];
        /** The first match, or undefined when none. */
        first(value: unknown): unknown;
        /** Normalized path strings like $['store']['book'][0]['author']. */
        paths(value: unknown): string[];
    }

    /** QR options: ecc L/M/Q/H, version 1-40, mask 0-7. */
    interface QROptions {
        ecc?: "L" | "M" | "Q" | "H";
        version?: number;
        mask?: number;
    }
    /** Renders a QR symbol; a symbol holds at most 2953 bytes. */
    function QREncode(text: string, opts?: QROptions): { version: number; size: number; modules: Uint8Array };
    /** The same symbol as text with two half-blocks per cell. */
    function QRToString(text: string, opts?: QROptions): string;
}
