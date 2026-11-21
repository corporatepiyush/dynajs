/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:compress
/** dyna:compress — one-shot codecs (zstd, brotli, gzip, deflate, snappy, LZ4), tar and zip archives, and reusable Compressor/Dictionary objects.
 *  Decompressors take maxOutputBytes (1..1 GiB, exact; exceeding it is a RangeError) and refuse trailing bytes, so bombs fail deterministically. */
declare module "dyna:compress" {
    /** Zstandard compression; level 1..22. Frames carry a content checksum (4 bytes; `checksum: false` omits it), so unzstd refuses a corrupted body. */
    function zstd(data: BytesInput, opts?: { level?: number; checksum?: boolean }): Uint8Array;
    /** Zstandard decompression; malformed input is refused. Output is capped at 1 GiB per call; an over-cap stream is refused, never truncated. */
    function unzstd(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** Brotli, level 0..11; on macOS the system codec has one fixed effort, so `level` is validated and otherwise ignored. */
    function brotli(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Brotli decompression. */
    function unbrotli(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** Snappy block compression. */
    function snappy(data: BytesInput): Uint8Array;
    /** Snappy block decompression. */
    function unsnappy(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** Raw LZ4 block (no header); `dict` seeds the match window; level 1..12 (clamped by the codec). */
    function lz4Compress(data: BytesInput, opts?: { level?: number; dict?: ByteView }): Uint8Array;
    /** Raw LZ4 block decompression; `dict` must be the dictionary used at compress time. */
    function lz4Decompress(data: ByteView, opts?: { level?: number; dict?: ByteView; maxOutputBytes?: number }): Uint8Array;
    /** LZ4 frame format with optional content checksum. */
    function lz4Frame(data: BytesInput, opts?: { level?: number; checksum?: boolean }): Uint8Array;
    /** LZ4 frame decompression; a bad checksum or structure is refused. */
    function lz4Unframe(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** RFC 1952 gzip framing. An absent level is 6, as in zlib; it was 1 before, so this is a behaviour change: a caller that wants the old speed and output must pass level 1 explicitly (level 1 output is byte-identical to before). Levels 1..5 (0, negatives and NaN count as 1) are greedy over progressively deeper hash chains and use fixed Huffman below 8 KiB; 6..9 match lazily as zlib does, over chains of 128, 256, 1024 and 4096, and a 3-byte match more than 4 KiB back is coded as literals; above 9 counts as 9. Measured on text, CSV and binary corpora, level 9 output is no larger than zlib -9 and level 6 is within 0.01% of zlib -6. At 6..9 every block of 16384 tokens is coded stored, fixed or dynamic, whichever is smallest, so incompressible stretches cost 5 bytes per 64 KiB; 1..5 code the whole input as one block. Out-of-range levels are not refused. Output keeps a 16-byte slack requirement. */
    function gzip(data: BytesInput, level?: number): Uint8Array;
    /** gzip with the level in an options bag (same encoder as the positional form); a non-number level is a TypeError, absent means level 6. */
    function gzip(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** RFC 1952 gzip, inverse of gzip above; CRC32 + ISIZE validated, a corrupted stream is refused. */
    function gunzip(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;

    /** Worst-case compressed size of `n` input bytes for one codec, so an output buffer can be allocated once. */
    function compressBound(n: number, opts: { algo: CompressorAlgo }): number;

    /** ustar archive entry for packing. */
    interface TarEntry {
        name: string;
        data?: ByteView;
        /** "file" or "directory" (link types are refused); absent means "directory" iff there is no data. */
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
    /** Writes a ustar archive. Names must be relative, <= 4096 bytes, with no NUL or `..` component (TarList/TarExtract apply the same gate unless allowUnsafeNames). */
    function TarPack(entries: TarEntry[]): Uint8Array;
    /** Reads archive metadata; `allowUnsafeNames` lifts the safe-name check. */
    function TarList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): TarRecord[];
    /** The TarList records with `data` added to every non-directory entry. */
    function TarExtract(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): TarRecord[];

    /** Zip entry to pack: optional method, mtime (unix s), mode (default 0644), comment; "deflate" is ADVISORY — a member that would not shrink is stored. */
    interface ZipEntry {
        name: string;
        data: ByteView;
        method?: "deflate" | "store";
        mtime?: number;
        mode?: number;
        comment?: string;
    }
    /** Central-directory listing record (mtime in unix seconds, 0 = none). */
    interface ZipRecord {
        name: string;
        size: number;
        mtime: number;
        mode: number;
        comment: string;
        type: string;
        compressedSize: number;
        crc32: number;
        method: string;
    }
    /** Zip member with its bytes (ZipExtractAll/ZipReadAt shape). */
    interface ZipMember extends ZipRecord {
        data: Uint8Array;
    }
    /** Writes a zip archive; method "deflate" or "store". */
    function ZipPack(entries: ZipEntry[], opts?: { method?: "deflate" | "store" }): Uint8Array;
    /** Central-directory listing. Lists exactly the number of entries the end record declares; a directory holding more is a SyntaxError. */
    function ZipList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): ZipRecord[];
    /** Extracts one member by exact name (CRC and size verified); each call walks the directory, so use ZipExtractAll to read every member. */
    function ZipRead(bytes: ByteView, name: string, opts?: { allowUnsafeNames?: boolean }): Uint8Array;
    /** Entry by-index in central-directory order; out of range is a RangeError. */
    function ZipReadAt(bytes: ByteView, index: number, opts?: { allowUnsafeNames?: boolean }): ZipMember;
    /** Options of ZipExtractAll. */
    interface ZipExtractOptions {
        /** Destination directory (string or Path); absent = no writes. */
        dir?: string | import("dyna:file").Path;
        /** Opt-in escape: names containing `..` are then written OUTSIDE `dir`; without it such archives are refused. */
        allowUnsafeNames?: boolean;
    }
    /** Every member with its bytes; given a destination, also written under it. ALL OR NOTHING: names, sizes and CRCs are validated before any file is created.
     *  Refused: absolute names, backslashes, NUL, `..`, symlinks under the destination; output is capped at 1 GiB across all members and per member. */
    function ZipExtractAll(bytes: ByteView, dir?: string | import("dyna:file").Path | ZipExtractOptions, opts?: ZipExtractOptions): ZipMember[];
    /** Raw DEFLATE (RFC 1951, no framing); level 1..12 with gzip's meaning (absent is 6; 10..12 are 9), RangeError outside. Same encoder as deflated ZipPack members. `format: "zlib"` wraps the same body in RFC 1950 framing: header 0x78 then FLEVEL as zlib sets it (level 1 -> 0x01, 2..5 -> 0x5e, 6 -> 0x9c, 7 and up -> 0xda), and the big-endian Adler-32 of the input after the body; "raw" is the default and anything else is a TypeError. */
    function deflate(data: BytesInput, opts?: { level?: number; format?: "raw" | "zlib" }): Uint8Array;
    /** Raw DEFLATE (RFC 1951) decompression; trailing bytes are refused. Output is capped at 1 GiB per call or maxOutputBytes when given. `format: "zlib"` reads RFC 1950 framing: any window size (CINFO 0..7) is accepted; a bad header, a preset dictionary (FDICT, unsupported), a missing or extra trailer byte and an Adler-32 mismatch are each a SyntaxError naming the fault. */
    function inflate(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number; format?: "raw" | "zlib" }): Uint8Array | string;

    /** The codec names Compressor accepts. */
    type CompressorAlgo = "gzip" | "lz4" | "lz4frame" | "zstd" | "brotli" | "snappy";
    /** A compiled codec: configuration and codec scratch owned once and reused across calls; prefer it to the one-shots for many buffers. */
    class Compressor implements DynResource {
        /** algo: gzip | lz4 | lz4frame | zstd | brotli | snappy; `level` follows that codec's one-shot bounds and default (gzip 6); `checksum` is lz4frame-only, `dict` lz4-only. */
        constructor(opts: { algo: CompressorAlgo; level?: number; checksum?: boolean; dict?: ByteView });
        /** Compresses one buffer with this object's configuration. */
        compress(data: BytesInput): Uint8Array;
        /** Decompresses one buffer; `maxOutputBytes` lowers the output cap for this call. */
        decompress(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
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
        /** The automaton is compiled here from non-empty phrases; it pays off when payloads are built from a fixed vocabulary. */
        constructor(phrases: string[]);
        /** Compresses one buffer with this object's configuration. */
        compress(data: BytesInput): Uint8Array;
        /** Decompresses one buffer; `maxOutputBytes` lowers the output cap for this call. */
        decompress(data: ByteView, opts?: { maxOutputBytes?: number }): Uint8Array;
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
