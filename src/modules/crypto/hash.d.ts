/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:hash
/** dyna:hash — digests and checksums, one-shot or streaming (Hasher): MD5, SHA-1/2/3, Keccak, SHAKE, BLAKE2/3, CRC32 (hardware-accelerated), xxHash. */
declare module "dyna:hash" {
    /** Keyed options: BLAKE2b keys are 0..64 bytes, BLAKE2s 0..32; an EMPTY key is legal and means unkeyed (RFC 7693). */
    interface Blk2Opts {
        key?: BytesInput;
        length?: number;
    }
    /** BLAKE3 keys are exactly 32 bytes; {context} (alias {deriveKey}) runs derive-key mode, whose output is exactly 32 bytes. */
    interface Blk3Opts {
        key?: BytesInput;
        length?: number;
        context?: string;
        deriveKey?: string;
    }
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
    /** Adler-32 (RFC 1950), the zlib stream checksum. */
    function Adler32(data: BytesInput): number;
    /** Content-defined chunk END offsets (FastCDC gear hash with normalised masks); defaults min 2048, avg 8192 (a power of two), max 65536.
     *  Inserting bytes moves only the chunk boundaries near the edit. */
    function CDCChunks(data: BytesInput, opts?: { min?: number; avg?: number; max?: number }): Uint32Array;
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
    /** BLAKE3 Merkle-tree hash; length 1..2^20 bytes; an options object in the second position carries {key} / {context}. */
    function BLAKE3(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE3(data: BytesInput, opts?: Blk3Opts): Uint8Array;
    function BLAKE3Hex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE3Hex(data: BytesInput, opts?: Blk3Opts): string;
    /** BLAKE2b; 1..64 bytes.: {key}. */
    function BLAKE2b(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2b(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2bHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2bHex(data: BytesInput, opts?: Blk2Opts): string;
    /** BLAKE2s; 1..32 bytes.: {key}. */
    function BLAKE2s(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2s(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2sHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2sHex(data: BytesInput, opts?: Blk2Opts): string;
    /** Murmur3_128; the second argument is a seed, not a length. */
    function Murmur3_128(data: BytesInput, seed?: number): Uint8Array;
    function Murmur3_128Hex(data: BytesInput, seed?: number): string;
    /** 32-bit xxHash as a number. */
    function XXHash32(data: BytesInput, seed?: number): number;
    /** 64-bit xxHash as 16 hex characters by default; {as: "bigint" | "bytes"} returns the exact value or its little-endian bytes. */
    function XXHash64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    /** XXH3-64 (default secret), the modern cousin of XXHash64; NOT a security primitive. */
    function XXH3_64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;

    /** Streaming digest over every one-shot algorithm name ("sha256", "blake3", ...); {length} sizes the extensible-output ones (shake*, blake2*, blake3). */
    class Hasher implements DynResource {
        constructor(algorithm: string, opts?: { length?: number });
        /** Absorbs bytes; returns this for chaining. */
        update(data: BytesInput): this;
        /** Finalizes a copy; the stream stays usable. */
        digest(): Uint8Array;
        /** The same as a hex string. */
        digestHex(): string;
        /** Finalizes a copy into the caller's buffer and returns the byte count; RangeError when it is smaller than digestSize; the hasher stays usable. */
        digestInto(buf: Uint8Array): number;
        /** Returns the hasher to its initial state. */
        reset(): void;
        readonly algorithm: string;
        readonly digestSize: number;
        /** Releases the native state early (idempotent); every method and getter throws TypeError afterwards. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}