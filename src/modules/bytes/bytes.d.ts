/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:bytes
/** dyna:bytes — copied byte buffers (Bytes), UTF-8-safe strings (Text), search and fixed-width reads/writes; every method is also a free function taking the buffer first.
 *  METHOD <-> FREE-FUNCTION MAP: same names except Bytes.equals(v) <-> equal(a, b) and includes <-> contains; method fill returns the handle, free fill the view. */
declare module "dyna:bytes" {
    /** Copied byte buffer: construction, slicing, search, text interpretation; fixed-width access is a DataView over `array`. */
    class Bytes {
        /** The bytes are copied, never aliased, so a later write through the source cannot invalidate the cached `isAscii`/`isValidUtf8` flags. */
        constructor(data: string | ByteView);
        /** Zero-filled buffer; n coerces like ToIndex (NaN -> 0, 3.7 -> 3); a negative or Infinity n is TypeError, above 2^31-1 RangeError. */
        static alloc(n: number): Bytes;
        /** True when `v` is a Bytes handle. */
        static isBytes(v: unknown): v is Bytes;
        /** One allocation, sized in a first pass; every element must be a byte-addressed view. */
        static concat(list: ByteView[]): Bytes;
        /** Variadic form; a leading array and further views mix freely. */
        static concat(...views: ByteView[]): Bytes;
        /** The byte count. */
        get length(): number;
        /** True when no byte has the high bit set; cached at construction, recomputed once `array` has been read, so writes through it are seen. */
        get isAscii(): boolean;
        /** True when the bytes are well-formed UTF-8, cached like isAscii; also a getter on Text and the free function isValidUtf8(data). */
        get isValidUtf8(): boolean;
        /** The live backing Uint8Array, bounded to this handle's bytes (a slice's array starts and ends with the slice). */
        get array(): Uint8Array;
        /** A new Bytes handle that is a view sharing the owner's ArrayBuffer. */
        slice(start?: number, end?: number): Bytes;
        /** Lexicographic byte comparison; -1, 0, or 1. */
        compare(other: ByteView): number;
        /** True when the other view has identical length and bytes. */
        equals(other: ByteView): boolean;
        /** First position of a byte value (0..255) or byte view from `fromIndex` (negative clamps to 0); -1 when absent. */
        indexOf(needle: number | ByteView, fromIndex?: number): number;
        /** Last position of the needle, searching backward from `fromIndex`; the empty needle matches at `length`. */
        lastIndexOf(needle: number | ByteView, fromIndex?: number): number;
        /** True when the needle occurs. */
        includes(needle: number | ByteView): boolean;
        /** Count of non-overlapping occurrences from `fromIndex`; the empty needle counts `length - fromIndex + 1`. */
        count(needle: number | ByteView, fromIndex?: number): number;
        /** First position at or after `fromIndex` holding any byte of `chars`, or -1. */
        indexOfAny(chars: ByteView, fromIndex?: number): number;
        /** True when the needle (byte value or view) sits exactly at `fromIndex` (default 0). */
        startsWith(needle: number | ByteView, fromIndex?: number): boolean;
        /** True when the needle ENDS exactly at `end` (exclusive; default `length`). */
        endsWith(needle: number | ByteView, end?: number): boolean;
        toString(): string;
    }

    /** Wraps a JS string and caches isWide in one scan at construction. */
    class Text {
        /** Wraps a JS string and caches `isWide` (whether any code unit is above U+00FF) in one scan at construction. */
        constructor(s: string);
        /** True when any code unit is above U+00FF. */
        get isWide(): boolean;
        /** The wrapped string. */
        get value(): string;
        /** Method-vs-getter note: a GETTER, not a method — true when the string encodes to well-formed UTF-8 (it holds no lone surrogate). */
        get isValidUtf8(): boolean;
        /** True when the string has no lone surrogate. */
        isValidUtf16(): boolean;
        /** UTF-8 code points of the string. */
        countUtf8(): number;
        /** Code points, surrogate pairs counted once. */
        countUtf16(): number;
        /** Each input byte as a Latin-1 code point re-encoded to UTF-8. */
        latin1ToUtf8(): Uint8Array;
        /** Throws RangeError on invalid UTF-8 or any code point above 0xFF. */
        utf8ToLatin1(): Uint8Array;
        /** UTF-16LE bytes from UTF-8; strict, throws RangeError on malformed input. */
        utf8ToUtf16(): Uint8Array;
        /** Strict/lossless UTF-16LE to UTF-8; throws on odd length or an ill-formed surrogate. */
        utf16ToUtf8(): Uint8Array;
        /** The string as a Bytes handle. */
        toBytes(): Bytes;
        toJSON(): string;
        toString(): string;
    }

    /** Lexicographic byte comparison; -1, 0, or 1. */
    function compare(a: ByteView, b: ByteView): number;
    /** True for identical bytes. */
    function equal(a: ByteView, b: ByteView): boolean;
    /** First position of the needle (byte value or view) from `fromIndex`, or -1. */
    function indexOf(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** Last position of the needle, searching backward from `fromIndex`. */
    function lastIndexOf(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** True when the needle occurs. */
    function contains(buf: ByteView, needle: number | ByteView): boolean;
    /** Number of non-overlapping occurrences; the scan starts at `fromIndex`. */
    function count(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** Concatenates byte views into one Uint8Array. */
    function concat(list: ByteView[]): Uint8Array;
    /** Overlap-safe byte copy returning the number of bytes copied. */
    function copy(dst: Uint8Array, src: ByteView, dstOff?: number, srcOff?: number, len?: number): number;
    /** Well-formed UTF-8 check over a string or byte view. */
    function isValidUtf8(data: BytesInput): boolean;
    /** Well-formed UTF-16LE check (even length, paired surrogates). */
    function isValidUtf16(u16bytes: ByteView): boolean;
    /** UTF-8 code points (assumes valid UTF-8). */
    function countUtf8(data: BytesInput): number;
    /** Code points, pairs once, no validation. */
    function countUtf16(u16bytes: ByteView): number;
    /** Each input byte as a Latin-1 code point re-encoded to UTF-8. */
    function latin1ToUtf8(bytes: ByteView): Uint8Array;
    /** Throws RangeError on invalid UTF-8 or code points above 0xFF. */
    function utf8ToLatin1(bytes: ByteView): Uint8Array;
    /** Strict UTF-8 to UTF-16LE; throws on malformed input. */
    function utf8ToUtf16(bytesOrString: BytesInput): Uint8Array;
    /** Strict UTF-16LE to UTF-8; throws on odd length or an ill-formed surrogate. */
    function utf16ToUtf8(u16bytes: ByteView): Uint8Array;
    /** Decodes a byte view from a legacy single-byte charset; unknown label throws RangeError. */
    function decode(bytes: ByteView, label: string): string;
    /** Encodes a string into a byte view; unexpressible code points become `?`. For utf-8 a lone surrogate becomes U+FFFD (EF BF BD), as TextEncoder does. */
    function encode(text: string, label: string): Uint8Array;
    /** True for every built label plus utf-8/utf8; ASCII case-insensitive. */
    function encodingExists(label: string): boolean;
    /** The array of every label this build can decode, beginning with utf-8. */
    function encodings(): string[];

}
