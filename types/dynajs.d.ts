/**
 * DynaJS ambient type declarations.
 *
 * Covers: every `dyna:*` native module as `declare module "dyna:..."`, the
 * WHATWG globals the engine ships (fetch, Request, Response, Headers,
 * FormData, AbortController, AbortSignal, TextEncoder/TextDecoder,
 * performance, console, print, timers), the `std`/`os` modules available
 * with `--std`, and the core prototype extensions (Array, String, Number,
 * Object, Date, RegExp) as `declare global` interface augmentation.
 *
 * Pure types only: no runtime code, no implementations.
 *
 * Reference it from tsconfig/jsconfig:
 *   { "compilerOptions": { "types": ["../types/dynajs"] } }
 */

/* ------------------------------------------------------------------ *
 *  Shared helper types
 * ------------------------------------------------------------------ */

/** A byte-addressed view: a typed array of 1-byte elements, a DataView, or an ArrayBuffer. */
type ByteView = Uint8Array | Int8Array | Uint8ClampedArray | DataView | ArrayBuffer;

/** Input accepted wherever raw bytes are read: text encoded as UTF-8, or a byte view. */
type BytesInput = string | ByteView;

/** 4-byte-element typed arrays accepted by the f32 SIMD kernels. */
type F32Like = Float32Array | Int32Array | Uint32Array;

/** Decimal rounding modes (IEEE 754-2008 decimal128 names). */
type RoundingMode = "up" | "down" | "ceil" | "floor" | "halfUp" | "halfDown" | "halfEven" | "halfOdd";

/** Segment-tree fold operations. */
type SegOp = "sum" | "min" | "max";

/** A native resource: released explicitly or by the GC finalizer. */
interface DynResource {
    close(): void;
    dispose(): void;
    readonly closed: boolean;
    readonly [Symbol.dispose]: () => void;
}

/* ================================================================== *
 *  dyna:bytes
 * ================================================================== */
declare module "dyna:bytes" {
    /** Copied byte buffer: construction, slicing, search, fixed-width reads/writes, text interpretation. */
    class Bytes {
        constructor(data: string | ByteView);
        /** Zero-filled buffer; lengths up to 2^31 bytes. */
        static alloc(n: number): Bytes;
        /** True when `v` is a Bytes handle. */
        static isBytes(v: unknown): v is Bytes;
        /** One allocation, sized in a first pass; every element must be a byte-addressed view. */
        static concat(list: ByteView[]): Bytes;
        /** The byte count. */
        get length(): number;
        /** True when no byte has the high bit set; computed once at construction. */
        get isAscii(): boolean;
        /** True when the bytes are well-formed UTF-8; computed at construction. */
        get isValidUtf8(): boolean;
        /** The backing Uint8Array. */
        get array(): Uint8Array;
        /** A new Bytes handle that is a view sharing the owner's ArrayBuffer. */
        slice(start?: number, end?: number): Bytes;
        /** Lexicographic byte comparison; -1, 0, or 1. */
        compare(other: ByteView): number;
        /** True when the other view has identical length and bytes. */
        equals(other: ByteView): boolean;
        /** First position of a byte value (0..255) or byte view; -1 when absent. */
        indexOf(needle: number | ByteView): number;
        /** Last position of the needle; the empty needle matches at `length`. */
        lastIndexOf(needle: number | ByteView): number;
        /** True when the needle occurs. */
        includes(needle: number | ByteView): boolean;
        /** Number of non-overlapping occurrences; the empty needle counts `length + 1`. */
        count(needle: number | ByteView): number;
        /** First position holding any byte of the `chars` view, or -1. */
        indexOfAny(chars: ByteView): number;
        /** Sets buf[start..end) to the low 8 bits of `val`; returns the underlying Uint8Array. */
        fill(val: number, start?: number, end?: number): Uint8Array;
        /** Decodes the raw bytes as UTF-8 (invalid sequences become U+FFFD); alias of toString. */
        toUtf8(): string;
        toString(): string;
        /** 1-byte read at `off`; throws RangeError when offset + width exceeds the buffer. */
        readUint8(off: number): number;
        readInt8(off: number): number;
        /** Fixed-width 2-byte reads with explicit endianness. */
        readUint16LE(off: number): number;
        readUint16BE(off: number): number;
        readInt16LE(off: number): number;
        readInt16BE(off: number): number;
        /** Fixed-width 4-byte reads with explicit endianness. */
        readUint32LE(off: number): number;
        readUint32BE(off: number): number;
        readInt32LE(off: number): number;
        readInt32BE(off: number): number;
        /** Fixed-width 8-byte integer reads; always BigInt. */
        readBigUint64LE(off: number): bigint;
        readBigUint64BE(off: number): bigint;
        readBigInt64LE(off: number): bigint;
        readBigInt64BE(off: number): bigint;
        /** Fixed-width 4-byte float read. */
        readFloatLE(off: number): number;
        readFloatBE(off: number): number;
        /** Fixed-width 8-byte float read. */
        readDoubleLE(off: number): number;
        readDoubleBE(off: number): number;
        /** Writes one byte and returns the offset after it. */
        writeUint8(off: number, val: number): number;
        writeInt8(off: number, val: number): number;
        /** 2-byte writes; returns the offset after the value. */
        writeUint16LE(off: number, val: number): number;
        writeUint16BE(off: number, val: number): number;
        writeInt16LE(off: number, val: number): number;
        writeInt16BE(off: number, val: number): number;
        /** 4-byte writes; returns the offset after the value. */
        writeUint32LE(off: number, val: number): number;
        writeUint32BE(off: number, val: number): number;
        writeInt32LE(off: number, val: number): number;
        writeInt32BE(off: number, val: number): number;
        /** 8-byte integer writes; the 64-bit forms take a BigInt. */
        writeBigUint64LE(off: number, val: bigint): number;
        writeBigUint64BE(off: number, val: bigint): number;
        writeBigInt64LE(off: number, val: bigint): number;
        writeBigInt64BE(off: number, val: bigint): number;
        /** 4-byte float write. */
        writeFloatLE(off: number, val: number): number;
        writeFloatBE(off: number, val: number): number;
        /** 8-byte float write. */
        writeDoubleLE(off: number, val: number): number;
        writeDoubleBE(off: number, val: number): number;
    }

    /** Wraps a JS string and caches isWide in one scan at construction. */
    class Text {
        constructor(s: string);
        /** True when any code unit is above U+00FF. */
        get isWide(): boolean;
        /** The wrapped string. */
        get value(): string;
        /** True when the string's UTF-8 encoding is well-formed. */
        isValidUtf8(): boolean;
        /** True when the string has no lone surrogate. */
        isValidUtf16(): boolean;
        /** UTF-8 code points of the string. */
        countUtf8(): number;
        /** Code points, surrogate pairs counted once. */
        countUtf16(): number;
        /** The string's UTF-8 bytes as a Uint8Array. */
        toUtf8(): Uint8Array;
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

    /** A Uint8Array aliasing exactly the bytes `view` spans; the only non-copying function here. */
    function bytesOf(view: ByteView): Uint8Array;
    /** Lexicographic byte comparison; -1, 0, or 1. */
    function compare(a: ByteView, b: ByteView): number;
    /** True for identical bytes. */
    function equal(a: ByteView, b: ByteView): boolean;
    /** First position of the needle (byte value or view), or -1. */
    function indexOf(buf: ByteView, needle: number | ByteView): number;
    /** Last position of the needle. */
    function lastIndexOf(buf: ByteView, needle: number | ByteView): number;
    /** True when the needle occurs. */
    function contains(buf: ByteView, needle: number | ByteView): boolean;
    /** Number of non-overlapping occurrences. */
    function count(buf: ByteView, needle: number | ByteView): number;
    /** Concatenates byte views into one Uint8Array. */
    function concat(list: ByteView[]): Uint8Array;
    /** Overlap-safe byte copy returning the number of bytes copied. */
    function copy(dst: Uint8Array, src: ByteView, dstOff?: number, srcOff?: number, len?: number): number;
    /** Sets buf[start..end) to the low 8 bits of `val`; returns buf. */
    function fill(buf: Uint8Array, val: number, start?: number, end?: number): Uint8Array;
    /** Decodes raw bytes as UTF-8 (invalid sequences become U+FFFD). */
    function toUtf8(buf: ByteView): string;
    /** Encodes a string to a fresh Uint8Array. */
    function fromUtf8(str: string): Uint8Array;
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
    /** Encodes a string into a byte view; unexpressible code points become `?`. */
    function encode(text: string, label: string): Uint8Array;
    /** True for every built label plus utf-8/utf8; ASCII case-insensitive. */
    function encodingExists(label: string): boolean;
    /** The array of every label this build can decode, beginning with utf-8. */
    function encodings(): string[];

    /* The fixed-width accessors also exist as FREE FUNCTIONS over
       (view, offset[, value]) — the same names, no Bytes wrapper. */
    function readUint8(view: ByteView, off: number): number;
    function readInt8(view: ByteView, off: number): number;
    function readUint16LE(view: ByteView, off: number): number;
    function readUint16BE(view: ByteView, off: number): number;
    function readInt16LE(view: ByteView, off: number): number;
    function readInt16BE(view: ByteView, off: number): number;
    function readUint32LE(view: ByteView, off: number): number;
    function readUint32BE(view: ByteView, off: number): number;
    function readInt32LE(view: ByteView, off: number): number;
    function readInt32BE(view: ByteView, off: number): number;
    function readBigUint64LE(view: ByteView, off: number): bigint;
    function readBigUint64BE(view: ByteView, off: number): bigint;
    function readBigInt64LE(view: ByteView, off: number): bigint;
    function readBigInt64BE(view: ByteView, off: number): bigint;
    function readFloatLE(view: ByteView, off: number): number;
    function readFloatBE(view: ByteView, off: number): number;
    function readDoubleLE(view: ByteView, off: number): number;
    function readDoubleBE(view: ByteView, off: number): number;
    function writeUint8(view: ByteView, off: number, val: number): number;
    function writeInt8(view: ByteView, off: number, val: number): number;
    function writeUint16LE(view: ByteView, off: number, val: number): number;
    function writeUint16BE(view: ByteView, off: number, val: number): number;
    function writeInt16LE(view: ByteView, off: number, val: number): number;
    function writeInt16BE(view: ByteView, off: number, val: number): number;
    function writeUint32LE(view: ByteView, off: number, val: number): number;
    function writeUint32BE(view: ByteView, off: number, val: number): number;
    function writeInt32LE(view: ByteView, off: number, val: number): number;
    function writeInt32BE(view: ByteView, off: number, val: number): number;
    function writeBigUint64LE(view: ByteView, off: number, val: bigint): number;
    function writeBigUint64BE(view: ByteView, off: number, val: bigint): number;
    function writeBigInt64LE(view: ByteView, off: number, val: bigint): number;
    function writeBigInt64BE(view: ByteView, off: number, val: bigint): number;
    function writeFloatLE(view: ByteView, off: number, val: number): number;
    function writeFloatBE(view: ByteView, off: number, val: number): number;
    function writeDoubleLE(view: ByteView, off: number, val: number): number;
    function writeDoubleBE(view: ByteView, off: number, val: number): number;
}

/* ================================================================== *
 *  dyna:cli
 * ================================================================== */
declare module "dyna:cli" {
    /** Styles `text` with the named ANSI style, Node's util.styleText signature. */
    function StyleText(style: string, text: string): string;
    /** The list of styles the engine can apply. */
    function Styles(): string[];
    /** True when the stream with the given fd (default stdout) is a TTY. */
    function IsTTY(fd?: number): boolean;
    /** The terminal column count. */
    function Columns(): number;
    /** The terminal color depth: 1, 4, 8, or 24 (bits per channel). */
    function ColorDepth(fd?: number): number;

    /** A command-line parser: options, arguments, subcommands, help. */
    class Command {
        constructor(name?: string);
        get name(): string;
        /** Describes this command for help output. */
        describe(text: string): this;
        /** Registers a named option. */
        option(flags: string, description?: string, opts?: { type?: "boolean" | "string" | "number"; required?: boolean; variadic?: boolean; default?: unknown }): this;
        /** Registers a positional argument. */
        argument(name: string, description?: string): this;
        /** Registers a subcommand. */
        command(sub: Command): this;
        /** Permits unknown options instead of refusing them. */
        allowUnknown(v: boolean): this;
        /** Parses an argument vector (defaults to scriptArgs). */
        parse(args: string[]): unknown;
        /** Prints the help text. */
        help(): string;
    }
}

/* ================================================================== *
 *  dyna:compress
 * ================================================================== */
declare module "dyna:compress" {
    /** Zstandard compression; level 1..22. */
    function zstd(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Zstandard decompression; malformed or oversized input is refused. */
    function unzstd(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;
    /** Brotli compression; level 0..11. */
    function brotli(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Brotli decompression. */
    function unbrotli(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;
    /** Snappy block compression. */
    function snappy(data: BytesInput): Uint8Array;
    /** Snappy block decompression. */
    function unsnappy(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;
    /** Raw LZ4 block (no header); `dict` seeds the match window. */
    function lz4Compress(data: BytesInput, opts?: { level?: number; dict?: ByteView }): Uint8Array;
    /** Raw LZ4 block decompression; `dict` must be the dictionary used at compress time. */
    function lz4Decompress(data: ByteView, opts?: { dict?: ByteView }): Uint8Array;
    /** LZ4 frame format with optional content checksum. */
    function lz4Frame(data: BytesInput, opts?: { level?: number; checksum?: boolean }): Uint8Array;
    /** LZ4 frame decompression; a bad checksum or structure is refused. */
    function lz4Unframe(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;
    /** RFC 1952 gzip framing. */
    function gzip(data: BytesInput): Uint8Array;
    /** Full RFC 1951 inflate with trailer validation. */
    function gunzip(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;

    /** ustar archive entry for packing. */
    interface TarEntry {
        name: string;
        data?: ByteView;
        type?: string;
        mode?: number;
        mtime?: number;
    }
    /** Metadata record read from an archive. */
    interface TarRecord {
        name: string;
        size: number;
        mtime: number;
        mode: number;
        type: "file" | "directory" | "symlink" | "link" | "device" | "fifo";
        linkname?: string;
        data?: Uint8Array;
    }
    /** Writes a ustar archive; names must be safe. */
    function TarPack(entries: TarEntry[]): Uint8Array;
    /** Reads archive metadata; `allowUnsafeNames` lifts the safe-name check. */
    function TarList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): TarRecord[];
    /** The TarList records with `data` added to every non-directory entry. */
    function TarExtract(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): TarRecord[];

    /** Zip entry for packing. */
    interface ZipEntry {
        name: string;
        data: ByteView;
    }
    /** Central-directory listing record. */
    interface ZipRecord {
        name: string;
        size: number;
        mtime: number;
        type: string;
        compressedSize: number;
        crc32: number;
        method: string;
    }
    /** Writes a zip archive; method "deflate" or "store". */
    function ZipPack(entries: ZipEntry[], opts?: { method?: "deflate" | "store" }): Uint8Array;
    /** Central-directory listing. */
    function ZipList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): ZipRecord[];
    /** Extracts one member by exact name; CRC/size mismatches are refused. */
    function ZipRead(bytes: ByteView, name: string, opts?: { allowUnsafeNames?: boolean }): Uint8Array;

    /** A compiled codec: configuration and scratch owned once, reused across calls. */
    type CompressorAlgo = "gzip" | "lz4" | "lz4frame" | "zstd" | "brotli" | "snappy";
    class Compressor implements DynResource {
        constructor(opts: { algo: CompressorAlgo; level?: number; checksum?: boolean; dict?: ByteView });
        compress(data: BytesInput): Uint8Array;
        decompress(data: ByteView): Uint8Array;
        /** The configured codec name. */
        readonly algo: CompressorAlgo;
        /** CRC-32C of the dictionary, or null when none is set. */
        readonly dictId: number | null;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Aho-Corasick automaton replacing known phrases with codes. */
    class Dictionary implements DynResource {
        constructor(phrases: string[]);
        compress(data: BytesInput): Uint8Array;
        decompress(data: ByteView): Uint8Array;
        /** A stable hash of the phrase list. */
        readonly id: number;
        /** The number of phrases. */
        readonly size: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/* ================================================================== *
 *  dyna:config
 * ================================================================== */
declare module "dyna:config" {
    /** TOML 1.0 parsing and serialization. */
    namespace TOML {
        /** Parses a full TOML 1.0 document; key collisions and leading zeros are refused. */
        function parse(text: string): Record<string, unknown>;
        /** Serializes a plain object root; NaN/Infinity render as nan/inf/-inf. */
        function stringify(value: unknown): string;
    }
    /** Classic INI reading. */
    namespace INI {
        /** Reads [section] headers, key=value pairs, key[]=v lists, and bare keys as true. */
        function parse(text: string): Record<string, unknown>;
    }
    /** dotenv grammar .env parsing. */
    namespace Env {
        /** Parses KEY=value records; lines without `=` are skipped. */
        function parse(text: string): Record<string, string>;
    }
    /** Front-matter splitting (YAML/TOML/JSON fences); data stays text. */
    namespace FrontMatter {
        /** Splits at a first-line fence; `data`/`lang` are null when absent. */
        function split(text: string): { data: string | null; body: string; lang: string | null };
    }
}

/* ================================================================== *
 *  dyna:crypto
 * ================================================================== */
declare module "dyna:crypto" {
    /** A PEM key pair. */
    interface KeyPair {
        privateKey: string;
        publicKey: string;
    }

    /** OpenBSD $2b$ bcrypt. */
    namespace Bcrypt {
        /** Hashes a password (max 72 bytes) with a fresh salt; rounds 4..31. */
        function hash(password: string, rounds?: number): string;
        /** Constant-time verify; the hash's cost is capped at 20. */
        function verify(password: string, hash: string): boolean;
    }

    /** Argon2id v0x13 (RFC 9106). */
    namespace Argon2id {
        /** Options; memory in KiB, salt at least 8 bytes. */
        interface Argon2idOpts {
            iterations?: number;
            memory?: number;
            parallelism?: number;
            hashLen?: number;
        }
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts): Uint8Array;
        /** Recomputes with the same parameters and compares in constant time. */
        function verify(password: BytesInput, salt: ByteView, expectedHash: ByteView, opts?: Argon2idOpts): boolean;
    }

    /** RSA PKCS#1 v1.5 keys and signatures. */
    namespace RSA {
        function generate(bits?: 2048 | 3072 | 4096): KeyPair;
        /** md is "sha1"|"sha256"|"sha384"|"sha512" (case-insensitive). */
        function sign(md: string, privateKey: string, msg: BytesInput): Uint8Array;
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView): boolean;
    }

    /** X.509 certificate parsing and self-signed generation. */
    namespace X509 {
        interface X509Info {
            subject: string;
            issuer: string;
            serialNumber: string;
            version: number;
            notBefore: number;
            notAfter: number;
            fingerprint: string;
            sans: { dns: string[]; ip: string[]; email: string[] };
        }
        /** Parses a PEM string or DER bytes; malformed input is refused. */
        function parse(cert: BytesInput): X509Info;
        /** Builds a v3 self-signed certificate signed with SHA-256. */
        function generateSelfSigned(opts: { key: string; subject?: string; days?: number }): string;
    }

    /** ECDSA raw R||S or DER signatures. */
    namespace ECDSA {
        function generate(curve?: "P-256" | "P-384"): KeyPair;
        /** md "sha1"|"sha256"|"sha384"|"sha512"; format "raw" (default) or "der". */
        function sign(md: string, privateKey: string, msg: BytesInput, opts?: { format?: "raw" | "der" }): Uint8Array;
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView, opts?: { format?: "raw" | "der" }): boolean;
    }

    /** ECDH key agreement over X9.63. */
    namespace ECDH {
        function generate(curve?: "P-256" | "P-384"): KeyPair;
        /** The raw shared secret; a small-order peer point is refused. */
        function derive(privateKey: string, peerPublicKey: string): Uint8Array;
    }

    /** Ed25519 one-shot signatures; raw 32-byte keys. */
    function Ed25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    function Ed25519Sign(privateKey: ByteView, message: BytesInput): Uint8Array;
    function Ed25519Verify(publicKey: ByteView, message: BytesInput, signature: ByteView): boolean;

    /** X25519 key agreement; raw 32-byte keys. */
    function X25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    function X25519Derive(privateKey: ByteView, peerPublicKey: ByteView): Uint8Array;

    /** AES-GCM AEAD; key 16, 24, or 32 bytes. */
    class AESGCM implements DynResource {
        constructor(key: ByteView);
        /** Encrypts and appends the 16-byte tag; nonce must be exactly 12 bytes. */
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        /** Decrypts and authenticates; a forged tag throws `authentication failed`. */
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** ChaCha20-Poly1305 AEAD; key exactly 32 bytes. */
    class ChaCha20Poly1305 implements DynResource {
        constructor(key: ByteView);
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Streaming HMAC with a derived key schedule reused across calls. */
    class Hmac implements DynResource {
        constructor(algorithm: string, key: BytesInput);
        /** A complete MAC; the object is ready for the next message. */
        sign(msg: BytesInput): Uint8Array;
        signHex(msg: BytesInput): string;
        /** Streaming absorb; returns this. */
        update(msg: BytesInput): this;
        /** Finishes the accumulated stream. */
        digest(): Uint8Array;
        digestHex(): string;
        /** Constant-time compare; tag may be raw bytes or a hex string. */
        verify(msg: BytesInput, tag: BytesInput): boolean;
        readonly algorithm: string;
        readonly digestSize: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** One-shot HMAC. */
    function HMAC(algorithm: string, key: BytesInput, data: BytesInput): Uint8Array;
    function HMACHex(algorithm: string, key: BytesInput, data: BytesInput): string;

    /** RFC 5869 extract-and-expand key derivation. */
    function HKDF(opts: { hash?: string; key: BytesInput; salt?: BytesInput; info?: BytesInput; length?: number }): Uint8Array;

    /** RFC 8018 PBKDF2. */
    function PBKDF2(opts: { hash?: string; password: BytesInput; salt?: BytesInput; iterations?: number; length?: number }): Uint8Array;

    /** RFC 7914 scrypt. */
    function Scrypt(password: BytesInput, salt: BytesInput, opts?: { N?: number; r?: number; p?: number; keyLen?: number }): Uint8Array;

    /** OS entropy; the CSPRNG path, not the seeded PRNG. */
    function RandomBytes(count?: number): Uint8Array;

    /** Constant-time comparison; different lengths return false. */
    function TimingSafeEqual(a: ByteView, b: ByteView): boolean;

    /** RFC 4226 HOTP; digits 6..8, algo any Hmac name. */
    function HOTPGenerate(secret: BytesInput, counter: number, opts?: { digits?: number; algo?: string }): string;

    /** RFC 6238 TOTP; atSec is explicit so results are testable. */
    function TOTPGenerate(secret: BytesInput, opts?: { atSec?: number; period?: number; digits?: number; algo?: string }): string;

    /** JWS signing; HS/RS/ES algorithms. */
    function JWTSign(payload: unknown, key: BytesInput, opts?: { alg?: string }): string;

    /** JWT verification; `algorithms` is a required allowlist. */
    function JWTVerify(token: string, key: BytesInput, opts: { algorithms: string[] }): unknown;
}

/* ================================================================== *
 *  dyna:csv
 * ================================================================== */
declare module "dyna:csv" {
    /** A file-backed table whose every method load-modify-stores the bound file. */
    class CSVFile implements DynResource {
        constructor(path: import("dyna:file").Path);
        /** Creates the file from headers and optional rows. */
        create(opts: { headers: string[]; rows?: (string | number)[][]; overwrite?: boolean }): { path: import("dyna:file").Path; rows: number };
        /** Loads the file; options offset/limit/columns. */
        read(opts?: { offset?: number; limit?: number; columns?: string[] }): { headers: string[]; rows: string[][]; totalRows: number };
        /** Appends rows (positional arrays or objects keyed by header). */
        addRow(opts: { rows: (string | number)[] | Record<string, unknown>[] }): { added: number; totalRows: number };
        /** Sets one cell. */
        updateCell(opts: { row: number; column?: string; columnIndex?: number; value: string }): { row: number; column: string; value: string };
        /** Removes the data row at `row`. */
        removeRow(opts: { row: number }): { removed: number; totalRows: number };
        /** Appends a column, filling rows with defaultValue. */
        addColumn(opts: { column: string; defaultValue?: string }): { column: string; totalColumns: number };
        /** Drops the named or indexed column. */
        removeColumn(opts: { column?: string; columnIndex?: number }): { removedIndex: number; totalColumns: number };
        /** Renames a column; a no-op when names match. */
        renameColumn(opts: { oldName: string; newName: string }): { oldName: string; newName: string };
        /** One column's values over a [start, end) window (window over 1000 rows refused). */
        readColumnValuesRange(opts: { column: string; start?: number; end?: number }): (string | number)[];
        /** Rows [start, end) as arrays; default is a single row. */
        readRowRange(opts?: { start?: number; end?: number }): { headers: string[]; rows: string[][] };
        /** Projects columns over a [start, end) window, capped at 100 rows. */
        selectColumnRange(opts: { columns: string[]; start?: number; end?: number }): { columns: string[]; rows: string[][] };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/* ================================================================== *
 *  dyna:decimal
 * ================================================================== */
declare module "dyna:decimal" {
    /** Exact arbitrary-precision decimal; IEEE 754-2008 decimal128 context (34 digits). */
    interface DecimalOptions {
        precision?: number;
        rounding?: RoundingMode | string;
    }
    class Decimal {
        /** value: decimal string, JS number (through its shortest round-trip text), or another Decimal. */
        constructor(value: string | number | Decimal);
        /** Exact addition. */
        add(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        sub(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        mul(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** The only arithmetic that rounds; division by zero throws. */
        div(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        mod(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Integer exponentiation; exponent in -10000..10000. */
        pow(n: number, opts?: DecimalOptions): Decimal;
        /** The magnitude. */
        abs(): Decimal;
        /** The negation; -0 is 0. */
        neg(): Decimal;
        /** -1, 0, or 1. */
        cmp(x: Decimal | string | number): number;
        /** True when the values are equal (1.5 equals 1.50). */
        equals(x: Decimal | string | number): boolean;
        /** A new Decimal rounded to dp decimal places; dp in -1000..1000. */
        round(dp?: number, rounding?: RoundingMode | string): Decimal;
        /** A string with exactly dp digits after the point. */
        toFixed(dp?: number, rounding?: RoundingMode | string): string;
        /** The exact decimal text. */
        toString(): string;
        toJSON(): string;
        /** The one place a Decimal may become approximate: ToNumber over the exact text. */
        toNumber(): number;
        isZero(): boolean;
        /** -1, 0, or 1. */
        sign(): number;
        /** The number of significant digits. */
        digits(): number;
    }

    /** Integral money: an integer count of minor units plus a 3-letter currency tag. */
    class Money {
        constructor(minorUnits: number, currency: string, opts?: { minorDigits?: number });
        /** Both operands must share the currency code. */
        add(x: Money): Money;
        sub(x: Money): Money;
        cmp(x: Money): number;
        equals(x: Money): boolean;
        /** Scales by an integer; a fractional multiplier throws. */
        mul(n: number): Money;
        /** Splits into shares whose sum is exactly the original. */
        allocate(shares: number[]): Money[];
        /** The decimal amount with the currency's minor digits. */
        toString(): string;
        toJSON(): string;
        /** The minor-unit integer. */
        amount(): number;
        /** The 3-letter code. */
        currency(): string;
        /** "$19.99" for major currencies, "19.99 USD" otherwise. */
        format(): string;
        /** The amount as an exact Decimal. */
        toDecimal(): Decimal;
    }
}

/* ================================================================== *
 *  dyna:encoding
 * ================================================================== */
declare module "dyna:encoding" {
    /** Lowercase hex string, SIMD-accelerated. */
    function HexEncode(data: BytesInput): string;
    /** Returns a Uint8Array; throws SyntaxError on odd length or an invalid digit. */
    function HexDecode(text: string): Uint8Array;
    /** RFC 4648 base64 (`+/`, padded). */
    function Base64Encode(data: BytesInput): string;
    function Base64Decode(text: string): Uint8Array;
    /** RFC 4648 section 5 base64url (no padding). */
    function Base64URLEncode(data: BytesInput): string;
    function Base64URLDecode(text: string): Uint8Array;
    /** RFC 4648 base32, `=` padded. */
    function Base32Encode(data: BytesInput): string;
    function Base32Decode(text: string): Uint8Array;
    /** Extended-hex base32. */
    function Base32HexEncode(data: BytesInput): string;
    function Base32HexDecode(text: string): Uint8Array;
    /** Adobe-less ascii85 with the `z` shorthand; input capped at 4096 bytes. */
    function Base85Encode(data: BytesInput): string;
    function Base85Decode(text: string): Uint8Array;
    /** Bitcoin base58; leading zero bytes become leading `1`s. */
    function Base58Encode(data: BytesInput): string;
    function Base58Decode(text: string): Uint8Array;
    /** Base58 with a double-SHA256 checksum appended. */
    function Base58CheckEncode(data: BytesInput): string;
    function Base58CheckDecode(text: string): Uint8Array;
    /** Encode in a caller-supplied alphabet of 2..255 distinct characters. */
    function BaseXEncode(data: BytesInput, alphabet: string): string;
    function BaseXDecode(text: string, alphabet: string): Uint8Array;
    /** LEB128 varint encoding of a non-negative value. */
    function PutUvarint(value: number | bigint): Uint8Array;
    /** Zigzag-encoded signed LEB128. */
    function PutVarint(value: number | bigint): Uint8Array;
    /** Decodes; returns [value, bytesRead]; magnitude is bigint when it exceeds 2^53-1. */
    function Uvarint(buf: ByteView): [number | bigint, number];
    function Varint(buf: ByteView): [number | bigint, number];

    /** Charset detection options. */
    interface DetectOptions {
        fallback?: string;
        allowList?: string[];
    }
    /** Deterministic charset detection over a byte view; throws TypeError when allowList excludes the verdict. */
    function DetectEncoding(data: ByteView, opts?: DetectOptions): string;
    /** Same as DetectEncoding under its lowercase name. */
    function detectEncoding(data: ByteView, opts?: DetectOptions): string;

    /** JSON5 superset parsing; depth capped at 256. */
    function JSON5Parse(text: string): unknown;
    /** JSON5 output with unquoted keys and NaN/Infinity literals; indent clamped to 0-10. */
    function JSON5Stringify(value: unknown, opts?: { indent?: number }): string;
    /** RFC 8785 canonical JSON; NaN/Infinity rejected. The canonical form is always compact, so `indent` is accepted and ignored. */
    function StableStringify(value: unknown, opts?: { indent?: number }): string;

    /** Compiled RFC 9535 JSONPath expression, reusable across queries. */
    class JSONPath {
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

/* ================================================================== *
 *  dyna:hash
 * ================================================================== */
declare module "dyna:hash" {
    /** MD5 (RFC 1321). */
    function MD5(data: BytesInput): Uint8Array;
    function MD5Hex(data: BytesInput): string;
    /** SHA-1 (FIPS 180-4). */
    function SHA1(data: BytesInput): Uint8Array;
    function SHA1Hex(data: BytesInput): string;
    /** SHA-224. */
    function SHA224(data: BytesInput): Uint8Array;
    function SHA224Hex(data: BytesInput): string;
    /** SHA-256, the engine's default digest. */
    function SHA256(data: BytesInput): Uint8Array;
    function SHA256Hex(data: BytesInput): string;
    /** SHA-384. */
    function SHA384(data: BytesInput): Uint8Array;
    function SHA384Hex(data: BytesInput): string;
    /** SHA-512. */
    function SHA512(data: BytesInput): Uint8Array;
    function SHA512Hex(data: BytesInput): string;
    /** IEEE 802.3 CRC-32 as a non-negative number. */
    function CRC32(data: BytesInput): number;
    /** CRC-32C (Castagnoli polynomial). */
    function CRC32C(data: BytesInput): number;
    /** FIPS 202 SHA-3, 224..512 bits. */
    function SHA3_224(data: BytesInput): Uint8Array;
    function SHA3_224Hex(data: BytesInput): string;
    function SHA3_256(data: BytesInput): Uint8Array;
    function SHA3_256Hex(data: BytesInput): string;
    function SHA3_384(data: BytesInput): Uint8Array;
    function SHA3_384Hex(data: BytesInput): string;
    function SHA3_512(data: BytesInput): Uint8Array;
    function SHA3_512Hex(data: BytesInput): string;
    /** Original Keccak padding, the form Ethereum uses. */
    function Keccak256(data: BytesInput): Uint8Array;
    function Keccak256Hex(data: BytesInput): string;
    /** SHAKE128 extensible output; length 1..2^20 bytes. */
    function SHAKE128(data: BytesInput, length?: number): Uint8Array;
    function SHAKE128Hex(data: BytesInput, length?: number): string;
    function SHAKE256(data: BytesInput, length?: number): Uint8Array;
    function SHAKE256Hex(data: BytesInput, length?: number): string;
    /** BLAKE3 Merkle-tree hash; length 1..2^20 bytes. */
    function BLAKE3(data: BytesInput, length?: number): Uint8Array;
    function BLAKE3Hex(data: BytesInput, length?: number): string;
    /** BLAKE2b; 1..64 bytes. */
    function BLAKE2b(data: BytesInput, length?: number): Uint8Array;
    function BLAKE2bHex(data: BytesInput, length?: number): string;
    /** BLAKE2s; 1..32 bytes. */
    function BLAKE2s(data: BytesInput, length?: number): Uint8Array;
    function BLAKE2sHex(data: BytesInput, length?: number): string;
    /** Murmur3_128; the second argument is a seed, not a length. */
    function Murmur3_128(data: BytesInput, seed?: number): Uint8Array;
    function Murmur3_128Hex(data: BytesInput, seed?: number): string;
    /** 32-bit xxHash as a number. */
    function XXHash32(data: BytesInput, seed?: number): number;
    /** 64-bit xxHash as a 16-character hex string (a number cannot hold 64 bits exactly). */
    function XXHash64(data: BytesInput, seed?: number): string;

    /** Streaming digest over md5|sha1|sha224|sha256|sha384|sha512. */
    class Hasher {
        constructor(algorithm: string);
        /** Absorbs bytes; returns this for chaining. */
        update(data: BytesInput): this;
        /** Finalizes a copy; the stream stays usable. */
        digest(): Uint8Array;
        digestHex(): string;
        /** Returns the hasher to its initial state. */
        reset(): void;
        readonly algorithm: string;
        readonly digestSize: number;
    }
}
/* ================================================================== *
 *  dyna:file
 * ================================================================== */
declare module "dyna:file" {
    /** A value handle over one normalized path; immutable, refcounted. */
    class Path {
        /** Joins and normalizes the segments; at least one segment is required. */
        constructor(...segments: (string | Path)[]);
        /** Current working directory. */
        static cwd(): Path;
        /** $HOME, else the passwd entry. */
        static home(): Path;
        /** $TMPDIR, else /tmp, trailing slash stripped. */
        static temp(): Path;
        /** True when `v` is a Path. */
        static isPath(v: unknown): v is Path;
        /** "/" on this platform. */
        static readonly sep: string;
        /** ":" on this platform. */
        static readonly delimiter: string;
        /** The parent directory as a new Path. */
        get dirname(): Path;
        /** The final component as a string. */
        get basename(): string;
        /** The extension, including the dot. */
        get extname(): string;
        get isAbsolute(): boolean;
        /** `this` leads, then the arguments; returns a new Path. */
        join(...segments: (string | Path)[]): Path;
        /** join that resolves `.`/`..`; an absolute argument rebases. */
        resolve(...segments: (string | Path)[]): Path;
        /** The relative path from `this` to `other`. */
        relativeTo(other: Path): Path;
        /** Byte equality of the normalized forms; false for a non-Path argument. */
        equals(other: unknown): boolean;
        /** Basename with the given suffix removed. */
        basenameWithout(suffix: string): string;
        toString(): string;
        toJSON(): string;
    }

    /** stat/lstat result. */
    interface Stat {
        size: number;
        mode: number;
        isDir: boolean;
        isFile: boolean;
        isSymlink: boolean;
        mtimeMs: number;
        atimeMs: number;
        ctimeMs: number;
        uid: number;
        gid: number;
        ino: number;
        nlink: number;
    }

    /** A handle over one path; every method is the free function with the path supplied once. */
    class File {
        /** Accepts a Path or a string. */
        constructor(path: Path | string);
        readonly path: Path;
        /** Whole file as a string; invalid UTF-8 becomes U+FFFD. */
        readText(): string;
        /** Whole file as a Uint8Array. */
        readBytes(): Uint8Array;
        /** Write string or bytes; {append:true} appends; returns the byte count. */
        writeText(data: BytesInput, opts?: { append?: boolean }): number;
        /** Alias of writeText. */
        writeBytes(data: BytesInput, opts?: { append?: boolean }): number;
        /** writeText with append supplied. */
        append(data: BytesInput): number;
        stat(): Stat;
        lstat(): Stat;
        /** Boolean; never throws. */
        exists(): boolean;
        /** Unlink the file or empty directory. */
        remove(): void;
        /** The resolved Path. */
        realPath(): Path;
        /** Change permissions. */
        chmod(mode: number): void;
        /** Rename; on success the handle names the new location. */
        moveTo(dest: Path): this;
        /** Byte copy to a new File; refuses an existing destination. */
        copyTo(dest: Path): File;
        reader(opts?: { bufferSize?: number }): FileReader;
        writer(opts?: { bufferSize?: number; preallocate?: number; append?: boolean }): FileWriter;
        toString(): string;
        toJSON(): string;
    }

    /** Buffered sequential reader over a strictly-opened fd. */
    class FileReader implements DynResource {
        constructor(path: Path, opts?: { bufferSize?: number });
        /** Up to `n` bytes as a string, "" at EOF; `n` omitted reads all. */
        read(n?: number): string;
        /** The next line without its trailing newline; null at a clean EOF. */
        readLine(): string | null;
        /** The rest of the file. */
        readAll(): string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Buffered sequential writer; flushes buffered bytes on teardown. */
    class FileWriter implements DynResource {
        constructor(path: Path, opts?: { bufferSize?: number; preallocate?: number; append?: boolean });
        /** Accepts a string, ArrayBuffer, or any TypedArray/DataView; returns bytes accepted. */
        write(data: BytesInput): number;
        /** Push buffered bytes to the fd. */
        flush(): void;
        /** Flush then durable-sync (F_FULLFSYNC on Darwin). */
        sync(): void;
        /** The same durability off the loop; returns a Promise. */
        syncAsync(): Promise<void>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An advisory exclusive lock via flock(2). */
    class FileLock implements DynResource {
        constructor(path: Path | string, opts?: { retry?: number; retryMs?: number });
        /** Calls fn, then releases the lock no matter what fn did; the lock is consumed. */
        withLock<T>(fn: () => T): T;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Kernel-event file watching (kqueue/inotify), one classifier for both. */
    class Watcher implements DynResource {
        constructor(path: Path, opts?: { recursive?: boolean; debounceMs?: number; ignore?: string[] });
        /** Arm the watch; event type one of change/add/addDir/unlink/unlinkDir. */
        start(cb: (event: { type: string; path: Path }) => void): void;
        stats(): { entries: number; directories: number; events: number; truncated: boolean; debounceMs: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A compiled glob pattern over the same matcher glob() walks with. */
    class Glob {
        constructor(pattern: string);
        /** Lexical match only, no filesystem access. */
        matches(path: Path): boolean;
        /** The glob() walk with an optional cwd; returns matching Paths. */
        expand(cwd?: Path): Path[];
        /** The subset of the array matching the pattern. */
        filter(paths: Path[]): Path[];
        readonly pattern: string;
        readonly hasWildcard: boolean;
    }

    /** Whole file as a string; strict open first. */
    function readFile(path: Path): string;
    /** Write string or bytes; O_CREAT, truncate unless append; returns the byte count. */
    function writeFile(path: Path, data: BytesInput, opts?: { append?: boolean }): number;
    /** Async read; {bytes:true} resolves a Uint8Array. */
    function readFileAsync(path: Path, opts?: { bytes?: boolean }): Promise<string | Uint8Array>;
    /** Async write; the payload is copied before the call returns. */
    function writeFileAsync(path: Path, data: BytesInput, opts?: { append?: boolean }): Promise<number>;
    /** The inline/offloaded counters and the 1 MiB thresholds. */
    function asyncStats(): { inline: number; offloaded: number; readMin: number; writeMin: number };

    /** Follows the final component. */
    function stat(path: Path): Stat;
    /** Does not follow the final component. */
    function lstat(path: Path): Stat;
    /** Boolean; never throws; uses lstat, so a dangling symlink reports true. */
    function exists(path: Path): boolean;

    /** Sorted entries; `.`/`..` excluded. */
    function readDir(path: Path): { name: string; isDir: boolean; isFile: boolean; isSymlink: boolean }[];
    /** Create a directory; recursive creates missing parents. */
    function makeDir(path: Path, opts?: { recursive?: boolean; mode?: number }): void;
    /** Unlink a file or an empty directory. */
    function remove(path: Path): void;
    /** Recursive, symlink-safe removal; a missing path is a no-op. */
    function removeAll(path: Path): void;
    /** rename(2). */
    function rename(from: Path, to: Path): void;
    /** Byte copy through the kernel; refusing an existing destination is the default. */
    function copyFile(from: Path, to: Path, opts?: { overwrite?: boolean }): void;
    /** rename(2), falling back to copy-then-unlink across filesystems. */
    function move(from: Path, to: Path): void;
    /** MIME type from magic bytes, not the extension. */
    function sniffType(pathOrBytes: Path | ByteView): string;

    /** Creates the link; only the link location is a Path. */
    function symlink(target: string, linkpath: Path): void;
    /** The stored target verbatim, as a string. */
    function readLink(path: Path): string;
    /** The fully resolved path as a Path. */
    function realPath(path: Path): Path;
    function chmod(path: Path, mode: number): void;

    /** Walk the filesystem matching *, **, ?, [...]; returns sorted Paths. */
    function glob(pattern: string, opts?: { cwd?: Path }): Path[];

    /** The system temp directory as a Path. */
    function tempDir(): Path;
    /** mkdtemp under the temp dir. */
    function makeTempDir(prefix?: string): Path;
    /** mkstemp; returns the path of the empty file. */
    function makeTempFile(prefix?: string): Path;
    /** Per-user platform data directory. */
    function dataDir(app?: string): Path;
    /** Per-user platform config directory. */
    function configDir(app?: string): Path;
    /** Per-user platform cache directory. */
    function cacheDir(app?: string): Path;
    /** System-wide data directory variant. */
    function dataDirSite(app?: string): Path;
    function configDirSite(app?: string): Path;
    function cacheDirSite(app?: string): Path;
}

/* ================================================================== *
 *  dyna:html
 * ================================================================== */
declare module "dyna:html" {
    /** An element node in the dyna:xml-compatible tree shape. */
    interface HTMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | HTMLElement)[];
    }

    /** Parses HTML into an array of root nodes. */
    function HTMLParse(text: string): HTMLElement[];
    /** Serializes a node back to HTML. */
    function HTMLStringify(node: HTMLElement): string;
    /** Extracts the visible text of a node or node array. */
    function HTMLText(node: HTMLElement | HTMLElement[]): string;
    /** Renders Markdown to HTML (through the module's escaper). */
    function MarkdownToHTML(text: string, opts?: unknown): string;

    /** A compiled CSS selector over the parsed tree. */
    class Selector {
        constructor(text: string);
        /** Every matching element. */
        all(doc: HTMLElement | HTMLElement[]): HTMLElement[];
        /** The first match, or null. */
        first(doc: HTMLElement | HTMLElement[]): HTMLElement | null;
        /** True when the single node matches (no combinators allowed). */
        matches(node: HTMLElement): boolean;
    }

    /** An allow-list HTML sanitizer. */
    class Sanitizer {
        /** An allow-list is required; there is no default policy. */
        constructor(opts: { allow: Record<string, unknown>; protocols?: Record<string, string[]> });
        clean(html: string): string;
    }

    /** A compiled template with escaping. */
    class Template {
        constructor(source: string, opts?: { escape?: boolean });
        /** Renders with the given data scope. */
        render(data?: unknown): string;
    }
}

/* ================================================================== *
 *  dyna:http
 * ================================================================== */
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
    /** Content negotiation: the best candidate index for the Accept header, or null. */
    function Negotiate(header: string, candidates: string[]): number | null;
    /** Content negotiation by media-type token. */
    function NegotiateToken(header: string, candidates: string[]): string | null;
    /** Parses a Range header against a size. */
    function RangeParse(header: string, size: number): unknown;
    /** Parses a Cookie header into an object. */
    function CookieParse(header: string): Record<string, string>;
    /** Serializes a cookie; options maxAge/domain/path/sameSite/secure/httpOnly. */
    function CookieSerialize(name: string, value: string, opts?: Record<string, unknown>): string;
    /** True when the If-None-Match header matches the etag. */
    function ETagMatch(header: string, etag: string): boolean;
    /** Parses a multipart body against the boundary in the Content-Type header. */
    function MultipartParse(contentType: string, body: ByteView): { name: string; filename?: string; body: Uint8Array }[];
    /** Formats multipart parts; returns {body, contentType}. */
    function MultipartFormat(parts: { name: string; value?: string; body?: ByteView; filename?: string }[], boundary?: string): { body: Uint8Array; contentType: string };

    /** A blocking HTTP client over one connection. */
    class HTTPClient implements DynResource {
        constructor(maxBody?: number);
        /** GET; returns {status, statusText, ok, headers, body}. */
        get(url: string, headers?: Record<string, string>): HTTPResponse;
        post(url: string, body?: BytesInput, headers?: Record<string, string>): HTTPResponse;
        request(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): HTTPResponse;
        getAsync(url: string, headers?: Record<string, string>): Promise<HTTPResponse>;
        postAsync(url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        requestAsync(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        setTimeout(ms: number): void;
        disconnect(): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A blocking HTTP response. */
    interface HTTPResponse {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        body: string;
    }

    /** A thread-pool HTTP server serving static routes. */
    class HTTPServer implements DynResource {
        constructor(opts?: { port?: number });
        start(): void;
        stop(): void;
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A single-threaded reactor HTTP server. */
    class HTTPServerAsync implements DynResource {
        constructor(opts?: { port?: number; backlog?: number; maxConns?: number });
        start(): void;
        stop(): void;
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A routed HTTP application server. */
    class App implements DynResource {
        constructor(opts?: { port?: number; idleTimeoutMs?: number; compress?: boolean; metrics?: boolean });
        /** Registers a strict JSON-RPC 2.0 endpoint. */
        rpc(path: string, methods: Record<string, (...args: unknown[]) => unknown>): this;
        /** Serves a static document root at a URL prefix. */
        static(prefix: string, root: import("dyna:file").Path, opts?: { maxFileSize?: number }): this;
        /** Proxies a URL prefix to a host/port. */
        proxy(prefix: string, opts: { host?: string; port?: number }): this;
        /** Registers an upload endpoint. */
        upload(path: string, opts: { dir?: import("dyna:file").Path; maxFileSize?: number }, handler: (req: unknown, res: unknown) => void): this;
        /** Registers a WebSocket endpoint. */
        ws(path: string, handler: (socket: WsClient) => void): this;
        /** Registers a server-sent events endpoint. */
        sse(path: string, handler: (req: unknown, res: unknown) => void): this;
        start(): void;
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A WebSocket client. */
    class WsClient implements DynResource {
        constructor(url: string, handlers: { open?: (ws: WsClient) => void; message?: (ws: WsClient, data: unknown) => void; close?: () => void });
        send(data: BytesInput): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** WHATWG fetch. */
    function fetch(input: string | Request, init?: RequestInit): Promise<Response>;
    const Request: RequestConstructor;
    const Response: ResponseConstructor;
    const Headers: HeadersConstructor;
    const AbortController: AbortControllerConstructor;
    const AbortSignal: AbortSignalConstructor;
    const FormData: FormDataConstructor;
}

/* ================================================================== *
 *  dyna:net
 * ================================================================== */
declare module "dyna:net" {
    /** Networking: addresses and CIDR prefixes, sockets, DNS, protocol clients, rate limiting and metrics, plus the shared HTTP surface re-exported from dyna:http. */

    /* ---- shared HTTP surface, re-exported from dyna:http ---- */
    /** WHATWG fetch; the same function as dyna:http.fetch. */
    const fetch: typeof import("dyna:http").fetch;
    /** Same class as dyna:http.Request; re-exported here. */
    const Request: typeof import("dyna:http").Request;
    /** Same class as dyna:http.Response; re-exported here. */
    const Response: typeof import("dyna:http").Response;
    /** Same class as dyna:http.Headers; re-exported here. */
    const Headers: typeof import("dyna:http").Headers;
    /** Same class as dyna:http.FormData; re-exported here. */
    const FormData: typeof import("dyna:http").FormData;
    /** Same class as dyna:http.AbortController; re-exported here. */
    const AbortController: typeof import("dyna:http").AbortController;
    /** Same class as dyna:http.AbortSignal; re-exported here. */
    const AbortSignal: typeof import("dyna:http").AbortSignal;
    /** Same class as dyna:http.HTTPClient; re-exported here. */
    const HTTPClient: typeof import("dyna:http").HTTPClient;
    /** Same class as dyna:http.HTTPServer; re-exported here. */
    const HTTPServer: typeof import("dyna:http").HTTPServer;
    /** Same class as dyna:http.HTTPServerAsync; re-exported here. */
    const HTTPServerAsync: typeof import("dyna:http").HTTPServerAsync;
    /** Same class as dyna:http.App; re-exported here. */
    const App: typeof import("dyna:http").App;
    /** Same class as dyna:http.WsClient; re-exported here. */
    const WsClient: typeof import("dyna:http").WsClient;
    /** Same function as dyna:http.ContentTypeParse; re-exported here. */
    const ContentTypeParse: typeof import("dyna:http").ContentTypeParse;
    /** Same function as dyna:http.ContentTypeFormat; re-exported here. */
    const ContentTypeFormat: typeof import("dyna:http").ContentTypeFormat;
    /** Same function as dyna:http.CookieParse; re-exported here. */
    const CookieParse: typeof import("dyna:http").CookieParse;
    /** Same function as dyna:http.CookieSerialize; re-exported here. */
    const CookieSerialize: typeof import("dyna:http").CookieSerialize;
    /** Same function as dyna:http.ETagMatch; re-exported here. */
    const ETagMatch: typeof import("dyna:http").ETagMatch;
    /** Same function as dyna:http.Negotiate; re-exported here. */
    const Negotiate: typeof import("dyna:http").Negotiate;
    /** Same function as dyna:http.NegotiateToken; re-exported here. */
    const NegotiateToken: typeof import("dyna:http").NegotiateToken;
    /** Same function as dyna:http.RangeParse; re-exported here. */
    const RangeParse: typeof import("dyna:http").RangeParse;
    /** Same function as dyna:http.MultipartParse; re-exported here. */
    const MultipartParse: typeof import("dyna:http").MultipartParse;
    /** Same function as dyna:http.MultipartFormat; re-exported here. */
    const MultipartFormat: typeof import("dyna:http").MultipartFormat;

    /* ---- addresses and prefixes ---- */
    /** A parsed IP address. */
    interface ParsedAddr {
        readonly is4: boolean;
        readonly is6: boolean;
        readonly bytes: Uint8Array;
        readonly string: string;
    }
    /** A parsed CIDR prefix. */
    interface ParsedPrefix {
        readonly addr: string;
        readonly bits: number;
    }
    /** Parses an IPv4/IPv6 address (a 4-in-6 form parses as IPv6); throws TypeError on a malformed address. */
    function parseAddr(addr: string): ParsedAddr;
    /** Parses "addr/bits" into {addr, bits}; the addr is formatted canonically; throws TypeError on a malformed prefix. */
    function parsePrefix(cidr: string): ParsedPrefix;
    /** True when the prefix contains the address; throws TypeError when either argument is malformed. */
    function contains(prefix: string, addr: string): boolean;
    /** The prefix's network address (host bits zeroed), canonically formatted; throws TypeError on a malformed prefix. */
    function masked(cidr: string): string;
    /** The RFC 5952 canonical text of an address; a 4-in-6 address formats as "::ffff:a.b.c.d". */
    function canonical(addr: string): string;
    /** True when the string parses as an IP address; never throws. */
    function isValid(addr: string): boolean;
    /** Total order on addresses: -1, 0, or 1; IPv4 sorts before IPv6. */
    function compareAddr(a: string, b: string): -1 | 0 | 1;
    /** True for 127.0.0.0/8 and ::1. */
    function isLoopback(addr: string): boolean;
    /** True for RFC 1918 space and fc00::/7. */
    function isPrivate(addr: string): boolean;
    /** True for 224.0.0.0/4 and ff00::/8. */
    function isMulticast(addr: string): boolean;
    /** True for 0.0.0.0 and ::. */
    function isUnspecified(addr: string): boolean;
    /** True for 169.254.0.0/16 and fe80::/10. */
    function isLinkLocalUnicast(addr: string): boolean;
    /** True for a global unicast address (not loopback, multicast, link-local or unspecified). */
    function isGlobalUnicast(addr: string): boolean;
    /** True for 224.0.0.0/24 and ff02::/16. */
    function isLinkLocalMulticast(addr: string): boolean;

    /** A compiled CIDR prefix: parsed and masked once at construction, then cheap to test. */
    class Prefix {
        constructor(cidr: string);
        /** True when the address is inside the prefix; an unparseable address is false, not an error. */
        contains(addr: string): boolean;
        /** True when the two prefixes share any address; different families never overlap. */
        overlaps(other: Prefix): boolean;
        /** The network address, canonically formatted. */
        readonly masked: string;
        /** The prefix length. */
        readonly bits: number;
        /** True when this is an IPv4 prefix. */
        readonly isIPv4: boolean;
    }

    /* ---- rate limiting and metrics ---- */
    /** A token bucket over a fixed, direct-mapped table; the table cannot grow. */
    class RateLimiter {
        constructor(opts: { tokensPerSec: number; burst?: number; slots?: number });
        /** True when the key may proceed, consuming `cost` tokens (default 1); cost must be > 0. */
        allow(key: string, cost?: number): boolean;
        /** The key's current token count. */
        tokens(key: string): number;
        /** Clears one key, or the whole table when no key is given. */
        reset(key?: string): void;
        /** Running totals and table geometry. */
        readonly stats: { allowed: number; denied: number; slots: number; live: number; tokensPerSec: number; burst: number };
    }

    /** A fixed registry of counters, gauges and histograms with a Prometheus text scrape. */
    const Metrics: {
        /** Increments a counter; a negative or NaN increment is refused. */
        counter(name: string, value?: number, labels?: Record<string, string>): void;
        /** Sets a gauge to a value. */
        gauge(name: string, value: number, labels?: Record<string, string>): void;
        /** Records an observation into the 5ms..1s buckets. */
        histogram(name: string, value: number, labels?: Record<string, string>): void;
        /** The registry as Prometheus text exposition. */
        scrape(): string;
        /** Empties the registry; intended for tests. */
        reset(): void;
    };

    /* ---- DNS ---- */
    /** One decoded A or AAAA answer record. */
    interface DNSRecord {
        readonly name: string;
        readonly type: number;
        readonly ttl: number;
        readonly address: string;
    }
    /** A UDP DNS client; answers are matched by connected socket, CSPRNG query ID and echoed question. */
    class DNSResolver implements DynResource {
        constructor(opts?: { server?: string; port?: number; timeoutMs?: number });
        /** Queries a name; `type` is the record type (A is 1). The callback receives (err, records). */
        query(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A UDP DNS server with an amplification cap and a per-source token bucket. */
    class DNSServer implements DynResource {
        constructor(opts?: { port?: number; host?: string });
        /** Starts answering; the handler maps (name, type) to an address string, or null for no answer. */
        start(handler: (name: string, type: number) => string | null): void;
        /** The bound port (resolved when constructed with port 0). */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /* ---- protocol clients ---- */
    /** Redis client options. */
    interface RedisOptions {
        host?: string;
        path?: string;
        port?: number;
        db?: number;
        username?: string;
        password?: string;
        binary?: boolean;
        bigint?: boolean;
        maxReplyBytes?: number;
        maxPending?: number;
        connectTimeoutMs?: number;
        commandTimeoutMs?: number;
    }
    /** A Redis client: every command returns a promise, and replies are matched to commands by strict FIFO. */
    class Redis implements DynResource {
        constructor(opts?: RedisOptions);
        /** Sends one command and resolves with its reply (RESP3 when the server answers HELLO 3). */
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        /** One round trip for an array of commands; resolves to one reply per command. */
        pipeline(commands: (string | number | Uint8Array | ArrayBuffer)[][]): Promise<unknown[]>;
        /** Registers a push/message or error handler; returns the client. */
        on(event: "push" | "message" | "error", handler: (data: unknown) => void): this;
        /** The negotiated RESP protocol: 2 or 3. */
        readonly protocol: number;
        /** True once the handshake (HELLO/AUTH/SELECT) has completed. */
        readonly ready: boolean;
        /** Commands issued but not yet answered. */
        readonly pending: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** PostgreSQL client options. */
    interface PostgreSQLOptions {
        host?: string;
        path?: string;
        port?: number;
        user?: string;
        password?: string;
        database?: string;
        applicationName?: string;
        raw?: boolean;
        bytes?: boolean;
        textResults?: boolean;
        statementCacheSize?: number;
        prepareAfter?: number;
        bigint?: boolean;
        insecureAuth?: boolean;
        maxMessageBytes?: number;
        maxPending?: number;
        queryTimeoutMs?: number;
        connectTimeoutMs?: number;
    }
    /** The prepared-statement cache's live state. */
    interface StatementCacheStats {
        readonly size: number;
        readonly max: number;
        readonly prepareAfter: number;
        readonly preparedHits: number;
        readonly unnamed: number;
    }
    /** A PostgreSQL client; parameters are bound by the extended protocol, never interpolated. */
    class PostgreSQL implements DynResource {
        constructor(opts?: PostgreSQLOptions);
        /** Runs one statement; without a params array it uses the simple protocol, otherwise the extended one. */
        query(sql: string, params?: unknown[]): Promise<unknown>;
        /** Asks the server to cancel the running query over a fresh connection; the server sends no reply. */
        cancel(): void;
        /** Registers a notice, notification or error handler; returns the client. */
        on(event: "notice" | "notification" | "error", handler: (data: unknown) => void): this;
        /** True once the startup handshake has completed. */
        readonly ready: boolean;
        /** The statement cache's size and how often each arm was taken. */
        readonly statementCache: StatementCacheStats;
        /** Queries issued but not yet answered. */
        readonly pending: number;
        /** The server's backend PID (valid once ready). */
        readonly backendPid: number;
        /** The transaction status character: I (idle), T (in transaction), E (failed). */
        readonly transactionStatus: string;
        /** The server parameters from startup (server_version, client_encoding, ...). */
        readonly parameters: Record<string, string>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** SQLite options. */
    interface SQLiteOptions {
        readonly?: boolean;
        bigint?: boolean;
    }
    /** A SQLite database handle; every value is bound, never interpolated. */
    class SQLite implements DynResource {
        constructor(path: string, opts?: SQLiteOptions);
        /** Runs a statement and returns one object per result row. */
        query(sql: string, params?: unknown[]): Record<string, unknown>[];
        /** Runs a statement without result rows; returns the number of rows changed. */
        exec(sql: string, params?: unknown[]): number;
        /** The rowid of the most recent successful INSERT. */
        readonly lastInsertRowId: number;
        /** The linked library's version, read at runtime. */
        readonly version: string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /* ---- sockets and proxying ---- */
    /** A TCP connection handed to a handler; write() is refused on a TLS connection before the handshake. */
    interface TCPConn {
        write(data: BytesInput): void;
        close(): void;
    }
    /** TCP server and client event handlers. */
    interface TCPHandlers {
        /** Fires when a connection lands; on failure conn is null and err names the reason. */
        connect?: (conn: TCPConn | null, err: string | null) => void;
        /** Fires with a copy of the received bytes. */
        data?: (conn: TCPConn, bytes: Uint8Array) => void;
        close?: (conn: TCPConn) => void;
    }
    /** Options for TCPServer.connect. */
    interface TCPConnectOptions {
        host?: string;
        port?: number;
        path?: string;
        connectTimeoutMs?: number;
        maxConnections?: number;
        idleTimeoutMs?: number;
    }
    /** A TCP server, and (via the static connect) a TCP client; runs on the shared io reactor. */
    class TCPServer implements DynResource {
        constructor(opts?: { port?: number; path?: string; maxConnections?: number; idleTimeoutMs?: number });
        /** Connects to a peer and returns the client resource; events arrive through TCPHandlers. */
        static connect(opts: TCPConnectOptions, handlers?: TCPHandlers): TCPServer;
        /** Binds and accepts; a port of 0 resolves into `.port`. */
        start(handlers?: TCPHandlers): void;
        /** The bound port. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A bound UDP socket; binds in the constructor. */
    class UDPSocket implements DynResource {
        constructor(opts?: { port?: number; host?: string });
        /** Arms the receive path; `message(data, from)` receives a copy of each datagram. */
        start(handlers?: { message?: (data: Uint8Array, from: { address: string; port: number }) => void }): void;
        /** Sends a datagram to host:port, where host is an IPv4 address; returns the bytes sent. */
        send(data: BytesInput, host: string, port: number): number;
        /** The bound port (resolved when constructed with port 0). */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** One upstream entry for TCPProxy. */
    interface TCPUpstream {
        host?: string;
        port: number;
    }
    /** TCPProxy counters. */
    interface TCPProxyStats {
        readonly live: number;
        readonly accepted: number;
        readonly refused: number;
        readonly idleClosed: number;
        readonly connectFailed: number;
        readonly bytesUp: number;
        readonly bytesDown: number;
    }
    /** An L4 byte reverse proxy; no JS runs on the data path. */
    class TCPProxy implements DynResource {
        constructor(opts: { port: number; upstream: TCPUpstream | TCPUpstream[]; maxConns?: number; idleTimeoutMs?: number; connectTimeoutMs?: number });
        /** Binds and starts forwarding; a port of 0 resolves into `.port`. */
        start(): void;
        /** Live pairs and cumulative counters. */
        stats(): TCPProxyStats;
        /** The bound port. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** RFC 6555 Happy Eyeballs: both address families race in parallel, the first success wins. */
    function connectHappy(host: string, port: number, opts?: { fallbackMs?: number }, handlers?: TCPHandlers): TCPServer;
}


/* ================================================================== *
 *  dyna:json
 * ================================================================== */
declare module "dyna:json" {
    /** RFC 6901 JSON Pointer. */
    namespace Pointer {
        /** Walks pointer; missing members and out-of-range indices throw. */
        function get<T = unknown>(doc: unknown, pointer: string): T;
        /** Same walk; a missing target returns false instead of throwing. */
        function has(doc: unknown, pointer: string): boolean;
        /** Mutates doc in place (RFC 6902 add semantics) and returns it. */
        function set<T>(doc: T, pointer: string, value: unknown): T;
        /** Mutates doc in place and returns it. */
        function remove<T>(doc: T, pointer: string): T;
        /** ~ -> ~0 and / -> ~1. */
        function escape(token: string): string;
        /** Reverses escape; throws on a `~` not followed by 0 or 1. */
        function unescape(token: string): string;
    }

    /** RFC 6902 JSON Patch. */
    const Patch: {
        /** Runs the six RFC 6902 ops on a PRIVATE deep copy; the input is never written. */
        apply(doc: unknown, ops: { op: string; path: string; value?: unknown; from?: string }[]): unknown;
    };
}

/* ================================================================== *
 *  dyna:log
 * ================================================================== */
declare module "dyna:log" {
    /** Leveled structured logging to stderr, one JSON object per line. */
    type LogLevel = "trace" | "debug" | "info" | "warn" | "error" | "fatal" | "silent";
    class Logger {
        constructor(opts?: { level?: LogLevel; name?: string; timestamp?: "epoch" | "iso" | false; base?: Record<string, unknown> });
        /** Each emits one line below the configured level. */
        trace(msg: unknown, ...fields: unknown[]): void;
        debug(msg: unknown, ...fields: unknown[]): void;
        info(msg: unknown, ...fields: unknown[]): void;
        warn(msg: unknown, ...fields: unknown[]): void;
        error(msg: unknown, ...fields: unknown[]): void;
        fatal(msg: unknown, ...fields: unknown[]): void;
        /** A new Logger with fields appended to the base prefix. */
        child(fields: Record<string, unknown>): Logger;
        /** Whether the level passes the current threshold. */
        enabled(level: LogLevel): boolean;
        /** Get/set the level. */
        level: LogLevel;
    }

    /** Returns a function that prints only when DEBUG matches (comma-separated globs). */
    function Debug(namespace: string): (...args: unknown[]) => void;
}

/* ================================================================== *
 *  dyna:matcher
 * ================================================================== */
declare module "dyna:matcher" {
    /** A compiled single-pattern matcher. */
    class Matcher {
        constructor(pattern: string, opts?: { algo?: "kmp" | "bmh" | "boyer-moore" });
        /** Code-unit offset of the first match, or -1. */
        firstIn(text: string): number;
        test(text: string): boolean;
        countIn(text: string): number;
        /** Every match offset. */
        allIn(text: string): number[];
        /** Non-overlapping left-to-right replacement. */
        replaceAllIn(text: string, repl: string): string;
        readonly length: number;
        readonly algo: string;
    }

    /** A byte-trie Aho-Corasick multi-pattern matcher. */
    class MultiMatcher {
        constructor(patterns: string[]);
        /** The pattern index and offset of the earliest hit, or null. */
        firstIn(text: string): { index: number; at: number } | null;
        test(text: string): boolean;
        /** Every emitted hit (overlapping matches each count). */
        countIn(text: string): number;
        allIn(text: string): { index: number; at: number }[];
        readonly size: number;
        readonly states: number;
    }

    /** Exact edit distance in code points, Myers bit-parallel below 64. */
    function Levenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Bigram multiset similarity in [0, 1]. */
    function DiceCoefficient(a: string, b: string): number;
    /** A diff hunk: -1 deleted, 1 inserted, 0 common. */
    interface DiffHunk {
        op: -1 | 0 | 1;
        text: string;
    }
    /** Myers diff tokenised by character. */
    function DiffChars(a: string, b: string): DiffHunk[];
    /** Myers diff tokenised by word. */
    function DiffWords(a: string, b: string): DiffHunk[];
    /** Myers diff tokenised by line. */
    function DiffLines(a: string, b: string): DiffHunk[];
}

/* ================================================================== *
 *  dyna:mathx
 * ================================================================== */
declare module "dyna:mathx" {
    /** Mathematical constants, written with enough digits for one correctly-rounded conversion. */
    const E: number;
    const Pi: number;
    const Phi: number;
    const Sqrt2: number;
    const SqrtE: number;
    const SqrtPi: number;
    const Ln2: number;
    const Ln10: number;
    const Log2E: number;
    const Log10E: number;
    const MaxInt32: number;
    const MinInt32: number;
    const MaxSafeInteger: number;
    const MaxInt64: bigint;

    /** Smallest positive normal double (DBL_MIN). */
    function realmin(): number;
    /** Largest finite double (DBL_MAX). */
    function realmax(): number;
    /** 2^53, the largest integer every double below it represents exactly. */
    function flintmax(): number;
    /** The gap to the next representable double away from zero; bare eps is eps(1). */
    function eps(x?: number): number;

    /** C99 round (ties away from zero). */
    function round(x: number): number;
    /** Round half to even. */
    function roundToEven(x: number): number;
    /** Truncate toward zero. */
    function fix(x: number): number;
    /** 1, -1, or x itself (-0 and NaN pass through). */
    function sign(x: number): number;
    /** The sign bit directly. */
    function signbit(x: number): boolean;
    /** C trunc. */
    function trunc(x: number): number;
    /** Splits into [intPart, fracPart]. */
    function modf(x: number): [number, number];

    /** MATLAB floored modulo: mod(-7, 3) is 2; mod(a, 0) is a. */
    function mod(a: number, b: number): number;
    /** Truncated fmod. */
    function rem(a: number, b: number): number;
    function fmod(a: number, b: number): number;
    /** C99 round-to-nearest remainder. */
    function remainder(a: number, b: number): number;
    /** Integer division with an explicit rounding mode: "fix"|"floor"|"ceil"|"round". */
    function idivide(a: number, b: number, mode?: "fix" | "floor" | "ceil" | "round"): number;
    /** The real n-th root; defined for negative x with odd integer n. */
    function nthroot(x: number, n: number): number;

    /** Gamma function (tgamma). */
    function gamma(x: number): number;
    function cbrt(x: number): number;
    function hypot(a: number, b: number): number;
    function copysign(a: number, b: number): number;
    function nextafter(a: number, b: number): number;
    function expm1(x: number): number;
    function log1p(x: number): number;
    function log2(x: number): number;
    /** The unbiased floating-point exponent (logb). */
    function logb(x: number): number;
    /** 2^x (exp2), the inverse of log2. */
    function pow2(x: number): number;
    function deg2rad(x: number): number;
    function rad2deg(x: number): number;
    /** The smallest p with 2^p >= |x|; nextpow2(0) is 0. */
    function nextpow2(x: number): number;
    /** x * 2**n. */
    function scalbn(x: number, n: number): number;
    /** Identical to scalbn (MATLAB spelling). */
    function ldexp(frac: number, exp: number): number;
    /** Splits into x = frac * 2**exp with |frac| in [0.5, 1). */
    function frexp(x: number): [number, number];
    /** ±Inf and NaN give 2^31-1, 0 gives -(2^31), else the unbiased exponent. */
    function ilogb(x: number): number;
    /** Tests infinity, optionally restricted to one sign. */
    function isInf(x: number, sign?: number): boolean;
    function isNaN(x: number): boolean;

    function erf(x: number): number;
    function erfc(x: number): number;
    /** Inverts erf; erfinv(±1) is ±Inf. */
    function erfinv(y: number): number;
    /** erfinv(1 - y), domain [0, 2]. */
    function erfcinv(y: number): number;
    /** exp(x^2)*erfc(x), finite where erfc underflows. */
    function erfcx(x: number): number;

    /** [log|Gamma(x)|, sign of Gamma(x)] via the reentrant lgamma_r. */
    function lgamma(x: number): [number, number];
    /** log|Gamma| without the sign. */
    function gammaln(x: number): number;
    function beta(a: number, b: number): number;
    function betaln(a: number, b: number): number;
    /** Digamma; psi(0) is ±Inf, negative integers are NaN. */
    function psi(x: number): number;
    /** n-th derivative, order in [0, 64]. */
    function polygamma(n: number, x: number): number;
    /** Regularised incomplete gamma (x first); "upper" selects the complement. */
    function gammainc(x: number, a: number, tail?: "upper"): number;
    /** Inverts P(a, x) = p. */
    function gammaincinv(p: number, a: number): number;
    /** Regularised incomplete beta. */
    function betainc(x: number, a: number, b: number): number;
    function betaincinv(p: number, a: number, b: number): number;
    /** E1(x), defined for x > 0. */
    function expint(x: number): number;

    /** Integer order; non-integer order is not offered. */
    function besselj(n: number, x: number): number;
    function bessely(n: number, x: number): number;
    /** Real order. */
    function besseli(nu: number, x: number): number;
    function besselk(nu: number, x: number): number;
    /** I_nu(x) e^-x. */
    function besseliScaled(nu: number, x: number): number;
    /** K_nu(x) e^x. */
    function besselkScaled(nu: number, x: number): number;
    /** Hankel function J_n ± i Y_n; kind 1 or 2; returns [re, im]. */
    function besselh(n: number, x: number, kind: 1 | 2): [number, number];

    /** Complete elliptic integrals [K, E] from one AGM iteration. */
    function ellipke(m: number): [number, number];
    /** Jacobi elliptic functions {sn, cn, dn}. */
    function ellipj(u: number, m: number): { sn: number; cn: number; dn: number };

    /** Associated Legendre functions P_n^m for the whole column m = 0..n. */
    function legendre(n: number, x: number): number[];
    /** The single value P_n^m(x); degree capped at 150. */
    function legendreP(n: number, m: number, x: number): number;
    /** All four Airy values from one evaluation. */
    function airy(x: number): { ai: number; aip: number; bi: number; bip: number };

    /** Deterministic Miller-Rabin with a 12-witness set; proven for every uint64. */
    function isPrime(n: number): boolean;
    /** Ascending prime factors with multiplicity; factor(1) is []. */
    function factor(n: number): number[];
    /** Every prime <= n by sieve, up to 5e7. */
    function primes(n: number): number[];

    function gcd(a: number | bigint, b: number | bigint): bigint;
    function lcm(a: number | bigint, b: number | bigint): bigint;
    /** n! exactly, capped at 10000. */
    function factorial(n: number): bigint;
    /** BigInt only; the magnitude as unsigned. */
    function abs(n: bigint): bigint;
    /** Minimum bits to represent the magnitude; bitLen(0n) is 0. */
    function bitLen(n: bigint): number;
    /** Set bits in the magnitude. */
    function popcount(n: bigint): number;

    /** Binomial coefficient, built multiplicatively. */
    function nchoosek(n: number, k: number): number;
    /** Every permutation, reverse lexicographic, at most 8 elements. */
    function perms(v: number[]): number[][];
    /** Rational approximation by continued fractions within relative tolerance. */
    function rat(x: number, tol?: number): [number, number];

    /** n points inclusive of both ends; the last point is exactly b. */
    function linspace(a: number, b: number, n?: number): number[];
    /** 10^t over the linspace grid. */
    function logspace(a: number, b: number, n?: number): number[];
    function cumsum(v: number[]): number[];
    function cumprod(v: number[]): number[];
    /** Adjacent differences, one element shorter. */
    function diff(v: number[]): number[];

    /** Width-parameterised fixed-width bit primitives. */
    namespace bits {
        const uintSize: number;
        function leadingZeros8(x: number): number;
        function leadingZeros16(x: number): number;
        function leadingZeros32(x: number): number;
        function leadingZeros64(x: bigint): number;
        function trailingZeros8(x: number): number;
        function trailingZeros16(x: number): number;
        function trailingZeros32(x: number): number;
        function trailingZeros64(x: bigint): number;
        function onesCount8(x: number): number;
        function onesCount16(x: number): number;
        function onesCount32(x: number): number;
        function onesCount64(x: bigint): number;
        function len8(x: number): number;
        function len16(x: number): number;
        function len32(x: number): number;
        function len64(x: bigint): number;
        function reverse8(x: number): number;
        function reverse16(x: number): number;
        function reverse32(x: number): number;
        function reverse64(x: bigint): bigint;
        function reverseBytes16(x: number): number;
        function reverseBytes32(x: number): number;
        function reverseBytes64(x: bigint): bigint;
        function rotateLeft8(x: number, k: number): number;
        function rotateLeft16(x: number, k: number): number;
        function rotateLeft32(x: number, k: number): number;
        function rotateLeft64(x: bigint, k: number): bigint;
        /** Widening add with carry in and out. */
        function add32(a: number, b: number, carry: number): [number, number];
        function add64(a: bigint, b: bigint, carry: bigint): [bigint, bigint];
        function sub32(a: number, b: number, borrow: number): [number, number];
        function sub64(a: bigint, b: bigint, borrow: bigint): [bigint, bigint];
        /** Full-width product, high word first. */
        function mul32(a: number, b: number): [number, number];
        function mul64(a: bigint, b: bigint): [bigint, bigint];
        /** Divides the double-width hi:lo by y; throws on y==0 or y<=hi. */
        function div32(hi: number, lo: number, y: number): [number, number];
        function div64(hi: bigint, lo: bigint, y: bigint): [bigint, bigint];
        function rem32(hi: number, lo: number, y: number): number;
        function rem64(hi: bigint, lo: bigint, y: bigint): bigint;
    }

    /** Compiles an arithmetic string to an RPN program; no eval, no scope. */
    class Expression {
        constructor(text: string);
        /** Free variables in first-use order. */
        variables(): string[];
        /** Evaluates, reading only own data properties. */
        eval(vars?: Record<string, number>): number;
    }
}

/* ================================================================== *
 *  dyna:ml
 * ================================================================== */
declare module "dyna:ml" {
    /** A matrix as an array of rows, or a flat Float64Array plus (rows, cols). */
    type Matrix = number[][] | Float64Array;
    type Target = number[] | Float64Array;

    /** Shared estimator options. */
    interface TreeOpts {
        nEstimators?: number;
        maxDepth?: number;
        minSamplesSplit?: number;
        minSamplesLeaf?: number;
        maxFeatures?: number;
        maxBins?: number;
        seed?: number;
    }
    interface FitOpts {
        sampleWeight?: number[] | Float64Array;
    }

    /** Closed-form ordinary least squares. */
    class LinearRegression implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): LinearRegression;
        static load(path: import("dyna:file").Path): LinearRegression;
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        readonly coef: number[];
        readonly intercept: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Full-batch gradient descent logistic regression. */
    class LogisticRegression implements DynResource {
        constructor(opts?: { learningRate?: number; maxIter?: number; tol?: number; l1?: number; l2?: number; C?: number; penalty?: "l1" | "l2" | "elasticnet" | "none"; classWeight?: "balanced" });
        static deserialize(bytes: Uint8Array | ArrayBuffer): LogisticRegression;
        static load(path: import("dyna:file").Path): LogisticRegression;
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        readonly classes: number[];
        readonly coef: number[][];
        readonly intercept: number | number[];
        readonly nIter: number;
        readonly converged: boolean;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Lloyd's algorithm with k-means++ seeding. */
    class KMeans implements DynResource {
        constructor(nClusters?: number, seed?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): KMeans;
        static load(path: import("dyna:file").Path): KMeans;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        readonly inertia: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** SMO support-vector classifier. */
    class SVC implements DynResource {
        constructor(opts?: { kernel?: "linear" | "rbf" | "poly"; C?: number; gamma?: number; coef0?: number; degree?: number; tol?: number; maxIter?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): SVC;
        static load(path: import("dyna:file").Path): SVC;
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        decisionFunction(X: Matrix, rows?: number, cols?: number): number[] | number[][];
        readonly nSupportVectors: number;
        readonly classes: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** EM soft clustering over diagonal-covariance Gaussians. */
    class GaussianMixture implements DynResource {
        constructor(k?: number, opts?: { seed?: number; maxIter?: number; tol?: number; regCovar?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianMixture;
        static load(path: import("dyna:file").Path): GaussianMixture;
        fit(X: Matrix, rows?: number, cols?: number): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        readonly weights: number[];
        readonly means: number[][];
        readonly variances: number[][];
        readonly logLikelihood: number;
        readonly nIter: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Naive Bayes with per-class Gaussian densities. */
    class GaussianNB implements DynResource {
        constructor(varSmoothing?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianNB;
        static load(path: import("dyna:file").Path): GaussianNB;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        readonly classes: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** CART classifier over Gini impurity. */
    class DecisionTreeClassifier implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeClassifier;
        static load(path: import("dyna:file").Path): DecisionTreeClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** CART regressor minimising variance. */
    class DecisionTreeRegressor implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeRegressor;
        static load(path: import("dyna:file").Path): DecisionTreeRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Bagged decision trees; a fixed seed reproduces a forest exactly. */
    class RandomForestClassifier implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestClassifier;
        static load(path: import("dyna:file").Path): RandomForestClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Bagged regression trees. */
    class RandomForestRegressor implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestRegressor;
        static load(path: import("dyna:file").Path): RandomForestRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for regression. */
    class GradientBoostingRegressor implements DynResource {
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingRegressor;
        static load(path: import("dyna:file").Path): GradientBoostingRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for classification. */
    class GradientBoostingClassifier implements DynResource {
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingClassifier;
        static load(path: import("dyna:file").Path): GradientBoostingClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Second-order (Newton) boosting; NaN means "missing". */
    class XGBRegressor implements DynResource {
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBRegressor;
        static load(path: import("dyna:file").Path): XGBRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** The classifier twin of XGBRegressor. */
    class XGBClassifier implements DynResource {
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBClassifier;
        static load(path: import("dyna:file").Path): XGBClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        predictProba(X: Matrix, rows?: number, cols?: number): number[][];
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        readonly featureImportances: number[];
        readonly depth: number;
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Principal components by cyclic Jacobi diagonalisation. */
    class PCA implements DynResource {
        constructor(nComponents?: number, whiten?: boolean);
        static deserialize(bytes: Uint8Array | ArrayBuffer): PCA;
        static load(path: import("dyna:file").Path): PCA;
        fit(X: Matrix, rows?: number, cols?: number): this;
        transform(X: Matrix, rows?: number, cols?: number): number[][];
        fitTransform(X: Matrix, rows?: number, cols?: number): number[][];
        inverseTransform(X: Matrix, rows?: number, cols?: number): number[][];
        readonly components: number[][];
        readonly mean: number[];
        readonly explainedVariance: number[];
        readonly explainedVarianceRatio: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours classifier; lazy, distances stay squared. */
    class KNClassifier implements DynResource {
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNClassifier;
        static load(path: import("dyna:file").Path): KNClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours regressor. */
    class KNRegressor implements DynResource {
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNRegressor;
        static load(path: import("dyna:file").Path): KNRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        predict(X: Matrix, rows?: number, cols?: number): number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Density-based clustering with a grid index for low-dimensional data. */
    class DBScan implements DynResource {
        constructor(eps?: number, minPts?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DBScan;
        static load(path: import("dyna:file").Path): DBScan;
        fit(X: Matrix, rows?: number, cols?: number): this;
        readonly labels: number[];
        readonly nClusters: number;
        readonly eps: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Per-column z-score scaling; constant columns report std 1.0. */
    class StandardScaler implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): StandardScaler;
        static load(path: import("dyna:file").Path): StandardScaler;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): this;
        transform(X: Matrix, rows?: number, cols?: number): number[][];
        fitTransform(X: Matrix, rows?: number, cols?: number): number[][];
        inverseTransform(X: Matrix, rows?: number, cols?: number): number[][];
        readonly mean: number[];
        readonly std: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Per-column min-max scaling to [0, 1]. */
    class MinMaxScaler implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): MinMaxScaler;
        static load(path: import("dyna:file").Path): MinMaxScaler;
        fit(X: Matrix, rows?: number, cols?: number): this;
        transform(X: Matrix, rows?: number, cols?: number): number[][];
        fitTransform(X: Matrix, rows?: number, cols?: number): number[][];
        inverseTransform(X: Matrix, rows?: number, cols?: number): number[][];
        readonly dataMin: number[];
        readonly dataMax: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An immutable compressed-sparse-row matrix over doubles. */
    class CSR implements DynResource {
        constructor(values: number[] | Float64Array, columns: number[] | Int32Array, rowPointers: number[] | Int32Array, cols: number);
        /** Drops exact zeros from a dense matrix. */
        static fromDense(X: Matrix, rows?: number, cols?: number): CSR;
        /** The dense form; throws if it does not fit in memory. */
        toDense(): number[][];
        /** Row i as a dense Array. */
        row(i: number): number[];
        readonly rows: number;
        readonly cols: number;
        readonly nnz: number;
        readonly density: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A composition of feature stages and a final estimator. */
    class Pipeline implements DynResource {
        constructor(stages: unknown[]);
        fit(X: Matrix, y: Target): this;
        predict(X: Matrix): number[];
        predictProba(X: Matrix): number[][];
        transform(X: Matrix): number[][];
        /** The i-th stage (negative counts from the end). */
        stage(i: number): unknown;
        readonly length: number;
        readonly fitted: boolean;
        readonly estimator: unknown;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Mean of squared errors. */
    function meanSquaredError(yTrue: Target, yPred: Target): number;
    function meanAbsoluteError(yTrue: Target, yPred: Target): number;
    /** Coefficient of determination; a constant yTrue scores 1.0 for exact, else 0.0. */
    function r2Score(yTrue: Target, yPred: Target): number;
    function accuracy(yTrue: Target, yPred: Target): number;
    /** Mean negative log-likelihood; yPred may be a class matrix or a binary vector. */
    function logLoss(yTrue: Target, yPred: Target | Target[]): number;
    /** Indexed [true][pred]; labels non-negative integers up to 4095. */
    function confusionMatrix(yTrue: Target, yPred: Target): number[][];
    function precision(yTrue: Target, yPred: Target, positive?: number): number;
    function recall(yTrue: Target, yPred: Target, positive?: number): number;
    function f1(yTrue: Target, yPred: Target, positive?: number): number;
    function specificity(yTrue: Target, yPred: Target, positive?: number): number;
    function balancedAccuracy(yTrue: Target, yPred: Target, positive?: number): number;
    function matthewsCorrcoef(yTrue: Target, yPred: Target, positive?: number): number;
    function cohenKappa(yTrue: Target, yPred: Target, positive?: number): number;
    /** beta > 1 weights recall. */
    function fbeta(yTrue: Target, yPred: Target, beta: number, positive?: number): number;
    /** Exact Mann-Whitney U; needs both a positive and a negative sample. */
    function rocAuc(yTrue: Target, yScore: Target, positive?: number): number;
    function averagePrecision(yTrue: Target, yScore: Target, positive?: number): number;

    /** Indices, not data. */
    function trainTestSplit(n: number | Target, opts?: { testSize?: number; shuffle?: boolean; seed?: number }): { train: number[]; test: number[] };
    function kFold(n: number | Target, opts?: { k?: number; folds?: number; shuffle?: boolean; seed?: number }): { train: number[]; test: number[] }[];
    function stratifiedKFold(y: Target, opts?: { k?: number; shuffle?: boolean; seed?: number }): { train: number[]; test: number[] }[];
    /** Per-fold scores; estimatorFactory is () => new Model(...). */
    function crossValScore(estimatorFactory: () => { fit(X: Matrix, y: Target): unknown; predict(X: Matrix): number[]; close(): void }, X: Matrix, y: Target, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number }): number[];
    /** Exhaustive parameter search. */
    function gridSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number }): { best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] };
    /** Random parameter search over nIter sampled points. */
    function randomSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { nIter?: number; k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number }): { best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] };

    /** Replaces every non-finite entry with its column's finite mean. */
    function imputeMean(X: Matrix, rows?: number, cols?: number): number[][];
    /** Removes rows holding a non-finite value; `kept` lists the survivors. */
    function dropMissing(X: Matrix, y?: Target, rows?: number, cols?: number): { X: number[][]; y: Float64Array | undefined; kept: number[] };
}
/* ================================================================== *
 *  dyna:random
 * ================================================================== */
declare module "dyna:random" {
    /** A seedable xoshiro256** PRNG; a given seed is deterministic and reproducible. */
    class Random {
        constructor(seed?: number | bigint);
        /** A full 64-bit draw, always BigInt. */
        nextU64(): bigint;
        /** The top 53 bits as an exact Number in [0, 2^53). */
        nextU53(): number;
        /** A double in [0, 1). */
        nextFloat(): number;
        /** Uniform in [0, bound) by rejection sampling; the result type mirrors the argument. */
        nextBounded(bound: number): number;
        nextBounded(bound: bigint): bigint;
        /** Fills any byte-width typed array with fresh random bytes. */
        fill(typedArray: ArrayBufferView): this;
    }
}

/* ================================================================== *
 *  dyna:schema
 * ================================================================== */
declare module "dyna:schema" {
    /** JSON Schema Draft 2020-12 validation. */
    interface SchemaError {
        path: string;
        message: string;
        keyword: string;
    }
    interface SchemaResult {
        valid: boolean;
        errors: SchemaError[];
    }
    /** A compiled schema: reusable, thread-safe, pure dispatch at validate time. */
    interface CompiledSchema {
        validate(instance: unknown): SchemaResult;
    }
    const Schema: {
        /** Compiles the schema once into a native node tree. */
        compile(schema: unknown): CompiledSchema;
        /** Compiles and caches on the schema object; accepts an already-compiled schema. */
        validate(schema: unknown | CompiledSchema, instance: unknown): SchemaResult;
    };
}

/* ================================================================== *
 *  dyna:scrape
 * ================================================================== */
declare module "dyna:scrape" {
    /** robots.txt parsing. */
    class Robots implements DynResource {
        constructor(text: string, opts?: { agent?: string });
        /** True when the path is allowed for the configured agent. */
        allows(path: string): boolean;
        /** The crawl delay for the agent, or null when unset. */
        crawlDelay(): number | null;
        /** The sitemap URLs listed in the file. */
        sitemaps(): string[];
        readonly ruleCount: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Field extraction spec: a selector per field. */
    class Extractor implements DynResource {
        constructor(spec: Record<string, unknown>);
        /** Runs against a parsed document; options.base resolves relative links. */
        run(doc: unknown, opts?: { base?: string }): { values: Record<string, unknown>; missing: string[] };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Polite HTTP retrieval with robots policy, retries and backoff. */
    class Fetcher implements DynResource {
        constructor(opts: { agent: string; client: unknown });
        /** GET a URL; returns the HTTP response with url/fromCache added. */
        get(url: string): FetcherResponse;
        stats(): { fetched: number; skippedByRobots: number; retried: number; throttledMs: number; bytes: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A fetched page. */
    interface FetcherResponse {
        status: number;
        headers: Record<string, string>;
        body: string;
        url: string;
        fromCache: boolean;
    }

    /** Bounded traversal over a Fetcher plus an Extractor. */
    class Crawl implements DynResource {
        constructor(fetcher: Fetcher, opts?: { maxPages?: number; maxDepth?: number; sameHost?: boolean; linkField?: string });
        /** Seeds the crawl with an http(s) url; the iterator yields pages. */
        start(seed: string, extractor?: Extractor, parse?: (html: string) => unknown): this;
        /** One page per call: {value, done}. */
        next(): IteratorResult<unknown>;
        /** The collected pages. */
        pages(): unknown;
        [Symbol.iterator](): Iterator<unknown>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/* ================================================================== *
 *  dyna:semver
 * ================================================================== */
declare module "dyna:semver" {
    /** A parsed semver 2.0.0 version. */
    interface SemVer {
        major: number;
        minor: number;
        patch: number;
        prerelease: (string | number)[];
        build: string[];
        version: string;
    }

    /** Parses major.minor.patch[-pre][+build]; values above MAX_SAFE throw. */
    function parse(version: string): SemVer;
    function isValid(version: string): boolean;
    /** Trims, strips a leading `=`, returns the normalized version or null. */
    function clean(version: string): string | null;
    /** The first run of digits normalized into X.Y.Z, or null. */
    function coerce(version: string): string | null;
    function compare(a: string, b: string): -1 | 0 | 1;
    function eq(a: string, b: string): boolean;
    function neq(a: string, b: string): boolean;
    function gt(a: string, b: string): boolean;
    function gte(a: string, b: string): boolean;
    function lt(a: string, b: string): boolean;
    function lte(a: string, b: string): boolean;
    /** Ascending in-place sort by precedence. */
    function sort(versions: string[]): string[];
    function major(v: string): number;
    function minor(v: string): number;
    function patch(v: string): number;
    function prerelease(v: string): (string | number)[] | null;
    /** Bumps the version; release major|minor|patch|premajor|preminor|prepatch|prerelease. */
    function inc(version: string, release: string, identifier?: string): string;
    /** One-shot range match; a range string is parsed per call. */
    function satisfies(version: string, range: string): boolean;
    function maxSatisfying(versions: string[], range: string): string | null;
    function minSatisfying(versions: string[], range: string): string | null;

    /** A compiled range expression. */
    class Range {
        constructor(rangeString: string);
        /** Whether the version matches the compiled range. */
        test(version: string): boolean;
        /** The versions that match, preserving input order. */
        filter(versions: string[]): string[];
        maxSatisfying(versions: string[]): string | null;
        minSatisfying(versions: string[]): string | null;
        readonly source: string;
        readonly setCount: number;
    }
}

/* ================================================================== *
 *  dyna:serialize
 * ================================================================== */
declare module "dyna:serialize" {
    /** Protobuf wire encoding/decoding driven by a plain-object descriptor. */
    interface ProtoField {
        name: string;
        number: number;
        type: string;
        repeated?: boolean;
        packed?: boolean;
        keyType?: string;
        valueType?: string;
        message?: { fields: ProtoField[] };
    }
    const Proto: {
        /** Wire encoding; strict about JS types and out-of-range numbers. */
        encode(value: unknown, schema: { fields: ProtoField[] }): Uint8Array;
        /** The untrusted surface: lengths validated, nesting capped at 64. */
        decode(bytes: ByteView, schema: { fields: ProtoField[] }): unknown;
    };

    /** Canonical DER codec. */
    interface ASN1Node {
        cls: number;
        tag: number;
        constructed: boolean;
        value: unknown;
    }
    const ASN1: {
        encode(node: ASN1Node): Uint8Array;
        decode(bytes: Uint8Array | ArrayBuffer): ASN1Node;
        seq(children: ASN1Node[]): ASN1Node;
        set(children: ASN1Node[]): ASN1Node;
        int(value: number): ASN1Node;
        bool(value: boolean): ASN1Node;
        null(): ASN1Node;
        octets(bytes: ByteView): ASN1Node;
        bitString(bytes: ByteView, unused: number): ASN1Node;
        oid(str: string): ASN1Node;
        utf8(str: string): ASN1Node;
        printable(str: string): ASN1Node;
        utcTime(str: string): ASN1Node;
        generalizedTime(str: string): ASN1Node;
        context(tag: number, content: ByteView): ASN1Node;
        contextC(tag: number, children: ASN1Node[]): ASN1Node;
    };

    /** MessagePack encoding; refuses symbols and functions. */
    function MsgPackEncode(value: unknown): Uint8Array;
    function MsgPackDecode(bytes: ByteView): unknown;
    /** RFC 8949 CBOR with the same walker, bounds and refusals. */
    function CBOREncode(value: unknown): Uint8Array;
    function CBORDecode(bytes: ByteView): unknown;
    /** CBOR with map keys sorted byte-wise (deterministic form). */
    function CBORCanonical(value: unknown): Uint8Array;
    /** Canonical CBOR run through XXH64, returned as 16 lowercase hex chars. */
    function ValueHash(value: unknown): string;
    /** Deep clone preserving cycles and shared references; functions refused. */
    function structuredClone(value: unknown): unknown;
}

/* ================================================================== *
 *  dyna:simd
 * ================================================================== */
declare module "dyna:simd" {
    /** Arithmetic sum of a float array. */
    function sum(a: F32Like): number;
    function max(a: F32Like): number;
    function min(a: F32Like): number;
    /** Index of the extreme; throws on an empty array. */
    function argmax(a: F32Like): number;
    function argmin(a: F32Like): number;
    /** The 1-norm. */
    function normL1(a: F32Like): number;
    /** The Euclidean norm. */
    function normL2(a: F32Like): number;

    /** Elementwise: out[i] = a[i] op b[i]; all lengths must match. */
    function add(out: F32Like, a: F32Like, b: F32Like): F32Like;
    function sub(out: F32Like, a: F32Like, b: F32Like): F32Like;
    function mul(out: F32Like, a: F32Like, b: F32Like): F32Like;
    function div(out: F32Like, a: F32Like, b: F32Like): F32Like;
    function abs(out: F32Like, a: F32Like): F32Like;
    /** In-place z[i] += a[i] * b[i]. */
    function fma(z: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Inner product. */
    function dot(a: F32Like, b: F32Like): number;

    /** In-place scalar ops. */
    function scale(a: F32Like, s: number): F32Like;
    function addScalar(a: F32Like, s: number): F32Like;
    /** y[i] += alpha * x[i]. */
    function axpy(y: F32Like, alpha: number, x: F32Like): F32Like;
    /** a[i] = alpha * a[i] + beta. */
    function affine(a: F32Like, alpha: number, beta: number): F32Like;

    /** In-place activations (out may alias in). */
    function sigmoid(a: F32Like): F32Like;
    function relu(a: F32Like): F32Like;
    function relu6(a: F32Like): F32Like;
    function leakyRelu(a: F32Like, slope: number): F32Like;
    function elu(a: F32Like, alpha: number): F32Like;
    function tanhFast(a: F32Like): F32Like;
    function gelu(a: F32Like): F32Like;
    function silu(a: F32Like): F32Like;
    /** Stable max-shifted softmax in place. */
    function softmax(a: F32Like): F32Like;
    function logSoftmax(a: F32Like): F32Like;

    /** In-place unary math; NaN propagates. */
    function vexp(a: F32Like): F32Like;
    function vlog(a: F32Like): F32Like;
    function vsqrt(a: F32Like): F32Like;
    function vrsqrt(a: F32Like): F32Like;
    function vinv(a: F32Like): F32Like;

    /** Vector-to-scalar distances over equal-length pairs. */
    function distL2(a: F32Like, b: F32Like): number;
    function distL1(a: F32Like, b: F32Like): number;
    function distCos(a: F32Like, b: F32Like): number;
    function distCheb(a: F32Like, b: F32Like): number;

    /** BLAS-2/3, row-major, explicit dimensions. */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    function gemvT(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    function gemm(c: F32Like, a: F32Like, b: F32Like, m: number, n: number, k: number, alpha: number, beta: number): F32Like;

    /** In-place min(max(x, lo), hi). */
    function clamp(a: F32Like, lo: number, hi: number): F32Like;
    /** In-place binarise: x > t ? 1.0 : 0.0. */
    function threshold(a: F32Like, t: number): F32Like;
    /** Indices of the k largest values (a fresh array, unspecified order). */
    function topkIndices(vals: F32Like, k: number): Uint32Array;

    /** Zero-copy Float64Array kernels. */
    function f64Sum(a: Float64Array): number;
    function f64Dot(a: Float64Array, b: Float64Array): number;
    function f64Max(a: Float64Array): number;
    function f64Min(a: Float64Array): number;
    function f64Scale(a: Float64Array, s: number): Float64Array;
    function f64Axpy(y: Float64Array, alpha: number, x: Float64Array): Float64Array;

    /** Zero-copy Int32Array kernels only. */
    function i32Sum(a: Int32Array): number;
    function i32Min(a: Int32Array): number;
    function i32Max(a: Int32Array): number;
    function i32Dot(a: Int32Array, b: Int32Array): number;
    function i32Add(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    function i32Mul(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    function i32Scale(a: Int32Array, s: number): Int32Array;

    /** Inclusive prefix scan, in place. */
    function cumsum(a: Int32Array | Float32Array): Int32Array | Float32Array;
    function cummax(a: Int32Array | Float32Array): Int32Array | Float32Array;
}

/* ================================================================== *
 *  dyna:structures
 * ================================================================== */
declare module "dyna:structures" {
    /** Adjacency-list graph over integer node ids. */
    class Graph {
        constructor(opts?: { directed?: boolean; weighted?: boolean });
        static deserialize(bytes: Uint8Array | ArrayBuffer): Graph;
        addNode(): number;
        /** Adds a directed (or both-direction) edge; nodes grow on demand. */
        addEdge(u: number, v: number, w?: number): this;
        /** Targets of u's outgoing edges. */
        neighbors(u: number): number[];
        hasEdge(u: number, v: number): boolean;
        readonly nodeCount: number;
        readonly edgeCount: number;
        bfs(src: number): number[];
        dfs(src: number): number[];
        /** Shortest distances; a single distance to dst when given. */
        dijkstra(src: number): number[];
        dijkstra(src: number, dst: number): number;
        bellmanFord(src: number): number[];
        topologicalSort(): number[];
        /** Component id per node (weak components on a directed graph). */
        connectedComponents(): number[];
        /** All-pairs distance matrix; refused for n > 1024. */
        floydWarshall(): number[][];
        /** Minimum spanning forest. */
        mst(): { weight: number; edges: [number, number, number][] };
        aStar(src: number, dst: number, heuristic: (node: number) => number): { dist: number; path: number[] };
        serialize(): Uint8Array;
    }

    /** Capacity-bounded string-to-value cache with LRU eviction. */
    class LRU<V = unknown> {
        constructor(capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: V) => void });
        static deserialize(bytes: Uint8Array | ArrayBuffer): LRU<unknown>;
        get(key: string): V | undefined;
        put(key: string, value: V): this;
        set(key: string, value: V): this;
        setWithTTL(key: string, value: V, ms: number): this;
        has(key: string): boolean;
        delete(key: string): boolean;
        /** Reclaims every expired entry now; returns the count removed. */
        purgeExpired(): number;
        readonly size: number;
        readonly capacity: number;
        readonly stats: { hits: number; misses: number; evictions: number; expired: number; size: number; capacity: number };
        serialize(): Uint8Array;
    }

    /** Binary heap ordered by a JS comparator or natural number order. */
    class Heap<V = number> {
        constructor(comparator?: (a: V, b: V) => number);
        static deserialize(bytes: Uint8Array | ArrayBuffer, cmp?: (a: unknown, b: unknown) => number): Heap<unknown>;
        /** Insert and sift; returns the new size. */
        push(v: V): number;
        pop(): V | undefined;
        peek(): V | undefined;
        readonly size: number;
        readonly length: number;
        serialize(): Uint8Array;
    }

    /** Atkinson/Sack/Santoro/Strothotte min-max heap. */
    class MinMaxHeap<V = number> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): MinMaxHeap<unknown>;
        push(priority: number, value?: V): this;
        popMin(): V | undefined;
        popMax(): V | undefined;
        peekMin(): V | undefined;
        peekMax(): V | undefined;
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Set of numbers in sorted order (skiplist). */
    class SortedSet {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): SortedSet;
        add(x: number): this;
        has(x: number): boolean;
        delete(x: number): boolean;
        first(): number | undefined;
        last(): number | undefined;
        floor(x: number): number | undefined;
        ceil(x: number): number | undefined;
        /** Ascending keys in [lo, hi]. */
        rangeQuery(lo: number, hi: number): number[];
        toArray(): number[];
        readonly size: number;
        [Symbol.iterator](): Iterator<number>;
        serialize(): Uint8Array;
    }

    /** Skiplist map from numeric key to JS value. */
    class SortedMap<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): SortedMap<unknown>;
        set(k: number, v: V): this;
        get(k: number): V | undefined;
        has(k: number): boolean;
        delete(k: number): boolean;
        firstKey(): number | undefined;
        lastKey(): number | undefined;
        floorKey(k: number): number | undefined;
        ceilKey(k: number): number | undefined;
        rangeQuery(lo: number, hi: number): [number, V][];
        keys(): number[];
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Ordered map on numeric keys as a B-tree of order 32. */
    class BTree<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): BTree<unknown>;
        set(k: number, v: V): this;
        get(k: number): V | undefined;
        has(k: number): boolean;
        delete(k: number): boolean;
        firstKey(): number | undefined;
        lastKey(): number | undefined;
        floorKey(k: number): number | undefined;
        ceilKey(k: number): number | undefined;
        rangeQuery(lo: number, hi: number): [number, V][];
        keys(): number[];
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, V]>;
        serialize(): Uint8Array;
    }

    /** Double-ended queue with O(1) push/pop at both ends. */
    class Deque<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): Deque<unknown>;
        pushBack(v: V): number;
        pushFront(v: V): number;
        popFront(): V | undefined;
        popBack(): V | undefined;
        peekFront(): V | undefined;
        peekBack(): V | undefined;
        get(i: number): V | undefined;
        readonly length: number;
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        serialize(): Uint8Array;
    }

    /** Doubly-linked list of JS values; element identity is stable. */
    class List<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): List<unknown>;
        pushFront(v: V): number;
        pushBack(v: V): number;
        popFront(): V | undefined;
        popBack(): V | undefined;
        front(): V | undefined;
        back(): V | undefined;
        readonly length: number;
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        serialize(): Uint8Array;
    }

    /** Fixed-capacity circular buffer; push overwrites the oldest when full. */
    class RingBuffer<V = unknown> {
        constructor(capacity: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RingBuffer<unknown>;
        push(v: V): number;
        get(i: number): V | undefined;
        readonly length: number;
        readonly capacity: number;
        readonly full: boolean;
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        serialize(): Uint8Array;
    }

    /** Dynamic bit set backed by 64-bit words. */
    class BitSet {
        constructor(nbits?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): BitSet;
        set(i: number): this;
        clear(i: number): this;
        flip(i: number): this;
        get(i: number): boolean;
        /** The first set bit at position >= from, or -1. */
        nextSet(from: number): number;
        /** Number of set bits. */
        readonly count: number;
        and(other: BitSet): this;
        or(other: BitSet): this;
        xor(other: BitSet): this;
        toArray(): number[];
        [Symbol.iterator](): Iterator<number>;
        serialize(): Uint8Array;
    }

    /** Disjoint-set forest with path halving and union by rank. */
    class UnionFind {
        constructor(n?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): UnionFind;
        find(x: number): number;
        union(x: number, y: number): boolean;
        connected(x: number, y: number): boolean;
        /** Number of disjoint components. */
        readonly count: number;
        /** Element count. */
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Fenwick tree over a fixed-size vector of doubles. */
    class Fenwick {
        constructor(n: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): Fenwick;
        update(i: number, delta: number): this;
        /** Sum of positions [0..i] inclusive. */
        prefixSum(i: number): number;
        /** Sum of [lo..hi] inclusive; 0 for an empty range. */
        rangeQuery(lo: number, hi: number): number;
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Iterative segment tree with an associative fold: "sum"|"min"|"max". */
    class SegTree {
        constructor(n: number, op?: SegOp);
        static deserialize(bytes: Uint8Array | ArrayBuffer): SegTree;
        update(i: number, value: number): this;
        rangeQuery(lo: number, hi: number): number;
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Probabilistic set membership; no false negatives. */
    class BloomFilter {
        constructor(bits: number, hashes?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): BloomFilter;
        add(key: string): this;
        mayContain(key: string): boolean;
        readonly bits: number;
        readonly hashes: number;
        serialize(): Uint8Array;
    }

    /** Set of byte strings with prefix queries. */
    class Trie {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): Trie;
        insert(key: string): this;
        has(key: string): boolean;
        delete(key: string): boolean;
        keysWithPrefix(prefix: string): string[];
        /** The longest stored key that is a prefix of str, or "". */
        longestPrefix(str: string): string;
        readonly size: number;
        [Symbol.iterator](): Iterator<string>;
        serialize(): Uint8Array;
    }

    /** String key to uint64 count; counts saturate at 2^64-1. */
    class Multiset {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): Multiset;
        add(key: string, n?: number): number;
        remove(key: string, n?: number): number;
        count(key: string): number;
        has(key: string): boolean;
        setCount(key: string, count: number): this;
        delete(key: string): boolean;
        clear(): void;
        elementSet(): string[];
        entrySet(): [string, number][];
        /** Distinct key count. */
        readonly size: number;
        /** Sum of all counts. */
        readonly totalSize: number;
        [Symbol.iterator](): Iterator<[string, number]>;
        serialize(): Uint8Array;
    }

    /** String key to a growing value array. */
    class Multimap<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): Multimap<unknown>;
        put(key: string, value: V): this;
        get(key: string): V[];
        count(key: string): number;
        /** Removes every value for the key; returns how many. */
        delete(key: string): number;
        removeAt(key: string, index: number): V | undefined;
        keys(): string[];
        entries(): [string, V][];
        /** Total values. */
        readonly size: number;
        /** Distinct keys. */
        readonly keyCount: number;
        [Symbol.iterator](): Iterator<[string, V]>;
        serialize(): Uint8Array;
    }

    /** Two-way string-to-string map. */
    class BiMap {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): BiMap;
        set(key: string, value: string): this;
        forceSet(key: string, value: string): this;
        get(key: string): string | undefined;
        keyOf(value: string): string | undefined;
        has(key: string): boolean;
        hasValue(value: string): boolean;
        delete(key: string): boolean;
        deleteValue(value: string): boolean;
        entries(): [string, string][];
        inverseEntries(): [string, string][];
        clear(): void;
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string]>;
        serialize(): Uint8Array;
    }

    /** Sparse two-dimensional string-to-string-to-value map. */
    class Table<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): Table<unknown>;
        put(row: string, col: string, value: V): this;
        get(row: string, col: string): V | undefined;
        has(row: string, col: string): boolean;
        delete(row: string, col: string): boolean;
        row(r: string): [string, V][];
        column(c: string): [string, V][];
        cells(): [string, string, V][];
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string, V]>;
        serialize(): Uint8Array;
    }

    /** Set of closed numeric intervals, kept disjoint and merged on insert. */
    class RangeSet {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): RangeSet;
        add(lo: number, hi: number): this;
        remove(lo: number, hi: number): this;
        contains(x: number): boolean;
        encloses(lo: number, hi: number): boolean;
        intersects(lo: number, hi: number): boolean;
        ranges(): [number, number][];
        /** The gaps of [lo, hi] outside the set. */
        complement(lo: number, hi: number): [number, number][];
        clear(): void;
        readonly size: number;
        readonly measure: number;
        [Symbol.iterator](): Iterator<[number, number]>;
        serialize(): Uint8Array;
    }

    /** Map from closed numeric intervals to values; overlapping puts split. */
    class RangeMap<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): RangeMap<unknown>;
        put(lo: number, hi: number, value: V): this;
        get(x: number): V | undefined;
        remove(lo: number, hi: number): this;
        entries(): [number, number, V][];
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, number, V]>;
        serialize(): Uint8Array;
    }

    /** Closed-interval store with overlap enumeration. */
    class IntervalTree<V = unknown> {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): IntervalTree<unknown>;
        insert(lo: number, hi: number, value: V): this;
        overlapping(lo: number, hi: number): [number, number, V][];
        at(x: number): [number, number, V][];
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Count-min sketch: depth rows of width saturating counters. */
    class CountMinSketch {
        constructor(width: number, depth?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): CountMinSketch;
        add(key: string, n?: number): this;
        count(key: string): number;
        merge(other: CountMinSketch): this;
        readonly width: number;
        readonly depth: number;
        readonly totalCount: number;
        serialize(): Uint8Array;
    }

    /** Cardinality estimator over string keys. */
    class HyperLogLog {
        constructor(precision?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): HyperLogLog;
        add(key: string): this;
        count(): number;
        merge(other: HyperLogLog): this;
        readonly precision: number;
        readonly registers: number;
        serialize(): Uint8Array;
    }
}

/* ================================================================== *
 *  dyna:sys
 * ================================================================== */
declare module "dyna:sys" {
    /** A snapshot object of the current environment. */
    function env(): Record<string, string>;
    function getEnv(name: string): string | undefined;
    /** Overwrites an entry; refuses empty names, `=` in a name, or NUL anywhere. */
    function setEnv(name: string, value: string): void;
    /** The process argument vector (argv[0] first). */
    function args(): string[];
    function cwd(): string;
    function chDir(path: string): void;
    /** "darwin", "linux" or "unknown". */
    function platform(): string;
    function pid(): number;
    function hostName(): string;
    function homeDir(): string;
    /** {model, cores?, threads, mhz?, features} of the selected SIMD dispatch. */
    function cpuInfo(): { model: string; cores?: number; threads: number; mhz?: number; features: string[] };
    function memInfo(): { total: number; free: number; available: number };
    /** [1, 5, 15]-minute load averages. */
    function loadAvg(): [number, number, number];
    function uptime(): number;
    function diskUsage(path: string): { total: number; free: number; available: number };
    /** The engine's live allocation counters plus OS peakRss. */
    function memoryUsage(): { mallocCount: number; mallocSize: number; objCount: number; peakRss: number };

    /** Run a program with argv, no shell anywhere. */
    interface ExecResult {
        code: number | null;
        signal: string | null;
        stdout: string | Uint8Array;
        stderr: string | Uint8Array;
        timedOut: boolean;
    }
    interface ExecOptions {
        cwd?: string;
        env?: Record<string, string>;
        input?: BytesInput;
        timeoutMs?: number;
        maxBuffer?: number;
        encoding?: "utf8" | "bytes";
    }
    function Exec(command: string, args?: string[], options?: ExecOptions): ExecResult;
    /** Resolve a program name against PATH; null when not found. */
    function Which(name: string): string | null;
}

/* ================================================================== *
 *  dyna:time
 * ================================================================== */
declare module "dyna:time" {
    const Nanosecond: number;
    const Microsecond: number;
    const Millisecond: number;
    const Second: number;
    const Minute: number;
    const Hour: number;

    /** Parses "300ms", "-1.5h", "2h45m", "0"; returns a Number or BigInt of nanoseconds. */
    function parseDuration(str: string): number | bigint;
    /** The inverse of parseDuration; 0 is "0s". */
    function durationString(ns: number | bigint): string;

    /** A calendar duration; years fold into months and weeks into days. */
    class Duration {
        constructor(opts?: { years?: number; months?: number; weeks?: number; days?: number; hours?: number; minutes?: number; seconds?: number; milliseconds?: number });
        readonly years: number;
        readonly months: number;
        readonly days: number;
        /** 1, -1 or 0. */
        readonly sign: number;
        /** True when every component is zero. */
        readonly blank: boolean;
        /** ISO 8601; a mixed-sign value throws. */
        toString(): string;
    }

    /** {sec, nsec} from CLOCK_REALTIME. */
    function now(): { sec: number; nsec: number };
    /** BigInt nanoseconds since the Unix epoch. */
    function nowUnixNano(): bigint;
    function nowMillis(): number;
    /** BigInt nanoseconds from CLOCK_MONOTONIC. */
    function monotonicNano(): bigint;

    /** RFC 3339; nsec emitted only when non-zero; utc defaults true. */
    function formatRFC3339(sec: number, nsec?: number, utc?: boolean): string;
    /** Go-style layout tokens 2006 Jan Mon 01 02 15 04 05. */
    function formatUnix(sec: number, layout: string): string;
    /** Strict RFC 3339 parse; returns {sec, nsec}. */
    function parseRFC3339(str: string): { sec: number; nsec: number };
    /** Unix seconds (UTC); an out-of-range month carries into the year. */
    function date(y: number, mo: number, d: number, h?: number, mi?: number, s?: number): number;
    /** {year, month, day, hour, min, sec, weekday, yday}; weekday 0 = Sunday. */
    function fromUnix(sec: number): { year: number; month: number; day: number; hour: number; min: number; sec: number; weekday: number; yday: number };

    /** A compiled Go-style layout. */
    class Format {
        constructor(layout: string);
        format(sec: number): string;
        /** The inverse, strict; fields the layout omits default to 1970-01-01T00:00:00Z. */
        parse(str: string): number;
        readonly layout: string;
    }

    /** An immutable calendar date, proleptic Gregorian. */
    class PlainDate {
        constructor(year: number, month: number, day: number);
        readonly year: number;
        readonly month: number;
        readonly day: number;
        /** ISO 8601, Monday is 1. */
        readonly dayOfWeek: number;
        readonly dayOfYear: number;
        readonly daysInMonth: number;
        readonly daysInYear: number;
        readonly inLeapYear: boolean;
        /** Days since 1970-01-01. */
        readonly epochDay: number;
        /** Months move first and clamp, then days are added exactly. */
        add(duration: Duration): PlainDate;
        subtract(duration: Duration): PlainDate;
        /** A Duration of whole months plus the remaining days. */
        until(other: PlainDate): Duration;
        compare(other: PlainDate): -1 | 0 | 1;
        /** ISO 8601; years outside 0..9999 print signed with six digits. */
        toString(): string;
    }

    /** A date and time of day with no zone; adding time carries into the date. */
    class PlainDateTime {
        constructor(year: number, month: number, day: number, hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly year: number;
        readonly month: number;
        readonly day: number;
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        readonly epochDay: number;
        readonly dayOfWeek: number;
        add(duration: Duration): PlainDateTime;
        subtract(duration: Duration): PlainDateTime;
        toPlainDate(): PlainDate;
        toPlainTime(): PlainTime;
        compare(other: PlainDateTime): -1 | 0 | 1;
        toString(): string;
    }

    /** A wall-clock time of day; one integer millisecond count since midnight. */
    class PlainTime {
        constructor(hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        readonly msSinceMidnight: number;
        /** Wrap at midnight; a duration in months is refused. */
        add(duration: Duration): PlainTime;
        subtract(duration: Duration): PlainTime;
        compare(other: PlainTime): -1 | 0 | 1;
        toString(): string;
    }

    /** RFC 5545 recurrence rules, UTC whole-second unix time. */
    interface RRuleOptions {
        freq: "YEARLY" | "MONTHLY" | "WEEKLY" | "DAILY" | "HOURLY" | "MINUTELY" | "SECONDLY";
        interval?: number;
        count?: number;
        until?: Date | string | number;
        dtstart?: Date | string | number;
        wkst?: number | string;
        bymonth?: number[];
        bymonthday?: number[];
        byyearday?: number[];
        byweekno?: number[];
        bysetpos?: number[];
        byweekday?: (string | number)[];
    }
    class RRule {
        constructor(opts: RRuleOptions);
        /** Parses "RRULE:FREQ=..." parts plus optional DTSTART: lines. */
        static fromString(str: string, opts?: { dtstart?: Date | string | number }): RRule;
        /** Every occurrence as Dates; an uncounted infinite rule refuses. */
        all(limit?: number): Date[];
        /** Occurrences in the window; inc makes both ends inclusive. */
        between(start: Date | number, end: Date | number, inc?: boolean): Date[];
        /** The first occurrence strictly after fromDate, or null. */
        next(fromDate?: Date | number): Date | null;
        /** The last occurrence strictly before fromDate. */
        prev(fromDate?: Date | number): Date | null;
        /** The rule back in RFC 5545 text form. */
        toString(): string;
    }

    /** Natural-language date parsing per locale. */
    class DateParser {
        constructor(locale?: string, opts?: { now?: number });
        /** Unix seconds, or null when nothing matches. */
        parse(text: string): number | null;
        readonly locale: string;
        readonly dayFirst: boolean;
    }

    /** Strict ISO parse of "YYYY-MM-DD". */
    function parseDate(text: string): PlainDate;
    function dateFromEpochDay(n: number): PlainDate;
    /** Strict parse of "HH:MM[:SS[.mmm]]". */
    function parseTime(text: string): PlainTime;
}

/* ================================================================== *
 *  dyna:uring
 * ================================================================== */
declare module "dyna:uring" {
    /** Whole file as a string via the io_uring bulk reader (Linux only). */
    function readFile(path: import("dyna:file").Path): string;
    /** Whole file as a string via the blocking pread(2) reference reader. */
    function readFileSync(path: import("dyna:file").Path): string;
    /** Reads the whole file and returns its byte count and a 32-bit FNV-1a rolling checksum; useUring selects the reader (default true). */
    function checksum(path: import("dyna:file").Path, useUring?: boolean): { bytes: number; sum: number };
}

/* ================================================================== *
 *  dyna:url
 * ================================================================== */
declare module "dyna:url" {
    /** WHATWG-style URL parsing. */
    class URL {
        constructor(input: string, base?: string);
        readonly href: string;
        readonly protocol: string;
        readonly username: string;
        readonly password: string;
        readonly host: string;
        readonly hostname: string;
        readonly port: string;
        readonly pathname: string;
        readonly search: string;
        readonly hash: string;
        readonly origin: string;
        toJSON(): string;
        toString(): string;
    }
    /** IDNA 2008 (UTS #46) mapping; options.transitional selects transitional processing. */
    function domainToASCII(domain: string, options?: { transitional?: boolean }): string;
    function domainToUnicode(domain: string, options?: { transitional?: boolean }): string;
    /** RFC 3492 encoding; input over 1024 code points is refused. */
    function punycodeEncode(text: string): string;
    function punycodeDecode(text: string): string;
    /** Percent-encodes the object's own enumerable string keys into a=1&b=2. */
    function formEncode(obj: Record<string, unknown>): string;
    /** Decodes `+` as space, keeps the LAST value per key. */
    function formDecode(text: string): Record<string, string>;
    /** encodeURIComponent plus !'()~. */
    function encodeURIComponentStrict(text: string): string;
}

/* ================================================================== *
 *  dyna:uuid
 * ================================================================== */
declare module "dyna:uuid" {
    /** Random version-4 UUID. */
    function v4(): string;
    /** Time-ordered version-7 UUID, monotonic within a process. */
    function v7(): string;
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
    /** URL-safe ID over the default 64-symbol alphabet; size 1..4096. */
    function NanoID(size?: number): string;
    /** The same generator over a caller-supplied alphabet of 2..256 ASCII symbols. */
    function NanoIDAlphabet(alphabet: string, size?: number): string;
    /** A 26-character Crockford base32 ULID; atMillis must fit 48 bits. */
    function ULID(atMillis?: number): string;
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

/* ================================================================== *
 *  dyna:validate
 * ================================================================== */
declare module "dyna:validate" {
    /** ASCII letters only. */
    function IsAlpha(text: string): boolean;
    /** ASCII letters and digits. */
    function IsAlphanumeric(text: string): boolean;
    /** Every byte below 0x80. */
    function IsAscii(text: string): boolean;
    /** The practical email grammar; quoted strings and comments are refused. */
    function IsEmail(text: string): boolean;
    /** Luhn check digit over 12..19 digits. */
    function IsCreditCard(text: string): boolean;
    /** Country-length check plus mod-97 over the rearranged digits. */
    function IsIBAN(text: string): boolean;
    /** RFC 1035 label grammar; an IP literal is not a domain. */
    function IsDomain(text: string): boolean;
    /** The dyna:url constructor accepts it, with a non-empty host for special schemes. */
    function IsURL(text: string): boolean;
    /** Lowercase letters, digits, single hyphens; at most 64 chars. */
    function IsSlug(text: string): boolean;
    /** The RFC 4122 canonical 8-4-4-4-12 form only. */
    function IsUUID(text: string): boolean;
    /** JWS Compact Serialization with a JSON header naming an alg. */
    function IsJWT(text: string): boolean;
    /** The dyna:semver parser accepts it. */
    function IsSemver(text: string): boolean;
    /** ITU-T E.164: optional `+` and at most 15 digits. */
    function IsE164(text: string): boolean;
}

/* ================================================================== *
 *  dyna:xml
 * ================================================================== */
declare module "dyna:xml" {
    /** A document-tree element node. */
    interface XMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | XMLElement)[];
    }

    /** Parses a document; options trim (default true) and entities "strict"|"keep". */
    function XMLParse(text: string, opts?: { trim?: boolean; entities?: "strict" | "keep" }): XMLElement;
    /** Serializes a node; indent 0..16 spaces; nesting beyond 256 throws. */
    function XMLStringify(node: XMLElement, opts?: { indent?: number }): string;
    /** Collapses an element into a plain object keyed by element name. */
    function XMLToObject(node: XMLElement): Record<string, unknown>;

    /** Streaming SAX handlers. */
    interface SAXHandlers {
        onOpen?: (name: string, attrs: Record<string, string>) => void;
        onClose?: (name: string) => void;
        onText?: (text: string) => void;
        onCData?: (text: string) => void;
        onComment?: (text: string) => void;
        onPI?: (target: string, data: string) => void;
    }
    /** Streaming SAX parser; a token interrupted by a chunk boundary resumes. */
    class SAXParser {
        constructor(handlers: SAXHandlers);
        /** Feeds a string or any byte view. */
        write(chunk: BytesInput): void;
        /** Finalizes the stream; trailing content throws. */
        end(): void;
    }
}

/* ================================================================== *
 *  dyna:yaml
 * ================================================================== */
declare module "dyna:yaml" {
    /** Parses exactly one document; multi-document input is refused. */
    function Parse(text: string): unknown;
    /** Parses every `---`-separated document into an array. */
    function ParseAll(text: string): unknown[];
    /** Serializes a value as a YAML document; indent 1..10 spaces. */
    function Stringify(value: unknown, opts?: { indent?: number }): string;
}
/* ================================================================== *
 *  dyna:dataframe
 * ================================================================== */
declare module "dyna:dataframe" {
    /** Columnar tables over TypedArrays; string columns are dictionary-encoded. */
    interface DataFrameColumn {
        name: string;
        type: string;
    }
    interface GroupResult {
        keys: (string | number)[];
        values: Float64Array;
    }
    interface GroupArrays {
        keys: (string | number)[];
        values: Float64Array[];
    }
    interface DataFrame {
        /** Row count. */
        readonly ROWS: number;
        /** Column count. */
        readonly COLS: number;
        /** Column names in column order. */
        readonly COLUMNS: string[];
        /** Column name to a short type tag: f64 f32 i32 u32 i16 u16 i8 u8 str. */
        DTYPES(): Record<string, string>;
        /** One {name, type} entry per column, in order. */
        SCHEMA(): DataFrameColumn[];
        /** {rows, cols, dtypes, bytes, total_bytes}. */
        INFO(): { rows: number; cols: number; dtypes: Record<string, string>; bytes: Record<string, number>; total_bytes: number };
        /** {columns: {name: bytes}, total}. */
        MEMORY_USAGE(): { columns: Record<string, number>; total: number };
        /** Column name to a fresh TypedArray copy or array of strings. */
        TO_COLUMNS(): Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | string[]>;
        /** One object per row. */
        TO_RECORDS(): Record<string, number | string>[];
        /** The TO_RECORDS array serialised; NaN and Infinity become null. */
        TO_JSON(): string;
        /** Header row plus one row per frame row, RFC 4180 quoting. */
        TO_CSV(): string;
        /** Builds a frame from an array of row objects. */
        FROM_RECORDS(rows: Record<string, unknown>[]): DataFrame;
        /** A fresh frame whose columns are exact copies. */
        COPY(): DataFrame;
        /** The named columns in the order given. */
        SELECT(names: string[]): DataFrame;
        /** The complement, in column order. */
        DROP_COLUMNS(names: string[]): DataFrame;
        /** Renames columns per {old: new}. */
        RENAME(map: Record<string, string>): DataFrame;
        /** The rows where the ROWS-byte mask is nonzero. */
        FILTER(mask: Uint8Array): DataFrame;
        /** Rows [start, end), clamped and negative-indexed like Array.prototype.slice. */
        SLICE(start: number, end?: number): DataFrame;
        /** n rows without replacement via a Fisher-Yates partial shuffle. */
        SAMPLE(n: number, seed?: number): DataFrame;
        /** 1 where the column's value is in values. */
        ISIN(col: string, values: (number | string)[]): Uint8Array;
        /** The same shape; rows where mask is 0 become fill. */
        MASK(mask: Uint8Array, fill?: number | string): DataFrame;

        /** Reductions over (col[, mask]); masked-out rows do not contribute. */
        SUM(col: string, mask?: Uint8Array): number;
        MIN(col: string, mask?: Uint8Array): number | undefined;
        MAX(col: string, mask?: Uint8Array): number | undefined;
        MEAN(col: string, mask?: Uint8Array): number;
        COUNT(col: string, mask?: Uint8Array): number;
        PRODUCT(col: string, mask?: Uint8Array): number;
        DOT_PRODUCT(a: string, b: string, mask?: Uint8Array): number;
        VARIANCE(col: string, mask?: Uint8Array): number;
        STDDEV(col: string, mask?: Uint8Array): number;
        VARIANCE_POP(col: string, mask?: Uint8Array): number;
        STDDEV_POP(col: string, mask?: Uint8Array): number;
        SKEW(col: string, mask?: Uint8Array): number;
        KURTOSIS(col: string, mask?: Uint8Array): number;
        SKEW_SAMP(col: string, mask?: Uint8Array): number;
        KURT_SAMP(col: string, mask?: Uint8Array): number;
        SEM(col: string, mask?: Uint8Array): number;
        COUNT_NULLS(col: string, mask?: Uint8Array): number;
        MEAN_WEIGHTED(valueCol: string, weightCol: string, mask?: Uint8Array): number;
        /** Exact integer sum; throws RangeError when the total overflows a Number. */
        SUM_CHECKED(col: string, mask?: Uint8Array): number;
        /** {count, sum, mean, min, max, variance, stddev, skew, kurtosis} in one pass. */
        DESCRIBE(col: string, mask?: Uint8Array): { count: number; sum: number; mean: number; min: number; max: number; variance: number; stddev: number; skew: number; kurtosis: number };
        /** Shannon entropy in bits over the empirical value distribution. */
        ENTROPY(col: string, mask?: Uint8Array): number;
        MAD(col: string, mask?: Uint8Array): number | undefined;
        MEDIAN_ABSOLUTE_DEVIATION(col: string, mask?: Uint8Array): number | undefined;

        /** Bitwise folds over integer columns; empty-selection identities as documented. */
        BITWISE_AND(col: string, mask?: Uint8Array): number;
        BITWISE_OR(col: string, mask?: Uint8Array): number;
        BITWISE_XOR(col: string, mask?: Uint8Array): number;
        GROUP_BIT_AND(key: string, val: string, mask?: Uint8Array): GroupResult;
        GROUP_BIT_OR(key: string, val: string, mask?: Uint8Array): GroupResult;
        GROUP_BIT_XOR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The count of DISTINCT non-negative integer values, one bit per value. */
        GROUP_BITMAP(col: string, mask?: Uint8Array): number;

        /** Positional access; n defaults to 5 and is clamped to the frame. */
        HEAD(col: string, n?: number, mask?: Uint8Array): Float64Array;
        TAIL(col: string, n?: number, mask?: Uint8Array): Float64Array;
        FIRST(col: string, mask?: Uint8Array): number | undefined;
        LAST(col: string, mask?: Uint8Array): number | undefined;
        ARG_MIN(col: string, mask?: Uint8Array): number | undefined;
        ARG_MAX(col: string, mask?: Uint8Array): number | undefined;

        /** Mask-producing comparisons over a column. */
        GT(col: string, value: number): Uint8Array;
        GE(col: string, value: number): Uint8Array;
        LT(col: string, value: number): Uint8Array;
        LE(col: string, value: number): Uint8Array;
        EQ(col: string, value: number | string): Uint8Array;
        NE(col: string, value: number | string): Uint8Array;
        /** 1 where lo <= col[i] <= hi, inclusive at both ends. */
        BETWEEN(col: string, lo: number, hi: number): Uint8Array;
        IS_NA(col: string): Uint8Array;
        NOT_NA(col: string): Uint8Array;
        /** True when every mask byte is nonzero. */
        ALL(mask: Uint8Array): boolean;
        ANY(mask: Uint8Array): boolean;
        /** The mask packed into ceil(ROWS/32) words, LSB first. */
        BITMASK(mask: Uint8Array): Uint32Array;
        BOOL_AND(col: string, mask?: Uint8Array): boolean;
        BOOL_OR(col: string, mask?: Uint8Array): boolean;
        BOOL_XOR(col: string, mask?: Uint8Array): boolean;
        /** 1 on the FIRST occurrence of each distinct value. */
        DROP_DUPLICATES(col: string, mask?: Uint8Array): Uint8Array;
        /** 1 where none of the named columns is NaN; no arguments: every numeric column. */
        DROP_NA(...cols: string[]): Uint8Array;

        /** Elementwise verbs returning a Float64Array of ROWS entries. */
        ABS(col: string): Float64Array;
        ROUND(col: string): Float64Array;
        FLOOR(col: string): Float64Array;
        CEIL(col: string): Float64Array;
        SQRT(col: string): Float64Array;
        LOG(col: string): Float64Array;
        EXP(col: string): Float64Array;
        SIGN(col: string): Float64Array;
        CLIP(col: string, lo: number, hi: number): Float64Array;
        FILL_NA(col: string, value: number): Float64Array;
        ADD(col: string, x: number | string): Float64Array;
        SUB(col: string, x: number | string): Float64Array;
        MUL(col: string, x: number | string): Float64Array;
        DIV(col: string, x: number | string): Float64Array;
        POW(col: string, x: number | string): Float64Array;
        /** k - col; number-only operand. */
        RSUB(col: string, k: number): Float64Array;
        /** k / col; number-only operand. */
        RDIV(col: string, k: number): Float64Array;
        /** a where the mask byte is nonzero, else b. */
        WHERE(mask: Uint8Array, a: string | number, b: string | number): Float64Array;

        /** Grouped verbs; the key column must be integer or string. */
        GROUP_BY_SUM(key: string, val: string, mask?: Uint8Array): GroupResult;
        GROUP_BY_MEAN(key: string, val: string, mask?: Uint8Array): GroupResult;
        GROUP_BY_MIN(key: string, val: string, mask?: Uint8Array): GroupResult;
        GROUP_BY_MAX(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Rows per group; takes no value column. */
        GROUP_BY_COUNT(key: string, mask?: Uint8Array): GroupResult;
        SUM_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        MIN_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        MAX_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        GROUP_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        GROUP_UNIQ_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        GROUP_ARRAY_MOVING_SUM(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        GROUP_ARRAY_MOVING_AVG(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        GROUP_ARRAY_SORTED(key: string, val: string, mask?: Uint8Array): GroupArrays;
        GROUP_ARRAY_LAST(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        GROUP_ARRAY_SAMPLE(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The values present in EVERY group. */
        GROUP_ARRAY_INTERSECT(key: string, val: string, mask?: Uint8Array): Float64Array;
        /** A dense array of size slots; later rows overwrite earlier ones. */
        GROUP_ARRAY_INSERT_AT(value: string, position: string, size: number, fill?: number, mask?: Uint8Array): Float64Array;
        GROUP_CONCAT(col: string, sep?: string, mask?: Uint8Array): string;
        JSON_AGG(key: string, value: string, mask?: Uint8Array): string;
        JSON_OBJECT_AGG(key: string, value: string, mask?: Uint8Array): string;
        JSON_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;
        JSON_OBJECT_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;

        /** Ordering: sorting, ranking, frequency. NaN sorts last. */
        SORT(col: string, mask?: Uint8Array): Float64Array;
        ARG_SORT(col: string, mask?: Uint8Array): Uint32Array;
        /** Average ranks; ties share the mean of their positions. */
        RANK(col: string, mask?: Uint8Array): Float64Array;
        /** Ranks counting distinct values. */
        DENSE_RANK(col: string, mask?: Uint8Array): Float64Array;
        PERCENT_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** SQL NTILE; first n % buckets tiles take one extra row. */
        NTILE(col: string, buckets: number, mask?: Uint8Array): Float64Array;
        N_LARGEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        N_SMALLEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** Distinct values in first-seen order; strings for a string column. */
        UNIQUE(col: string, mask?: Uint8Array): Float64Array | string[];
        N_UNIQUE(col: string, mask?: Uint8Array): number;
        /** Exact distinct count, or n+1 meaning "more than n". */
        UNIQ_UP_TO(col: string, n: number, mask?: Uint8Array): number;
        VALUE_COUNTS(col: string, mask?: Uint8Array): GroupResult;
        TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** The most frequent value; ties go to the first in row order. */
        MODE(col: string, mask?: Uint8Array): number | string | undefined;
        APPROX_COUNT_DISTINCT(col: string, mask?: Uint8Array): number;
        APPROX_TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** Ranks by summed weight, not count. */
        APPROX_TOP_SUM(col: string, weightCol: string, k: number, mask?: Uint8Array): GroupResult;
        TOP_K_WEIGHTED(col: string, weightCol: string | undefined, k: number, mask?: Uint8Array): GroupResult;
        /** The value holding strictly more than half the total weight. */
        ANY_HEAVY(col: string, weightCol?: string, mask?: Uint8Array): number | undefined;
        /** MinHash Jaccard estimate between two columns. */
        APPROX_SIMILARITY(a: string, b: string, mask?: Uint8Array): number;

        /** Quantiles: select rather than sort. */
        QUANTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        PERCENTILE_CONT(col: string, q: number, mask?: Uint8Array): number | undefined;
        PERCENTILE_DISC(col: string, q: number, mask?: Uint8Array): number | undefined;
        MEDIAN(col: string, mask?: Uint8Array): number | undefined;
        QUANTILE_EXACT_LOW(col: string, q: number, mask?: Uint8Array): number | undefined;
        QUANTILE_EXACT_HIGH(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Many interpolating quantiles from ONE gather. */
        QUANTILES(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** Many approximate quantiles off ONE t-digest. */
        QUANTILES_TDIGEST(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        APPROX_PERCENTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        QUANTILE_EXACT_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        QUANTILE_TDIGEST_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** Equal-width bins over the observed range; edges has bins+1 entries. */
        HISTOGRAM(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };
        HISTOGRAM_NORMALIZED(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };

        /** Scans: windowed and sequential verbs over exactly ROWS entries. */
        CUM_SUM(col: string, mask?: Uint8Array): Float64Array;
        CUM_PROD(col: string, mask?: Uint8Array): Float64Array;
        CUM_MAX(col: string, mask?: Uint8Array): Float64Array;
        CUM_MIN(col: string, mask?: Uint8Array): Float64Array;
        /** out[i] = col[i - periods]; the vacated head/tail is NaN. */
        SHIFT(col: string, periods?: number): Float64Array;
        DIFF(col: string, periods?: number): Float64Array;
        ROLLING_SUM(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MEAN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MIN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MAX(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_VAR(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_STD(col: string, w: number, mask?: Uint8Array): Float64Array;
        /** Exponential moving average; alpha in (0, 1]. */
        EMA(col: string, alpha: number, mask?: Uint8Array): Float64Array;
        PCT_CHANGE(col: string, periods?: number, mask?: Uint8Array): Float64Array;
        ZSCORE(col: string, mask?: Uint8Array): Float64Array;
        /** Sum of positive consecutive differences. */
        DELTA_SUM(col: string, mask?: Uint8Array): number;
        /** DELTA_SUM in timestamp order, not row order. */
        DELTA_SUM_TIMESTAMP(valueCol: string, timeCol: string, mask?: Uint8Array): number;

        /** Pairwise statistics; regression verbs take (y, x). */
        COV_POP(a: string, b: string, mask?: Uint8Array): number;
        COV_SAMP(a: string, b: string, mask?: Uint8Array): number;
        CORR(a: string, b: string, mask?: Uint8Array): number;
        REGR_SLOPE(y: string, x: string, mask?: Uint8Array): number;
        REGR_INTERCEPT(y: string, x: string, mask?: Uint8Array): number;
        REGR_R2(y: string, x: string, mask?: Uint8Array): number;
        REGR_AVG_X(y: string, x: string, mask?: Uint8Array): number;
        REGR_AVG_Y(y: string, x: string, mask?: Uint8Array): number;
        REGR_COUNT(x: string, y: string, mask?: Uint8Array): number;
        REGR_SXX(y: string, x: string, mask?: Uint8Array): number;
        REGR_SYY(y: string, x: string, mask?: Uint8Array): number;
        REGR_SXY(y: string, x: string, mask?: Uint8Array): number;
        /** Spearman: Pearson over the average ranks. */
        RANK_CORR(x: string, y: string, mask?: Uint8Array): number;
        CORR_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        COV_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** Change per unit time across the WHOLE selection. */
        RATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The most recent interval only. */
        IRATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The slope joining the leftmost and rightmost points by x value. */
        BOUNDING_RATIO(x: string, y: string, mask?: Uint8Array): number;
        EXPONENTIAL_TIME_DECAYED_AVG(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        EXPONENTIAL_TIME_DECAYED_SUM(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        EXPONENTIAL_TIME_DECAYED_COUNT(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        EXPONENTIAL_TIME_DECAYED_MAX(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** Half-open [lo, hi) ranges merged into their union. */
        RANGE_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { starts: Float64Array; ends: Float64Array };
        /** The interval common to ALL ranges. */
        RANGE_INTERSECT_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { start: number; end: number } | undefined;

        /** Reshape; produced frames carry fresh copies of the data. */
        JOIN(other: DataFrame, leftKey: string, rightKey: string, how?: "inner" | "left" | "right" | "outer"): DataFrame;
        ASOF_JOIN(other: DataFrame, leftTime: string, rightTime: string): DataFrame;
        CONCAT(other: DataFrame): DataFrame;
        /** Buckets a sorted numeric time column; agg sum|mean|min|max|count. */
        RESAMPLE(timeCol: string, interval: number, agg?: "sum" | "mean" | "min" | "max" | "count"): DataFrame;
        /** One row per distinct index value, one column per distinct key value. */
        PIVOT(index: string, columns: string, values: string, agg?: string): DataFrame;
        /** Long form: each (row, valueVar) pair becomes one output row. */
        MELT(idVars: string[], valueVars: string[]): DataFrame;
    }

    /** Builds a frame from an object mapping column name to a TypedArray or string array. */
    const DataFrame: {
        new (columns: Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | string[]>): DataFrame;
    };
}
/* ================================================================== *
 *  WHATWG globals, std/os modules, and core prototype extensions
 * ================================================================== */

/* ---- std / os modules (available with --std) ---------------------- */

declare module "std" {
    interface StdFile {
        readonly eof: boolean;
        readonly error: boolean;
        readByte(): number;
        writeByte(b: number): void;
        readBytes(max: number): Uint8Array;
        readAsString(max: number): string;
        writeBytes(bytes: Uint8Array): void;
        writeStr(str: string): void;
        close(): void;
        seek(offset: number, whence: number): void;
        tell(): number;
    }
    const __stdin: StdFile;
    const __stdout: StdFile;
    const __stderr: StdFile;
    export { __stdin as in, __stdout as out, __stderr as err };
    /** Format and print to stdout. */
    function printf(fmt: string, ...args: unknown[]): void;
    /** Format to a string. */
    function sprintf(fmt: string, ...args: unknown[]): string;
    function puts(str: string): void;
    function getenv(name: string): string | undefined;
    function setenv(name: string, value: string): void;
    function unsetenv(name: string): void;
    /** "name=value" entries of the whole environment. */
    function getenviron(): string[];
    function exit(code?: number): void;
    function gc(): void;
    /** Evaluates the given script in global scope. */
    function evalScript(script: string, options?: unknown): unknown;
    /** Loads a script file and evaluates it. */
    function loadScript(filename: string): unknown;
    /** Reads a whole file as a string, or null when it cannot be read. */
    function loadFile(filename: string): string | null;
    /** Opens a file; mode "r", "w", "a", "r+", ... */
    function open(filename: string, mode: string, error?: Error): StdFile | null;
    /** Opens a pipe to a command. */
    function popen(command: string, mode: string, error?: Error): StdFile | null;
    function fdopen(fd: number, mode: string, error?: Error): StdFile | null;
    function tmpfile(): StdFile;
    /** Fetches a URL; timeout in ms. */
    function urlGet(url: string, options?: { binary?: boolean; full?: boolean; headers?: Record<string, string>; maxSize?: number; timeout?: number }): unknown;
    /** strerror(errno). */
    function strerror(errno: number): string;
    /** JSON parse with extensions (Date serialization, ...). */
    function parseExtJSON(str: string): unknown;
    /** The standard Error constructor. */
    const Error: ErrorConstructor;
    const SEEK_SET: number;
    const SEEK_CUR: number;
    const SEEK_END: number;
}

declare module "os" {
    /** The number of milliseconds since an arbitrary point. */
    function now(): number;
    function platform(): string;
    function getpid(): number;
    function getcwd(): string;
    function chdir(path: string): void;
    interface OsStat {
        dev: number;
        ino: number;
        mode: number;
        nlink: number;
        uid: number;
        gid: number;
        rdev: number;
        size: number;
        blocks: number;
        atime: number;
        mtime: number;
        ctime: number;
    }
    function stat(path: string): OsStat | null;
    function lstat(path: string): OsStat | null;
    function readdir(path: string): [string[], number];
    function readlink(path: string): [string, number];
    function realpath(path: string): [string, number];
    function rename(oldpath: string, newpath: string): number;
    function remove(path: string): number;
    function mkdir(path: string, mode?: number): number;
    function symlink(target: string, linkpath: string): number;
    function utimes(path: string, atime: number, mtime: number): number;
    function open(path: string, flags: number, mode?: number): [number, number];
    function close(fd: number): number;
    function read(fd: number, buffer: Uint8Array, offset: number, length: number, position?: number): [number, number];
    function write(fd: number, buffer: Uint8Array | string, offset?: number, length?: number, position?: number): [number, number];
    function seek(fd: number, position: number, whence: number): [number, number];
    function dup(fd: number): [number, number];
    function dup2(oldfd: number, newfd: number): [number, number];
    function pipe(): [number, number, number] | number;
    function isatty(fd: number): boolean;
    function ttySetRaw(fd: number, raw: boolean): void;
    function ttyGetWinSize(fd: number): [number, number] | null;
    interface ExecResult {
        status: number;
        signal: number;
        data?: Uint8Array;
        errCode: number;
        error: string;
    }
    function exec(args: string[], options?: { blocking?: boolean; usePath?: boolean; file?: string; cwd?: string; stdin?: unknown; stdout?: unknown; stderr?: unknown; env?: Record<string, string>; uid?: number; gid?: number }): ExecResult | null;
    function waitpid(pid: number, options?: number): [number, number];
    function kill(pid: number, sig: number): number;
    function signal(signal: number, handler: (signal: number) => void): void;
    function sleep(delay: number): void;
    function sleepAsync(delay: number): Promise<void>;
    function setTimeout(cb: (...args: unknown[]) => void, delay: number, ...args: unknown[]): number;
    function clearTimeout(id: number): void;
    function setInterval(cb: (...args: unknown[]) => void, delay: number, ...args: unknown[]): number;
    function clearInterval(id: number): void;
    function setReadHandler(fd: number, cb: (fd: number) => void): void;
    function setWriteHandler(fd: number, cb: (fd: number) => void): void;
    const O_RDONLY: number;
    const O_WRONLY: number;
    const O_RDWR: number;
    const O_ACCMODE: number;
    const O_APPEND: number;
    const O_CREAT: number;
    const O_EXCL: number;
    const O_TRUNC: number;
    const O_BINARY: number;
    const WNOHANG: number;
    const SIGABRT: number;
    const SIGALRM: number;
    const SIGCHLD: number;
    const SIGCONT: number;
    const SIGFPE: number;
    const SIGHUP: number;
    const SIGILL: number;
    const SIGINT: number;
    const SIGPIPE: number;
    const SIGQUIT: number;
    const SIGSEGV: number;
    const SIGSTOP: number;
    const SIGTERM: number;
    const SIGTSTP: number;
    const SIGTTIN: number;
    const SIGTTOU: number;
    const SIGUSR1: number;
    const SIGUSR2: number;
    const S_IFMT: number;
    const S_IFBLK: number;
    const S_IFCHR: number;
    const S_IFDIR: number;
    const S_IFIFO: number;
    const S_IFLNK: number;
    const S_IFREG: number;
    const S_IFSOCK: number;
    const S_ISUID: number;
    const S_ISGID: number;
    const Worker: {
        new (script: string, options?: unknown): Worker;
    };
    interface Worker {
        postMessage(value: unknown): void;
        onmessage?: (ev: { data: unknown }) => void;
        onerror?: (err: unknown) => void;
        terminate(): void;
    }
}

/* ---- WHATWG globals the engine ships ----------------------------- */

/** Identity for `using` disposal. */
interface SymbolConstructor {
    readonly dispose: symbol;
}

interface AbortSignal {
    readonly aborted: boolean;
    readonly reason: unknown;
    onabort: ((this: AbortSignal, ev: { type: string; target: AbortSignal }) => void) | null;
    addEventListener(type: "abort", listener: (ev: { type: string; target: AbortSignal }) => void): void;
    removeEventListener(type: "abort", listener: (ev: { type: string; target: AbortSignal }) => void): void;
    throwIfAborted(): void;
    /** Internal: fires the abort. */
    _abort(reason?: unknown): void;
}
interface AbortSignalConstructor {
    new (): AbortSignal;
    abort(reason?: unknown): AbortSignal;
    timeout(delayMs: number): AbortSignal;
}
declare const AbortSignal: AbortSignalConstructor;

interface AbortController {
    readonly signal: AbortSignal;
    abort(reason?: unknown): void;
}
interface AbortControllerConstructor {
    new (): AbortController;
}
declare const AbortController: AbortControllerConstructor;

type HeadersInit = Headers | string[][] | Record<string, string>;
interface Headers {
    append(name: string, value: string): void;
    delete(name: string): void;
    get(name: string): string | null;
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

interface FormData {
    append(name: string, value: string | Uint8Array | ArrayBuffer, filename?: string): void;
    delete(name: string): void;
    get(name: string): string | Uint8Array | ArrayBuffer | null;
    getAll(name: string): (string | Uint8Array | ArrayBuffer)[];
    has(name: string): boolean;
    set(name: string, value: string | Uint8Array | ArrayBuffer, filename?: string): void;
    forEach(cb: (value: string | Uint8Array | ArrayBuffer, key: string, parent: FormData) => void, thisArg?: unknown): void;
    keys(): IterableIterator<string>;
    values(): IterableIterator<string | Uint8Array | ArrayBuffer>;
    entries(): IterableIterator<[string, string | Uint8Array | ArrayBuffer]>;
    [Symbol.iterator](): IterableIterator<[string, string | Uint8Array | ArrayBuffer]>;
}
interface FormDataConstructor {
    new (): FormData;
}
declare const FormData: FormDataConstructor;

interface RequestInit {
    method?: string;
    headers?: HeadersInit;
    body?: unknown;
    signal?: AbortSignal | null;
    timeout?: number;
}
interface Request {
    readonly method: string;
    readonly url: string;
    readonly headers: Headers;
    readonly signal: AbortSignal | null;
    text(): Promise<string>;
    json(): Promise<unknown>;
    bytes(): Promise<Uint8Array>;
    arrayBuffer(): Promise<ArrayBuffer>;
}
interface RequestConstructor {
    new (input: string | Request, init?: RequestInit): Request;
}
declare const Request: RequestConstructor;

interface ResponseInit {
    status?: number;
    statusText?: string;
    headers?: HeadersInit;
    url?: string;
}
interface Response {
    readonly status: number;
    readonly statusText: string;
    readonly ok: boolean;
    readonly headers: Headers;
    readonly url: string;
    readonly bodyUsed: boolean;
    text(): Promise<string>;
    json(): Promise<unknown>;
    bytes(): Promise<Uint8Array>;
    arrayBuffer(): Promise<ArrayBuffer>;
    clone(): Response;
}
interface ResponseConstructor {
    new (body?: unknown, init?: ResponseInit): Response;
}
declare const Response: ResponseConstructor;

declare const fetch: {
    (input: string | Request, init?: RequestInit): Promise<Response>;
};

declare class TextEncoder {
    constructor();
    readonly encoding: string;
    encode(input?: string): Uint8Array;
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number };
}
declare class TextDecoder {
    constructor(encoding?: string, opts?: { fatal?: boolean; ignoreBOM?: boolean });
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    decode(buffer?: Uint8Array | ArrayBuffer): string;
}

interface Performance {
    now(): number;
}
declare const performance: Performance;

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

/** Standard timers; return a numeric id. */
declare function setTimeout(cb: (...args: unknown[]) => void, ms?: number, ...args: unknown[]): number;
declare function clearTimeout(id: number): void;
declare function setInterval(cb: (...args: unknown[]) => void, ms?: number, ...args: unknown[]): number;
declare function clearInterval(id: number): void;

/* ---- core prototype extensions (project additions) --------------- */

interface String {
    /** A lazily-computed helper value. */
    lazy: unknown;
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
    /** Case-insensitive equality. */
    equalsIgnoreCase(other: string): boolean;
    /** Byte-wise comparison, -1, 0, or 1. */
    compareBytes(other: string): number;
    /** Splits into at most n pieces. */
    splitN(sep: string | RegExp, n: number): string[];
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
    /** Escapes HTML-significant characters. */
    escapeHTML(): string;
    /** Unescapes HTML entities. */
    unescapeHTML(): string;
    /** Strips HTML tags. */
    stripTags(): string;
    /** The number of occurrences of `sub`. */
    count(sub: string): number;
    /** Parses the string as a number in base `base` (2..36, default 10), or NaN. */
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
    /** Brace-placeholder formatting: {0}/{name} from args (positional or one object); {{ }} escapes a literal brace. */
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
    /** Percent-decodes (decodeURIComponent; decodeURI when `param` — asymmetric to escapeURL). */
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

interface Array<T> {
    /** True when the array is empty. */
    isEmpty(): boolean;
    /** The first element, or undefined. */
    first(): T | undefined;
    /** The last element, or undefined. */
    last(): T | undefined;
    /** The sum of numeric elements. */
    sum(): number;
    /** The mean of numeric elements. */
    average(): number;
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
    /** Everything after the first n elements. */
    drop(n: number): T[];
    /** The last n elements. */
    takeLast(n: number): T[];
    /** Everything except the last n elements. */
    dropLast(n: number): T[];
    /** A stable sort by a key function. */
    sortBy<K>(key: (v: T) => K): T[];
    /** The insertion index for a sorted array; `comparator` must match the array's sort order. */
    sortedIndexOf(value: T, comparator?: (a: T, b: T) => number): number;
    /** Groups elements by a key function into a Record. */
    groupBy<K extends string | number>(key: (v: T) => K): Record<K, T[]>;
    /** A fresh array in random order. */
    shuffle(): T[];
    /** n random elements without replacement. */
    sample(n?: number): T[];
    /** The distinct elements, first occurrence kept; `map` picks the dedup key. */
    unique(map?: ((v: T) => unknown) | keyof T): T[];
    uniq(map?: ((v: T) => unknown) | keyof T): T[];
    /** The distinct elements by a key function. */
    uniqBy<K>(key: (v: T) => K): T[];
    /** The elements present in both arrays. */
    intersect(other: T[]): T[];
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
    zipWith<U, R>(other: U[], fn: (a: T, b: U) => R): R[];
    /** Inserts `v` between every pair of elements. */
    intersperse(v: T): T[];
    /** Flattens one level. */
    flatten(): T extends unknown[] ? T[number][] : T[];
    /** The matrix transpose (array of arrays). */
    transpose(): T[][];
    /** The Cartesian product with another array. */
    xprod<U>(other: U[]): [T, U][];
    /** Sliding windows of size n. */
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
    /** The first element (alias of first). */
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
    /** Removes the first occurrence of `value`. */
    remove(value: T): T[];
    /** Removes every occurrence of `value`. */
    exclude(value: T): T[];
    /** Removes the [from, to) range. */
    removeRange(from: number, to: number): T[];
    /** Splits at the first index where pred changes. */
    splitWhen(pred: (v: T) => boolean): [T[], T[]];
    /** Keeps the elements for which `pred(element, y)` holds for some y in `other`. */
    innerJoin(other: T[], pred: (a: T, b: T) => boolean): T[];
    /** True when the array starts with the given prefix. */
    startsWith(prefix: T[]): boolean;
    endsWith(suffix: T[]): boolean;
    /** Flattens nested arrays one level (alias of flatten). */
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
    reduceBy<K extends string | number, R>(key: (v: T) => K, fn: (acc: R, v: T) => R, seed: R): Record<K, R>;
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
    /** A lazily-computed helper value. */
    lazy: unknown;
}

interface ArrayConstructor {
    /** Repeats `value` n times. */
    repeat<T>(value: T, n: number): T[];
    /** Creates an array from an async iterable. */
    fromAsync<T>(iterable: AsyncIterable<T> | Iterable<T | PromiseLike<T>>): Promise<T[]>;
}

interface Map<K, V> {
    /** The stored value for `key`, inserting `value` (default undefined) when absent. */
    getOrInsert(key: K, value?: V): V;
    /** The stored value for `key`, inserting `fn(key)` when absent. */
    getOrInsertComputed(key: K, fn: (key: K) => V): V;
}

interface SetConstructor {
    /** Groups the iterable into a Map of key -> values array in input order. */
    groupBy<K, T>(items: Iterable<T>, fn: (value: T) => K): Map<K, T[]>;
}

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
    /** Zero-pads to `place` digits; `sign` forces the sign, `base` is 2..36. */
    pad(place?: number, sign?: boolean, base?: number): string;
    /** The number as lowercase hex, zero-padded to `place` digits. */
    hex(place?: number): string;
    /** Locale-style grouping formatting: place, thousands separator, decimal separator. */
    format(place?: number, thousands?: string, decimal?: string): string;
    /** A compact human abbreviation (1.2k, 3.4M). */
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

interface NumberConstructor {
    /** An iterator of numbers [start, end). */
    range(start: number, end?: number, step?: number): Iterable<number>;
}

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
    /** Swaps keys and values. */
    invert(obj: object): Record<string, string>;
    invertObj(obj: object): Record<string, string>;
    objOf<K extends string, V>(key: K, value: V): Record<K, V>;
    /** An object of the picked keys. */
    pick(obj: object, keys: string[]): Record<string, unknown>;
    /** An object without the given keys. */
    omit(obj: object, keys: string[]): Record<string, unknown>;
    pickBy(obj: object, pred: (value: unknown, key: string) => boolean): Record<string, unknown>;
    /** [key, value] pairs. */
    toPairs(obj: object): [string, unknown][];
    /** An object from [key, value] pairs. */
    fromPairs(pairs: [string, unknown][]): Record<string, unknown>;
    /** A copy with a property set. */
    assoc(obj: object, key: string, value: unknown): Record<string, unknown>;
    /** A copy with a property removed. */
    dissoc(obj: object, key: string): Record<string, unknown>;
    /** Calls fn with obj and returns obj. */
    tap<T>(fn: (v: T) => void, value: T): T;
    /** A shallow clone. */
    clone<T>(value: T): T;
    /** Deep structural equality. */
    equals(a: unknown, b: unknown): boolean;
    /** Object.is identity. */
    identical(a: unknown, b: unknown): boolean;
    /** The value at a key. */
    prop(obj: object, key: string): unknown;
    propOr(def: unknown, key: string, obj: object): unknown;
    /** The values at several keys. */
    props(obj: object, keys: string[]): unknown[];
    /** The value at a nested path. */
    path(obj: object, path: string[]): unknown;
    pathOr(def: unknown, path: string[], obj: object): unknown;
    /** Values at several paths. */
    paths(obj: object, paths: string[][]): unknown[];
    /** A copy with a nested path set. */
    assocPath(obj: object, path: string[], value: unknown): Record<string, unknown>;
    /** A copy with a nested path removed. */
    dissocPath(obj: object, path: string[]): Record<string, unknown>;
    hasPath(obj: object, path: string[]): boolean;
    /** True when the key is present anywhere on the chain. */
    has(key: string, obj: object): boolean;
    hasIn(key: string, obj: object): boolean;
    keysIn(obj: object): string[];
    valuesIn(obj: object): unknown[];
    propEq(key: string, value: unknown, obj: object): boolean;
    eqProps(key: string, a: object, b: object): boolean;
    pathEq(path: string[], value: unknown, obj: object): boolean;
    /** True when obj satisfies every {key: predicate}. */
    where(spec: Record<string, (v: unknown) => boolean>, obj: object): boolean;
    /** True when obj matches the {key: value} spec. */
    whereEq(spec: Record<string, unknown>, obj: object): boolean;
    /** Shallow merge; later sources win. */
    mergeRight(a: object, b: object): Record<string, unknown>;
    merge(a: object, b: object): Record<string, unknown>;
    mergeLeft(a: object, b: object): Record<string, unknown>;
    /** Deep merge. */
    mergeDeepRight(a: object, b: object): Record<string, unknown>;
    mergeDeepLeft(a: object, b: object): Record<string, unknown>;
    /** The value at a path, or undefined. */
    get(path: string | string[], obj: object): unknown;
    /** A copy with a path set. */
    set(path: string | string[], value: unknown, obj: object): Record<string, unknown>;
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
    pickAll(obj: object, keys: string[]): Record<string, unknown>;
    /** Projects objects onto the given keys. */
    project(keys: string[], objs: object[]): Record<string, unknown>[];
    propSatisfies(pred: (v: unknown) => boolean, key: string, obj: object): boolean;
    pathSatisfies(pred: (v: unknown) => boolean, path: string[], obj: object): boolean;
    /** True when obj satisfies any of the where spec's predicates. */
    whereAny(spec: Record<string, (v: unknown) => boolean>, obj: object): boolean;
    /** A copy with keys renamed per {old: new}. */
    renameKeys(map: Record<string, string>, obj: object): Record<string, unknown>;
    propIs(type: string, key: string, obj: object): boolean;
    /** Groups array items by a key function. */
    groupBy<K extends string | number>(items: unknown[], key: (item: unknown) => K): Record<K, unknown[]>;
    /** Legacy accessors. */
    __defineGetter__(property: string, getter: (this: unknown) => unknown): void;
    __defineSetter__(property: string, setter: (this: unknown, value: unknown) => void): void;
    __lookupGetter__(property: string): ((this: unknown) => unknown) | undefined;
    __lookupSetter__(property: string): ((this: unknown, value: unknown) => void) | undefined;
}

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

interface RegExpConstructor {
    /** Escapes a literal string for use in a RegExp. */
    escape(text: string): string;
}

interface RegExp {
    /** Whether the v flag is set. */
    readonly unicodeSets: boolean;
}
