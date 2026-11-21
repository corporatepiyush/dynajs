/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:serialize
/** dyna:serialize — MessagePack, CBOR, BSON, structuredClone; decoders bounded for hostile input. */
declare module "dyna:serialize" {
    /** Options of the MessagePack/CBOR codecs and ValueHash. A non-object value (null and undefined mean none), an unknown key or a non-boolean bigint is a TypeError. */
    interface CodecOptions {
        /** Exact integer mode, false by default (owner decision D8). Decoders: an int beyond +-(2^53-1) becomes a BigInt instead of a Number rounded like JSON.parse. Encoders: an integer Number beyond +-(2^53-1) is written as a float64 instead of an int, so a decoder in bigint mode can tell it from the equal BigInt. */
        bigint?: boolean;
    }
    /** MessagePack encoding; symbols and functions are refused. The decoders cap depth and validate lengths before allocating. By default an integer Number in [-2^63, 2^64) is an int (uint64 from 2^63) and every other Number (fractions, -0, beyond) is a float64; with { bigint: true } only integers within +-(2^53-1) are ints. A BigInt in the int64 or uint64 range is an int in both modes (RangeError beyond). Strings are well-formed UTF-8 in both modes: a lone surrogate encodes as U+FFFD (WTF-8 bytes would not be valid MessagePack str). */
    function MsgPackEncode(value: unknown, opts?: CodecOptions): Uint8Array;
    /** Decodes one MessagePack value; depth-capped, lengths validated before allocation. By default every int is a Number, rounded like JSON.parse beyond +-(2^53-1); { bigint: true } decodes such an int to an exact BigInt. Invalid UTF-8, encoded surrogates included, decodes to U+FFFD per maximal subpart. */
    function MsgPackDecode(bytes: ByteView, opts?: CodecOptions): unknown;
    /** RFC 8949 CBOR with the same walker, bounds and refusals as MessagePack, except that a BigInt beyond the 64-bit int range is written as the shortest tag 2/3 bignum (magnitude up to 131072 bytes) instead of a RangeError. */
    function CBOREncode(value: unknown, opts?: CodecOptions): Uint8Array;
    /** Decodes one CBOR value under the same bounds and the same int and UTF-8 policy; major type 1 reaches -2^64. Tag 2/3 bignums decode like any int: a Number by default (rounded once from the exact value, Infinity past the double range), with { bigint: true } a BigInt beyond +-(2^53-1); the tag must wrap a definite byte string of at most 131072 bytes (SyntaxError / RangeError). */
    function CBORDecode(bytes: ByteView, opts?: CodecOptions): unknown;
    /** CBOR with map keys sorted byte-wise (deterministic form); the same options as CBOREncode. */
    function CBORCanonical(value: unknown, opts?: CodecOptions): Uint8Array;
    /** Canonical CBOR run through XXH64, returned as 16 lowercase hex chars; it inherits the CBOREncode policy and options. A lone surrogate hashes as U+FFFD in both modes. By default an integer Number in [-2^63, 2^64) hashes as an int, so ValueHash(2**60) === ValueHash(2n ** 60n); with { bigint: true } it hashes as a float64 and the two differ. */
    function ValueHash(value: unknown, opts?: CodecOptions): string;
    /** BSON (bsonspec.org 1.1) encoding of one document: a top-level object (not an array, Date, RegExp or bytes) whose own enumerable string keys are written in insertion order.
     *  Numbers: an integer in the int32 range is int32, any other number (and -0) is double; BigInt is int64 (RangeError outside it). Strings are UTF-8 (a lone surrogate becomes U+FFFD).
     *  Date is UTC datetime (RangeError when invalid), RegExp is a regex with its flags mapped to BSON options (i m s u; g y d are dropped), an ArrayBuffer or byte view is binary subtype 0,
     *  ObjectId/Timestamp/Decimal128/Binary/BSONRegExp/MinKey/MaxKey/Code are their BSON types. An undefined property is skipped; undefined or a hole in an array is null.
     *  A key or regex pattern with a NUL character, a function, a symbol and nesting beyond 200 levels throw. */
    function BSONEncode(document: object): Uint8Array;
    /** Decodes exactly one BSON document that spans the whole input. The bytes are untrusted: the document is fully validated first (every length bounded by its container, terminators,
     *  UTF-8 of keys and strings, boolean 0/1, known types, nesting <= 200) and a violation throws TypeError("BSONDecode: <what> at byte <offset>") before any value is built;
     *  decoding runs over a private copy, so nothing reached during decode can change the input under it.
     *  int32 and double are numbers, int64 is a BigInt, binary subtype 0 (and the old subtype 2) is a Uint8Array, other subtypes are Binary, a regex whose options are within i m s u is a RegExp
     *  (otherwise, or when JavaScript rejects the pattern, a BSONRegExp), the deprecated symbol type is a string and undefined is undefined; DBPointer and code-with-scope throw TypeError. */
    function BSONDecode(bytes: ByteView): Record<string, unknown>;
    /** The 12-byte BSON ObjectId: 4-byte big-endian seconds, 5 random bytes fixed per process, 3-byte big-endian counter from a random start. */
    class ObjectId {
        /** No argument generates a fresh id; a 24-character hex string, 12 bytes or another ObjectId copies one (anything else is a TypeError). */
        constructor(id?: string | ByteView | ObjectId);
        /** The 24 lowercase hex characters. */
        toHexString(): string;
        /** The same as toHexString(). */
        toString(): string;
        /** The same as toHexString(), so JSON.stringify writes the hex. */
        toJSON(): string;
        /** True for an ObjectId or a hex string naming the same 12 bytes. */
        equals(other: ObjectId | string): boolean;
        /** The embedded creation time, second precision. */
        getTimestamp(): Date;
        /** A copy of the 12 bytes. */
        readonly id: Uint8Array;
        /** Whether the constructor would accept the value (an absent value is false). */
        static isValid(id: unknown): boolean;
        /** An id whose time is the given seconds and whose other 8 bytes are zero (a range bound for queries by time). */
        static createFromTime(seconds: number): ObjectId;
    }
    /** The BSON timestamp: t seconds and i ordinal, both unsigned 32-bit. */
    class Timestamp {
        /** From { t, i } (integers within 0..2^32-1) or a BigInt whose high word is t and low word is i. */
        constructor(value: { t: number; i: number } | bigint);
        /** The seconds. */
        readonly t: number;
        /** The ordinal within the second. */
        readonly i: number;
        /** "Timestamp(t, i)". */
        toString(): string;
        /** { $timestamp: { t, i } } (MongoDB Extended JSON). */
        toJSON(): { $timestamp: { t: number; i: number } };
    }
    /** IEEE 754-2008 decimal128 (BID encoding), carried exactly; arithmetic is not provided. */
    class Decimal128 {
        /** Parses [-+]digits[.digits][E[-+]digits], Infinity or NaN. More than 34 significant digits that would need rounding, or an exponent outside -6176..6111, is a RangeError; other text is a TypeError. */
        constructor(value: string);
        /** The canonical string of the BSON Decimal128 specification (scientific notation when the exponent is positive or the adjusted exponent is below -6). */
        toString(): string;
        /** { $numberDecimal: string }. */
        toJSON(): { $numberDecimal: string };
        /** The nearest double. */
        toNumber(): number;
    }
    /** BSON binary with a subtype other than 0 (subtype 0 decodes to a plain Uint8Array). */
    class Binary {
        /** Copies the bytes; subType is 0..255 (default 0). */
        constructor(bytes: ByteView, subType?: number);
        /** A copy of the payload. */
        readonly buffer: Uint8Array;
        /** The BSON binary subtype. */
        readonly subType: number;
        /** The payload length in bytes. */
        readonly length: number;
    }
    /** A BSON regex kept as text: options JavaScript has no flag for (l, x), or a pattern JavaScript rejects. */
    class BSONRegExp {
        /** Neither string may contain a NUL character. */
        constructor(pattern: string, options?: string);
        /** The pattern text. */
        readonly pattern: string;
        /** The option letters. */
        readonly options: string;
    }
    /** The BSON MinKey: below every other value in BSON order. */
    class MinKey {
        constructor();
        /** { $minKey: 1 }. */
        toJSON(): { $minKey: 1 };
    }
    /** The BSON MaxKey: above every other value in BSON order. */
    class MaxKey {
        constructor();
        /** { $maxKey: 1 }. */
        toJSON(): { $maxKey: 1 };
    }
    /** BSON JavaScript code (type 0x0D), carried as text and never evaluated. */
    class Code {
        constructor(code: string);
        /** The source text. */
        readonly code: string;
    }
    /** Deep clone (the global structuredClone), HTML StructuredSerializeWithTransfer: preserves cycles and shared refs; accessors are read via [[Get]] into data properties; Error (by name), DOMException, boxed primitives and every view kind (DataView included, subclasses as their intrinsic) clone; functions, symbols, Proxies, detached buffers and other exotic objects throw DataCloneError.
     *  options.transfer: ArrayBuffers are detached after a successful clone, streams move (same thread); a duplicate, a SharedArrayBuffer, a detached buffer or a non-transferable throws DataCloneError, a non-object TypeError. */
    function structuredClone(value: unknown, options?: { transfer?: Iterable<unknown> }): unknown;
}
