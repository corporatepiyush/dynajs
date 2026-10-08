/** DynaJS ambient types: every `dyna:*` module, the WHATWG globals, `std`/`os` (--std only), and the optional prototype extensions (--no-prototypes removes them).
 *  House rules: option bags are STRICT (unknown key -> TypeError naming the valid set); untrusted input is capped and refused by name, never truncated; `*Into` forms fill a caller buffer without allocating. */

// Runtime model: one JS thread runs the event loop; a shared reactor (kqueue/epoll, io_uring on Linux builds) drives sockets and timers; blocking work offloads to an IO thread pool (--io-threads).

// ---- Shared helper types ----

/** A byte-addressed view: a typed array of 1-byte elements, a DataView, or an ArrayBuffer. */
type ByteView = Uint8Array | Int8Array | Uint8ClampedArray | DataView | ArrayBuffer;

/** Bytes input: a string (encoded as UTF-8) or a byte view. BORROW RULE: the buffer is on loan for the call; resize/transfer from a hook mid-call throws TypeError. */
type BytesInput = string | ByteView;

/** 4-byte typed arrays the f32 SIMD kernels accept; Int32Array/Uint32Array are BIT-CAST (raw 32-bit patterns read as IEEE floats), never converted. */
type F32Like = Float32Array | Int32Array | Uint32Array;

/** Any typed array, all element widths; DataView and ArrayBuffer are NOT included (byte-filling natives such as Random.fill refuse them). */
type AnyTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

/** Integer typed arrays: the only kinds the Atomics read-modify-write ops accept (BigInt kinds take/return BigInt). */
type AnyIntegerTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | BigInt64Array | BigUint64Array;

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

/** dyna:bytes — copied byte buffers (Bytes), UTF-8-safe strings (Text), search and fixed-width reads/writes; every method is also a free function taking the buffer first.
 *  METHOD <-> FREE-FUNCTION MAP: same names except Bytes.equals(v) <-> equal(a, b) and includes <-> contains; method fill returns the handle, free fill the view. */
declare module "dyna:bytes" {
    /** Copied byte buffer: construction, slicing, search, fixed-width reads/writes, text interpretation. */
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
        /** True when no byte has the high bit set; computed once at construction. */
        get isAscii(): boolean;
        /** True when the bytes are well-formed UTF-8 (computed at construction); also a getter on Text and the free function isValidUtf8(data). */
        get isValidUtf8(): boolean;
        /** The backing Uint8Array. */
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
        /** A fresh Uint8Array copy of buf[off .. off+len); RangeError out of bounds (`slice` is the view form). */
        readBytes(off: number, len: number): Uint8Array;
        /** Copies `src` at `off` (overlap-safe) and returns the byte count; RangeError if the write would pass the end. */
        writeBytes(off: number, src: ByteView): number;
        /** Fills and returns the HANDLE (chainable); `.array` recovers the view, and the free fill(buf, ...) returns the view. */
        fill(val: number, start?: number, end?: number): Bytes;
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

    /** Fixed-width accessors as free functions over (view, offset[, value]): the same names as the Bytes methods. */
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

/** dyna:cli — terminal programs: argument parsing (Command), ANSI styling, prompts, raw-mode key input, progress bars. */
declare module "dyna:cli" {
    // $DYNAJS_BYTECODE_CACHE is a developer speedup over TRUSTED input: never enable it for scripts in directories writable by others.

    /** Styles text with a named ANSI style or list (also "256:n", "#rrggbb", "rgb:r,g,b" and bg forms); escapes drop under $NO_COLOR or a non-TTY; unknown names throw. */
    function StyleText(style: string | string[], text: string): string;
    /** The list of styles the engine can apply. */
    function Styles(): string[];
    /** True when the stream with the given fd (default stdout) is a TTY. */
    function IsTTY(fd?: number): boolean;
    /** Terminal columns: TIOCGWINSZ on fd (stdout default), else $COLUMNS, else 80. */
    function Columns(fd?: number): number;
    /** The terminal color depth: 0 (none), 4 (16 colors), 8 (256), or 24 (truecolor). */
    function ColorDepth(fd?: number): number;

    /** Prints `message` and reads one stdin line; empty or EOF yields `opts.default` (else ""); an answer over 1 MiB is a RangeError. */
    function prompt(message: string, opts?: { default?: string }): string;
    /** Prints `message` and reads a line: "y"/"yes" (case-insensitive) is true, anything else including EOF is false. */
    function confirm(message: string): boolean;
    /** Arrow-key menu in raw mode; returns the chosen option, or null on Escape/Ctrl-C/EOF. The terminal mode is restored on every exit path. */
    function select(message: string, options: string[]): string | null;
    /** Reads one key event in raw mode; null at EOF. A lone ESC is told from a sequence head by a 50 ms quiet window. */
    function keypress(): {
        /** "up", "enter", "a", ... -- null for an unrecognized sequence. */
        name: string | null;
        /** The bytes the event consumed, UTF-8 decoded (malformed runs appear as U+FFFD). */
        sequence: string;
        ctrl: boolean;
        meta: boolean;
        shift: boolean;
    } | null;

    /** A single-line progress bar with a time-based ETA. */
    class ProgressBar {
        constructor(opts: { total: number });
        /** Redraws the bar at `n` of `total`; reaching `total` finishes the line and later updates are refused. */
        update(n: number): this;
    }

    /** A single-line spinner over the frames "|/-\". */
    class Spinner {
        constructor(opts?: { text?: string });
        /** Starts the animation; chainable. */
        start(): this;
        /** Advances one frame and redraws. */
        tick(): this;
        /** Clears the line, or leaves `text` as the final line (with a newline). */
        stop(text?: string): this;
    }

    /** Renders rows as aligned columns ("grid"), TSV, or RFC 4180 CSV. */
    function Table(rows: (string | number)[][], opts?: {
        /** Header row, rendered first ("grid" rules it off with dashes). */
        head?: string[];
        /** Per-column alignment; "left" is the default for unlisted columns. */
        align?: ("left" | "right" | "center")[];
        /** "grid" (default), "tsv", or "csv". */
        format?: "grid" | "tsv" | "csv";
    }): string;

    /** A command-line parser: options, positional arguments, subcommands, generated help. */
    class Command {
        /** Constructs an argument parser built from declarative specs. */
        constructor(name?: string);
        get name(): string;
        /** Describes this command for help output. */
        describe(text: string): this;
        /** Registers an option; precedence is CLI > `env` variable > default. Numbers use a strict finite-decimal grammar ("NaN", "0x10" are refused). */
        option(flags: string, description?: string, opts?: { type?: "boolean" | "string" | "number"; required?: boolean; variadic?: boolean; default?: unknown; env?: string }): this;
        /** Registers a positional argument. */
        argument(name: string, description?: string): this;
        /** Registers a subcommand. */
        command(sub: Command): this;
        /** Permits unknown options instead of refusing them. */
        allowUnknown(v: boolean): this;
        /** Registers the handler a leaf parse calls with (options, args); its return lands on the parse result's `result`. */
        action(fn: (options: Record<string, unknown>, args: string[]) => unknown): this;
        /** Sets the version and enables `--version` (chainable); with no argument returns the version ("" when unset). */
        version(v: string): this;
        version(): string;
        /** Parses argv (default scriptArgs) into { options, arguments, command }; pass T to narrow. A NUL byte in argv is a TypeError. */
        parse<T = { options: Record<string, unknown>; arguments: string[]; command: string | null }>(args?: string[]): T;
        /** Prints the help text. */
        help(): string;
    }
}

/** dyna:compress — one-shot codecs (zstd, brotli, gzip, deflate, snappy, LZ4), tar and zip archives, and reusable Compressor/Dictionary objects.
 *  Decompressors take maxOutputBytes (1..1 GiB, exact; exceeding it is a RangeError) and refuse trailing bytes, so bombs fail deterministically. */
declare module "dyna:compress" {
    /** Zstandard compression; level 1..22. */
    function zstd(data: BytesInput, opts?: { level?: number }): Uint8Array;
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
    /** RFC 1952 gzip framing; level >= 6 = dynamic-Huffman, every other value (absent, 0..5, negatives, NaN) = fixed-Huffman; out-of-range levels are not refused. Output keeps a 16-byte slack requirement. */
    function gzip(data: BytesInput, level?: number): Uint8Array;
    /** gzip with the level in an options bag (same encoder as the positional form); a non-number level is a TypeError, absent means fixed-Huffman. */
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
    /** Raw DEFLATE (RFC 1951, no framing); level 1..12, RangeError outside. Same encoder as deflated ZipPack members. */
    function deflate(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Raw DEFLATE (RFC 1951) decompression; trailing bytes are refused. Output is capped at 1 GiB per call or maxOutputBytes when given. */
    function inflate(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;

    /** The codec names Compressor accepts. */
    type CompressorAlgo = "gzip" | "lz4" | "lz4frame" | "zstd" | "brotli" | "snappy";
    /** A compiled codec: configuration and codec scratch owned once and reused across calls; prefer it to the one-shots for many buffers. */
    class Compressor implements DynResource {
        /** algo: gzip | lz4 | lz4frame | zstd | brotli | snappy; `level` follows that codec's one-shot bounds; `checksum` is lz4frame-only, `dict` lz4-only. */
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

/** dyna:config — configuration files: TOML 1.0, INI, `.env` (with optional process-environment assignment), front matter. */
declare module "dyna:config" {
    /** TOML 1.0; date-times stay RFC 3339 strings. SyntaxError: a quote run of six or more, or `[[x]]` extending anything but a previous `[[x]]` header. */
    namespace TOML {
        /** Parses a full TOML 1.0 document; key collisions and leading zeros are refused. */
        function parse(text: string): Record<string, unknown>;
        /** Serializes a plain object root; NaN/Infinity render as nan/inf/-inf. `null`/`undefined` are a TypeError (TOML 1.0 has no null literal). */
        function stringify(value: unknown): string;
    }
    /** Classic INI reading and writing. */
    namespace INI {
        /** Reads [section] headers, key=value pairs, key[]=v lists, and bare keys as true. */
        function parse(text: string): Record<string, unknown>;
        /** Object to INI: top-level scalars, then one [section] per object value; values parse back as strings; deeper nesting, arrays and null are refused. */
        function stringify(record: Record<string, unknown>): string;
    }
    /** dotenv grammar .env parsing and loading. */
    namespace Env {
        /** Parses KEY=value records; lines without `=` are skipped. */
        function parse(text: string): Record<string, string>;
        /** Parses a .env file and assigns it into the process environment. Defaults: assign true, override false, expand false; with expand, values expand in file order.
         *  Caps: file 8 MiB; keys are 1..4096 bytes without `=` or NUL; a NUL in a value is a TypeError, an expansion above 1 MiB a RangeError. */
        function load(path: string, opts?: { assign?: boolean; override?: boolean; expand?: boolean }): Record<string, string>;
        /** Record to .env text; values must be strings, and those that cannot sit bare are double-quoted with the escapes parse() expands. */
        function stringify(record: Record<string, string>): string;
    }
    /** Front-matter splitting (YAML/TOML/JSON fences); data stays text. */
    namespace FrontMatter {
        /** Splits at a first-line fence; `data`/`lang` are null when absent. */
        function split(text: string): { data: string | null; body: string; lang: string | null };
    }
}

/** dyna:crypto — AEAD (AES-GCM, ChaCha20-Poly1305), signatures (RSA, ECDSA, Ed25519), key exchange, password hashing, HMAC/KDFs, OTP, X.509, JWT.
 *  Secrets compare in constant time. The OpenSSL-backed names (AEAD classes, RSA/ECDSA/ECDH/X509, Ed25519/X25519, Scrypt) exist only in CONFIG_TLS=y builds: feature-detect via the namespace. */
declare module "dyna:crypto" {
    /** keyed/derive options for the BLAKE one-shots re-exported here. */
    interface Blk2Opts {
        key?: BytesInput;
        length?: number;
    }
    /** BLAKE3: key exactly 32 bytes; {context}/{deriveKey} = derive-key. */
    interface Blk3Opts {
        key?: BytesInput;
        length?: number;
        context?: string;
        deriveKey?: string;
    }
    /** A PEM key pair. */
    interface KeyPair {
        privateKey: string;
        publicKey: string;
    }

    /** OpenBSD $2b$ bcrypt. Like PBKDF2/Scrypt/Argon2id it runs on the calling thread for its whole cost: hash passwords on a Worker in a server. */
    namespace Bcrypt {
        /** Hashes a password with a fresh salt; rounds 4..31 (else RangeError); a password over bcrypt's 72-byte limit is a RangeError, never truncated. */
        function hash(password: string, rounds?: number): string;
        /** Constant-time verify; accepts $2a$/$2b$/$2y$ prefixes; the hash's cost is capped at 20. */
        function verify(password: string, hash: string): boolean;
    }

    /** Argon2id v0x13 (RFC 9106). */
    namespace Argon2id {
        /** Argon2id options: memory in KiB, salt at least 8 bytes; `encoded` selects hash()'s return form. */
        interface Argon2idOpts {
            iterations?: number;
            memory?: number;
            parallelism?: number;
            hashLen?: number;
            /** Default true: hash() returns the PHC string `$argon2id$v=19$m=..,t=..,p=..$salt$hash`; false returns the raw tag bytes. */
            encoded?: boolean;
        }
        /** PHC string form (default): the salt and every parameter ride in the string, so storing it is the whole contract. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded?: true }): string;
        /** Raw tag form with { encoded: false }. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded: false }): Uint8Array;
        /** Verifies a PHC string (untrusted input). Caps: iterations <= 16, parallelism <= 16, memory <= 1048576 (1 GiB) KiB; more is a RangeError. */
        function verify(hashString: string, password: BytesInput): boolean;
        /** Raw form: recomputes with the same parameters and compares in constant time. */
        function verify(password: BytesInput, salt: ByteView, expectedHash: ByteView, opts?: Argon2idOpts): boolean;
    }

    /** RSA keys and signatures: PKCS#1 v1.5, RSASSA-PSS and RSA-OAEP. */
    namespace RSA {
        /** Defaults to 2048 bits. */
        function generate(bits?: 2048 | 3072 | 4096): KeyPair;
        /** md is "sha1"|"sha256"|"sha384"|"sha512" (case-insensitive). */
        function sign(md: string, privateKey: string, msg: BytesInput): Uint8Array;
        /** True when the signature checks against the public key. */
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView): boolean;
        /** RSASSA-PSS. saltLen: a byte count, "max", or "auto" (default; interoperates with OpenSSL); mgf1Hash defaults to md. */
        function signPSS(md: string, privateKey: string, msg: BytesInput,
                         opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): Uint8Array;
        /** RSASSA-PSS (RFC 8017): the same digest table and the same 2048-bit key floor as `sign`/`verify`, with the padding that carries a randomized salt. */
        function verifyPSS(md: string, publicKey: string, msg: BytesInput, sig: ByteView,
                           opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): boolean;
        /** RSA-OAEP: seal to a public key, open with the private one. md defaults to "sha256"; `label` must round-trip exactly; 2048-bit key floor. */
        namespace OAEP {
            function seal(publicKey: string, plaintext: BytesInput,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
            function open(privateKey: string, sealed: ByteView,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
        }
    }

    /** X.509 certificate parsing and self-signed generation. */
    namespace X509 {
        /** The parsed fields of a certificate. */
        interface X509Info {
            subject: string;
            issuer: string;
            serialNumber: string;
            version: number;
            /** OpenSSL ASN1_TIME text (e.g. "Sep 15 19:48:26 2026 GMT"). */
            notBefore: string;
            notAfter: string;
            fingerprint: string;
            sans: { dns: string[]; ip: string[]; email: string[] };
        }
        /** Parses a PEM string or DER bytes; malformed input is refused. */
        function parse(cert: BytesInput): X509Info;
        /** Builds a v3 self-signed SHA-256 certificate (days defaults to 365); `sans` are classified by shape (email, IP, DNS); unsupported extensions are refused by name. */
        function generateSelfSigned(opts: {
            key: string;
            subject?: string;
            days?: number;
            sans?: string[];
            extensions?: { name: string; value: string; critical?: boolean }[];
        }): string;
    }

    /** ECDSA raw R||S or DER signatures. */
    namespace ECDSA {
        /** The curve defaults to P-256. */
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** md "sha1"|"sha256"|"sha384"|"sha512"; format "raw" (default) or "der". */
        function sign(md: string, privateKey: string, msg: BytesInput, opts?: { format?: "raw" | "der" }): Uint8Array;
        /** True for a valid raw or DER signature. A raw signature of the wrong length is a plain false. */
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView, opts?: { format?: "raw" | "der" }): boolean;
    }

    /** ECDH key agreement over X9.63. */
    namespace ECDH {
        /** The curve defaults to P-256. */
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** The raw shared secret; a small-order peer point is refused. */
        function derive(privateKey: string, peerPublicKey: string): Uint8Array;
    }

    /** Ed25519 one-shot signatures; raw 32-byte keys. */
    function Ed25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    /** A 64-byte signature over the whole message. Ed25519 is one-shot by construction. */
    function Ed25519Sign(privateKey: ByteView, message: BytesInput): Uint8Array;
    /** True when the signature is valid. A wrong-size signature returns false, indistinguishable from any other forgery. */
    function Ed25519Verify(publicKey: ByteView, message: BytesInput, signature: ByteView): boolean;

    /** X25519 key agreement; raw 32-byte keys. A small-order (incl. all-zero) peer key is a TypeError. */
    function X25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    /** The 32-byte shared secret. */
    function X25519Derive(privateKey: ByteView, peerPublicKey: ByteView): Uint8Array;

    /** PEM <-> raw converters between 32-byte raw keys and PKCS#8 / SPKI PEM (byte-exact with openssl genpkey). */
    function Ed25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    /** The bridge between the module's 32-byte raw keys and the PEM world (PKCS#8 `-----BEGIN PRIVATE KEY-----`, SPKI `-----BEGIN PUBLIC KEY-----`). */
    function Ed25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };
    function X25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    function X25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };

    /** AES-GCM AEAD; key exactly 16/24/32 bytes (else TypeError), nonce exactly 12 bytes (else TypeError). */
    class AESGCM implements DynResource {
        /** Binds the key once. AES-NI / ARMv8 crypto extensions are selected by the backend at runtime. */
        constructor(key: ByteView);
        /** Encrypts and appends the 16-byte tag; nonce must be exactly 12 bytes. */
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        /** Decrypts and authenticates; a forged tag throws `authentication failed`. */
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        /** Seals into the CALLER's buffer (no allocation), returns bytes written; {tagOut} detaches the 16-byte tag, {prefixNonce:true} emits nonce||ciphertext||tag. */
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        /** The open twin: {tag} reads a detached tag, {prefixNonce:true} reads the leading nonce from the blob; a forged tag throws like open(). */
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        /** Seals with a fresh CSPRNG 12-byte nonce and returns it beside ciphertext||tag; random nonces bound one key to ~2^32 messages. */
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** ChaCha20-Poly1305 AEAD; key exactly 32 bytes, nonce exactly 12 bytes (else TypeError). */
    class ChaCha20Poly1305 implements DynResource {
        /** `seal`, `open`, `sealInto`, `openInto`, `close`, `dispose` and `closed` behave exactly as `AESGCM`'s, with the same 12-byte nonce, 16-byte tag, and throw-on-forgery rule. */
        constructor(key: ByteView);
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Streaming HMAC with a derived key schedule reused across calls. */
    class Hmac implements DynResource {
        /** Derives the block-sized key schedule once; every later `sign`/`verify` reuses it. */
        constructor(algorithm: string, key: BytesInput);
        /** A complete MAC; the object is ready for the next message. */
        sign(msg: BytesInput): Uint8Array;
        /** A complete MAC; the object is ready for the next message (finalise resets). */
        signHex(msg: BytesInput): string;
        /** Streaming absorb; returns this. */
        update(msg: BytesInput): this;
        /** Finishes the accumulated stream. */
        digest(): Uint8Array;
        /** Finish the accumulated stream. */
        digestHex(): string;
        /** Returns the HMAC to its initial state, discarding any update() prefix; the key schedule is kept. */
        reset(): void;
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
    /** The one-shot HMAC; `HMACHex` returns the same digest as lowercase hex. */
    function HMACHex(algorithm: string, key: BytesInput, data: BytesInput): string;

    /** The unkeyed digests and checksums of dyna:hash, re-exported (same functions) so PKCE challenges and fingerprints need no second import. */
    function MD5(data: BytesInput): Uint8Array;
    function MD5Hex(data: BytesInput): string;
    function SHA1(data: BytesInput): Uint8Array;
    function SHA1Hex(data: BytesInput): string;
    function SHA224(data: BytesInput): Uint8Array;
    function SHA224Hex(data: BytesInput): string;
    function SHA256(data: BytesInput): Uint8Array;
    function SHA256Hex(data: BytesInput): string;
    function SHA384(data: BytesInput): Uint8Array;
    function SHA384Hex(data: BytesInput): string;
    function SHA512(data: BytesInput): Uint8Array;
    function SHA512Hex(data: BytesInput): string;
    function CRC32(data: BytesInput): number;
    function CRC32C(data: BytesInput): number;
    function XXHash32(data: BytesInput, seed?: number): number;
    function XXHash64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    /** XXH3-64 (default secret); same shapes as XXHash64. Not a security primitive. */
    function XXH3_64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    function SHA3_224(data: BytesInput): Uint8Array;
    function SHA3_224Hex(data: BytesInput): string;
    function SHA3_256(data: BytesInput): Uint8Array;
    function SHA3_256Hex(data: BytesInput): string;
    function SHA3_384(data: BytesInput): Uint8Array;
    function SHA3_384Hex(data: BytesInput): string;
    function SHA3_512(data: BytesInput): Uint8Array;
    function SHA3_512Hex(data: BytesInput): string;
    function Keccak256(data: BytesInput): Uint8Array;
    function Keccak256Hex(data: BytesInput): string;
    function SHAKE128(data: BytesInput, length?: number): Uint8Array;
    function SHAKE128Hex(data: BytesInput, length?: number): string;
    function SHAKE256(data: BytesInput, length?: number): Uint8Array;
    function SHAKE256Hex(data: BytesInput, length?: number): string;
    /** Keyed/derive forms take an options object second: BLAKE2 keys are 0..64 (b) / 0..32 (s) bytes, BLAKE3 keys exactly 32; {context} derives a 32-byte key. */
    function BLAKE3(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE3(data: BytesInput, opts?: Blk3Opts): Uint8Array;
    function BLAKE3Hex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE3Hex(data: BytesInput, opts?: Blk3Opts): string;
    function BLAKE2b(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2b(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2bHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2bHex(data: BytesInput, opts?: Blk2Opts): string;
    function BLAKE2s(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2s(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2sHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2sHex(data: BytesInput, opts?: Blk2Opts): string;
    function Murmur3_128(data: BytesInput, seed?: number): Uint8Array;
    function Murmur3_128Hex(data: BytesInput, seed?: number): string;

    /** RFC 5869 HKDF extract-and-expand. Defaults: hash "sha256", length 32; an absent `salt` is the all-zero salt of hash length. */
    function HKDF(opts: { hash?: string; key: BytesInput; salt?: BytesInput; info?: BytesInput; length?: number }): Uint8Array;

    /** RFC 8018 PBKDF2: hash "sha256", iterations 100000, length 32 by default; `salt` is REQUIRED (TypeError); iterations x output blocks capped at 2^24. */
    function PBKDF2(opts: { hash?: string; password: BytesInput; salt: BytesInput; iterations?: number; length?: number }): Uint8Array;

    /** RFC 7914 scrypt; budgets (RangeError): memory 128*r*(N+p+2) <= 1 GiB, work N*r*p <= 2^26. Runs on the calling thread. */
    function Scrypt(password: BytesInput, salt: BytesInput, opts?: { N?: number; r?: number; p?: number; keyLen?: number }): Uint8Array;

    /** OS-entropy CSPRNG bytes: the door for security material; use dyna:random's seeded Random for reproducible or fast simulation. */
    function RandomBytes(count?: number): Uint8Array;

    /** Constant-time comparison; different lengths return false (oauth2.secureCompare is the string-friendly form).
     *  Module rule: key/secret arguments must be a string, ArrayBuffer or byte view and are never stringified; anything else is a TypeError. */
    function TimingSafeEqual(a: ByteView, b: ByteView): boolean;

    /** RFC 4226 HOTP; digits 6..8, algo any Hmac name. */
    function HOTPGenerate(secret: BytesInput, counter: number, opts?: { digits?: number; algo?: string }): string;

    /** RFC 6238 TOTP; atSec is explicit so results are testable. */
    function TOTPGenerate(secret: BytesInput, opts?: { atSec?: number; period?: number; digits?: number; algo?: string }): string;

    /** HOTP verification in constant time; a malformed code is FALSE like a wrong guess, never a throw. */
    function HOTPVerify(secret: BytesInput, counter: number, code: string, opts?: { digits?: number; algo?: string }): boolean;

    /** TOTP verification; {window} (default 0, at most 4096 periods) accepts counters within +/- window, each compared in constant time. */
    function TOTPVerify(secret: BytesInput, code: string, opts?: { atSec?: number; period?: number; digits?: number; algo?: string; window?: number }): boolean;

    /** JWS signing; HS/RS/ES algorithms. */
    function JWTSign(payload: unknown, key: BytesInput, opts?: { alg?: string }): string;

    /** Verifies a JWT: signature, header alg in the required `algorithms` allowlist (`none` always refused), key family matching the alg; `crit` headers refused.
     *  exp/nbf/aud/iss are NOT validated here: use dyna:oauth2's verifyJWT for claims. Pass T to type the payload. */
    function JWTVerify<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[] }): T;
}

/** dyna:csv — RFC 4180 CSV: in-memory parse/stringify, streaming batches, and CSVFile, a file-backed table.
 *  Cells are VERBATIM: a leading = + - @ can run as a spreadsheet formula; escape it or use DataFrame TO_CSV {escapeFormulas:true}. */
declare module "dyna:csv" {
    /** Parses CSV text in memory; a leading BOM is stripped and empty text is the empty table.
     *  Rows are shaped to the first row's width (short rows pad with "", long rows truncate); rows in syntax errors are 1-based. */
    function parse(text: string, opts?: {
        /** Field separator; exactly one ASCII char, not `"` / CR / LF, != quote. */
        delimiter?: string;
        /** Quote char; exactly one ASCII char, not `,` / CR / LF, != delimiter. */
        quote?: string;
        /** Default true; false parses headerless text: `headers` is [] and every row is data. */
        hasHeader?: boolean;
        /** Default false (tolerant); true throws a SyntaxError naming the row on garbage after a closing quote or an unterminated quote. */
        strict?: boolean;
    }): { headers: string[]; rows: string[][]; totalRows: number };
    /** Serializes rows to CSV text, every line ending in LF. string[][] rows are written verbatim (the header is the caller's first row).
     *  Record rows take their columns from the FIRST row's keys and get a header line unless hasHeader:false; mixing the two forms is refused. */
    function stringify(rows: string[][] | Record<string, unknown>[], opts?: {
        delimiter?: string;
        quote?: string;
        /** Default true; false suppresses the derived header of object rows. */
        hasHeader?: boolean;
    }): string;
    /** A file-backed table: each method load-modify-stores the bound file; writes are atomic whole-file replacements. */
    class CSVFile implements DynResource {
        /** Binds the path; touches no disk until a method runs. */
        constructor(path: import("dyna:file").Path);
        /** Creates the file from headers and optional rows. */
        create(opts: { headers: string[]; rows?: (string | number)[][]; overwrite?: boolean }): { path: import("dyna:file").Path; rows: number };
        /** Loads the file; options offset/limit/columns. delimiter/quote change the input dialect (read-only support: mutators rewrite the canonical comma/quote pair). */
        read(opts?: { offset?: number; limit?: number; columns?: string[]; strict?: boolean; delimiter?: string; quote?: string }): { headers: string[]; rows: string[][]; totalRows: number };
        /** Appends rows ({rows}: positional arrays or objects keyed by header); a bare array is ONE positional row, truncated to the header's width. */
        addRow(opts: { rows: (string | number)[][] | Record<string, unknown>[]; strict?: boolean; durable?: boolean } | (string | number)[]): { added: number; totalRows: number };
        /** Sets one cell. */
        updateCell(opts: { row: number; column?: string; columnIndex?: number; value: string; strict?: boolean; durable?: boolean }): { row: number; column: string; value: string };
        /** Removes the data row at `row`; `removed` echoes the removed row's index. */
        removeRow(opts: { row: number; strict?: boolean; durable?: boolean }): { removed: number; totalRows: number };
        /** Appends a column, filling rows with defaultValue. */
        addColumn(opts: { column: string; defaultValue?: string; strict?: boolean; durable?: boolean }): { column: string; totalColumns: number };
        /** Drops the named or indexed column. */
        removeColumn(opts: { column?: string; columnIndex?: number; strict?: boolean; durable?: boolean }): { removedIndex: number; totalColumns: number };
        /** Renames a column; a no-op when names match. */
        renameColumn(opts: { oldName: string; newName: string; strict?: boolean; durable?: boolean }): { oldName: string; newName: string };
        /** One column's values over a [start, end) window -- or its {offset, limit} alias (never both forms); the default 1000-row window cap is raised with maxRows. */
        readColumnValuesRange(opts: { column: string; start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): (string | number)[];
        /** Rows [start, end) as arrays; default is a single row. Accepts {offset, limit} instead of {start, end} (never both); the default 100-row cap is raised with maxRows. */
        readRowRange(opts?: { start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): { headers: string[]; rows: string[][] };
        /** Projects columns over a [start, end) window, capped at 100 rows ({offset, limit} alias accepted, never both forms; maxRows raises the cap). */
        selectColumnRange(opts: { columns: string[]; start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): { columns: string[]; rows: string[][] };
        /** Async-iterable batches {headers, rows, offset, totalRows} of at most `batch` rows (default 1000, max 100000); each next() re-parses the file, so prefer large batches. */
        rows(opts?: { batch?: number; delimiter?: string; quote?: string; strict?: boolean }): AsyncIterableIterator<{ headers: string[]; rows: string[][]; offset: number; totalRows: number }>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:decimal — exact decimal arithmetic (IEEE 754-2008 decimal128, 34 digits by default) and Money in integer minor units. */
declare module "dyna:decimal" {
    // Rounding defaults differ on purpose: round() is halfEven (IEEE), toFixed() is halfUp (invoices); pass a RoundingMode when it matters.

    /** `precision` is 1..5000 significant digits (default 34); exact ops (add/sub/mul/mod/divmod) ignore it; ln/log10/exp throw past a fixed work budget. */
    interface DecimalOptions {
        precision?: number;
        rounding?: RoundingMode | string;
    }
    /** An exact decimal value; methods return new Decimals and accept Decimal, string or number operands (strings avoid binary-float input error). */
    class Decimal {
        /** value: decimal string, JS number (through its shortest round-trip text), or another Decimal. */
        constructor(value: string | number | Decimal);
        /** Exact addition — addition/multiplication are EXACT here, so `opts` is accepted and ignored on add/sub/mul/mod (only div rounds). */
        add(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        sub(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        mul(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** The only arithmetic that rounds; division by zero throws. */
        div(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Exact truncated remainder: the sign follows the dividend (like JS %), mod by zero throws, opts ignored. */
        mod(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Integer exponentiation; exponent in -10000..10000. */
        pow(n: number, opts?: DecimalOptions): Decimal;
        /** Square root, correctly rounded half-even (`rounding` ignored); a negative value throws RangeError. */
        sqrt(opts?: DecimalOptions): Decimal;
        /** e^x, correctly rounded half-even; RangeError past Emax = 999999; results below the subnormal range are 0. */
        exp(opts?: DecimalOptions): Decimal;
        /** Natural log, correctly rounded half-even; zero or negative throws RangeError. */
        ln(opts?: DecimalOptions): Decimal;
        /** Base-10 log, correctly rounded half-even and exact for powers of ten; zero or negative throws RangeError. */
        log10(opts?: DecimalOptions): Decimal;
        /** The integer toward -Infinity. Exact; no rounding context. */
        floor(): Decimal;
        /** The integer toward +Infinity. Exact; no rounding context. */
        ceil(): Decimal;
        /** The integer toward zero. Exact; no rounding context. */
        trunc(): Decimal;
        /** Exact truncated-division pair [q, r]: q toward zero, r with the dividend's sign, a == q*b + r; division by zero throws. */
        divmod(x: Decimal | string | number, opts?: DecimalOptions): [Decimal, Decimal];
        /** Truncates toward zero as a bigint; a fractional part or a magnitude at or past 2^63 throws RangeError. */
        toBigInt(): bigint;
        /** The value zero. The constants are ordinary Decimals. */
        static readonly ZERO: Decimal;
        static readonly ONE: Decimal;
        static readonly TWO: Decimal;
        static readonly TEN: Decimal;
        static readonly NEG_ONE: Decimal;
        /** The magnitude. */
        abs(): Decimal;
        /** The negation; -0 is 0. */
        neg(): Decimal;
        /** -1, 0, or 1. The short legacy form of compare. */
        cmp(x: Decimal | string | number): number;
        /** True when the values are equal (1.5 equals 1.50). */
        equals(x: Decimal | string | number): boolean;
        /** A new Decimal rounded to dp decimal places; dp in -1000..1000; default mode halfEven. */
        round(dp?: number, rounding?: RoundingMode | string): Decimal;
        /** A string with exactly dp digits after the point; default mode halfUp; a negative value that rounds to zero still prints "-0.00". */
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
        /** `new Money(1999, "USD")` is $19.99. Money is integer arithmetic with a unit, not float-shaped arithmetic. Defaults: minorDigits 2. */
        constructor(minorUnits: number, currency: string, opts?: { minorDigits?: number });  /* minorUnits and minorDigits must be integers; the code is upper-cased; opts must be an object when given */
        /** Parses the human form ("-1,234.56") digit by digit, never through a double; a value FINER than the minor unit is refused, not rounded ("1.500" ok, "1.005" throws). */
        static fromString(str: string, currency: string, opts?: { minorDigits?: number }): Money;
        /** From an exact Decimal; `d * 10^minorDigits` must be an integer or it is refused, not rounded. */
        static fromDecimal(d: Decimal, currency: string, opts?: { minorDigits?: number }): Money;
        /** From a raw minor-unit count: an integer number, a bigint, or an amount string that is whole minor units ("5.0" ok, "5.5" throws). */
        static fromMinor(n: number | bigint | string, currency: string, opts?: { minorDigits?: number }): Money;
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
        /** The minor-unit integer as a Number: exact through 2^53; use toString()/toDecimal() beyond. */
        amount(): number;
        /** The 3-letter code. */
        currency(): string;
        /** "$19.99" for major currencies, "19.99 USD" otherwise. */
        format(): string;
        /** The amount as an exact Decimal. */
        toDecimal(): Decimal;
    }
}

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
/** dyna:file — the filesystem: Path values, whole-file and buffered IO, atomic writes, locks, watchers, globs, temp files, platform directories. */
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
        /** Whether the path is absolute. */
        get isAbsolute(): boolean;
        /** `this` leads, then the arguments; returns a new Path. */
        join(...segments: (string | Path)[]): Path;
        /** join that resolves `.`/`..` LEXICALLY (absolute argument rebases); no filesystem/symlink access; NUL in a segment is a TypeError. */
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
        /** File metadata, following symlinks (lstat() describes the link itself). */
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
        /** Byte copy to a new File; refuses an existing destination unless `{overwrite: true}`. */
        copyTo(dest: Path, opts?: { overwrite?: boolean }): File;
        /** Opens a buffered FileReader on this file. */
        reader(opts?: { bufferSize?: number }): FileReader;
        /** Opens a buffered FileWriter; `append` adds to the end, `preallocate` reserves bytes up front. */
        writer(opts?: { bufferSize?: number; preallocate?: number; append?: boolean }): FileWriter;
        toString(): string;
        toJSON(): string;
    }

    /** Buffered sequential reader. read/readLine/readAll are STRING paths (UTF-8, invalid bytes become U+FFFD); readInto/readBytes are raw BYTE paths. */
    class FileReader implements DynResource {
        /** bufferSize defaults to 128 KiB (clamped to 4 KiB..64 MiB). */
        constructor(path: Path, opts?: { bufferSize?: number });
        /** Up to `n` bytes as a string, "" at EOF; `n` omitted reads all. */
        read(n?: number): string;
        /** The next line without its trailing newline; null at a clean EOF. */
        readLine(): string | null;
        /** The rest of the file. */
        readAll(): string;
        /** Copies up to `buf.length` raw bytes into `buf`; returns the count, 0 = EOF. No UTF-8 repair. */
        readInto(buf: ByteView): number;
        /** Up to `n` raw bytes as a fresh Uint8Array (empty at EOF); `n` omitted reads all. */
        readBytes(n?: number): Uint8Array;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Buffered sequential writer; flushes buffered bytes on teardown. */
    class FileWriter implements DynResource {
        /** bufferSize defaults to 128 KiB; `append` adds to the end; `preallocate` reserves bytes up front. */
        constructor(path: Path, opts?: { bufferSize?: number; preallocate?: number; append?: boolean });
        /** Accepts a string, ArrayBuffer, or any TypedArray/DataView; returns bytes accepted. */
        write(data: BytesInput): number;
        /** Push buffered bytes to the fd. */
        flush(): void;
        /** Flush then durable-sync (F_FULLFSYNC on Darwin). */
        sync(): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An advisory exclusive lock via flock(2). */
    class FileLock implements DynResource {
        /** `retry` extra attempts (default 0: fail at once when held), spaced `retryMs` apart (default 100). */
        constructor(path: Path | string, opts?: { retry?: number; retryMs?: number });
        /** Calls fn, then releases the lock no matter what fn did; the lock is consumed. */
        withLock<T>(fn: () => T): T;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Kernel-event file watching (kqueue/inotify); events { path, kind } reach both the start() callback and async iteration. */
    class Watcher implements DynResource {
        constructor(path: Path, opts?: { recursive?: boolean; debounceMs?: number; ignore?: string[] });  /* opts must be an object when given: a string/number throws TypeError */
        /** Arms the watch (path is a plain string); a restart reopens the event stream a stop() ended. */
        start(cb?: (event: { path: string; kind: "change" | "add" | "addDir" | "unlink" | "unlinkDir" }) => void): void;
        /** Halts the watch and ends the stream: buffered events still drain through next(), then { done: true }. Idempotent; start() may follow. */
        stop(): void;
        /** One async pull: a Promise of { value, done }; a parked pull resolves done on stop()/close(). */
        next(): Promise<{ value: { path: string; kind: string }; done: false } | { value: undefined; done: true }>;
        /** What a for-await `break` calls: ends the iteration now (buffered events dropped) and stops the watch. */
        return(): Promise<{ value: undefined; done: true }>;
        /** The watcher itself, so `for await (const ev of w)` works. */
        [Symbol.asyncIterator](): AsyncIterator<{ path: string; kind: string }>;
        /** `{ entries, directories, events, truncated, debounceMs }`. */
        stats(): { entries: number; directories: number; events: number; truncated: boolean; debounceMs: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A compiled glob for LEXICAL whole-path matching (no filesystem access): `*`, `**` and `?` all cross `/` and no leading-dot rule applies, unlike glob()'s walk. */
    class Glob {
        constructor(pattern: string);
        /** Lexical match only, no filesystem access. */
        matches(path: Path): boolean;
        /** One-shot `new Glob(pattern).matches(path)`; a Path contributes its normalised bytes, a string is used verbatim. */
        static match(path: Path | string, pattern: string): boolean;
        /** The glob() walk with an optional cwd; returns matching Paths. */
        expand(cwd?: Path): Path[];
        /** The subset of the array matching the pattern. */
        filter(paths: Path[]): Path[];
        readonly pattern: string;
        readonly hasWildcard: boolean;
    }

    /** Whole file as a string (invalid UTF-8 becomes U+FFFD); `{bytes: true}` returns a Uint8Array. */
    function readFile(path: Path, opts?: { bytes?: boolean }): string | Uint8Array;
    /** Whole file as a Uint8Array; same as `readFile(path, {bytes: true})`. */
    function readBytes(path: Path): Uint8Array;
    /** Write string or bytes; O_CREAT, truncate unless append; returns the byte count; undefined/null is a TypeError. */
    function writeFile(path: Path, data: BytesInput, opts?: { append?: boolean }): number;

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
    /** Byte copy through the kernel; refusing an existing destination is the default. Returns the source byte count. */
    function copyFile(from: Path, to: Path, opts?: { overwrite?: boolean }): number;
    /** rename(2), falling back to copy-then-unlink across filesystems; refuses a destination that resolves to the same file. */
    function move(from: Path, to: Path): void;
    /** MIME type from magic bytes, not the extension. */
    function sniffType(pathOrBytes: Path | ByteView): string;

    /** Creates the link; only the link location is a Path; a target with an embedded NUL is a TypeError. */
    function symlink(target: string, linkpath: Path): void;
    /** The stored target verbatim, as a string. */
    function readLink(path: Path): string;
    /** The fully resolved path as a Path. */
    function realPath(path: Path): Path;
    /** Changes permissions. */
    function chmod(path: Path, mode: number): void;

    /** Walks the filesystem and returns sorted Paths: `*` stays within one segment, `**` spans any number (never through symlinked directories), `?`, `[a-z]`, `[!x]`.
     *  No brace expansion; wildcards skip dotfiles unless the segment starts with `.`; `{cwd}` roots the walk. */
    function glob(pattern: string, opts?: { cwd?: Path }): Path[];

    /** The system temp directory as a Path. */
    function tempDir(): Path;
    /** mkdtemp under the temp dir; the prefix is a plain name fragment (a slash, a leading `.` or more than 200 bytes is a RangeError). */
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

/** dyna:html — server-side web content: an HTML5 parser into a shared element tree, CSS-selector queries, an allow-list sanitizer, markdown, templates. */
declare module "dyna:html" {
    /** An element node, the SHARED tree shape: HTMLParse produces it, dyna:scrape's Extractor and Crawl consume it, dyna:xml uses the same layout. */
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
    /** Renders Markdown to HTML; allowRawHTML defaults FALSE (raw HTML is escaped; true is the trusted-input opt-in). */
    function MarkdownToHTML(text: string, opts?: { allowRawHTML?: boolean }): string;

    /** A compiled CSS selector over the parsed tree. */
    class Selector {
        constructor(text: string);
        /** Every matching element. */
        all(doc: HTMLElement | HTMLElement[]): HTMLElement[];
        /** The first match, or undefined when none. */
        first(doc: HTMLElement | HTMLElement[]): HTMLElement | undefined;
        /** True when the single node matches (no combinators allowed). */
        matches(node: HTMLElement): boolean;
    }

    /** Allow-list sanitizer; `protocols` maps "tag.attr" to permitted URL schemes. Output is re-escaped, so re-parsing the output introduces no new structure. */
    class Sanitizer {
        /** An allow-list is required; there is no default policy. */
        constructor(opts: { allow: Record<string, unknown>; protocols?: Record<string, string[]> });
        /** Returns the sanitized HTML. */
        clean(html: string): string;
    }

    /** Rewrites URL-bearing attributes (href, src, srcset candidates, ...) IN PLACE through fn(url, tag, attr); null/undefined keeps the original; returns the count replaced. */
    function rewriteLinks(doc: HTMLElement | HTMLElement[], fn: (url: string, tag: string, attr: string) => unknown): number;

    /** A compiled template; `escape` defaults to TRUE ({escape:false} is the trusted-input opt-in). Caps: 65536 tags, sections 64 deep. */
    class Template {
        constructor(source: string, opts?: { escape?: boolean });
        /** Renders with the data scope; at most 1048576 section elements per render (RangeError, not a hang). */
        render(data?: unknown): string;
    }
}

/** dyna:http — the web platform: WHATWG fetch/Request/Response/Headers/FormData, HTTP servers (App, HTTPServer, HTTPServerAsync), WebSockets, SSE, header codecs.
 *  Import this OR dyna:net, not both: dyna:net re-exports this surface alongside sockets and database clients. */
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
    /** Content negotiation: the best candidate STRING for an Accept header, or null; q=0 excludes, ties follow the header's order. */
    function Negotiate(header: string, candidates: string[]): string | null;
    /** Picks the best supported token by q-value; "*" selects the first candidate. */
    function NegotiateToken(header: string, candidates: string[]): string | null;
    /** Parses a Range header against a size: inclusive {start, end} ranges, "unsatisfiable" when none fits, null when malformed. */
    function RangeParse(header: string, size: number): { start: number; end: number }[] | "unsatisfiable" | null;
    /** Parses a Cookie header into an object; a repeated name keeps the LAST value. */
    function CookieParse(header: string): Record<string, string>;
    /** CookieSerialize options; STRICT bag (an unknown key is a TypeError naming it). */
    interface CookieOptions {
        maxAge?: number;
        domain?: string;
        path?: string;
        sameSite?: "Strict" | "Lax" | "None" | "strict" | "lax" | "none";
        secure?: boolean;
        httpOnly?: boolean;
    }
    /** Serializes a cookie; absent options emit exactly `name=value`. */
    function CookieSerialize(name: string, value: string, opts?: CookieOptions): string;
    /** True when the If-None-Match header matches the etag. */
    function ETagMatch(header: string, etag: string): boolean;
    /** Parses a multipart body against the boundary in the Content-Type header. */
    function MultipartParse(contentType: string, body: ByteView): { name: string; filename?: string; body: Uint8Array }[];
    /** Formats multipart parts; returns {body, contentType}. */
    function MultipartFormat(parts: { name: string; value?: string; body?: ByteView; filename?: string }[], boundary?: string): { body: Uint8Array; contentType: string };

    /** A BLOCKING HTTP client over one connection. Choose global fetch for async one-shots, this for explicit timeout/maxBody control, scrape's Fetcher for crawling. */
    class HTTPClient implements DynResource {
        /** maxBody caps the buffered response (default 16 MiB). HTTPS verifies certificate and hostname against the platform trust store. */
        constructor(maxBody?: number);
        /** GET returning {status, statusText, ok, headers, body}. BLOCKS the thread, so it can never reach an App in this same process (its handlers need that thread) — use fetch() there. */
        get(url: string, headers?: Record<string, string>): HTTPResponse;
        /** POST with an optional body; same blocking contract as get(). */
        post(url: string, body?: BytesInput, headers?: Record<string, string>): HTTPResponse;
        /** Any method; same blocking contract as get(). */
        request(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): HTTPResponse;
        /** Called once per request with the RESOLVED IP between connect and the first request byte; return false or throw to refuse the connection. */
        onConnect?: (ip: string) => unknown;
        /** Per-read/per-write socket timeout in ms (0 disables); a buffered exchange is also bounded as a whole at 20x this value. */
        setTimeout(ms: number): void;
        /** Drops the current connection. */
        disconnect(): void;
        /** One exchange with the body left on the socket: the StreamedResponse's read(buf) pulls bytes on demand; maxBody still applies; close() releases the socket. */
        getStream(url: string, headers?: Record<string, string>): StreamedResponse;
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
        /** Header names as the server spelled them; repeated names join as "v1, v2". Over https an unframed body must end with close_notify, else "truncated body". */
        headers: Record<string, string>;
        body: string;
    }

    /** The getStream response: head fields plus dyna:stream's ByteSource shape (read/close), so pipe(), lines(), ndjson() and inflate() consume it directly. */
    interface StreamedResponse {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        /** The final url of the exchange (one hop: no redirect chasing). */
        url: string;
        contentType: string;
        /** Fills the caller's buffer and resolves with the count; 0 = end of body. Rejects on truncation, the max body, or malformed chunked framing. */
        read(buf: Uint8Array): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A static route table: path -> body or {status, contentType, body}. App runs JS handlers; HTTPServer and HTTPServerAsync serve these tables off-thread. */
    type StaticRoutes = Record<string, string | { status?: number; contentType?: string; body?: string }>;

    /** A static-route HTTP/1.1 server on its own reactor thread, so a blocking HTTPClient in the same script can call it; binds in the constructor. */
    class HTTPServer implements DynResource {
        /** Defaults: port 0 (ephemeral, read it back from `.port`), all interfaces, requestTimeoutMs 30000; a request is capped at 16 KiB (431/413); `workers` is echoed only. */
        constructor(opts?: {
            port?: number;
            host?: string;
            workers?: number;
            backlog?: number;
            requestTimeoutMs?: number;
            routes?: StaticRoutes;
        });
        /** Binds the listener and starts serving. */
        start(): void;
        /** Stops accepting, closes live connections, and joins the reactor thread. */
        stop(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A static-route server multiplexing every connection on one background kqueue/epoll thread; a request is capped at 1 MiB. */
    class HTTPServerAsync implements DynResource {
        /** Defaults: port 0 (ephemeral), all interfaces, idleTimeoutMs 30000, maxConns 8192 (0 = unbounded). */
        constructor(opts?: {
            port?: number;
            host?: string;
            backlog?: number;
            idleTimeoutMs?: number;
            maxConns?: number;
            routes?: StaticRoutes;
        });
        /** Binds the listener and starts serving. */
        start(): void;
        /** Stops accepting and drains the workers. */
        stop(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A routed application server: handlers are plain JS on the event-loop thread through the shared reactor; binds at start(). */
    class App implements DynResource {
        /** Defaults: port 0, all interfaces, idleTimeoutMs 30000, maxConns 8192, compress true (gzip when the client accepts it), metrics false. */
        constructor(opts?: { port?: number; host?: string; idleTimeoutMs?: number; maxConns?: number; compress?: boolean; metrics?: boolean;
            /** listen(2) backlog; integer 1..65535, default 1024. */
            backlog?: number;
            /** Validated (1..64) and echoed for config parity with HTTPServer; App is single-reactor, so it sizes nothing. Default 1. */
            workers?: number; });
        /** Registers a strict JSON-RPC 2.0 endpoint (typed route: bypasses use()). */
        rpc(path: string, methods: Record<string, (...args: unknown[]) => unknown>): this;
        // An rpc handler may return a ByteSource (or { stream, contentType?, status? }) instead of JSON: the reply is then chunked, outside the JSON-RPC envelope.

        /** Serves a document root at a URL prefix. `allow` lists extensions or media types (else 403); maxFileSize defaults to 32 MiB (413); `..` is 403, dotfiles 404; symlinks are never followed. */
        static(prefix: string, root: import("dyna:file").Path, opts?: { maxFileSize?: number; allow?: string[] }): this;
        /** Proxies a URL prefix to a host/port. */
        proxy(prefix: string, opts: { host?: string; port?: number }): this;
        /** Upload endpoint: the handler gets the saved path and {size, contentType}; a non-matching declared Content-Type is 415, a body over maxFileSize (default 16 MiB) is 413. */
        upload(path: string, opts: { dir?: import("dyna:file").Path; maxFileSize?: number; allow?: string[] }, handler: (savedPath: string, meta: { size: number; contentType: string }) => void): this;
        /** WebSocket endpoint. `upgrade` runs BEFORE the 101 with origin/host/cookie/protocol; a falsy return or throw answers 403 — check `origin` when cookies authenticate. */
        ws(path: string, handlers: { upgrade?: (req: { origin: string | null; host: string | null; cookie: string | null; protocol: string | null }) => unknown; open?: (socket: WsConn) => void; message?: (socket: WsConn, data: string | Uint8Array, isBinary: boolean) => void; close?: (socket: WsConn, code: number, reason: string) => void }): this;
        /** Server-sent events endpoint; open and close receive the stream. */
        sse(path: string, handlers: { open?: (stream: SseConn) => void; close?: (stream: SseConn) => void }): this;
        /** Dynamic route for one method. Pattern segments: literal, `:param` (one decoded segment) or a trailing `*rest`; first registered wins; a method mismatch is 405.
         *  The handler's return is the response: string (text/plain), bytes, {status?, body?, contentType?}, or any value as JSON; thenables are awaited; a throw is a 500. */
        get(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for POST. */
        post(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PUT. */
        put(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PATCH. */
        patch(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for DELETE (the method name cannot be a keyword). */
        del(pattern: string, handler: (req: AppRequest) => unknown): this;
        /** Appends SYNC middleware run before matched dynamic handlers with the same AppRequest; returning { response } short-circuits. Typed routes (rpc/static/ws/...) bypass it. */
        use(fn: (req: AppRequest) => { response?: unknown } | void): this;
        /** Binds the listener and starts serving. */
        start(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        /** The ctor's workers option (default 1); echoed only. */
        get workers(): number;
        /** The effective listen(2) backlog: the ctor option, or 1024. */
        get backlog(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Server-side WebSocket connection, handed to App.ws handlers. */
    interface WsConn {
        send(data: BytesInput): void;
        close(): void;
    }

    /** Server-side SSE stream, handed to the App.sse open handler. */
    interface SseConn {
        send(data: string): void;
        close(): void;
    }

    /** The request context handed to dynamic-route handlers and `app.use` middleware. */
    interface AppRequest {
        /** The method as sent; matching is exact and case-sensitive (HEAD is not aliased to GET). */
        method: string;
        /** The raw request path (percent-encoding as received), query stripped. */
        path: string;
        /** `:param` and `*rest` captures, percent-decoded; {} when the pattern has none. */
        params: Record<string, string>;
        /** The query string, percent-decoded (`+` is a space); a bare key is "", a repeated key keeps the LAST value. */
        query: Record<string, string>;
        /** Headers with lower-cased names; list fields join with ", ", Cookie with "; "; any other duplicate is a 400 before dispatch. */
        headers: Record<string, string>;
        /** The raw body as a string; the whole request is bounded at 1 MiB (body 413, headers 431, target 414). */
        body: string;
    }

    /** A WebSocket client. */
    class WsClient implements DynResource {
        /** Connects to the URL and dispatches open/message/close to the handlers. */
        constructor(url: string, handlers: { open?: (ws: WsClient) => void; message?: (ws: WsClient, data: string | Uint8Array, isBinary: boolean) => void; close?: (ws: WsClient, code: number, reason: string) => void });
        /** Sends a text frame (string) or binary frame (bytes). A closed connection silently drops the frame. */
        send(data: BytesInput): void;
        /** Performs polite teardown: a close frame first, then the connection. */
        close(): void;
        /** Performs polite teardown: a close frame first, then the connection. */
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** WHATWG fetch subset (no redirect/credentials/integrity keys). Follows at most 20 redirects; authorization/cookie headers are dropped when a hop leaves the origin; no cookie jar. */
    function fetch(input: string | Request, init?: RequestInit): Promise<Response>;
    /** The web-platform classes, the same objects as the globals. */
    const Request: RequestConstructor;
    const Response: ResponseConstructor;
    const Headers: HeadersConstructor;
    const AbortController: AbortControllerConstructor;
    const AbortSignal: AbortSignalConstructor;
    const FormData: FormDataConstructor;
}

/** dyna:net — everything under HTTP: TCP/UDP/unix sockets, TLS, DNS client and server, Redis/PostgreSQL/SQLite clients, rate limiting, metrics, an L4 proxy.
 *  Also re-exports dyna:http (the same objects). Network replies are untrusted: every decoder bounds what a peer can make it allocate. */
declare module "dyna:net" {
    /** Networking: addresses and CIDR prefixes, sockets, DNS, protocol clients, rate limiting and metrics, plus the shared HTTP surface re-exported from dyna:http. */

    // ---- HTTP surface re-exported from dyna:http: each name IS the same object, not a second definition ----

    /** WHATWG fetch; the same function object as dyna:http.fetch (identity verified). */
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

    /** A compiled CIDR prefix (contains/overlaps/masked/bits), parsed and masked once; unrelated to dyna:structures' string Trie. */
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
    /** A token bucket over a fixed-size, open-addressed table; the table cannot grow. */
    class RateLimiter {
        /** Token-bucket table keyed by string: `tokensPerSec` (alias `refill`) is required, `burst` (alias `capacity`) defaults to one second; 1024 fixed slots unless `grow: true`. */
        constructor(opts: { tokensPerSec?: number; refill?: number; burst?: number; capacity?: number; slots?: number; grow?: boolean });
        /** True when the key may proceed, consuming `cost` tokens (default 1); cost must be > 0. */
        allow(key: string, cost?: number): boolean;
        /** The key's current token count. */
        tokens(key: string): number;
        /** Clears one key, or the whole table when no key is given. */
        reset(key?: string): void;
        /** Running totals and table geometry. */
        readonly stats: { allowed: number; denied: number; slots: number; live: number; grew: number; tokensPerSec: number; burst: number };
    }

    /** A registry of counters, gauges and histograms with a Prometheus text scrape; at most 256 series (RangeError beyond). */
    const Metrics: {
        /** Increments a counter; a negative or NaN increment is refused. */
        counter(name: string, value?: number, labels?: Record<string, string>): void;
        /** Sets a gauge to a value. */
        gauge(name: string, value: number, labels?: Record<string, string>): void;
        /** Records an observation; buckets are 5ms..1s by default, or 1..6 increasing `opts.buckets` edges fixed at the series' first use. */
        histogram(name: string, value: number, labels?: Record<string, string>, opts?: { buckets: number[] }): void;
        /** The registry as Prometheus text exposition. */
        scrape(): string;
        /** Empties the registry; intended for tests. */
        reset(): void;
    };

    /* ---- DNS ---- */
    /** One decoded answer record: `name`, `type` and `ttl` are always present; the payload field depends on the type (A/AAAA -> address, CNAME/NS/PTR -> target, MX -> priority+exchange, TXT -> chunks). */
    interface DNSRecord {
        readonly name: string;
        readonly type: number;
        readonly ttl: number;
        readonly address?: string;
        readonly target?: string;
        readonly priority?: number;
        readonly exchange?: string;
        readonly chunks?: string[];
    }
    /** A UDP DNS client; answers are matched by connected socket, CSPRNG query ID and echoed question. */
    class DNSResolver implements DynResource {
        /** timeoutMs must fit uint32 (else RangeError). `{ttl}` seconds (at most 86400) enables the answer cache: hits return a fresh deep copy; no single-flight. */
        constructor(opts?: { server?: string; port?: number; timeoutMs?: number; /** answer-cache cap in seconds (1..86400); absent/0 disables caching */ ttl?: number });
        /** Queries a name; `type` is the record type (A is 1). The callback receives (err, records). */
        query(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): void;
        /** Resolves decoded answers (type defaults to A); with a trailing callback returns undefined and delivers (err, records). */
        lookup(name: string, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): Promise<DNSRecord[]> | undefined;
        lookup(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): Promise<DNSRecord[]> | undefined;
        /** Sugar over lookup(name, 5): the chain targets. */
        resolveCname(name: string): Promise<string[]>;
        /** Sugar over lookup(name, 15): {priority, exchange} pairs. */
        resolveMx(name: string): Promise<{ priority: number; exchange: string }[]>;
        /** Sugar over lookup(name, 16): one chunk array per record. */
        resolveTxt(name: string): Promise<string[][]>;
        /** Sugar over lookup(name, 2): the nameservers. */
        resolveNs(name: string): Promise<string[]>;
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
        /** Connect through TLS; certificates are verified against the system trust store with hostname checking (`ca` pins a CA PEM file). */
        tls?: boolean;
        /** CA certificate PEM file pinned for the TLS handshake; omitted uses the system trust store. */
        ca?: string;
        binary?: boolean;
        bigint?: boolean;
        /** Caps any single bulk payload (and the element count of an aggregate) in a reply; default 64 MiB. */
        maxReplyBytes?: number;
        maxPending?: number;
        connectTimeoutMs?: number;
        commandTimeoutMs?: number;
    }
    /** A Redis client: commands return promises matched to replies by strict FIFO. Replies are UNTRUSTED: over-cap or malformed ones are refused by name and the connection torn down. */
    class Redis implements DynResource {
        constructor(opts?: RedisOptions);
        /** Sends one command (RESP3 when available). `HELLO` and `CLIENT REPLY` are refused: they change how many replies arrive, breaking positional matching. */
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        /** One round trip for many commands, one reply each; (P|S)SUBSCRIBE forms throw here (they answer per channel): issue them with command(). */
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
        /** Connect through TLS via SSLRequest; a server answering `N` ends the connection — there is no silent fallback to plaintext. */
        tls?: boolean;
        /** CA certificate PEM file pinned for the TLS handshake; omitted uses the system trust store. */
        ca?: string;
        raw?: boolean;
        bytes?: boolean;
        textResults?: boolean;
        /** Prepared-statement cache capacity (default 64); cached after `prepareAfter` (default 2) executions. */
        statementCacheSize?: number;
        prepareAfter?: number;
        bigint?: boolean;
        /** Default false: MD5/cleartext auth is refused on a plaintext connection unless true (accepted under `tls: true`). */
        insecureAuth?: boolean;
        maxMessageBytes?: number;
        /** Bound on ONE result set (wire bytes plus 64 per row), default 1 GiB; past it the connection fails with "result exceeds maxResultBytes". */
        maxResultBytes?: number;
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
        /** Runs one statement (extended protocol when params are given); `opts.maxRows` rejects that query when exceeded, leaving the connection usable. */
        query(sql: string, params?: unknown[], opts?: { maxRows?: number }): Promise<unknown>;
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
        /** Streams rows in constant memory. queryIter is NATIVE (a DECLARE CURSOR/FETCH FORWARD driver); a `break` always ends its transaction; {batch} rows per fetch, {maxRows} in total.
         *  Lazy: no IO until the first next(). src/pool.js no longer needs importing for this: its installer stands down when the native method exists. */
        queryIter(sql: string, params?: unknown[], opts?: { batch?: number; maxRows?: number }): AsyncIterableIterator<Record<string, unknown>>;
        /** The server parameters from startup (server_version, client_encoding, ...). */
        readonly parameters: Record<string, string>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    // PgPool/RedisPool are NOT exports of dyna:net: they are pure-JS classes imported by file path (see the src/pool.js block).

    /** SQLite options. */
    interface SQLiteOptions {
        readonly?: boolean;
        bigint?: boolean;
    }
    /** A SQLite handle (defensive mode, trusted_schema off); values are bound, never interpolated; exec() returns rows changed; --timeout-ms interrupts a running statement. */
    class SQLite implements DynResource {
        constructor(path: string, opts?: SQLiteOptions);
        /** Runs a statement and returns one object per row; duplicate column names collapse to the last one, so alias them in the SELECT. */
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
    /** A TCP connection handed to a handler; write is refused on a TLS connection before the handshake, and refused with a TypeError past the connection's high-water mark. */
    interface TCPConn {
        write(data: BytesInput): void;
        /** Bytes queued for this connection that the kernel has not taken (0 on a closed connection). */
        readonly bufferedAmount: number;
        close(): void;
    }
    /** TCP server and client event handlers. */
    interface TCPHandlers {
        /** Fires when a connection lands; on failure conn is null and err names the reason. */
        connect?: (conn: TCPConn | null, err: string | null) => void;
        /** Fires with a copy of the received bytes. */
        data?: (conn: TCPConn, bytes: Uint8Array) => void;
        /** Fires exactly once per emptying of the connection's outbound queue after a write left bytes queued; the producer's signal to resume. */
        drain?: (conn: TCPConn) => void;
        close?: (conn: TCPConn) => void;
    }
    /** Server TLS: cert/key required; requestCert turns on mTLS (needs ca). */
    interface TCPServerTLSOptions {
        cert: string;
        key: string;
        alpn?: string;
        ca?: string;
        requestCert?: boolean;
    }
    /** Client TLS: the verified name defaults to host; cert/key give mTLS; `ca` pins a CA; rejectUnauthorized:false disables verification. */
    interface TCPClientTLSOptions {
        ca?: string;
        servername?: string;
        alpn?: string | string[];
        minVersion?: string;
        rejectUnauthorized?: boolean;
        cert?: string;
        key?: string;
    }
    /** Options for TCPServer.connect; with a unix-socket `path`, TLS needs an explicit servername or rejectUnauthorized:false. */
    interface TCPConnectOptions {
        host?: string;
        port?: number;
        path?: string;
        connectTimeoutMs?: number;
        maxConnections?: number;
        idleTimeoutMs?: number;
        tls?: boolean | TCPClientTLSOptions;
        /** Outbound queue cap per connection in bytes (1..2^30); a write past it is refused with a TypeError until drain fires. */
        highWaterMark?: number;
    }
    /** A TCP server, and (via the static connect) a TCP client; runs on the shared io reactor. */
    class TCPServer implements DynResource {
        constructor(opts?: { port?: number; path?: string; maxConnections?: number; idleTimeoutMs?: number; tls?: TCPServerTLSOptions; /** outbound queue cap per connection in bytes (1..2^30), default 4 MiB */ highWaterMark?: number });
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
    /** An L4 byte reverse proxy with no JS on the data path. Defaults: maxConns 8192, connectTimeoutMs 10000, no idle timeout, listening on 0.0.0.0. */
    class TCPProxy implements DynResource {
        constructor(opts: { port: number; host?: string; upstream: TCPUpstream | TCPUpstream[]; maxConns?: number; idleTimeoutMs?: number; connectTimeoutMs?: number });
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

    /** Happy Eyeballs (simplified RFC 8305): both address families race, first success wins; fallbackMs is the whole-race deadline. `tls` options are required for TLS endpoints. */
    function connectHappy(host: string, port: number, opts?: { fallbackMs?: number; tls?: boolean | TCPClientTLSOptions }, handlers?: TCPHandlers): TCPServer;

    /** Process-wide count of throws swallowed from net event handlers (TCP connect/data/close, UDP message): the only diagnostic for them. */
    function swallowedHandlerThrows(): number;
}


/** src/pool.js — PgPool and RedisPool: pure-JS pooling over the dyna:net clients, shipped as a source file, NOT a builtin.
 *  Import by relative path (`import { PgPool } from "./src/pool.js"`); the wildcard module name below matches every such spelling. */
declare module "*/src/pool.js" {
    /** Connection-pool options shared by PgPool and RedisPool. */
    interface PoolOptions {
        /** Max simultaneous connections; the pool refuses to grow past this. */
        size?: number;
        /** Idle connections are evicted after this many milliseconds (0 = never). */
        idleMs?: number;
        /** How long acquire() waits for a free connection before timing out. */
        acquireTimeoutMs?: number;
    }
    interface PgPoolOptions extends PoolOptions {
        host?: string;
        port?: number;
        user?: string;
        password?: string;
        database?: string;
        path?: string;
        tls?: boolean;
        /** a CA bundle path/PEM pinned by every pooled connection (same contract as PostgreSQL's `ca`). */
        ca?: string;
        stmtCacheMax?: number;
        maxPending?: number;
    }
    /** A bounded pool of PostgreSQL clients (size 4, idleMs 30000, acquireTimeoutMs 5000); query/pipeline auto acquire-release, with(fn) pins one client; release() ignores a connection already free. */
    class PgPool {
        constructor(opts?: PgPoolOptions);
        readonly stats: { total: number; free: number; used: number; waiting: number; closed: boolean };
        query(sql: string, params?: unknown[], opts?: { maxRows?: number }): Promise<unknown>;
        /** One round trip; after a failed member the server skips the rest, and a later re-run re-prepares. */
        pipeline(stmts: [string, ...unknown[]][]): Promise<unknown[]>;
        /** Pins one client from the pool for a transaction or multi-step flow. */
        with<T>(fn: (client: import("dyna:net").PostgreSQL) => Promise<T>): Promise<T>;
        close(): Promise<void>;
    }
    interface RedisPoolOptions {
        host?: string;
        port?: number;
        path?: string;
        user?: string;
        password?: string;
        protocol?: 2 | 3;
        size?: number;
        acquireTimeoutMs?: number;
    }
    /** A bounded pool of Redis clients (size 4, acquireTimeoutMs 5000); the ACL user is spelled `user`; a failed command retires its connection. */
    class RedisPool {
        constructor(opts?: RedisPoolOptions);
        readonly stats: { total: number; free: number; used: number; waiting: number; closed: boolean };
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        pipeline(commands: (string | number | Uint8Array | ArrayBuffer)[][]): Promise<unknown[]>;
        close(): Promise<void>;
    }
}


/** dyna:json — JSON editing: Pointer (RFC 6901) addressing, Patch (RFC 6902) with copy-on-write, buffered NDJSON. */
declare module "dyna:json" {
    // Neighbours: stable stringify (RFC 8785), JSON5 and JSONPath live in dyna:encoding; streaming NDJSON is dyna:stream's ndjson().

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
        /** Runs the six RFC 6902 ops on a PRIVATE deep copy of plain data; non-plain values (Date/RegExp/Map/TypedArray) are shared by reference and the input is never written. */
        apply(doc: unknown, ops: { op: string; path: string; value?: unknown; from?: string }[]): unknown;
    };

    /** NDJSON, buffer-at-once (the streaming form is dyna:stream's ndjson); lines split on LF with one trailing CR stripped; blank lines are skipped. */
    namespace Ndjson {
        /** Parses every non-blank line; a malformed one throws SyntaxError naming its 1-based line number in the text. */
        function parse(text: string): unknown[];
        /** One compact JSON document per line with a trailing newline; an item with no JSON form throws TypeError naming its index. */
        function stringify(items: unknown[]): string;
    }
}

/** dyna:log — leveled structured logging: JSON lines or text, to stderr, a rolling file or a custom sink; control characters are escaped so a value cannot forge a line. */
declare module "dyna:log" {
    /** Log levels, least to most severe; "silent" disables output. */
    type LogLevel = "trace" | "debug" | "info" | "warn" | "error" | "fatal" | "silent";
    /** A leveled logger; child() derives one that adds fields to every line. */
    class Logger {
        /** Creates a logger; the ctor bag, its rollover sub-bag and child()'s options are all strict. */
        constructor(opts?: {
            level?: LogLevel;
            name?: string;
            timestamp?: "epoch" | "iso" | false;
            base?: Record<string, unknown>;
            /** Output path, or a sink `(line) => void` receiving each formatted line unbuffered; rollover needs a path. Default: stderr. */
            dest?: string | ((line: string) => void);
            /** Emit sampling: every Nth entry passing the level gate is written (deterministic counter per logger). Default 1. */
            sample?: number;
            /** Create dest's parent directories. */
            mkdir?: boolean;
            /** Batch file output: true = 64 KiB, or a byte size 1..1048576; flushed at normal exit (a crash loses the tail). */
            buffer?: boolean | number;
            /** Add "pid" to every line. */
            pid?: boolean;
            /** Add "hostname" to every line. */
            hostname?: boolean;
            /** "json" (default) or "text" (`LEVEL name: msg k=v`). */
            format?: "json" | "text";
            /** File rollover (needs dest). Lazy: checked before each emit. */
            rollover?: {
                /** Rotate at this size: bytes, or "500k"/"10m"/"2g". */
                size?: number | string;
                /** Rotate on this interval: "daily", "hourly", or milliseconds. */
                frequency?: "daily" | "hourly" | number;
                /** Rotated files to keep (plus the active one). */
                count?: number;
                /** Keep `dest` as a symlink to the active file; a pre-existing symlink is replaced, never written through. */
                symlink?: boolean;
            };
        });
        /** Each emits one line at its level. Shapes: (msg), (fields, msg), (err, msg), (err, fields, msg); a field colliding with a frame key (time/level/name/pid/hostname/msg/err) is dropped. */
        trace(msg: unknown, ...fields: unknown[]): void;
        debug(msg: unknown, ...fields: unknown[]): void;
        info(msg: unknown, ...fields: unknown[]): void;
        warn(msg: unknown, ...fields: unknown[]): void;
        error(msg: unknown, ...fields: unknown[]): void;
        fatal(msg: unknown, ...fields: unknown[]): void;
        /** A new Logger with fields appended to the base prefix; shares the parent's destination. The optional second argument is a strict options bag: `{ level }` only (fields is NOT a bag). */
        child(fields: Record<string, unknown>, opts?: { level?: LogLevel }): Logger;
        /** Whether the level passes the current threshold. */
        enabled(level: LogLevel): boolean;
        /** Land any buffered file output now. A no-op on stderr. */
        flush(): void;
        /** Get/set the level. */
        level: LogLevel;
    }

    /** Returns a printer active only when $DEBUG matches the namespace: comma-separated patterns, `-` negates, one `*` wildcard, last match wins. */
    function Debug(namespace: string): (...args: unknown[]) => void;
}

/** dyna:matcher — text search and similarity: compiled single- and multi-pattern (Aho-Corasick) matchers, edit distances, tries, Bloom filters, diffs. */
declare module "dyna:matcher" {
    /** A compiled single-pattern matcher. */
    class Matcher {
        /** `algo` is "kmp" (default) or "bmh" (alias "boyer-moore"); a pure-ASCII pattern searches the subject's bytes directly. */
        constructor(pattern: string, opts?: { algo?: "kmp" | "bmh" | "boyer-moore" });
        /** Code-unit offset of the first match from `fromIndex` (String.prototype.indexOf conventions), or -1. */
        firstIn(text: string, fromIndex?: number): number;
        /** True when a pattern occurs in `text`. */
        test(text: string): boolean;
        /** Number of matches in `text`. */
        countIn(text: string): number;
        /** Every match offset. */
        allIn(text: string): number[];
        /** Non-overlapping left-to-right replacement. */
        replaceAllIn(text: string, repl: string): string;
        readonly length: number;
        readonly algo: string;
    }

    /** A byte-trie Aho-Corasick multi-pattern matcher. Identical patterns are deduplicated: N identical hits at one position emit one, at the first index. */
    class MultiMatcher {
        constructor(patterns: string[]);
        /** The pattern index and offset of the earliest hit, or null. */
        firstIn(text: string): { index: number; at: number } | null;
        /** True when a pattern occurs in `text`. */
        test(text: string): boolean;
        /** Every emitted hit (overlapping matches each count). */
        countIn(text: string): number;
        /** Every hit as { index: pattern number, at: position }, ordered by end position (longest first at one end). */
        allIn(text: string): { index: number; at: number }[];
        /** Replaces non-overlapping hits left to right in one pass; `repl` is a string or repl(match, patternIndex). */
        replaceAllIn(text: string, repl: string | ((match: string, index: number) => string)): string;
        readonly size: number;
        readonly states: number;
    }

    /** Exact edit distance in code points, Myers bit-parallel below 64. */
    function Levenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Bigram multiset similarity in [0, 1]. */
    function DiceCoefficient(a: string, b: string): number;
    /** Jaro similarity with Winkler's prefix boost (prefix capped at 4, scale 0.1) in [0, 1]; code-point operands. */
    function JaroWinkler(a: string, b: string): number;
    /** Restricted Damerau-Levenshtein (optimal string alignment) in code points; exact while <= max, max + 1 beyond. */
    function DamerauLevenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** A diff hunk: -1 deleted, 1 inserted, 0 common. */
    interface DiffHunk {
        op: -1 | 0 | 1;
        text: string;
    }
    /** Myers diff by character; past its step budget throws RangeError("inputs too dissimilar"). */
    function DiffChars(a: string, b: string): DiffHunk[];
    /** Myers diff tokenised by word; same budget and "inputs too dissimilar" RangeError. */
    function DiffWords(a: string, b: string): DiffHunk[];
    /** Myers diff tokenised by line; same budget and "inputs too dissimilar" RangeError. */
    function DiffLines(a: string, b: string): DiffHunk[];
}

/** dyna:mathx — the math toolbox: constants, special functions, combinatorics, number theory, bit ops, linear algebra, and `stats` reductions.
 *  Statistics come in three layers: Array.prototype conveniences, mathx.stats (compensated sums, Float64Array fast path), DataFrame verbs (columnar, masked, histograms). */

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
    /** The gap to the next double away from zero (eps() is eps(1)); eps(realmax) is 2^971. */
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
    /** The C99 truncated remainder. */
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
    /** The next representable double after `a` in the direction of `b`. */
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
    /** Computed through lgamma, so it does not overflow for moderate arguments the way `tgamma(a)*tgamma(b)/tgamma(a+b)` does. */
    function beta(a: number, b: number): number;
    /** Natural log of the beta function. */
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
    /** Inverse of the regularized incomplete beta function. */
    function betaincinv(p: number, a: number, b: number): number;
    /** E1(x), defined for x > 0. */
    function expint(x: number): number;

    /** Integer order only. |order| above 16777216 that would not underflow at x is a RangeError. */
    function besselj(n: number, x: number): number;
    /** Bessel function of the second kind, Y_n(x). */
    function bessely(n: number, x: number): number;
    /** Real order. */
    function besseli(nu: number, x: number): number;
    /** Real order. Orders above 1e6 (and non-finite) answer +Infinity -- the limit of K_nu(x) as nu grows, for any finite x > 0 -- instead of running an O(nu) recurrence. */
    function besselk(nu: number, x: number): number;
    /** I_nu(x) e^-x. */
    function besseliScaled(nu: number, x: number): number;
    /** K_nu(x) e^x; the 1e6 order clamp (+Infinity above it) applies here too. */
    function besselkScaled(nu: number, x: number): number;
    /** Hankel function J_n ± i Y_n as [re, im]; kind 1|2; the order must be an int32. */
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

    /** Both arguments within the int64 range; wider BigInts are refused. */
    function gcd(a: number | bigint, b: number | bigint): bigint;
    /** Both arguments within the int64 range; wider BigInts are refused. */
    function lcm(a: number | bigint, b: number | bigint): bigint;
    /** n! exactly, capped at 10000. */
    function factorial(n: number): bigint;
    /** The magnitude as an unsigned BigInt; magnitudes of 2^64 or more are refused, not truncated. */
    function abs(n: bigint): bigint;
    /** Minimum bits to represent the magnitude (bitLen(0n) is 0); magnitudes of 2^64 or more are refused. */
    function bitLen(n: bigint): number;
    /** Set bits in the magnitude; magnitudes of 2^64 or more are refused. */
    function popcount(n: bigint): number;

    /** Binomial coefficient built multiplicatively in min(k, n-k) steps; huge orders return Infinity promptly, never hang. */
    function nchoosek(n: number, k: number): number;
    /** Every permutation, reverse lexicographic, at most 8 elements. */
    function perms(v: number[]): number[][];
    /** Rational approximation by continued fractions within relative tolerance. */
    function rat(x: number, tol?: number): [number, number];

    /** n points INCLUSIVE of both ends (the last is exactly b); Number.range is the exclusive-end stepper. */
    function linspace(a: number, b: number, n?: number): number[];
    /** 10^t over the linspace grid. */
    function logspace(a: number, b: number, n?: number): number[];
    /** Array-like inputs are capped at 16777216 elements (RangeError beyond). */
    function cumsum(v: number[]): number[];
    /** The running product. */
    function cumprod(v: number[]): number[];
    /** Adjacent differences, one element shorter; array-like inputs capped at 16777216 elements (RangeError beyond). */
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
        /** Rotate right; k reduces modulo the width, so a negative k rotates left. */
        function rotateRight8(x: number, k: number): number;
        function rotateRight16(x: number, k: number): number;
        function rotateRight32(x: number, k: number): number;
        function rotateRight64(x: bigint, k: number): bigint;
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

    /** Reductions over a Float64Array (fast path) or any array-like (capped at 16777216 elements). sum is Neumaier-compensated; median/quantile use R-7.
     *  Unlike DataFrame: empty input and zero variance THROW RangeError and a NaN element answers NaN; min/max follow Math.min/Math.max. */
    namespace stats {
        /** Compensated sum; empty array sums to +0. */
        function sum(a: ArrayLike<number>): number;
        /** sum / n; the empty array is 0/0 = NaN. */
        function mean(a: ArrayLike<number>): number;
        /** Two-pass variance, sample (n-1) by default; opts.pop divides by n. */
        function variance(a: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** sqrt(variance) under the same opts and gates. */
        function stddev(a: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** quantile(a, 0.5); empty throws RangeError. */
        function median(a: ArrayLike<number>): number;
        /** R-7 linear quantile (DataFrame.QUANTILE's convention); q in [0, 1] and a non-empty array, else RangeError. */
        function quantile(a: ArrayLike<number>, q: number): number;
        /** Math.min semantics: NaN poisons; empty is +Infinity. */
        function min(a: ArrayLike<number>): number;
        /** Math.max semantics: NaN poisons; empty is -Infinity. */
        function max(a: ArrayLike<number>): number;
        /** Sample covariance (n-1); opts.pop divides by n; a length mismatch throws RangeError. */
        function cov(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** Pearson r clamped to [-1, 1]; a length mismatch or zero variance throws RangeError. */
        function corr(a: ArrayLike<number>, b: ArrayLike<number>): number;
    }

    /** Compiles an arithmetic string to an RPN program; no eval, no scope. */
    class Expression {
        /** Compiles an arithmetic string to an RPN program over doubles with shunting-yard; there is no eval and no scope. */
        constructor(text: string);
        /** Free variables in first-use order. */
        variables(): string[];
        /** Evaluates, reading only own data properties. */
        eval(vars?: Record<string, number>): number;
    }
}

/** dyna:ml — models in C over contiguous doubles: linear/logistic regression, trees, forests, boosting, SVC, k-NN, k-means, DBSCAN, GMM, PCA, scalers, Pipeline, model selection.
 *  Estimators share fit/predict/serialize and are native resources; fit() refuses non-finite X (XGB* treat NaN as missing) and honours --timeout-ms. */
declare module "dyna:ml" {
    /** A matrix as an array of rows, or a flat Float64Array plus (rows, cols). */
    type Matrix = number[][] | Float64Array;
    /** Targets or labels, one per row. */
    type Target = number[] | Float64Array;

    /** Shared estimator options. */
    interface TreeOpts {
        /** Trees (forest) or boosting rounds; default 100. Fit time is linear in it; above 100000 is refused at construction. */
        nEstimators?: number;
        /** 0 or absent means the 1024 hard ceiling; above 1024 is REFUSED (trees grow by C recursion). */
        maxDepth?: number;
        minSamplesSplit?: number;
        minSamplesLeaf?: number;
        maxFeatures?: number;
        /** 0 (default) selects the EXACT splitter; above 0 the HISTOGRAM splitter with at most this many bins per feature: much faster, slightly coarser (255 is typical). */
        maxBins?: number;
        seed?: number;
    }
    /** Trailing fit() options: `sampleWeight` gives one weight per row. */
    interface FitOpts {
        sampleWeight?: number[] | Float64Array;
    }

    /** Closed-form ordinary least squares; a CSR X gives bit-identical coefficients to the dense fit. */
    class LinearRegression implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): LinearRegression;
        static load(path: import("dyna:file").Path): LinearRegression;
        /** The ridge scales with the mean weight, so scaling all weights leaves the fit unchanged. */
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Array of fitted weights (`[]` before fit). */
        readonly coef: number[];
        /** Fitted intercept (`0` before fit). */
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
        /** Defaults: learningRate 0.1, maxIter 3000, tol 1e-4, l1 = l2 = 0; `C` is the inverse penalty strength used with `penalty` (scikit-learn's convention). */
        constructor(opts?: { learningRate?: number; maxIter?: number; tol?: number; l1?: number; l2?: number; C?: number; penalty?: "l1" | "l2" | "elasticnet" | "none"; classWeight?: "balanced" });
        static deserialize(bytes: Uint8Array | ArrayBuffer): LogisticRegression;
        static load(path: import("dyna:file").Path): LogisticRegression;
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
        /** Distinct labels, ascending. */
        readonly classes: number[];
        /** Weight matrix (1 row binary, one per class multinomial). */
        readonly coef: number[][];
        /** Scalar (binary) or Array (multinomial). */
        readonly intercept: number | number[];
        /** Iterations run. */
        readonly nIter: number;
        /** Whether the gradient-norm stop fired. */
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
        /** nClusters defaults to 8. A negative seed keeps the non-reproducible default; pass a non-negative integer (or `{ seed }`) for reproducibility. */
        constructor(nClusters?: number, seed?: number | { seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): KMeans;
        static load(path: import("dyna:file").Path): KMeans;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Summed squared distance to assigned centroids of the last fit (`0` before fit). */
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
        /** Defaults: kernel "rbf", C 1, gamma 1/cols, coef0 0, degree 3, tol 1e-3, maxIter 1000; more than two classes train one-vs-rest (up to 256). */
        constructor(opts?: { kernel?: "linear" | "rbf" | "poly"; C?: number; gamma?: number; coef0?: number; degree?: number; tol?: number; maxIter?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): SVC;
        static load(path: import("dyna:file").Path): SVC;
        /** Needs at least two distinct labels. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** A FLAT Float64Array comes back when X is flat. */
        decisionFunction(X: Matrix | CSR, rows?: number, cols?: number): number[] | number[][] | Float64Array;
        /** Total support vectors across all binary machines (`0` before fit). */
        readonly nSupportVectors: number;
        /** Labels in ascending order. */
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
        /** Defaults: k 3, seed 12345, maxIter 200, tol 1e-3, regCovar 1e-6 (added to every variance so a component cannot collapse). */
        constructor(k?: number, opts?: { seed?: number; maxIter?: number; tol?: number; regCovar?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianMixture;
        static load(path: import("dyna:file").Path): GaussianMixture;
        /** Needs at least `k` rows. */
        fit(X: Matrix, rows?: number, cols?: number): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** A FLAT Float64Array comes back when X is flat. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][] | Float64Array;
        /** Mixing weights (sum 1). */
        readonly weights: number[];
        readonly means: number[][];
        /** K×cols, regularised. */
        readonly variances: number[][];
        /** Total log-likelihood of the last fit (`0` before fit). */
        readonly logLikelihood: number;
        /** EM iterations run. */
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
        /** varSmoothing defaults to 1e-9, scaled by the largest variance (scikit-learn's floor). */
        constructor(varSmoothing?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianNB;
        static load(path: import("dyna:file").Path): GaussianNB;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** A FLAT Float64Array comes back when X is flat. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][] | Float64Array;
        /** Labels seen at fit. */
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
        /** TreeOpts defaults: maxDepth 0 (unlimited up to 1024), minSamplesSplit 2, minSamplesLeaf 1, maxBins 0 (exact), seed 12345; a single tree ignores nEstimators. */
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeClassifier;
        static load(path: import("dyna:file").Path): DecisionTreeClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature, normalised to sum 1 (zeros when nothing ever split). */
        readonly featureImportances: number[];
        /** Deepest tree (0 before fit). */
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        /** Deepest tree (0 before fit). */
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
        /** TreeOpts defaults: nEstimators 100, maxDepth 0, minSamplesSplit 2, minSamplesLeaf 1, maxBins 0 (exact), seed 12345. */
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestClassifier;
        static load(path: import("dyna:file").Path): RandomForestClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for regression. Shared options (nEstimators <= 100000, maxDepth <= 1024, maxBins, seed) follow TreeOpts. */
    class GradientBoostingRegressor implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 3, learningRate 0.1, subsample 1, seed 12345. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingRegressor;
        static load(path: import("dyna:file").Path): GradientBoostingRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for classification. Shared options (nEstimators <= 100000, maxDepth <= 1024, maxBins, seed) follow TreeOpts. */
    class GradientBoostingClassifier implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 3, learningRate 0.1, subsample 1, seed 12345. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingClassifier;
        static load(path: import("dyna:file").Path): GradientBoostingClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
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
        /** Defaults: nEstimators 100, maxDepth 6, learningRate 0.3, lambda 1, alpha 0, gamma 0, minChildWeight 1, validationFraction 0.1, earlyStoppingRounds 0. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBRegressor;
        static load(path: import("dyna:file").Path): XGBRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        /** The rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit). */
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
        /** Defaults: nEstimators 100, maxDepth 6, learningRate 0.3, lambda 1, alpha 0, gamma 0, minChildWeight 1, validationFraction 0.1, earlyStoppingRounds 0. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBClassifier;
        static load(path: import("dyna:file").Path): XGBClassifier;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): number[][];
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        /** The rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit). */
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Principal components by cyclic Jacobi; fit() refuses (RangeError) when 60 * nFeatures^3 exceeds 2^36 (~1100 features). */
    class PCA implements DynResource {
        /** nComponents 0 (default) keeps every feature; `whiten` defaults to false. */
        constructor(nComponents?: number, whiten?: boolean);
        static deserialize(bytes: Uint8Array | ArrayBuffer): PCA;
        static load(path: import("dyna:file").Path): PCA;
        fit(X: Matrix, rows?: number, cols?: number): this;
        /** Centred rows projected onto the components. */
        transform(X: Matrix, rows?: number, cols?: number): number[][];
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. Never allocates. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): number;
        /** A FLAT Float64Array comes back when X is a flat Float64Array (rows x cols); nested input yields number[][]. */
        fitTransform(X: Matrix, rows?: number, cols?: number): number[][] | Float64Array;
        /** The projection back into feature space (used with `nComponents` < features to reconstruct). */
        inverseTransform(X: Matrix, rows?: number, cols?: number): number[][];
        /** NComponents × features (unit rows). */
        readonly components: number[][];
        /** Per-feature training mean. */
        readonly mean: number[];
        /** Eigenvalues of the sample (n-1) covariance, retained components. */
        readonly explainedVariance: number[];
        /** Each divided by the total variance (sums to ~1). */
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
        /** k defaults to 5; weights is "uniform" (default) or "distance". */
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNClassifier;
        static load(path: import("dyna:file").Path): KNClassifier;
        /** Requires at least `k` rows. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours regressor. */
    class KNRegressor implements DynResource {
        /** k defaults to 5; weights is "uniform" (default) or "distance". */
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNRegressor;
        static load(path: import("dyna:file").Path): KNRegressor;
        /** Requires at least `k` rows. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. Never allocates. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Density-based clustering with a grid index for low-dimensional data. */
    class DBScan implements DynResource {
        /** eps defaults to 0.5, minPts to 5. */
        constructor(eps?: number, minPts?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DBScan;
        static load(path: import("dyna:file").Path): DBScan;
        /** No weighted form; `sampleWeight` throws. */
        fit(X: Matrix, rows?: number, cols?: number): this;
        /** Array of per-row cluster ids, `-1` for noise (`[]` before fit). */
        readonly labels: number[];
        /** Cluster count (0 before fit). */
        readonly nClusters: number;
        /** The eps the model was constructed with. */
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
        /** A FLAT Float64Array comes back when X is a flat Float64Array (rows x cols); nested input yields number[][]. */
        transform(X: Matrix, rows?: number, cols?: number): number[][] | Float64Array;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. Never allocates. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): number;
        /** A FLAT Float64Array comes back when X is flat. */
        fitTransform(X: Matrix, rows?: number, cols?: number): number[][] | Float64Array;
        /** `x·std + mean`. Output shape mirrors the input shape. */
        inverseTransform(X: Matrix, rows?: number, cols?: number): number[][];
        /** Per-feature means. */
        readonly mean: number[];
        /** Per-feature standard deviations (`1.0` for constant columns). */
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
        /** Refuses `sampleWeight` (min and max are order statistics no positive weight can change). */
        fit(X: Matrix, rows?: number, cols?: number): this;
        /** A FLAT Float64Array comes back when X is flat. */
        transform(X: Matrix, rows?: number, cols?: number): number[][] | Float64Array;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. Never allocates. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): number;
        /** A FLAT Float64Array comes back when X is flat. */
        fitTransform(X: Matrix, rows?: number, cols?: number): number[][] | Float64Array;
        /** `x·(max−min) + min`. Output shape mirrors the input shape. */
        inverseTransform(X: Matrix, rows?: number, cols?: number): number[][];
        /** Per-column minima. */
        readonly dataMin: number[];
        /** Per-column maxima. */
        readonly dataMax: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An immutable compressed sparse row matrix (scipy layout). Every predict* accepts one and matches the dense answer bit for bit; only Linear/LogisticRegression fit one.
     *  Duplicate columns in a row SUM; the ctor validates everything: TypeError on shape/length mismatch, RangeError otherwise. */
    class CSR implements DynResource {
        constructor(values: number[] | Float64Array, columns: number[] | Int32Array, rowPointers: number[] | Int32Array, cols: number);
        /** Drops exact zeros from a dense matrix; a non-finite value is a RangeError naming the cell. */
        static fromDense(X: Matrix, rows?: number, cols?: number): CSR;
        /** The dense form (duplicate columns sum); throws if it does not fit in memory. */
        toDense(): number[][];
        /** Row i as a dense Array (duplicate columns sum). */
        row(i: number): number[];
        readonly rows: number;
        readonly cols: number;
        /** Nonzero count. */
        readonly nnz: number;
        /** `nnz / (rows·cols)`. */
        readonly density: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** DBScan is NOT a PipelineStage: it fits but cannot transform/predict, so a Pipeline has no position for it. */
    type PipelineStage = LinearRegression | LogisticRegression | KMeans | SVC
        | GaussianMixture | GaussianNB | DecisionTreeClassifier | DecisionTreeRegressor
        | RandomForestClassifier | RandomForestRegressor | GradientBoostingRegressor
        | GradientBoostingClassifier | XGBRegressor | XGBClassifier | PCA
        | KNClassifier | KNRegressor | StandardScaler | MinMaxScaler;

    /** A composition of feature stages and a final estimator. */
    class Pipeline implements DynResource {
        constructor(stages: PipelineStage[]);
        fit(X: Matrix, y: Target): this;
        /** Pushes X through the stages, ask the last one. */
        predict(X: Matrix): number[];
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix): number[][];
        /** Pushes through every stage that can transform, stopping before a final bare estimator. */
        transform(X: Matrix): number[][];
        /** The i-th stage (negative counts from the end). */
        stage(i: number): PipelineStage;
        readonly length: number;
        readonly fitted: boolean;
        /** The final stage. */
        readonly estimator: PipelineStage;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Mean of squared errors. */
    function meanSquaredError(yTrue: Target, yPred: Target): number;
    /** The mean absolute error. */
    function meanAbsoluteError(yTrue: Target, yPred: Target): number;
    /** Coefficient of determination; a constant yTrue scores 1.0 for exact, else 0.0. */
    function r2Score(yTrue: Target, yPred: Target): number;
    /** The fraction of equal pairs. */
    function accuracy(yTrue: Target, yPred: Target): number;
    /** Mean negative log-likelihood; yPred may be a class matrix or a binary vector. */
    function logLoss(yTrue: Target, yPred: Target | Target[]): number;
    /** Confusion matrix indexed [true][pred]; labels are integers 0..4095 (dense nLabels x nLabels). */
    function confusionMatrix(yTrue: Target, yPred: Target): number[][];
    /** Binary-classification metrics; `positive` names the positive class label. */
    function precision(yTrue: Target, yPred: Target, positive?: number): number;
    /** The recall. Defaults: positive 1. */
    function recall(yTrue: Target, yPred: Target, positive?: number): number;
    /** The F1 score. Defaults: positive 1. */
    function f1(yTrue: Target, yPred: Target, positive?: number): number;
    /** The specificity. Defaults: positive 1. */
    function specificity(yTrue: Target, yPred: Target, positive?: number): number;
    /** `(recall + specificity)/2`. Defaults: positive 1. */
    function balancedAccuracy(yTrue: Target, yPred: Target, positive?: number): number;
    /** The Matthews correlation coefficient, the one number that stays honest on imbalanced data. Defaults: positive 1. */
    function matthewsCorrcoef(yTrue: Target, yPred: Target, positive?: number): number;
    /** Agreement over chance. Defaults: positive 1. */
    function cohenKappa(yTrue: Target, yPred: Target, positive?: number): number;
    /** beta > 1 weights recall. */
    function fbeta(yTrue: Target, yPred: Target, beta: number, positive?: number): number;
    /** Exact Mann-Whitney U; lacking a positive or negative example is a RangeError; `positive` defaults to the largest label. */
    function rocAuc(yTrue: Target, yScore: Target, positive?: number): number;
    /** Area under the precision-recall curve, from scores. */
    function averagePrecision(yTrue: Target, yScore: Target, positive?: number): number;

    /** Indices, not data. testSize is a FRACTION in (0,1), default 0.25; the count form (testSize: 10) is a RangeError. */
    function trainTestSplit(n: number | Target, opts?: { testSize?: number; shuffle?: boolean; seed?: number }): { train: number[]; test: number[] };
    /** Default 5 folds; when both `k` and `folds` are given, `folds` wins. */
    function kFold(n: number | Target, opts?: { k?: number; folds?: number; shuffle?: boolean; seed?: number }): { train: number[]; test: number[] }[];
    /** K folds that preserve each class's proportion. */
    function stratifiedKFold(y: Target, opts?: { k?: number; shuffle?: boolean; seed?: number }): { train: number[]; test: number[] }[];
    /** Cross-validated scores, one per fold; estimatorFactory is () => new Model(...). `nJobs` is validated but folds run SEQUENTIALLY; `onProgress({done, total})` fires after each fold. */
    function crossValScore(estimatorFactory: () => { fit(X: Matrix, y: Target): unknown; predict(X: Matrix): number[]; close(): void }, X: Matrix, y: Target, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): number[];
    /** Exhaustive parameter search. */
    function gridSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): { best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] };
    /** Random parameter search over nIter sampled points. */
    function randomSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { nIter?: number; k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): { best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] };

    /** Replaces every non-finite entry with its column's finite mean; a column with NO finite value is a RangeError naming it. */
    function imputeMean(X: Matrix, rows?: number, cols?: number): number[][];
    /** Removes rows holding a non-finite value; `kept` lists the survivors. */
    function dropMissing(X: Matrix, y?: Target, rows?: number, cols?: number): { X: number[][]; y: Float64Array | undefined; kept: number[] };
}
/** dyna:random — a seedable xoshiro256** PRNG with distributions, shuffle/sample and checkpointable state; NOT for secrets (use crypto.RandomBytes). */
declare module "dyna:random" {
    /** A seedable xoshiro256** PRNG; a given seed is deterministic and reproducible. */
    class Random {
        /** The seed coerces via ToInt64 ("42" and 42n give the 42 stream); omitted draws from OS entropy. */
        constructor(seed?: number | bigint);
        /** A full 64-bit draw, always BigInt. */
        nextU64(): bigint;
        /** The top 53 bits as an exact Number in [0, 2^53). */
        nextU53(): number;
        /** A double in [0, 1). */
        nextFloat(): number;
        /** Uniform in [0, bound) by rejection sampling (no modulo bias); the result type mirrors the argument; a Number bound must be an integer in [1, 2^53]. */
        nextBounded(bound: number): number;
        nextBounded(bound: bigint): bigint;
        /** Fills any typed array with random bytes over an optional (offset, length) window in ELEMENTS; returns `this`; DataView/ArrayBuffer are refused. */
        fill(typedArray: AnyTypedArray, offset?: number, length?: number): this;
        /** n fresh random bytes; n must be an integer in [0, 2^30]. */
        bytes(n: number): Uint8Array;
        /** Normal(mu = 0, sigma = 1) by Marsaglia's polar method with no cached second draw, so state checkpoints replay exactly. */
        normal(mu?: number, sigma?: number): number;
        /** Exponential(lambda = 1) by inverse CDF; lambda must be finite and > 0. */
        exponential(lambda?: number): number;
        /** Exact Poisson(lambda) for lambda in [0, 2^31-1]; returns a non-negative integer. */
        poisson(lambda: number): number;
        /** In-place Fisher-Yates from THIS instance's stream (so it is reproducible); returns `this`; frozen collections are refused. */
        shuffle(arr: AnyTypedArray | unknown[]): this;
        /** n elements WITHOUT replacement; the source is not mutated; a typed array yields its own type. */
        sample(arr: AnyTypedArray | unknown[], n: number): AnyTypedArray | unknown[];
        /** One uniform element; an empty array is a RangeError. */
        choice(arr: AnyTypedArray | unknown[]): unknown;
        /** Advances the state by 2^128 draws without generating them: same-seed generators separated by a jump give disjoint streams. */
        jump(): this;
        /** longJump: the 2^192-advance variant, for 2^64 independent lanes. */
        longJump(): this;
        /** A 32-byte opaque snapshot of the generator state (four LE u64 words). */
        getState(): Uint8Array;
        /** Restores a state captured by getState(); an all-zero state is the xoshiro fixed point and is refused. */
        setState(state: Uint8Array): void;
    }
}

/** dyna:schema — JSON Schema (Draft 2020-12) validation: a schema compiles once into a native tree, validation is pure dispatch. */
declare module "dyna:schema" {
    /** JSON Schema Draft 2020-12 validation. */
    interface SchemaError {
        path: string;
        message: string;
        keyword: string;
    }
    /** The outcome of validate(); `errors` lists the failed keywords. */
    interface SchemaResult {
        valid: boolean;
        errors: SchemaError[];
    }
    /** A compiled schema: reusable, thread-safe, pure dispatch at validate time. */
    interface CompiledSchema {
        /** Validates an instance against the compiled schema. */
        validate(instance: unknown): SchemaResult;
    }
    const Schema: {
        /** Compiles the schema once. Draft 2020-12 core keywords only (including unevaluated* and relative $ref); there is no {draft} option. */
        compile(schema: unknown): CompiledSchema;
        /** Compiles and caches on the schema object; accepts an already-compiled schema. */
        validate(schema: unknown | CompiledSchema, instance: unknown): SchemaResult;
        /** Registers a custom "format" validator for string instances, live for already-compiled schemas; per runtime; duplicate names are refused. */
        registerFormat(name: string, fn: (value: string) => unknown): void;
    };
}

/** dyna:scrape — polite crawling: robots.txt policy, per-host pacing, jittered retries, conditional GETs, body caps, field extraction, resumable Crawl, sitemaps.
 *  Works over dyna:html's element tree; Fetcher gates every redirect hop (private hosts, https downgrades). */
declare module "dyna:scrape" {
    /** robots.txt parsing (RFC 9309). */
    class Robots implements DynResource {
        /** Parses a robots.txt body (RFC 9309). */
        constructor(text: string, opts?: { agent?: string });
        /** True when the path is allowed for the configured agent. */
        allows(path: string): boolean;
        /** The crawl delay for the agent, or null when unset. */
        crawlDelay(): number | null;
        /** The sitemap URLs listed in the file. */
        sitemaps(): string[];
        /** The number of rules of the winning group that survived parsing. */
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

    /** Polite HTTP retrieval with robots policy, retries and backoff. */
    class Fetcher implements DynResource {
        constructor(opts: {
            agent: string;
            /** Optional transport: omitted, the Fetcher builds its own HTTPClient capped at maxBodyBytes; inject one (or a mock) to drive the policy in tests. */
            client?: unknown;
            /** A proxy URL: validated, stored, and redacted in stats(); the built-in transport connects directly, so routing through it is an injected client's job. */
            proxy?: string;
            /** A CA bundle path/PEM: validated and stored (stats() redacts it); the built-in transport uses the platform trust store. */
            ca?: string;
            /** Connection pool size 1..64 (default 4): validated and echoed; the built-in transport opens one connection per request. */
            poolSize?: number;
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
            /** Redirect hops chased. Default 5. */
            maxRedirects?: number;
            /** Response-body cap in bytes. Default 8 MiB. */
            maxBodyBytes?: number;
            /** Default false: private/loopback/link-local targets refused by the SSRF gate; true allows them. */
            allowPrivateHosts?: boolean;
            /** retries/maxRedirects/maxBodyBytes/minDelayMs/robotsTtlMs must fit int32 (RangeError beyond, never truncated). */
        });
        /** GET a URL; returns the HTTP response with url/fromCache/notModified added. */
        get(url: string): FetcherResponse;
        /** The crawl statistics; `savedBytes` sums cached-body sizes served on revalidation. */
        stats(): {
            fetched: number; skippedByRobots: number; retried: number;
            throttledMs: number; bytes: number; revalidated: number; savedBytes: number;
            /** The stored simple options, redacted: proxy userinfo becomes `***@`, a PEM `ca` becomes `<pem-redacted>`. */
            proxy: string | null; ca: string | null; poolSize: number;
        };
        /** Fetches under get()'s policy (robots, pacing, retries, redirect gates, maxBodyBytes) but returns a streaming FetcherStream; no revalidation or response decorations. */
        getStream(url: string): FetcherStream;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A fetched page; on revalidation the status stays 304 with the cached body and its stored content-type. */
    interface FetcherResponse {
        status: number;
        headers: Record<string, string>;
        contentType: string;
        body: string;
        url: string;
        fromCache: boolean;
        notModified: boolean;
        /** X-Robots-Tag directives that apply to this fetcher's agent. */
        robotsDirectives?: string[];
        /** The `Link: rel="canonical"` target (RFC 8288), resolved against the request url. */
        canonicalUrl?: string;
    }

    /** The getStream response: head fields plus dyna:stream's ByteSource shape (read/close), consumed directly by pipe(), lines() and ndjson(). */
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
        /** Fills the caller's buffer and resolves with the count; 0 = end of body. Rejects on truncation, maxBodyBytes overflow or malformed chunking. */
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
            /** Pages fetched in parallel (1..16, default 1); above 1, next() returns a Promise, so drain with for await. Per-host politeness is unchanged. */
            concurrency?: number;
        });
        /** Seeds the crawl with an http(s) url; `parse` is the document parser, normally dyna:html's HTMLParse. */
        start(seed: string, extractor?: Extractor, parse?: (html: string) => import("dyna:html").HTMLElement[]): this;
        /** One page per call: {value, done} at concurrency 1, a Promise of it above. */
        next(): IteratorResult<CrawlPage> | Promise<IteratorResult<CrawlPage>>;
        /** The crawl itself as a lazy page iterator: one fetch per next(), never collected eagerly. */
        pages(): IterableIterator<CrawlPage>;
        [Symbol.iterator](): Iterator<CrawlPage>;
        /** Async iteration, required for a concurrent (concurrency > 1) crawl. */
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
        /** Fetch + parse + flatten, following a <sitemapindex> ONE level. RangeError on non-2xx, more than 1000 child sitemaps, or more than 50000 URLs in total. */
        function list(url: string, source?: Fetcher | {
            agent?: string; headers?: Record<string, string>;
            minDelayMs?: number; allowPrivateHosts?: boolean;
            maxBodyBytes?: number; retries?: number;
        }): string[];
    }
}

/** dyna:semver — semver 2.0.0: parse, compare, increment, and ranges with node-semver semantics. */
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
    /** Whether the string parses at all. */
    function isValid(version: string): boolean;
    /** Trims, strips a leading `=`, returns the normalized version or null. */
    function clean(version: string): string | null;
    /** The first run of digits normalized into X.Y.Z, or null. */
    function coerce(version: string): string | null;
    /** -1 | 0 | 1 by semver precedence (build metadata does not count). */
    function compare(a: string, b: string): -1 | 0 | 1;
    /** Whether the versions are equal. */
    function eq(a: string, b: string): boolean;
    /** Whether the versions differ. */
    function neq(a: string, b: string): boolean;
    /** Whether `a` is greater than `b`. */
    function gt(a: string, b: string): boolean;
    /** Whether `a` is greater than or equal to `b`. */
    function gte(a: string, b: string): boolean;
    /** Whether `a` is less than `b`. */
    function lt(a: string, b: string): boolean;
    /** Whether `a` is less than or equal to `b`. */
    function lte(a: string, b: string): boolean;
    /** Ascending sort by precedence; does not mutate the input. */
    function sort(versions: string[]): string[];
    /** The major field. */
    function major(v: string): number;
    /** The minor field. */
    function minor(v: string): number;
    /** The patch field. */
    function patch(v: string): number;
    /** The prerelease identifiers, or null when there are none. */
    function prerelease(v: string): (string | number)[] | null;
    /** Bumps by major|minor|patch|premajor|preminor|prepatch|prerelease|release; 'release' strips the prerelease and returns null when there is none. */
    function inc(version: string, release: string, identifier?: string): string | null;
    /** One-shot range match; a prerelease matches only ranges naming a prerelease at the same major.minor.patch. */
    function satisfies(version: string, range: string): boolean;
    /** The highest version in the list that satisfies the range, or null. */
    function maxSatisfying(versions: string[], range: string): string | null;
    /** The lowest version in the list that satisfies the range, or null. */
    function minSatisfying(versions: string[], range: string): string | null;
    /** The highest-precedence component that differs ("major" ... "build"), or null when identical. */
    function diff(a: string, b: string): "major" | "minor" | "patch" | "prerelease" | "build" | null;
    /** compare(), with build identifiers breaking a precedence tie (absent build sorts lowest). */
    function compareBuild(a: string, b: string): -1 | 0 | 1;
    /** Whether two ranges (strings or Range objects) intersect, by node-semver's comparator-pairwise rule. */
    function intersects(rangeA: string | Range, rangeB: string | Range): boolean;

    /** A compiled range expression; same prerelease rule as satisfies(); an invalid range is a TypeError. */
    class Range {
        /** A compiled `Range`. */
        constructor(rangeString: string);
        /** Whether the version matches the compiled range. */
        test(version: string): boolean;
        /** The versions that match, preserving input order. */
        filter(versions: string[]): string[];
        /** The highest matching version, or `null`. */
        maxSatisfying(versions: string[]): string | null;
        /** The lowest matching version, or `null`. */
        minSatisfying(versions: string[]): string | null;
        /** Whether this range intersects `other`. */
        intersects(other: string | Range): boolean;
        /** The original range text. */
        readonly source: string;
        /** The number of `||`-separated sets (the observable measure of compilation). */
        readonly setCount: number;
    }
}

/** dyna:serialize — binary interchange: protobuf wire codec, ASN.1 DER, MessagePack, CBOR, BSON, structuredClone; decoders are hardened for hostile input. */
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
    /** Protobuf wire codec driven by a { fields } schema object (no .proto compiler). */
    const Proto: {
        /** Wire encoding, strict about JS types and ranges; `oneof` is an ANNOTATION only (last one wins, as in proto2/3). */
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
    /** ASN.1 DER: node builders, encode and decode over one node tree. */
    const ASN1: {
        /** DER-encodes a node tree. */
        encode(node: ASN1Node): Uint8Array;
        /** Untrusted input: nesting <= 256, children per construct <= 65536, total nodes <= 1048576 (RangeError before any proportional allocation). */
        decode(bytes: Uint8Array | ArrayBuffer): ASN1Node;
        /** A SEQUENCE or SET node over the children. */
        seq(children: ASN1Node[]): ASN1Node;
        /** A SEQUENCE or SET node over the children. */
        set(children: ASN1Node[]): ASN1Node;
        /** Any integer Number or BigInt as minimal two's complement (64 content bytes at most); decodes to a Number up to 2^53, a BigInt beyond. */
        int(value: number | bigint): ASN1Node;
        /** A BOOLEAN node. */
        bool(value: boolean): ASN1Node;
        /** A NULL node. */
        null(): ASN1Node;
        /** An OCTET STRING node. */
        octets(bytes: ByteView): ASN1Node;
        /** BIT STRING with `unused` trailing pad bits. */
        bitString(bytes: ByteView, unused: number): ASN1Node;
        /** An OBJECT IDENTIFIER node. */
        oid(str: string): ASN1Node;
        /** A UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. */
        utf8(str: string): ASN1Node;
        /** IA5String (tag 22): ASCII only; non-ASCII refuses at encode. */
        ia5String(str: string): ASN1Node;
        /** BMPString (tag 30), UCS-2 big-endian; code points above U+FFFF and lone surrogates are refused. */
        bmpString(str: string): ASN1Node;
        /** UniversalString (tag 28), UCS-4 big-endian; lone surrogates are refused. */
        universalString(str: string): ASN1Node;
        /** A UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. */
        printable(str: string): ASN1Node;
        /** A UTCTime or GeneralizedTime node (the strings are emitted verbatim). */
        utcTime(str: string): ASN1Node;
        /** A UTCTime or GeneralizedTime node (the strings are emitted verbatim). */
        generalizedTime(str: string): ASN1Node;
        /** Context-specific primitive [tag] with raw content. */
        context(tag: number, content: ByteView): ASN1Node;
        /** Context-specific constructed [tag] wrapping child nodes. */
        contextC(tag: number, children: ASN1Node[]): ASN1Node;
    };

    /** MessagePack encoding; symbols and functions are refused. The decoders cap depth and validate lengths before allocating. */
    function MsgPackEncode(value: unknown): Uint8Array;
    /** Decodes one MessagePack value; depth-capped, lengths validated before allocation. */
    function MsgPackDecode(bytes: ByteView): unknown;
    /** RFC 8949 CBOR with the same walker, bounds and refusals as MessagePack. */
    function CBOREncode(value: unknown): Uint8Array;
    /** Decodes one CBOR value under the same bounds. */
    function CBORDecode(bytes: ByteView): unknown;
    /** CBOR with map keys sorted byte-wise (deterministic form). */
    function CBORCanonical(value: unknown): Uint8Array;
    /** Canonical CBOR run through XXH64, returned as 16 lowercase hex chars. */
    function ValueHash(value: unknown): string;
    /** Deep clone (the global structuredClone): preserves cycles and shared refs; functions and accessors are a TypeError; no transfer list. */
    function structuredClone(value: unknown): unknown;
}

/** dyna:simd — explicit vector math over typed arrays: reductions, dot products, distances, activations, matrix ops; the best kernel for the CPU is chosen at startup.
 *  Plain names are f32 (F32Like), `f64*` take Float64Array, `i32*` strictly Int32Array; most kernels accept a trailing (offset, length) window. */
declare module "dyna:simd" {
    // Reductions accumulate in vector lanes: they match a scalar loop to a relative tolerance, not bitwise; NaN propagates (argmax/argmin ignore it).

    /** Arithmetic sum of a float array. */
    function sum(a: F32Like): number;
    /** Largest element (min is the smallest); an array of infinities answers that infinity. */
    function max(a: F32Like): number;
    /** The minimum value. Throws `RangeError` on an empty array. */
    function min(a: F32Like): number;
    /** Index of the extreme; empty throws. Ties resolve to the first occurrence, identically on every backend. */
    function argmax(a: F32Like): number;
    /** The index of the minimum. Throws `RangeError` on an empty array. */
    function argmin(a: F32Like): number;
    /** The 1-norm. */
    function normL1(a: F32Like): number;
    /** The Euclidean norm. */
    function normL2(a: F32Like): number;

    /** Elementwise: out[i] = a[i] op b[i]; all lengths must match. */
    function add(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Elementwise out = a - b (mul and div likewise); returns out. */
    function sub(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = a[i] * b[i]` and returns `out`. All lengths must match. */
    function mul(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = a[i] / b[i]` and returns `out`. All lengths must match. */
    function div(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = |a[i]|` and returns `out`. */
    function abs(out: F32Like, a: F32Like): F32Like;
    /** In-place z[i] += a[i] * b[i]. */
    function fma(z: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Inner product. */
    function dot(a: F32Like, b: F32Like): number;

    /** In-place scalar ops. */
    function scale(a: F32Like, s: number): F32Like;
    /** In-place a[i] += s; returns a. */
    function addScalar(a: F32Like, s: number): F32Like;
    /** y[i] += alpha * x[i]. */
    function axpy(y: F32Like, alpha: number, x: F32Like): F32Like;
    /** a[i] = alpha * a[i] + beta. */
    function affine(a: F32Like, alpha: number, beta: number): F32Like;

    /** In-place activations using a fast exponential: SIMD and scalar agree to a relative tolerance of 1e-3 and may differ at saturation. */
    function sigmoid(a: F32Like): F32Like;
    /** In-place `max(x, 0)`; returns `a`. */
    function relu(a: F32Like): F32Like;
    /** In-place `min(max(x, 0), 6)`; returns `a`. */
    function relu6(a: F32Like): F32Like;
    /** In-place `x < 0 ? slope * x : x`; returns `a`. */
    function leakyRelu(a: F32Like, slope: number): F32Like;
    /** In-place `x < 0 ? alpha * (exp(x) - 1) : x`; returns `a`. */
    function elu(a: F32Like, alpha: number): F32Like;
    /** In-place fast tanh via the sigmoid identity; returns `a`. */
    function tanhFast(a: F32Like): F32Like;
    /** In-place Gaussian error linear unit; returns `a`. */
    function gelu(a: F32Like): F32Like;
    /** In-place `x * sigmoid(x)`, composed from `sigmoid` + `mul` so it is correct on every ISA; returns `a`. */
    function silu(a: F32Like): F32Like;
    /** Stable max-shifted softmax in place; a NaN input answers all-NaN. */
    function softmax(a: F32Like): F32Like;
    /** Log-softmax in place; returns `a`. Throws on an empty array. */
    function logSoftmax(a: F32Like): F32Like;

    /** In-place unary math; NaN propagates. */
    function vexp(a: F32Like): F32Like;
    /** In-place elementwise ln; vsqrt/vrsqrt/vinv are sqrt, 1/sqrt and 1/x, clamped to 0 where undefined (x < 0, x <= 0, x == 0). */
    function vlog(a: F32Like): F32Like;
    /** In-place square root; returns `a`. */
    function vsqrt(a: F32Like): F32Like;
    /** In-place reciprocal square root; returns `a`. */
    function vrsqrt(a: F32Like): F32Like;
    /** In-place reciprocal; returns `a`. */
    function vinv(a: F32Like): F32Like;

    /** Vector-to-scalar distances over equal-length pairs. */
    function distL2(a: F32Like, b: F32Like): number;
    /** Manhattan (L1) distance. */
    function distL1(a: F32Like, b: F32Like): number;
    /** Cosine distance: 1 - cosine similarity (0 for parallel vectors, 1 for orthogonal). */
    function distCos(a: F32Like, b: F32Like): number;
    /** Chebyshev (L-infinity) distance. */
    function distCheb(a: F32Like, b: F32Like): number;

    /** BLAS-2/3 kernels are row-major with explicit dimensions. gemv, 6-argument form: y = beta*y + A*x. */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** 7-argument gemv: y = alpha*A*x + beta*y (beta == 0 skips reading y). */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, alpha: number, beta: number): F32Like;
    /** Transposed gemv: y = beta*y + A^T*x. */
    function gemvT(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** General matrix multiply C = alpha*A*B + beta*C; the output must not share memory with an input. */
    function gemm(c: F32Like, a: F32Like, b: F32Like, m: number, n: number, k: number, alpha: number, beta: number): F32Like;

    // Windows: vector kernels (not gemv/gemvT/gemm/topkIndices) take a trailing (offset, length) or {offset, length|limit}: sum(a, 8, 16).

    /** Arithmetic mean = sum(a)/n. */
    function mean(a: F32Like, offset?: number, length?: number): number;
    function mean(a: F32Like, opts?: { offset?: number; length?: number; limit?: number }): number;
    /** POPULATION variance (divide by n), two-pass centered; multiply by n/(n-1) for the sample form DataFrame.VARIANCE uses. */
    function variance(a: F32Like): number;
    /** In-place z-score (a[i]-mean)/std; returns a. A constant array has no z-score: the output is all-NaN or rounding noise. */
    function normalize(a: F32Like): F32Like;
    /** Arithmetic mean over Float64Array = f64Sum(a)/n. */
    function f64Mean(a: Float64Array): number;
    /** Two-pass centered population variance over Float64Array. */
    function f64Variance(a: Float64Array): number;
    /** Mean over Int32Array with exact int64 accumulation and one final rounding. */
    function i32Mean(a: Int32Array): number;

    /** In-place min(max(x, lo), hi). */
    function clamp(a: F32Like, lo: number, hi: number): F32Like;
    /** In-place binarise: x > t ? 1.0 : 0.0. */
    function threshold(a: F32Like, t: number): F32Like;
    /** Indices of the k largest values (a fresh array, unspecified order). */
    function topkIndices(vals: F32Like, k: number): Uint32Array;

    /** Zero-copy Float64Array kernels. */
    function f64Sum(a: Float64Array): number;
    /** The inner product. Reorders additions and rounds slightly differently from a sequential sum. */
    function f64Dot(a: Float64Array, b: Float64Array): number;
    /** The maximum; throws on an empty array. Bit-exact. */
    function f64Max(a: Float64Array): number;
    /** The minimum; throws on an empty array. Bit-exact. */
    function f64Min(a: Float64Array): number;
    /** In-place `a[i] *= s`; returns `a`. Bit-exact. */
    function f64Scale(a: Float64Array, s: number): Float64Array;
    /** In-place y += alpha*x; returns y. */
    function f64Axpy(y: Float64Array, alpha: number, x: Float64Array): Float64Array;

    /** Zero-copy Int32Array kernels only. */
    function i32Sum(a: Int32Array): number;
    /** Exact minimum; throws on an empty array. */
    function i32Min(a: Int32Array): number;
    /** Exact maximum; throws on an empty array. */
    function i32Max(a: Int32Array): number;
    /** Sum of double products; matches a sequential dot to a relative tolerance. */
    function i32Dot(a: Int32Array, b: Int32Array): number;
    /** Writes into a separate `out`, wrapping per element (mod 2³², like `(a+b)|0`); returns `out`. */
    function i32Add(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    /** Writes into a separate `out`, keeping the low 32 bits per element (like `Math.imul`); returns `out`. */
    function i32Mul(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    /** In-place low-32-bit multiply; returns `a`. */
    function i32Scale(a: Int32Array, s: number): Int32Array;

    /** Inclusive prefix scan, in place (the output type follows the input). */
    function cumsum(a: Int32Array): Int32Array;
    function cumsum(a: Float32Array): Float32Array;
    /** Inclusive prefix maximum, in place; returns `a`. Exact for both (max rounds nothing). */
    function cummax(a: Int32Array): Int32Array;
    function cummax(a: Float32Array): Float32Array;
}

/** dyna:stream — pull-based byte streaming for data bigger than memory: ByteSource fills a buffer you own, ByteSink accepts bytes, pipe() connects them.
 *  Both are duck-typed, so fetch bodies and subprocess pipes plug in; lines()/ndjson() iterate chunk-safely, inflate()/deflate() add streaming codecs. */
declare module "dyna:stream" {
    /** A pull-based byte producer (not a WHATWG ReadableStream): read(buf) fills up to buf.length bytes and resolves the count, 0 = EOF. Any object with such a read qualifies. */
    interface ByteSource {
        /** Fills up to buf.length bytes; resolves with bytes read, 0 = EOF. */
        read(buf: Uint8Array): Promise<number>;
        /** Deterministic release; a file source closed before its first read never touches the disk. */
        close(): void;
        /** True once released. */
        readonly closed: boolean;
        [Symbol.dispose](): void;
    }

    /** A byte consumer: write(view) resolves with the count accepted (a duck-typed sink may accept fewer; pipe retries the rest). */
    interface ByteSink {
        /** Accepts bytes; resolves with the count accepted. */
        write(buf: ByteView): Promise<number>;
        /** Pushes buffered bytes to the underlying target. */
        flush(): Promise<void>;
        /** Deterministic release; a buffered sink flushes best-effort. */
        close(): void;
        /** True once released. */
        readonly closed: boolean;
        [Symbol.dispose](): void;
    }

    /** Options for `pipe`. */
    interface PipeOptions {
        /** Called after each accepted chunk with its byte count; a throw rejects the pipe and closes both sides. */
        onChunk?: (bytes: number) => void;
    }

    /** Options for `toFile`. */
    interface ToFileOptions {
        /** Append rather than truncate. */
        append?: boolean;
        /** Sink buffer size, clamped to 4 KiB..64 MiB (default 128 KiB). */
        bufferSize?: number;
    }

    /** Pumps src into dst through a 128 KiB chunk until EOF and resolves total bytes. Failure closes both sides; success closes neither (flush or close a file sink yourself). */
    function pipe(src: ByteSource, dst: ByteSink, opts?: PipeOptions): Promise<number>;

    /** A ByteSource over an in-memory COPY of `b` (a string is encoded as UTF-8). */
    function fromBytes(b: Uint8Array | string): ByteSource;

    /** A ByteSource over a file opened LAZILY at the first read (a bad path rejects there); sequential, so FIFOs stream too. */
    function fromFile(path: import("dyna:file").Path | string): ByteSource;

    /** A buffered ByteSink over a file created LAZILY at the first write (truncate, or `{append: true}`); errors surface on the flushing write or flush(), and stick. */
    function toFile(path: import("dyna:file").Path | string, opts?: ToFileOptions): ByteSink;

    /** Options for `lines`. */
    interface LinesOptions {
        /** Only "utf-8" (the default) is supported; anything else is refused. */
        encoding?: string;
    }

    /** Async iterable of lines, safe across ANY chunk boundary; strips CRLF, keeps empty lines, delivers an unterminated last line; `break` closes the source. */
    function lines(src: ByteSource, opts?: LinesOptions): AsyncIterableIterator<string>;

    /** Async iterable of NDJSON values; blank lines are skipped; a malformed line rejects with its 1-based line number and iteration can resume after it. */
    function ndjson(src: ByteSource): AsyncIterableIterator<unknown>;

    /** Streaming codecs accepted by `inflate`/`deflate`. */
    type StreamCodec = "gzip" | "zstd" | "brotli" | "lz4" | "deflate";

    /** A compressing sink: write() takes RAW bytes. ALWAYS end with `await sink.finish()` — close()/`using` alone drops the stream tail (truncated output). */
    interface DeflateByteSink extends ByteSink {
        /** Finalizes the stream, flushes and closes the wrapped sink; resolves with the total raw bytes compressed. */
        finish(): Promise<number>;
    }

    /** Options for `deflate`; level: zstd 1..22 (default 3), lz4 1..12 (default 1); gzip/deflate/brotli ignore it. */
    interface DeflateOptions {
        codec: StreamCodec;
        level?: number;
    }

    /** Options for `inflate`. */
    interface InflateOptions {
        codec: StreamCodec;
    }

    /** A decompressing ByteSource over `src` (trailers verified before EOF). zstd/brotli need their libraries, gzip/deflate are macOS-only here, lz4 works everywhere. */
    function inflate(src: ByteSource, opts: InflateOptions): ByteSource;

    /** A compressing ByteSink over `sink`; see DeflateByteSink for the finish() contract. */
    function deflate(sink: ByteSink, opts: DeflateOptions): DeflateByteSink;
}


/** dyna:structures — the containers JS never shipped: graphs, heaps, LRU, tries, B-trees, sorted maps, segment trees, bitsets, Bloom/count-min sketches, range sets.
 *  Each persists via serialize()/deserialize(); bytes are untrusted (lengths validated before allocating; at most 512 MiB + 64x the record's payload per record). */
declare module "dyna:structures" {
    /** Adjacency-list graph over integer node ids. */
    class Graph {
        /** Creates a graph; `directed` and `weighted` are fixed at construction. */
        constructor(opts?: { directed?: boolean; weighted?: boolean });
        /** Rebuilds a graph from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Graph;
        /** Adds a node and returns its id. */
        addNode(): number;
        /** Adds an edge (both directions when undirected); nodes grow on demand; `w` is honoured only on a `{ weighted: true }` graph, else 1.0. */
        addEdge(u: number, v: number, w?: number): this;
        /** Targets of u's outgoing edges. */
        neighbors(u: number): number[];
        /** Writes u's out-neighbors into `out` and returns the degree; RangeError (nothing written) when `out` is too short. Never allocates. */
        neighborsInto(u: number, out: Int32Array): number;
        /** The adjacency as CSR in two fresh Int32Arrays: edges[offsets[u] .. offsets[u+1]] are u's neighbors. */
        exportCSR(): { offsets: Int32Array; edges: Int32Array };
        /** False if `u` is out of range or no such edge exists. */
        hasEdge(u: number, v: number): boolean;
        /** The number of nodes. */
        readonly nodeCount: number;
        /** The number of `addEdge` calls (an undirected edge is one call). */
        readonly edgeCount: number;
        /** Node ids in breadth-first order from `src`. */
        bfs(src: number): number[];
        /** Node ids in depth-first order from `src`. */
        dfs(src: number): number[];
        /** Visitor form: calls visit(node) in BFS order, stops when it returns true, returns the count visited; allocates nothing per node. */
        bfs(src: number, opts: { visit: (node: number) => boolean | void }): number;
        /** Visitor form of dfs (smaller ids first); same contract as the bfs visitor. */
        dfs(src: number, opts: { visit: (node: number) => boolean | void }): number;
        /** Shortest distances; a single distance to dst when given. */
        dijkstra(src: number): number[];
        dijkstra(src: number, dst: number): number;
        /** Shortest distances from `src`; negative edge weights are allowed. */
        bellmanFord(src: number): number[];
        /** Node ids in topological order; a cycle throws RangeError. */
        topologicalSort(): number[];
        /** Visitor form: visits in topological order with early exit; a cycle still throws RangeError. */
        topologicalSort(opts: { visit: (node: number) => boolean | void }): number;
        /** Component id per node (weak components on a directed graph). */
        connectedComponents(): number[];
        /** All-pairs distance matrix; refused for n > 1024. */
        floydWarshall(): number[][];
        /** Minimum spanning forest. */
        mst(): { weight: number; edges: [number, number, number][] };
        /** A* search; closed nodes reopen on a shorter path, so an inconsistent heuristic stays optimal within a push budget of 64*n+1024 (RangeError beyond). */
        aStar(src: number, dst: number, heuristic: (node: number) => number): { dist: number; path: number[] };
        /** The graph as a compact type-tagged record with delta-varint encoded adjacency. */
        serialize(): Uint8Array;
    }

    /** Capacity-bounded string-to-value cache with LRU eviction. */
    class LRU<V = unknown> {
        /** LRU cache of `capacity` entries; `ttlMs` expires entries, `onEvict(key, value)` observes evictions. */
        constructor(capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: V) => void });
        /** Rebuilds from serialize() bytes; T flows through, so no cast is needed. */
        static deserialize<T>(this: new (capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: T) => void }) => LRU<T>, bytes: Uint8Array | ArrayBuffer): LRU<T>;
        /** The value and moves the entry to MRU. An expired entry is dropped (counted in `expired`) before the miss. */
        get(key: string): V | undefined;
        /** Inserts or updates, marking the entry MRU; past capacity the LRU entry is evicted. */
        put(key: string, value: V): this;
        /** Alias for `put`. */
        set(key: string, value: V): this;
        /** Stores an entry with its own lifetime in ms. */
        setWithTTL(key: string, value: V, ms: number): this;
        /** Presence without touching recency or counting a hit; an expired key reads absent. */
        has(key: string): boolean;
        /** Removes the entry if present. */
        delete(key: string): boolean;
        /** Reclaims every expired entry now; returns the count removed. */
        purgeExpired(): number;
        /** The number of entries. */
        readonly size: number;
        /** The capacity bound. */
        readonly capacity: number;
        /** `{ hits, misses, evictions, expired, size, capacity }`. */
        readonly stats: { hits: number; misses: number; evictions: number; expired: number; size: number; capacity: number };
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Binary heap ordered by a JS comparator or natural number order. */
    class Heap<V = number> {
        /** Creates a binary heap. The comparator must not push/pop its own heap (reentrancy throws `TypeError`). */
        constructor(comparator?: (a: V, b: V) => number);
        /** Rebuilds from serialize() bytes with the comparator; T flows from it. */
        static deserialize<T>(bytes: Uint8Array | ArrayBuffer, cmp: (a: T, b: T) => number): Heap<T>;
        static deserialize<T>(this: new (comparator?: (a: T, b: T) => number) => Heap<T>, bytes: Uint8Array | ArrayBuffer): Heap<T>;
        /** Insert and sift; returns the new size. */
        push(v: V): number;
        /** Removes and returns the root; `undefined` when empty. */
        pop(): V | undefined;
        /** The root without removal; never calls the comparator. */
        peek(): V | undefined;
        /** The element count. Getter; `Heap.length` is the same getter. */
        readonly size: number;
        readonly length: number;
        /** The contents as a compact type-tagged record. A comparator is not data, so the record cannot carry it. */
        serialize(): Uint8Array;
    }

    /** Atkinson/Sack/Santoro/Strothotte min-max heap. */
    class MinMaxHeap<V = number> {
        /** Creates an empty min-max heap. */
        constructor();
        /** Rebuilds a min-max heap from its record. */
        static deserialize<T>(this: new () => MinMaxHeap<T>, bytes: Uint8Array | ArrayBuffer): MinMaxHeap<T>;
        /** Inserts a value with a numeric priority. */
        push(priority: number, value?: V): this;
        /** Removes and returns the minimum's stored value. */
        popMin(): V | undefined;
        /** Removes and returns the maximum's stored value. */
        popMax(): V | undefined;
        /** The minimum without removal. */
        peekMin(): V | undefined;
        /** The maximum without removal. */
        peekMax(): V | undefined;
        /** The element count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Set of numbers in sorted order (the same ordered-tree core as BTree). */
    class SortedSet {
        /** Creates an empty set. NaN keys are rejected on insert (`RangeError`). */
        constructor();
        /** Rebuilds a set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): SortedSet;
        /** Inserts the key if absent; duplicates are no-ops. */
        add(x: number): this;
        /** Whether the key is present. */
        has(x: number): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(x: number): boolean;
        /** The smallest key. */
        first(): number | undefined;
        /** The largest key. */
        last(): number | undefined;
        /** The largest element <= x (ceil: the smallest >= x). */
        floor(x: number): number | undefined;
        /** The smallest key ≥ `x`; `undefined` if none or `x` is NaN. */
        ceil(x: number): number | undefined;
        /** Ascending keys in [lo, hi]. */
        rangeQuery(lo: number, hi: number): number[];
        /** All keys ascending. */
        toArray(): number[];
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Sorted map from numeric key to value (same core as BTree, but a DISTINCT serialize format and no [Symbol.iterator]). */
    class SortedMap<V = unknown> {
        /** Creates an empty map. NaN keys are rejected on insert (`RangeError`). */
        constructor();
        /** Rebuilds a map from its record. */
        static deserialize<T>(this: new () => SortedMap<T>, bytes: Uint8Array | ArrayBuffer): SortedMap<T>;
        /** Inserts or replaces. A NaN key throws `RangeError`. */
        set(k: number, v: V): this;
        /** The value; `undefined` for absent or NaN keys. */
        get(k: number): V | undefined;
        /** Whether the key is present. */
        has(k: number): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: number): boolean;
        /** The smallest key. */
        firstKey(): number | undefined;
        /** The largest key. */
        lastKey(): number | undefined;
        /** The largest key <= k (ceilKey: the smallest >= k). */
        floorKey(k: number): number | undefined;
        /** The smallest key ≥ `k`; `undefined` if none or `k` is NaN. */
        ceilKey(k: number): number | undefined;
        /** Ascending `[k, v]` pairs in `[lo, hi]`. */
        rangeQuery(lo: number, hi: number): [number, V][];
        /** Ascending keys. */
        keys(): number[];
        /** The key count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Ordered map on numeric keys as a B-tree of order 32; iterable over [key, value]; its serialize format differs from SortedMap's. */
    class BTree<V = unknown> {
        /** Creates an empty tree. NaN keys throw `TypeError` ("keys must be ordered numbers, not NaN"). */
        constructor();
        /** Rebuilds a tree from its record. */
        static deserialize<T>(this: new () => BTree<T>, bytes: Uint8Array | ArrayBuffer): BTree<T>;
        /** Inserts or replaces. O(log n). */
        set(k: number, v: V): this;
        /** The value. O(log n). */
        get(k: number): V | undefined;
        /** Whether the key is present. O(log n). */
        has(k: number): boolean;
        /** Removes the key, reporting whether it was present. O(log n). */
        delete(k: number): boolean;
        /** The smallest key. */
        firstKey(): number | undefined;
        /** The largest key. */
        lastKey(): number | undefined;
        /** The largest key <= k (ceilKey: the smallest >= k). */
        floorKey(k: number): number | undefined;
        /** The smallest key ≥ `k`; `undefined` if none. */
        ceilKey(k: number): number | undefined;
        /** Ascending `[k, v]` pairs in `[lo, hi]`. */
        rangeQuery(lo: number, hi: number): [number, V][];
        /** Ascending keys. */
        keys(): number[];
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Double-ended queue with O(1) push/pop at both ends. */
    class Deque<V = unknown> {
        /** Creates an empty deque; the buffer grows from 8. */
        constructor();
        /** Rebuilds a deque from its record. */
        static deserialize<T>(this: new () => Deque<T>, bytes: Uint8Array | ArrayBuffer): Deque<T>;
        /** Appends at the back and returns the new length. */
        pushBack(v: V): number;
        /** Prepends at the front and returns the new length. */
        pushFront(v: V): number;
        /** Removes and returns the front; `undefined` when empty. */
        popFront(): V | undefined;
        /** Removes and returns the back; `undefined` when empty. */
        popBack(): V | undefined;
        /** The front without removal. */
        peekFront(): V | undefined;
        /** The back without removal. */
        peekBack(): V | undefined;
        /** The element at index `i`. */
        get(i: number): V | undefined;
        /** The element count. */
        readonly length: number;
        /** A snapshot of the elements. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Doubly-linked list of JS values; element identity is stable. */
    class List<V = unknown> {
        /** Creates an empty list. */
        constructor();
        /** Rebuilds a list from its record. */
        static deserialize<T>(this: new () => List<T>, bytes: Uint8Array | ArrayBuffer): List<T>;
        /** Prepends and returns the new length. */
        pushFront(v: V): number;
        /** Appends and returns the new length. */
        pushBack(v: V): number;
        /** Removes and returns the front. */
        popFront(): V | undefined;
        /** Removes and returns the back. */
        popBack(): V | undefined;
        /** The front without removal. */
        front(): V | undefined;
        /** The back without removal. */
        back(): V | undefined;
        /** The element count. */
        readonly length: number;
        /** A head-to-tail snapshot. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Fixed-capacity circular buffer; push overwrites the oldest when full. */
    class RingBuffer<V = unknown> {
        /** Creates a fixed-capacity circular buffer. */
        constructor(capacity: number);
        /** Rebuilds a buffer from its record. */
        static deserialize<T>(this: new (capacity: number) => RingBuffer<T>, bytes: Uint8Array | ArrayBuffer): RingBuffer<T>;
        /** Appends, evicting the oldest when full; returns the count. */
        push(v: V): number;
        /** The element at index `i`. */
        get(i: number): V | undefined;
        /** The current count. */
        readonly length: number;
        /** The capacity. */
        readonly capacity: number;
        /** Whether the buffer holds `capacity` elements. */
        readonly full: boolean;
        /** The elements in insertion order. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Dynamic bit set backed by 64-bit words. */
    class BitSet {
        /** `nbits` is an initial capacity hint (up to 2^30 bits); the set grows on demand. */
        constructor(nbits?: number);
        /** Rebuilds a bit set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BitSet;
        /** Sets the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`. */
        set(i: number): this;
        /** Clears the bit. */
        clear(i: number): this;
        /** Flips the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`. */
        flip(i: number): this;
        /** False when the bit is past the allocated size. */
        get(i: number): boolean;
        /** The first set bit at position >= from, or -1. */
        nextSet(from: number): number;
        /** Number of set bits. */
        readonly count: number;
        /** In-place word AND; bits past `other` clear. */
        and(other: BitSet): this;
        /** In-place word OR; `this` grows as needed. */
        or(other: BitSet): this;
        /** In-place word XOR; `this` grows as needed. */
        xor(other: BitSet): this;
        /** Ascending indices of set bits. */
        toArray(): number[];
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Disjoint-set forest with path halving and union by rank. */
    class UnionFind {
        /** Elements are 0..n-1 (n up to 2^26). */
        constructor(n?: number);
        /** Rebuilds a forest from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): UnionFind;
        /** The representative of the set containing x. */
        find(x: number): number;
        /** Merges the sets of x and y; false when they were already joined. */
        union(x: number, y: number): boolean;
        /** Whether both elements are in the same component. */
        connected(x: number, y: number): boolean;
        /** Number of disjoint components. */
        readonly count: number;
        /** Element count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Fenwick tree over a fixed-size vector of doubles. */
    class Fenwick {
        /** Creates a Fenwick tree. */
        constructor(n: number);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Fenwick;
        /** Adds `delta` to position `i`. Out-of-range throws `RangeError`. */
        update(i: number, delta: number): this;
        /** Sum of positions [0..i] inclusive. */
        prefixSum(i: number): number;
        /** Sum of [lo..hi] inclusive; 0 for an empty range. */
        rangeQuery(lo: number, hi: number): number;
        /** The number of positions. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Iterative segment tree with an associative fold: "sum"|"min"|"max". */
    class SegTree {
        /** Creates a segment tree. Defaults: op "sum". */
        constructor(n: number, op?: SegOp);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): SegTree;
        /** Assigns leaf `i`, re-folding the path to the root. */
        update(i: number, value: number): this;
        /** The fold over `[lo..hi]`; the identity for an empty (`lo > hi`) range. */
        rangeQuery(lo: number, hi: number): number;
        /** The number of leaves. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Probabilistic set membership; no false negatives. */
    class BloomFilter {
        /** `bits` up to 2^30; `hashes` defaults to 3 (clamped to 1..32). */
        constructor(bits: number, hashes?: number);
        /** Rebuilds a filter from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BloomFilter;
        /** Sets the key's `hashes` bits. */
        add(key: string): this;
        /** False means definitely absent; true means probably present. */
        mayContain(key: string): boolean;
        /** The number of bits. */
        readonly bits: number;
        /** The number of hashes. */
        readonly hashes: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Set of byte strings with prefix queries. */
    class Trie {
        /** Creates an empty trie. */
        constructor();
        /** Rebuilds a trie from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Trie;
        /** Stores the key; re-inserting an existing key is a no-op. */
        insert(key: string): this;
        /** Whether the key is present. */
        has(key: string): boolean;
        /** Removes the key, reporting whether it was present; nodes are retained, only the marker clears. */
        delete(key: string): boolean;
        /** Every stored key starting with `prefix`. Ascending order is *not* guaranteed (sibling insertion order). */
        keysWithPrefix(prefix: string): string[];
        /** The longest stored key that is a prefix of str, or "". */
        longestPrefix(str: string): string;
        /** The number of stored keys. */
        readonly size: number;
        [Symbol.iterator](): Iterator<string>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** String key to uint64 count, saturating at 2^64-1; counts above 2^53-1 read back as the nearest double. */
    class Multiset {
        /** Creates an empty multiset. */
        constructor();
        /** Rebuilds a multiset from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Multiset;
        /** The key's new count. Defaults: n 1. */
        add(key: string, n?: number): number;
        /** Subtracts, returning the new count (flooring at 0; the record is dropped at 0). Defaults: n 1. */
        remove(key: string, n?: number): number;
        /** The key's count. */
        count(key: string): number;
        /** Whether the key is present. */
        has(key: string): boolean;
        /** Sets the count exactly. */
        setCount(key: string, count: number): this;
        /** Removes the key entirely, reporting whether it was present. */
        delete(key: string): boolean;
        /** Drops all entries. */
        clear(): void;
        /** The distinct keys. */
        elementSet(): string[];
        /** The keys with their counts. */
        entrySet(): [string, number][];
        /** Distinct key count. */
        readonly size: number;
        /** Sum of all counts. */
        readonly totalSize: number;
        [Symbol.iterator](): Iterator<[string, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** String key to a growing value array. */
    class Multimap<V = unknown> {
        /** Creates an empty multimap. */
        constructor();
        /** Rebuilds a multimap from its record. */
        static deserialize<T>(this: new () => Multimap<T>, bytes: Uint8Array | ArrayBuffer): Multimap<T>;
        /** Appends one value. */
        put(key: string, value: V): this;
        /** A fresh array of all values for the key (empty if none). */
        get(key: string): V[];
        /** The number of values for the key. */
        count(key: string): number;
        /** Removes every value for the key; returns how many. */
        delete(key: string): number;
        /** Removes the value at `index`. Both arguments required (`TypeError` otherwise). */
        removeAt(key: string, index: number): V | undefined;
        /** The distinct keys. */
        keys(): string[];
        /** Flattened pairs. */
        entries(): [string, V][];
        /** Total values. */
        readonly size: number;
        /** Distinct keys. */
        readonly keyCount: number;
        [Symbol.iterator](): Iterator<[string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Two-way string-to-string map. */
    class BiMap {
        /** Creates an empty bimap. */
        constructor();
        /** Rebuilds a bimap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BiMap;
        /** Throws `TypeError` if the value is already bound to another key. */
        set(key: string, value: string): this;
        /** Sets the pair, evicting any entry that already holds the value. */
        forceSet(key: string, value: string): this;
        /** Forward lookup. */
        get(key: string): string | undefined;
        /** The key mapped to `value` (the inverse lookup). */
        keyOf(value: string): string | undefined;
        /** Whether the key is bound. */
        has(key: string): boolean;
        /** Whether the value is bound. */
        hasValue(value: string): boolean;
        /** Drops the key and its value, reporting whether it was present. */
        delete(key: string): boolean;
        /** Drops the value and its key, reporting whether it was present. */
        deleteValue(value: string): boolean;
        /** The forward pairs. */
        entries(): [string, string][];
        /** The inverse pairs. */
        inverseEntries(): [string, string][];
        /** Drops all pairs. */
        clear(): void;
        /** The number of pairs. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Sparse two-dimensional string-to-string-to-value map. */
    class Table<V = unknown> {
        /** Creates an empty table. */
        constructor();
        /** Rebuilds a table from its record. */
        static deserialize<T>(this: new () => Table<T>, bytes: Uint8Array | ArrayBuffer): Table<T>;
        /** Sets a cell, replacing the old value. */
        put(row: string, col: string, value: V): this;
        /** The cell value. */
        get(row: string, col: string): V | undefined;
        /** Whether the cell is present. */
        has(row: string, col: string): boolean;
        /** Removes a cell, reporting whether it was present. */
        delete(row: string, col: string): boolean;
        row(r: string): [string, V][];
        /** The column's cells; a linear scan of the sparse cell array. */
        column(c: string): [string, V][];
        /** Every occupied cell. */
        cells(): [string, string, V][];
        /** The cell count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** A set of numeric intervals [lo, hi), kept disjoint and merged on insert. */
    class RangeSet {
        /** Creates an empty range set. */
        constructor();
        /** Rebuilds a range set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): RangeSet;
        /** Unions an interval; overlapping neighbours merge as needed. NaN bounds throw `RangeError`. */
        add(lo: number, hi: number): this;
        /** Subtracts an interval; overlapping neighbours split as needed. */
        remove(lo: number, hi: number): this;
        /** Whether the point is covered. */
        contains(x: number): boolean;
        /** True when [lo, hi) lies entirely inside the set. */
        encloses(lo: number, hi: number): boolean;
        /** Whether the interval overlaps any stored range. */
        intersects(lo: number, hi: number): boolean;
        /** The disjoint ranges in order. */
        ranges(): [number, number][];
        /** The gaps of [lo, hi) outside the set. */
        complement(lo: number, hi: number): [number, number][];
        /** Drops all ranges. */
        clear(): void;
        /** The number of disjoint ranges. */
        readonly size: number;
        /** The total covered length. */
        readonly measure: number;
        [Symbol.iterator](): Iterator<[number, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Map from numeric intervals [lo, hi) to values; overlapping puts split. */
    class RangeMap<V = unknown> {
        /** Creates an empty range map. */
        constructor();
        /** Rebuilds a range map from its record. */
        static deserialize<T>(this: new () => RangeMap<T>, bytes: Uint8Array | ArrayBuffer): RangeMap<T>;
        /** Assigns the whole span; an empty range (`hi <= lo`) stores nothing. */
        put(lo: number, hi: number, value: V): this;
        /** The value of the range containing `x`, or `undefined` if uncovered. */
        get(x: number): V | undefined;
        /** Clears a span, splitting neighbour ranges. */
        remove(lo: number, hi: number): this;
        /** The stored ranges in order. */
        entries(): [number, number, V][];
        /** The number of stored ranges. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, number, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Interval store with overlap enumeration ([lo, hi) spans). */
    class IntervalTree<V = unknown> {
        /** Creates an empty interval tree. */
        constructor();
        /** Rebuilds an interval tree from its record. */
        static deserialize<T>(this: new () => IntervalTree<T>, bytes: Uint8Array | ArrayBuffer): IntervalTree<T>;
        /** `lo` must not exceed `hi`; an inverted pair or a NaN bound is a RangeError. */
        insert(lo: number, hi: number, value: V): this;
        /** Entries whose interval overlaps [lo, hi); at(x) returns those containing x. */
        overlapping(lo: number, hi: number): [number, number, V][];
        /** The degenerate point query `[x, x]`. */
        at(x: number): [number, number, V][];
        /** The interval count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Count-min sketch: depth rows of width saturating counters. */
    class CountMinSketch {
        /** `depth` defaults to 5 (at most 64); width * depth must not exceed 2^24 counters. */
        constructor(width: number, depth?: number);
        /** Rebuilds a sketch from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): CountMinSketch;
        /** Counts `n` occurrences. Defaults: n 1. */
        add(key: string, n?: number): this;
        /** The estimate for the key; an overestimate (an upper bound). */
        count(key: string): number;
        /** Adds another sketch of the same shape into this one. */
        merge(other: CountMinSketch): this;
        /** The number of counters per row. */
        readonly width: number;
        /** The number of rows. */
        readonly depth: number;
        /** The total number of occurrences added. */
        readonly totalCount: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }

    /** Cardinality estimator over string keys. */
    class HyperLogLog {
        /** `precision` is 4..18, default 14 (16 KiB of registers). */
        constructor(precision?: number);
        /** Rebuilds an estimator from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): HyperLogLog;
        /** Folds the key's hash into one register. */
        add(key: string): this;
        /** The cardinality estimate. */
        count(): number;
        /** Unions the registers. Requires equal precision (else `TypeError`). */
        merge(other: HyperLogLog): this;
        /** The exponent: the estimator holds `2^precision` registers. */
        readonly precision: number;
        /** The number of registers (`2^precision`). */
        readonly registers: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
    }
}

/** dyna:sys — the OS boundary: environment, argv, process identity and limits, CPU/memory facts, Exec (blocking) and Spawn (async) subprocesses without a shell. */
declare module "dyna:sys" {
    /** A snapshot object of the current environment. */
    function env(): Record<string, string>;
    /** A NUL-bearing name is refused (no environment name can contain one). */
    function getEnv(name: string): string | undefined;
    /** Overwrites an entry; refuses empty names, `=` in a name, or NUL. NOT synchronized with worker/HTTP/pool threads: mutate before starting them. */
    function setEnv(name: string, value: string): void;
    /** The process argument vector (argv[0] first). */
    function args(): string[];
    /** Current working directory; grows past `PATH_MAX` rather than truncating. */
    function cwd(): string;
    /** A NUL-bearing path is refused rather than chdir'd by its prefix. */
    function chDir(path: string): void;
    /** "darwin", "linux", or the lowercased uname sysname ("freebsd", ...); "unknown" only when uname() fails. */
    function platform(): string;
    /** The uname machine name, normalised: aarch64 -> "arm64", amd64 -> "x86_64"; others pass through lowercased. */
    function arch(): string;
    /** The five utsname fields as strings; nodename matches hostName(). */
    function uname(): { sysname: string; nodename: string; release: string; version: string; machine: string };
    /** The real user id. */
    function getuid(): number;
    /** The real group id. */
    function getgid(): number;
    /** setuid(2); without privilege throws an Error with code "EPERM"; id must be a non-negative integer. */
    function setUid(id: number): void;
    /** setgid(2); same discipline as setUid. */
    function setGid(id: number): void;
    /** CPU time of THIS process as {user, system} in fractional seconds (getrusage). */
    function cpuUsage(): { user: number; system: number };
    /** The full getrusage(RUSAGE_SELF) struct; user/system in seconds, maxrss normalised to BYTES; the fault/switch counters are kernel-specific. */
    function rusage(): { user: number; system: number; maxrss: number; idrss: number; isrss: number; minflt: number; majflt: number; nswap: number; inblock: number; oublock: number; msgsnd: number; msgrcv: number; nsigs: number; nvcsw: number; nivcsw: number };
    /** The process id. */
    function pid(): number;
    /** The host name. */
    function hostName(): string;
    /** `$HOME`, else the passwd entry. */
    function homeDir(): string;
    /** {model, cores?, threads, mhz?, features} of the selected SIMD dispatch. */
    function cpuInfo(): { model: string; cores?: number; threads: number; mhz?: number; features: string[] };
    /** System memory in bytes. */
    function memInfo(): { total: number; free: number; available: number };
    /** [1, 5, 15]-minute load averages. */
    function loadAvg(): [number, number, number];
    /** Seconds since boot. */
    function uptime(): number;
    /** Volume statistics for the filesystem CONTAINING path. */
    function diskUsage(path: string): { total: number; free: number; available: number };
    /** The engine's allocation counters, OS peakRss, and the native ledger: nativeSize (libc memory held by dyna:* modules) and nativeLimit (the cap in force, 0 = none). */
    function memoryUsage(): { mallocCount: number; mallocSize: number; memoryUsedCount: number; memoryUsedSize: number; objCount: number; objSize: number; strCount: number; strSize: number; propCount: number; shapeCount: number; arrayCount: number; peakRss: number; nativeSize: number; nativeLimit: number };
    /** Caps module-native memory at `bytes` (0 = uncapped); allocation past it throws out-of-memory. RRule and the Temporal classes are accounted like every other native class.
     *  Under an operator ceiling (--native-memory-limit) a script may only LOWER the cap; memory owned by linked libraries (OpenSSL, SQLite) is not counted. */
    function setNativeMemoryLimit(bytes: number): void;

    /** Result of Exec, the canonical way to run a program: argv only, no shell, NUL-bearing strings refused (os.exec is the --std legacy form). */
    interface ExecResult {
        code: number | null;
        signal: string | null;
        stdout: string | Uint8Array;
        stderr: string | Uint8Array;
        timedOut: boolean;
    }
    /** Options for Exec. */
    interface ExecOptions {
        cwd?: string;
        /** REPLACES the child's environment entirely (execve semantics); parent env not merged. */
        env?: Record<string, string>;
        input?: BytesInput;
        timeoutMs?: number;
        /** Per-stream stdout/stderr cap in bytes (default 8 MiB); crossing it kills the child (RangeError). */
        maxBuffer?: number;
        encoding?: "utf8" | "bytes";
        /** Drops the child's credentials after chdir (gid before uid); non-negative integers. */
        uid?: number;
        gid?: number;
    }
    /** Runs a program to completion, BLOCKING the thread, and returns its ExecResult; throws when it cannot spawn. */
    function Exec(command: string, args?: string[], options?: ExecOptions): ExecResult;
    /** Resolves a program name against PATH; null when not found. */
    function Which(name: string): string | null;

    /* ---- Spawn: the async child with streaming stdio ------------ */

    /** One output pipe in ByteSource shape (read(buf) resolves a count, 0 = EOF). Buffered up to `maxPipe`, then the child BLOCKS: true backpressure. One read at a time. */
    interface SpawnPipe {
        read(buf: Uint8Array): Promise<number>;
        /** Stop draining. A pending read() is rejected, not left hanging. */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }

    /** The child's stdin as a sink (write/flush/close), present when stdin is a pipe; the queue is bounded by maxPipe, and EPIPE rejects flush and later writes. */
    interface SpawnStdin {
        write(buf: BytesInput): Promise<number>;
        flush(): Promise<void>;
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }

    /** Options for Spawn. */
    interface SpawnOptions {
        cwd?: string;
        env?: Record<string, string>;
        /** Written to the child's stdin, then EOF (implies stdin: "pipe"). */
        input?: BytesInput;
        /** "ignore" (default: /dev/null, the child reads EOF), "pipe" (p.stdin becomes writable), or "inherit". */
        stdin?: "pipe" | "ignore" | "inherit";
        /** SIGTERM at the deadline, SIGKILL after a 2 s grace; wait() reports timedOut. Pending reads settle at EOF even if a grandchild inherited the pipe. */
        timeoutMs?: number;
        /** Per-pipe buffer bound where backpressure starts (default 8 MiB, at most 2^30). */
        maxPipe?: number;
        /** Same discipline as Exec's uid/gid. */
        uid?: number;
        gid?: number;
        /** Whether empty PATH elements mean the cwd (execvp behaviour). Default true. */
        allowPathCwd?: boolean;
    }

    /** What Spawn.wait() resolves with. */
    interface SpawnResult {
        /** Exit code, or null when a signal killed the child (Exec's rule). */
        code: number | null;
        /** "SIGTERM", "SIGKILL", ... or null for a normal exit. */
        signal: string | null;
        /** True when opts.timeoutMs fired. */
        timedOut: boolean;
    }

    /** The async child process: argv only, no shell, nothing blocks the JS thread; stdout/stderr are ByteSource-shaped pipes on the shared reactor.
     *  A dropped or closed Spawn SIGKILLs and reaps the child GROUP; pending reads and wait() still settle. No zombies. */
    class Spawn {
        constructor(command: string, args?: string[], options?: SpawnOptions);
        /** The child's pid; -1 once closed. */
        get pid(): number;
        /** Asks the kernel right now (WNOHANG), not just the last tick. */
        get exited(): boolean;
        get closed(): boolean;
        readonly stdout: SpawnPipe;
        readonly stderr: SpawnPipe;
        readonly stdin: SpawnStdin;
        /** Resolves {code, signal, timedOut} when the child exits; one wait() at a time; replayed after exit. */
        wait(): Promise<SpawnResult>;
        /** Signals the child GROUP with a number (1..31) or name (default SIGTERM); true when the child was still running. */
        kill(signal?: number | string): boolean;
        /** Deterministic teardown: SIGKILL the group, reap, release everything, settle a parked wait(). */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }
}

/** dyna:time — clocks, RFC 3339 and Go-style layouts, durations, civil types (PlainDate/PlainTime/PlainDateTime), time zones, RFC 5545 recurrence (RRule). */
declare module "dyna:time" {
    // POSIX only. Clocks: now()/nowSec()/nowMillis()/nowUnixNano() read the WALL clock; monotonicNano() is MONOTONIC — use it to measure durations.

    /** Go-style duration units in NANOSECONDS (Second === 1e9): multiply to build the values parseDuration returns. */
    const Nanosecond: number;
    const Microsecond: number;
    const Millisecond: number;
    const Second: number;
    const Minute: number;
    const Hour: number;

    /** Parses "300ms", "-1.5h", "2h45m" into a count of NANOSECONDS: a `number` below 2^53 ns, at or above 2^53 ns a `bigint`. Units ns/us/ms/s/m/h; "d" is NOT accepted. */
    function parseDuration(str: string): number | bigint;
    /** parseDuration in MILLISECONDS, always a `number` (the correctly-rounded double of ns/1e6). */
    function parseDurationMs(str: string): number;
    /** parseDuration in SECONDS, always a `number`. */
    function parseDurationSecs(str: string): number;
    /** The inverse of parseDuration; 0 is "0s" ("2h45m" round-trips as "2h45m0s"). */
    function durationString(ns: number | bigint): string;

    /** A calendar duration; years fold into months and weeks into days. */
    class Duration {
        /** Components are limited to |value| <= 1e12 (RangeError beyond; keeps downstream int64 folds in range). */
        constructor(opts?: { years?: number; months?: number; weeks?: number; days?: number; hours?: number; minutes?: number; seconds?: number; milliseconds?: number });
        readonly years: number;
        readonly months: number;
        /** The day count. */
        readonly days: number;
        /** 1, -1 or 0. */
        readonly sign: number;
        /** True when every component is zero. */
        readonly blank: boolean;
        /** ISO 8601; a mixed-sign value throws. */
        toString(): string;
    }

    /** `new Duration({ milliseconds: ms })` as a call (non-integers truncate toward zero). */
    function durationMs(ms: number | bigint): Duration;
    /** `new Duration({ seconds: s })` as a call; fractional input truncates to whole seconds FIRST (use durationMs for sub-second precision). */
    function durationSecs(s: number | bigint): Duration;

    /** {sec, nsec} from CLOCK_REALTIME (wall time). */
    function now(): { sec: number; nsec: number };
    /** Whole seconds since the Unix epoch (wall time) -- now().sec as one number. */
    function nowSec(): number;
    /** BigInt nanoseconds since the Unix epoch (wall time); int64, so it ends in 2262. */
    function nowUnixNano(): bigint;
    /** Alias of nowUnixNano: the same clock under the duration-side name. */
    function nowNanos(): bigint;
    /** Milliseconds since the Unix epoch (wall time), like Date.now(). */
    function nowMillis(): number;
    /** BigInt nanoseconds from CLOCK_MONOTONIC (arbitrary origin): durations only, never compared across processes. */
    function monotonicNano(): bigint;

    /** RFC 3339 text for unix `sec`: the legacy tail (nsec, utc), or a strict bag { nsec, offsetMinutes } rendering at a fixed offset (-1439..1439). Slots are typed, never coerced. */
    function formatRFC3339(sec: number, nsec?: number, utc?: boolean): string;
    function formatRFC3339(sec: number, opts: { nsec?: number; offsetMinutes?: number }): string;
    /** Formats with Go-style layout tokens (2006 Jan Mon 01 02 15 04 05) plus %z; { offsetMinutes } fixes the offset (default UTC). */
    function formatUnix(sec: number, layout: string, opts?: { offsetMinutes?: number }): string;
    /** Strict RFC 3339 parse; returns {sec, nsec}. */
    function parseRFC3339(str: string): { sec: number; nsec: number };
    /** Unix seconds (UTC) from civil fields; an out-of-range month carries; |day| <= 1e9 and |hour/minute/second| <= 1e6, else RangeError. */
    function date(y: number, mo: number, d: number, h?: number, mi?: number, s?: number): number;
    /** {year, month, day, hour, min, sec, weekday, yday}; weekday 0 = Sunday. */
    function fromUnix(sec: number): { year: number; month: number; day: number; hour: number; min: number; sec: number; weekday: number; yday: number };

    /** A compiled Go-style layout; { offsetMinutes } fixes the offset for format() and for parse() unless the input's own %z supplies one. */
    class Format {
        constructor(layout: string, opts?: { offsetMinutes?: number });
        /** Formats unix seconds with this layout. */
        format(sec: number): string;
        /** The strict inverse; omitted fields default to 1970-01-01T00:00:00Z; an unsupported year is a SyntaxError. */
        parse(str: string): number;
        /** The original layout. */
        readonly layout: string;
    }

    /** An immutable calendar date, proleptic Gregorian. */
    class PlainDate {
        /** All three are required. An impossible date is refused, never rolled over: `31 February` throws. */
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
        /** `-1`, `0` or `1`. */
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
        /** The Duration from this to `other`: whole months first, then days, then the time remainder in ms, so a.add(a.until(b)) equals b exactly. */
        until(other: PlainDateTime): Duration;
        toPlainDate(): PlainDate;
        toPlainTime(): PlainTime;
        /** `-1`, `0` or `1`. */
        compare(other: PlainDateTime): -1 | 0 | 1;
        /** ISO 8601 with `T`, milliseconds omitted when zero. */
        toString(): string;
    }

    /** A wall-clock time of day; one integer millisecond count since midnight. */
    class PlainTime {
        /** `24:00` is refused (it names the same wall clock as `00:00`). */
        constructor(hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        /** The whole value. */
        readonly msSinceMidnight: number;
        /** Adds the time part, wrapping at midnight; a duration carrying MONTHS is a RangeError, weeks/days are ignored. */
        add(duration: Duration): PlainTime;
        subtract(duration: Duration): PlainTime;
        /** `-1`, `0` or `1`. */
        compare(other: PlainTime): -1 | 0 | 1;
        /** `"HH:MM:SS"`, milliseconds appended when non-zero. */
        toString(): string;
    }

    /** RFC 5545 recurrence rules, UTC whole-second unix time. */
    interface RRuleOptions {
        freq: "YEARLY" | "MONTHLY" | "WEEKLY" | "DAILY" | "HOURLY" | "MINUTELY" | "SECONDLY";
        interval?: number;
        /** count and until are MUTUALLY EXCLUSIVE (RFC 5545): both set is a TypeError (options and string parsers alike). */
        count?: number;
        until?: Date | string | number;
        dtstart?: Date | string | number;
        wkst?: number | "MO" | "TU" | "WE" | "TH" | "FR" | "SA" | "SU";
        bymonth?: number[];
        bymonthday?: number[];
        byyearday?: number[];
        byweekno?: number[];
        bysetpos?: number[];
        byweekday?: (string | number)[];
    }
    /** An RFC 5545 recurrence rule, expanded into Dates on demand. */
    class RRule {
        /** `BYHOUR`/`BYMINUTE`/`BYSECOND`/`BYEASTER` are refused rather than silently ignored. */
        constructor(opts: RRuleOptions);
        /** Parses "RRULE:FREQ=..." parts plus optional DTSTART: lines; `opts.dtstart` supplies the start otherwise. */
    static fromString(str: string, opts?: { dtstart?: Date | string | number }): RRule;
        /** Every occurrence as Dates (`limit` caps the count; an uncounted infinite rule refuses). Expansion has a budget of 2^28 day/position steps (RangeError). */
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
        /** `opts.now` pins the reference time; it is the only valid key. */
        constructor(locale?: string, opts?: { now?: number });
        /** Unix seconds, or null when nothing matches. */
        parse(text: string): number | null;
        readonly locale: string;
        readonly dayFirst: boolean;
    }

    /** Strict ISO "YYYY-MM-DD" parse (parseTime rejects a seconds field of 60; the RFC 3339 parsers fold it into the next minute). */
    function parseDate(text: string): PlainDate;
    /** The PlainDate `n` days after 1970-01-01. */
    function dateFromEpochDay(n: number): PlainDate;
    /** Strict parse of "HH:MM[:SS[.mmm]]". */
    function parseTime(text: string): PlainTime;
    /** Joins a PlainDate and a PlainTime by pure field copy (the inverse of toPlainDate()/toPlainTime()). */
    function toPlainDateTime(date: PlainDate, time: PlainTime): PlainDateTime;
}

/** dyna:uring — batched disk reads over io_uring; the module exists only in Linux builds with CONFIG_IO_URING=y. */
declare module "dyna:uring" {
    /** Whole file as a string via the io_uring bulk reader (Linux only); no size cap. */
    function readFile(path: import("dyna:file").Path): string;
    /** Whole file as a string via the blocking pread(2) reference reader; no size cap. */
    function readFileSync(path: import("dyna:file").Path): string;
    /** Whole file as a fresh Uint8Array via the io_uring bulk reader; same Path contract as readFile. */
    function readFileBytes(path: import("dyna:file").Path): Uint8Array;
    /** Byte count plus a 32-bit FNV-1a checksum of a whole file; NON-cryptographic (use dyna:hash for integrity). */
    function checksum(path: import("dyna:file").Path, useUring?: boolean): { bytes: number; sum: number };
}

/** dyna:url — WHATWG URL and URLSearchParams (the same classes as the globals), IDNA 2008 / UTS #46, Punycode, form encoding. */
declare module "dyna:url" {
    /** WHATWG URL. Input over 65536 bytes throws; a non-ASCII host over 4096 mapped code points is invalid; href/protocol/username/password are read-only. */
    class URL {
        /** Parses `input` against an optional `base` URL; a bad `base` or unparsable `input` throws, and input longer than 65536 bytes throws a RangeError. */
        constructor(input: string, base?: string);
        /** Parses without throwing: null for every failure the constructor would throw (a non-string argument is still a TypeError). */
        static parse(input: string, base?: string): URL | null;
        /** parse() as a predicate. */
        static canParse(input: string, base?: string): boolean;
        /** Resolves `rel` against `base` and returns the href; THROWS TypeError when either is unparseable. */
        static join(base: string, rel: string): string;
        readonly href: string;
        readonly protocol: string;
        readonly username: string;
        readonly password: string;
        /** Settable; a `:port` suffix splits off; a forbidden host code point or an empty value makes the assignment a no-op. */
        host: string;
        /** Settable (lowercased; the port is untouched); invalid input is a no-op. */
        hostname: string;
        /** Settable; digits only (anything else is a no-op); "" clears the port. */
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
    /** WHATWG query list; one obtained from url.searchParams writes through to that URL. entries/keys/values are live iterators (the *Array() twins return arrays). */
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
    /** IDNA 2008 / UTS #46 ToASCII (Unicode 16.0) with CONTEXTJ/CONTEXTO rules; {transitional} selects transitional processing; a disallowed name is a TypeError. */
    function domainToASCII(domain: string, options?: { transitional?: boolean }): string;
    /** IDNA ToUnicode: decodes punycode labels for display. */
    function domainToUnicode(domain: string, options?: { transitional?: boolean }): string;
    /** RFC 3492 encoding; input over 1024 code points is refused. */
    function punycodeEncode(text: string): string;
    /** RFC 3492 decoding; capped at 1024 UTF-8 octets (RangeError beyond), the decode-side DoS bound. */
    function punycodeDecode(text: string): string;
    /** Percent-encodes the object's own enumerable string keys into a=1&b=2. */
    function formEncode(obj: Record<string, unknown>): string;
    /** Decodes `+` as space, keeps the LAST value per key. */
    function formDecode(text: string): Record<string, string>;
    /** encodeURIComponent plus !'()~. */
    function encodeURIComponentStrict(text: string): string;
}

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

/** dyna:validate — boolean predicates for common formats: email, URL, UUID, IP, credit card, IBAN, JWT shape, base64, hex, JSON, MIME, RFC 3339, strong passwords. */
declare module "dyna:validate" {
    /** ASCII letters only. */
    function IsAlpha(text: string): boolean;
    /** ASCII letters and digits. */
    function IsAlphanumeric(text: string): boolean;
    /** Non-empty text whose every byte is below 0x80; IsAscii("") is false. */
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
    /** ITU-T E.164: optional `+`, 8..15 digits. */
    function IsE164(text: string): boolean;
    /** Strict dotted-quad: four decimal octets 0..255, no leading zeros, no whitespace. */
    function IsIPv4(text: string): boolean;
    /** RFC 4291 text grammar incl. `::` compression (once) and an embedded IPv4 tail; no zone ids. */
    function IsIPv6(text: string): boolean;
    /** IsIPv4 or IsIPv6. */
    function IsIP(text: string): boolean;
    /** Decimal 0..65535, no sign, no leading zeros; "0" is allowed (a kernel-assigned port). */
    function IsPort(text: string): boolean;
    /** RFC 4648 canonical base64: `+/` alphabet, proper padding, zero pad bits. */
    function IsBase64(text: string): boolean;
    /** RFC 4648 sec.5 base64url: the `-_` alphabet, unpadded and canonical (zero pad bits). */
    function IsBase64Url(text: string): boolean;
    /** An even-length run of hex digits -- the shape a byte string must have. */
    function IsHex(text: string): boolean;
    /** `#RGB`, `#RRGGBB` or `#RRGGBBAA`; the `#` is required. */
    function IsHexColor(text: string): boolean;
    /** JSON.parse accepts it (any JSON value; input cap 4096 bytes). */
    function IsJSON(text: string): boolean;
    /** RFC 6838 simplified: token type/subtype, optional `; name=value` parameters. */
    function IsMimeType(text: string): boolean;
    /** Full RFC 3339 date-time: `T` separator, `Z` or `+/-HH:MM` offset; the leap second `:60` is allowed. */
    function IsRFC3339(text: string): boolean;
    /** Strict ISO calendar date YYYY-MM-DD with real month/day validation (leap years included). */
    function IsDateString(text: string): boolean;
    /** Optional sign, digits, at most one decimal point; exponent notation is refused. */
    function IsNumeric(text: string): boolean;
    /** Printable ASCII with class minimums; defaults 8/1/1/1/1; symbols are printable non-alphanumeric ASCII. */
    function IsStrongPassword(text: string, opts?: {
        minLength?: number;
        minLower?: number;
        minUpper?: number;
        minDigits?: number;
        minSymbols?: number;
    }): boolean;
}

/** dyna:oauth2 — OAuth 2.0 helpers (RFC 6749/6750/7636): PKCE, state, bearer parsing, redirect/scope checks, claim-validating verifyJWT (needs CONFIG_TLS). No IO. */
declare module "dyna:oauth2" {
    /** 43..128 unreserved chars; 32 octet CSPRNG -> 43 char base64url */
    function generateCodeVerifier(): string;
    /** True for a PKCE code_verifier (43..128 unreserved characters, RFC 7636). */
    function isValidCodeVerifier(s: string): boolean;
    /** S256 = BASE64URL(SHA256(verifier)), plain = verifier; S256 default */
    function generateCodeChallenge(verifier: string, method?: "S256" | "plain"): string;
    /** Verifies a PKCE challenge; method "S256"|"plain" (else TypeError); constant-time. */
    function verifyCodeChallenge(verifier: string, challenge: string, method: string): boolean;
    /** CSPRNG bytes -> base64url; default 32 bytes */
    function generateState(bytes?: number): string;
    /** Constant-time compare over UTF-8 strings or raw bytes; the convenience form of crypto.TimingSafeEqual. */
    function secureCompare(a: BytesInput, b: BytesInput): boolean;
    /** Client Basic auth per RFC 6749 s2.3.1: "Basic " + base64(formEncode(id:secret)), prefix included. */
    function buildClientAuthHeader(clientId: string, clientSecret?: string): string;
    /** Verifies a JWT access token: alg allowlist (`none` refused), key family, then exp/nbf/aud/iss/scope; clockSkewSec 0..300; requireExp rejects a missing exp. */
    function verifyJWT<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[]; aud?: string | string[]; iss?: string; requiredScope?: string[]; clockSkewSec?: number; requireExp?: boolean }): T;
    /** NQCHAR scope tokens, space-delimited, per RFC 6749 A.4 */
    function parseScope(scope: string): string[];
    /** Joins scopes with single spaces. */
    function formatScope(scopes: string[]): string;
    /** "Bearer <b64token>" with b64token validation */
    function buildBearerHeader(token: string): string;
    /** The token from an `Authorization: Bearer ...` header value, or null. */
    function parseBearerHeader(header: string): string | null;
    /** headers.authorization/query/body; allowQuery/allowBody opt-in (default false) */
    function parseBearerFromRequest(req: { headers?: Record<string,string>; query?: string; body?: string; allowQuery?: boolean; allowBody?: boolean }): string | null;
    /** Validates the base64url `b64token` grammar. */
    function isValidBearerToken(token: string): boolean;
    /** WWW-Authenticate: Bearer realm, error, error_description, error_uri, scope. error is REQUIRED (RFC 6750 ). */
    function buildWWWAuthenticate(opts: { realm?: string; error: string; errorDescription?: string; error_description?: string; errorUri?: string; error_uri?: string; scope?: string }): string;
    /** Verbatim exact except loopback port-ignore per RFC 8252 */
    function isValidRedirectUri(candidate: string, registered: string[]): boolean;
    /** Builds the authorization URL (response_type=code), generating `state` when absent; the endpoint must be https (loopback excepted) unless allowInsecure. */
    function buildAuthorizationUrl(opts: { authorizationEndpoint: string; clientId: string; redirectUri: string; responseType?: "code"; scope?: string; state?: string; codeChallenge?: string; codeChallengeMethod?: "S256" | "plain"; allowPlain?: boolean; allowInsecure?: boolean; extraParams?: Record<string,string> }): { url: string; state: string };
    /** Parse ?code/state/error from redirect URL (query, not fragment for code flow); refuses implicit-flow fragments, code+error, duplicate params */
    function parseAuthorizationResponse(url: string): { code?: string; state?: string; error?: string; errorDescription?: string; errorUri?: string };
    /** application/x-www-form-urlencoded body builder; every value must be a string */
    function buildTokenRequestBody(params: Record<string,string>): string;
    /** Token response per RFC 6749: access_token + token_type Bearer required, b64token access_token */
    interface TokenResponse {
        access_token: string;
        token_type: string;
        expires_in?: number;
        refresh_token?: string;
        scope?: string;
        [key: string]: unknown;
    }
    /** Parses a token endpoint JSON body into a TokenResponse. */
    function parseTokenResponse(body: string): TokenResponse;
}

/** dyna:xml — XML three ways: a tree (XMLParse), streaming SAX (push or pull), and a streaming writer; size caps are on and no DTD is processed. */
declare module "dyna:xml" {
    /** A document-tree element node. */
    interface XMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | XMLElement)[];
    }

    /** Parses a document into a tree; `multiple: true` returns every root element. At most one DOCTYPE, before the root; only the five predefined entities exist.
     *  Limits: 256 MiB input (RangeError), 256 open elements (SyntaxError), 16 MiB per token. */
    function XMLParse(text: string, opts?: { trim?: boolean; entities?: "strict" | "keep"; multiple?: boolean }): XMLElement | XMLElement[];
    /** Serializes a node; indent 0..16 spaces; nesting beyond 256 throws. The options bag is strict (unknown key throws). */
    function XMLStringify(node: XMLElement, opts?: { indent?: number }): string;
    /** Collapses an element into a plain object keyed by element name. */
    function XMLToObject(node: XMLElement): Record<string, unknown>;

    /** A streaming document writer for output too deep or long for a tree: only the open-element stack is held; nesting to 65536. */
    class XmlWriter {
        constructor(opts?: { indent?: number });
        /** Writes `<name attrs>` (attribute values escaped) and pushes the element; invalid names throw. */
        open(name: string, attrs?: Record<string, string>): void;
        /** Appends escaped text (& < >). */
        text(s: string): void;
        /** Writes `</name>` for the innermost open element; throws when none is open. */
        close(): void;
        /** The bytes so far, WITHOUT closing anything. */
        toString(): string;
        /** Closes every open element and returns the complete document; idempotent. */
        finish(): string;
    }

    /** Streaming SAX handlers. */
    interface SAXHandlers {
        onOpen?: (name: string, attrs: Record<string, string>) => void;
        onClose?: (name: string) => void;
        onText?: (text: string) => void;
        onCData?: (text: string) => void;
        onComment?: (text: string) => void;
        onPI?: (target: string, data: string) => void;
    }
    /** One pull event: `name` on open/close/pi, `attrs` on open, `text` on text/cdata/comment/pi; `offset` is the absolute byte offset in the stream. */
    interface SAXEvent {
        event: "open" | "close" | "text" | "cdata" | "comment" | "pi";
        name?: string;
        attrs?: Record<string, string>;
        text?: string;
        offset: number;
    }
    /** Streaming SAX parser with XMLParse's document rules; total input is capped at 256 MiB (RangeError), the XMLParse limit.
     *  MODE: with a handlers object it is a PUSH parser (write() fires handlers); with none a PULL parser (write() buffers, next() returns one SAXEvent or null). */
    class SAXParser {
        /** The handlers object is also the options bag (`trim`, `entities`); omit it for the pull form, whose options come from the second argument. */
        constructor(handlers?: SAXHandlers & { trim?: boolean; entities?: "strict" | "keep" }, opts?: { entities?: "strict" | "keep" });
        /** Feeds a string or any byte view. */
        write(chunk: BytesInput): void;
        /** Finalizes the stream. Push parsers run their end-of-document checks here (trailing content throws); pull parsers defer them to the last next(). */
        end(): void;
        /** Pull form: scans forward and returns the next SAXEvent, or null while the input is drained (feed more, or the document is finished). Errors throw and are sticky. */
        next(): SAXEvent | null;
        /** Async-iterable wrapper: drives a pull parser over any sync/async iterable of chunks, yielding one SAXEvent per step. */
        static iterate(source: AsyncIterable<BytesInput> | Iterable<BytesInput>, opts?: { entities?: "strict" | "keep" }): AsyncGenerator<SAXEvent>;
    }
}

/** dyna:yaml — YAML 1.2 core schema: Parse, ParseAll, lazy ParseStream, Stringify; anchors and aliases are refused by name, never expanded. */
declare module "dyna:yaml" {
    /** Parses exactly one document. { maxDepth: 1..128 } lowers the nesting cap; { schema } accepts "core" and refuses "full" by name. */
    function Parse(text: string, opts?: { maxDepth?: number; schema?: "core" | "full" }): unknown;
    /** Parses every `---`-separated document into an array. Same options as Parse. */
    function ParseAll(text: string, opts?: { maxDepth?: number; schema?: "core" | "full" }): unknown[];
    /** Serializes to YAML. { indent } 1..10 (default 2); { width } > 0 folds long strings losslessly (0 = never); { sortKeys } orders keys bytewise; { flow } emits `[a, b]`. */
    function Stringify(value: unknown, opts?: {
        indent?: number;
        width?: number;
        sortKeys?: boolean;
        flow?: boolean;
    }): string;
    /** Lazy multi-document parsing: a SYNC iterator over the `---`-separated documents, one alive at a time; an error names its document and line and ends the iterator. */
    function ParseStream(text: string, opts?: { maxDepth?: number; schema?: "core" | "full" }): Iterator<unknown>;
}
/** dyna:dataframe — in-memory columnar analytics over TypedArrays: UPPERCASE verbs for filter masks, sort, group-by, joins, windows, quantiles, CSV out.
 *  Masks are Uint8Arrays produced by predicate verbs and accepted by aggregates; string columns are dictionary-encoded. */
declare module "dyna:dataframe" {
    /** One column's description. Numeric text uses `.` regardless of the host locale; POSIX GCC/Clang only (MSVC is not a verified target). */
    interface DataFrameColumn {
        name: string;
        type: string;
    }
    /** A grouped aggregate: keys[i] pairs with values[i]. */
    interface GroupResult {
        keys: (string | number)[];
        values: Float64Array;
    }
    /** Grouped arrays: keys[i] pairs with the Float64Array values[i]. */
    interface GroupArrays {
        keys: (string | number)[];
        values: Float64Array[];
    }
    /** A frame instance: shape getters plus the UPPERCASE verbs. */
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
        /** CSV text with a header row (RFC 4180 quoting); {escapeFormulas: true} prefixes `'` to cells starting with = + - @ (CSV injection); NaN prints as an empty cell. */
        TO_CSV(opts?: { escapeFormulas?: boolean }): string;
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
        /** Aggregates take a column name and an optional row mask (a Uint8Array from the predicate verbs). */
        MIN(col: string, mask?: Uint8Array): number | undefined;
        /** The maximum, ignoring NaN. */
        MAX(col: string, mask?: Uint8Array): number | undefined;
        /** The sum over the count. NaN on an empty selection. */
        MEAN(col: string, mask?: Uint8Array): number;
        /** The number of rows, or of nonzero mask bytes. */
        COUNT(col: string, mask?: Uint8Array): number;
        /** The product, accumulated in double for every column type. */
        PRODUCT(col: string, mask?: Uint8Array): number;
        /** The sum of `a[i] * b[i]` over the selection, via specialised same-type kernels and block-widened mixed pairs. */
        DOT_PRODUCT(a: string, b: string, mask?: Uint8Array): number;
        /** Sample (n-1) variance and standard deviation; the *_POP forms divide by n. */
        VARIANCE(col: string, mask?: Uint8Array): number;
        /** The sample standard deviation. */
        STDDEV(col: string, mask?: Uint8Array): number;
        /** The population variance (/n), defined from one row up. */
        VARIANCE_POP(col: string, mask?: Uint8Array): number;
        /** The population standard deviation. */
        STDDEV_POP(col: string, mask?: Uint8Array): number;
        /** The population skewness. */
        SKEW(col: string, mask?: Uint8Array): number;
        /** The population excess kurtosis. */
        KURTOSIS(col: string, mask?: Uint8Array): number;
        /** The sample skewness. 0 below n=3. */
        SKEW_SAMP(col: string, mask?: Uint8Array): number;
        /** The sample excess kurtosis. 0 below n=4. */
        KURT_SAMP(col: string, mask?: Uint8Array): number;
        /** Standard error of the mean. */
        SEM(col: string, mask?: Uint8Array): number;
        /** The number of NaN values in the selection. */
        COUNT_NULLS(col: string, mask?: Uint8Array): number;
        /** The sum of `w*x` over sum of `w`. A zero weight contributes nothing (it is not in the input set), and all-zero weights give NaN. */
        MEAN_WEIGHTED(valueCol: string, weightCol: string, mask?: Uint8Array): number;
        /** Exact integer sum over integer columns only (TypeError on float columns); RangeError when the total overflows a Number. */
        SUM_CHECKED(col: string, mask?: Uint8Array): number;
        /** {count, sum, mean, min, max, variance, stddev, skew, kurtosis} in one pass. */
        DESCRIBE(col: string, mask?: Uint8Array): { count: number; sum: number; mean: number; min: number; max: number; variance: number; stddev: number; skew: number; kurtosis: number };
        /** Shannon entropy in bits over the empirical value distribution. */
        ENTROPY(col: string, mask?: Uint8Array): number;
        /** The mean absolute deviation from the mean. `undefined` when nothing is selected. */
        MAD(col: string, mask?: Uint8Array): number | undefined;
        /** The median of `|x - median|`. `undefined` when nothing is selected. */
        MEDIAN_ABSOLUTE_DEVIATION(col: string, mask?: Uint8Array): number | undefined;

        /** Bitwise folds over integer columns; empty-selection identities as documented. */
        BITWISE_AND(col: string, mask?: Uint8Array): number;
        /** The bitwise OR of the selection. Identity 0. */
        BITWISE_OR(col: string, mask?: Uint8Array): number;
        /** The bitwise XOR of the selection. Identity 0. */
        BITWISE_XOR(col: string, mask?: Uint8Array): number;
        /** Each group's integer values fold as 32-bit patterns (ToUint32); the result is the unsigned value 0..4294967295. */
        GROUP_BIT_AND(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group bitwise OR of the integer value column. */
        GROUP_BIT_OR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group bitwise XOR of the integer value column. */
        GROUP_BIT_XOR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The count of DISTINCT non-negative integer values, one bit per value. */
        GROUP_BITMAP(col: string, mask?: Uint8Array): number;

        /** Positional access; n defaults to 5 and is clamped to the frame. */
        HEAD(col: string, n?: number, mask?: Uint8Array): Float64Array;
        /** The last `n` selected values, with the same clamping. */
        TAIL(col: string, n?: number, mask?: Uint8Array): Float64Array;
        /** The value of the first SELECTED row. `undefined` when nothing is selected (never NaN, so it cannot be confused with a NaN value). */
        FIRST(col: string, mask?: Uint8Array): number | undefined;
        /** The value of the last selected row. */
        LAST(col: string, mask?: Uint8Array): number | undefined;
        /** The row index of the minimum. Ties go to the FIRST occurrence, NaN values are skipped. */
        ARG_MIN(col: string, mask?: Uint8Array): number | undefined;
        /** The row index of the maximum. Ties go to the first occurrence. */
        ARG_MAX(col: string, mask?: Uint8Array): number | undefined;

        /** Mask-producing comparisons over a column. */
        GT(col: string, value: number): Uint8Array;
        /** Comparison verbs return a row mask (1 = match). */
        GE(col: string, value: number): Uint8Array;
        /** A mask that is 1 where `col[i] < value`. */
        LT(col: string, value: number): Uint8Array;
        /** A mask that is 1 where `col[i] <= value`. */
        LE(col: string, value: number): Uint8Array;
        /** Numeric equality mask; a string column throws TypeError — match strings with ISIN. */
        EQ(col: string, value: number): Uint8Array;
        /** Numeric inequality mask; same string-column refusal as EQ. */
        NE(col: string, value: number): Uint8Array;
        /** 1 where lo <= col[i] <= hi, inclusive at both ends. */
        BETWEEN(col: string, lo: number, hi: number): Uint8Array;
        /** Mask of NaN cells (NOT_NA is its complement). */
        IS_NA(col: string): Uint8Array;
        /** A mask that is 1 where the value is not NaN. */
        NOT_NA(col: string): Uint8Array;
        /** True when every mask byte is nonzero. */
        ALL(mask: Uint8Array): boolean;
        /** True when any mask byte is set. */
        ANY(mask: Uint8Array): boolean;
        /** The mask packed into ceil(ROWS/32) words, LSB first. */
        BITMASK(mask: Uint8Array): Uint32Array;
        /** The AND of the JS truthiness of the stored values (a float NaN is false, any nonzero int is true). */
        BOOL_AND(col: string, mask?: Uint8Array): boolean;
        /** The OR of the JS truthiness of the stored values. Vacuous over zero selected rows: false. */
        BOOL_OR(col: string, mask?: Uint8Array): boolean;
        /** The parity of the true count. Vacuous over zero selected rows: false. */
        BOOL_XOR(col: string, mask?: Uint8Array): boolean;
        /** 1 on the FIRST occurrence of each distinct value. */
        DROP_DUPLICATES(col: string, mask?: Uint8Array): Uint8Array;
        /** 1 where none of the named columns is NaN; no arguments: every numeric column. */
        DROP_NA(...cols: string[]): Uint8Array;

        /** Elementwise verbs return a Float64Array of ROWS entries; binary ones take a number or another COLUMN NAME (RSUB/RDIV: number only). */
        ABS(col: string): Float64Array;
        ABS(col: string, opts: { out: string }): this;
        /** Each element rounded half away from zero. */
        ROUND(col: string): Float64Array;
        ROUND(col: string, opts: { out: string }): this;
        /** The largest integer <= each value. */
        FLOOR(col: string): Float64Array;
        FLOOR(col: string, opts: { out: string }): this;
        /** The smallest integer >= each value. */
        CEIL(col: string): Float64Array;
        CEIL(col: string, opts: { out: string }): this;
        /** The square root of each element. */
        SQRT(col: string): Float64Array;
        SQRT(col: string, opts: { out: string }): this;
        /** The natural logarithm of each element. */
        LOG(col: string): Float64Array;
        LOG(col: string, opts: { out: string }): this;
        /** E raised to each value. */
        EXP(col: string): Float64Array;
        EXP(col: string, opts: { out: string }): this;
        /** -1, 0 or 1 per element. */
        SIGN(col: string): Float64Array;
        SIGN(col: string, opts: { out: string }): this;
        /** Each element clamped into `[lo, hi]`. NaN elements pass through unchanged. */
        CLIP(col: string, lo: number, hi: number): Float64Array;
        CLIP(col: string, lo: number, hi: number, opts: { out: string }): this;
        /** The column with NaN replaced by `value`. A NaN `value` is legal and is a no-op. */
        FILL_NA(col: string, value: number): Float64Array;
        FILL_NA(col: string, value: number, opts: { out: string }): this;
        /** Elementwise sum; a string operand is a column REFERENCE, so ADD("a", "b") adds two columns. */
        ADD(col: string, x: number | string): Float64Array;
        ADD(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise difference. */
        SUB(col: string, x: number | string): Float64Array;
        SUB(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise product. */
        MUL(col: string, x: number | string): Float64Array;
        MUL(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise quotient. */
        DIV(col: string, x: number | string): Float64Array;
        DIV(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise power. */
        POW(col: string, x: number | string): Float64Array;
        POW(col: string, x: number | string, opts: { out: string }): this;
        /** k - col; number-only operand. */
        RSUB(col: string, k: number): Float64Array;
        RSUB(col: string, k: number, opts: { out: string }): this;
        /** k / col; number-only operand. */
        RDIV(col: string, k: number): Float64Array;
        RDIV(col: string, k: number, opts: { out: string }): this;
        /** a where the mask byte is nonzero, else b. */
        WHERE(mask: Uint8Array, a: string | number, b: string | number): Float64Array;

        /** Grouped verbs: the key column is string or integer; integer keys are dense-window encoded and must lie in [0, 2^20), else RangeError. */
        GROUP_BY_SUM(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group mean. A group with no contributing row is NaN. */
        GROUP_BY_MEAN(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group minimum, ignoring NaN. An empty group is NaN. */
        GROUP_BY_MIN(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group maximum, ignoring NaN. An empty group is NaN. */
        GROUP_BY_MAX(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Rows per group; takes no value column. */
        GROUP_BY_COUNT(key: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_SUM under the name the map API uses. Identical call and result. */
        SUM_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_MIN by its map name. */
        MIN_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_MAX by its map name. */
        MAX_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Per-key arrays of values; the GROUP_ARRAY_* variants dedupe, sort, window or keep the last k. */
        GROUP_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** GROUP_ARRAY keeping only the first occurrence of each (group, value) pair. */
        GROUP_UNIQ_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** The per-group moving sum. O(m) block decomposition above `w` = 256, O(m*w) re-sum below, one shared threshold. */
        GROUP_ARRAY_MOVING_SUM(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        /** The per-group moving average. Divides by what CONTRIBUTED, never by `w`. */
        GROUP_ARRAY_MOVING_AVG(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        /** Each group's values sorted ascending, NaN last. Groups are qsorted, so one large group stays O(m log m). */
        GROUP_ARRAY_SORTED(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** The last `k` rows seen per group (a circular window, O(n) instead of O(n*k)). */
        GROUP_ARRAY_LAST(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The first k values in ROW order per group -- a deterministic head, not a random sample. */
        GROUP_ARRAY_SAMPLE(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The values present in EVERY group. */
        GROUP_ARRAY_INTERSECT(key: string, val: string, mask?: Uint8Array): Float64Array;
        /** A dense array of size slots; later rows overwrite earlier ones. */
        GROUP_ARRAY_INSERT_AT(value: string, position: string, size: number, fill?: number, mask?: Uint8Array): Float64Array;
        /** The selected values joined in row order with `sep`. String columns join their dictionary strings, numeric columns their numbers. */
        GROUP_CONCAT(col: string, sep?: string, mask?: Uint8Array): string;
        /** One JSON object mapping each key to an array of that group's values. */
        JSON_AGG(key: string, value: string, mask?: Uint8Array): string;
        /** One JSON object mapping each key to the LAST value that appeared. */
        JSON_OBJECT_AGG(key: string, value: string, mask?: Uint8Array): string;
        /** JSON_AGG, but throws a RangeError on any non-finite value instead of serialising null. */
        JSON_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;
        /** JSON_OBJECT_AGG with the same strictness. */
        JSON_OBJECT_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;

        /** Ordering: sorting, ranking, frequency. NaN sorts last. */
        SORT(col: string, mask?: Uint8Array): Float64Array;
        /** Row indices that sort the column ascending. */
        ARG_SORT(col: string, mask?: Uint8Array): Uint32Array;
        /** Average ranks; ties share the mean of their positions. */
        RANK(col: string, mask?: Uint8Array): Float64Array;
        /** Ranks counting distinct values. */
        DENSE_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** (min rank - 1) / (n - 1); a single valued row is 0. NaN rows stay NaN. */
        PERCENT_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** SQL NTILE; first n % buckets tiles take one extra row. */
        NTILE(col: string, buckets: number, mask?: Uint8Array): Float64Array;
        /** The `k` largest values in descending order. A `k` larger than the selection clamps. */
        N_LARGEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** The `k` smallest values in ascending order. */
        N_SMALLEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** Distinct values in first-seen order; strings for a string column. */
        UNIQUE(col: string, mask?: Uint8Array): Float64Array | string[];
        /** The count of distinct values PRESENT in the selection (a string column's dictionary may hold more). */
        N_UNIQUE(col: string, mask?: Uint8Array): number;
        /** Exact distinct count, or n+1 meaning "more than n"; numeric columns only. */
        UNIQ_UP_TO(col: string, n: number, mask?: Uint8Array): number;
        /** Distinct values with their counts (TOP_K keeps the k most frequent). */
        VALUE_COUNTS(col: string, mask?: Uint8Array): GroupResult;
        /** The `k` most FREQUENT values (the frequency question, not the magnitude one). */
        TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** The most frequent value; ties go to the first in row order. */
        MODE(col: string, mask?: Uint8Array): number | string | undefined;
        /** Sketch-based approximate distinct count. */
        APPROX_COUNT_DISTINCT(col: string, mask?: Uint8Array): number;
        /** A Space-Saving estimate of the `k` most frequent values. */
        APPROX_TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** Ranks by summed weight, not count. */
        APPROX_TOP_SUM(col: string, weightCol: string, k: number, mask?: Uint8Array): GroupResult;
        /** The `k` values with the largest summed weight (or frequency when the weight column is omitted). */
        TOP_K_WEIGHTED(col: string, weightCol: string | undefined, k: number, mask?: Uint8Array): GroupResult;
        /** The value holding strictly more than half the total weight. */
        ANY_HEAVY(col: string, weightCol?: string, mask?: Uint8Array): number | undefined;
        /** MinHash Jaccard estimate between two columns. */
        APPROX_SIMILARITY(a: string, b: string, mask?: Uint8Array): number;

        /** Quantiles: select rather than sort. */
        QUANTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Interpolated quantile for q in [0, 1]; PERCENTILE_DISC returns an actual element. */
        PERCENTILE_CONT(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The order statistic at `ceil(q * n)`, so the answer IS a value the column holds. */
        PERCENTILE_DISC(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** QUANTILE at 0.5. Pandas-style NaN for an empty selection. */
        MEDIAN(col: string, mask?: Uint8Array): number | undefined;
        /** The order statistic at `floor(q * (n-1))`, never interpolated. */
        QUANTILE_EXACT_LOW(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The order statistic at `ceil(q * (n-1))`, never interpolated. */
        QUANTILE_EXACT_HIGH(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Many interpolating quantiles from ONE gather. */
        QUANTILES(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** Many approximate quantiles off ONE t-digest. */
        QUANTILES_TDIGEST(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** One t-digest quantile. `undefined` for no values. */
        APPROX_PERCENTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The value at which cumulative weight first reaches `q` of the total, so a row weighing three counts as three rows. */
        QUANTILE_EXACT_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** The approximate form in bounded memory. */
        QUANTILE_TDIGEST_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** Equal-width bins over the observed range; edges has bins+1 entries. */
        HISTOGRAM(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };
        /** The same bins with counts divided by the number of contributing rows. */
        HISTOGRAM_NORMALIZED(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };

        /** Scans: windowed and sequential verbs over exactly ROWS entries. */
        CUM_SUM(col: string, mask?: Uint8Array): Float64Array;
        /** The running product (identity 1). */
        CUM_PROD(col: string, mask?: Uint8Array): Float64Array;
        /** The running maximum (identity -Infinity). */
        CUM_MAX(col: string, mask?: Uint8Array): Float64Array;
        /** The running minimum (identity +Infinity). */
        CUM_MIN(col: string, mask?: Uint8Array): Float64Array;
        /** out[i] = col[i - periods]; the vacated head/tail is NaN. */
        SHIFT(col: string, periods?: number): Float64Array;
        /** Difference from the row `periods` earlier (default 1). */
        DIFF(col: string, periods?: number): Float64Array;
        /** Rolling-window verbs over the trailing `w` rows. */
        ROLLING_SUM(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_SUM(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_SUM(col: string, w: number, opts: { out: string }): this;
        /** The trailing window mean. Divides by what CONTRIBUTED, never by `w`. */
        ROLLING_MEAN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MEAN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MEAN(col: string, w: number, opts: { out: string }): this;
        /** The trailing window minimum. Monotonic-deque O(n); NaN never enters the deque. */
        ROLLING_MIN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MIN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MIN(col: string, w: number, opts: { out: string }): this;
        /** The trailing window maximum. Same deque. */
        ROLLING_MAX(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MAX(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MAX(col: string, w: number, opts: { out: string }): this;
        /** The sample variance (ddof=1) per window, two passes, never a sum of squares around an uncentred mean. */
        ROLLING_VAR(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_VAR(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_VAR(col: string, w: number, opts: { out: string }): this;
        /** The sqrt of the rolling variance. */
        ROLLING_STD(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_STD(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_STD(col: string, w: number, opts: { out: string }): this;
        /** Exponential moving average; alpha in (0, 1]. */
        EMA(col: string, alpha: number, mask?: Uint8Array): Float64Array;
        EMA(col: string, alpha: number, mask: Uint8Array, opts: { out: string }): this;
        EMA(col: string, alpha: number, opts: { out: string }): this;
        /** `(x[i] - x[i-p]) / x[i-p]`. A zero previous value gives +/-Inf (the honest answer); 0/0 stays NaN. */
        PCT_CHANGE(col: string, periods?: number, mask?: Uint8Array): Float64Array;
        PCT_CHANGE(col: string, periods: number, mask: Uint8Array, opts: { out: string }): this;
        PCT_CHANGE(col: string, periods: number, opts: { out: string }): this;
        PCT_CHANGE(col: string, opts: { out: string }): this;
        /** `(x - mean) / sample stddev`, so it composes with STDDEV. */
        ZSCORE(col: string, mask?: Uint8Array): Float64Array;
        ZSCORE(col: string, mask: Uint8Array, opts: { out: string }): this;
        ZSCORE(col: string, opts: { out: string }): this;
        /** Sum of positive consecutive differences. */
        DELTA_SUM(col: string, mask?: Uint8Array): number;
        /** DELTA_SUM in timestamp order, not row order. */
        DELTA_SUM_TIMESTAMP(valueCol: string, timeCol: string, mask?: Uint8Array): number;

        /** Pairwise statistics; regression verbs take (y, x). */
        COV_POP(a: string, b: string, mask?: Uint8Array): number;
        /** The sample covariance (n-1). NaN below two selected rows. */
        COV_SAMP(a: string, b: string, mask?: Uint8Array): number;
        /** Pearson correlation of two columns; NaN on zero variance. */
        CORR(a: string, b: string, mask?: Uint8Array): number;
        /** Least-squares regression of y on x (the SQL REGR_* family). */
        REGR_SLOPE(y: string, x: string, mask?: Uint8Array): number;
        /** The regression intercept. */
        REGR_INTERCEPT(y: string, x: string, mask?: Uint8Array): number;
        /** The squared correlation, clamped to 1.0. A constant `y` gives NaN (SQL's special case of 1 is not followed). */
        REGR_R2(y: string, x: string, mask?: Uint8Array): number;
        /** The mean of the x column, under the (y, x) naming. */
        REGR_AVG_X(y: string, x: string, mask?: Uint8Array): number;
        /** The mean of the y column. */
        REGR_AVG_Y(y: string, x: string, mask?: Uint8Array): number;
        /** The number of selected rows. */
        REGR_COUNT(x: string, y: string, mask?: Uint8Array): number;
        /** The sum of (x - mean x)^2. Note the argument order: SXX is the INDEPENDENT column's sum of squares. */
        REGR_SXX(y: string, x: string, mask?: Uint8Array): number;
        /** The sum of (y - mean y)^2. */
        REGR_SYY(y: string, x: string, mask?: Uint8Array): number;
        /** The sum of (y - mean y)(x - mean x). */
        REGR_SXY(y: string, x: string, mask?: Uint8Array): number;
        /** Spearman: Pearson over the average ranks. */
        RANK_CORR(x: string, y: string, mask?: Uint8Array): number;
        /** The n*n row-major correlation matrix. */
        CORR_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** The covariance matrix over the same moments as COV_SAMP. */
        COV_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** Change per unit time across the WHOLE selection. */
        RATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The most recent interval only. */
        IRATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The slope joining the leftmost and rightmost points by x value. */
        BOUNDING_RATIO(x: string, y: string, mask?: Uint8Array): number;
        /** Exponentially time-decayed aggregates over a time column with decay constant `tau`. */
        EXPONENTIAL_TIME_DECAYED_AVG(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The weighted sum. */
        EXPONENTIAL_TIME_DECAYED_SUM(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The decayed row count (the weight denominator; the value is ignored). */
        EXPONENTIAL_TIME_DECAYED_COUNT(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The largest weighted value. */
        EXPONENTIAL_TIME_DECAYED_MAX(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** Half-open [lo, hi) ranges merged into their union. */
        RANGE_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { starts: Float64Array; ends: Float64Array };
        /** The interval common to ALL ranges. */
        RANGE_INTERSECT_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { start: number; end: number } | undefined;

        /** Reshape; produced frames carry fresh copies of the data. */
        JOIN(other: DataFrame, leftKey: string, rightKey: string, how?: "inner" | "left" | "right" | "outer"): DataFrame;
        /** As-of join: each left row takes the right row with the largest rightTime <= leftTime (NaN fill); both time columns integer and ascending. */
        ASOF_JOIN(other: DataFrame, leftTime: string, rightTime: string): DataFrame;
        /** Appends the rows of another frame with the same columns. */
        CONCAT(other: DataFrame): DataFrame;
        /** Buckets a sorted numeric time column; agg sum|mean|min|max|count. */
        RESAMPLE(timeCol: string, interval: number, agg?: "sum" | "mean" | "min" | "max" | "count"): DataFrame;
        /** One row per distinct index value, one column per distinct key value. */
        PIVOT(index: string, columns: string, values: string, agg?: string): DataFrame;
        /** Long form: each (row, valueVar) pair becomes one output row. */
        MELT(idVars: string[], valueVars: string[]): DataFrame;
    }


    /** Brand check: true only for frames this engine built, never for look-alike objects. */
    function isDataFrame(v: unknown): boolean;
    /** Builds a frame from { name: TypedArray | string[] }. ZERO-COPY: TypedArray columns are ALIASED, not copied; `number[]` columns are stored as STRINGS. */
    const DataFrame: {
        new (columns: Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | number[] | string[]>): DataFrame;
    };
}
// ---- std / os compatibility modules, WHATWG globals, and core prototype extensions ----

/** std — QuickJS-compatible stdio and environment module, present only under --std; failures are errno values, not throws. */
declare module "std" {
    interface StdFile {
        /** Method, not a property (matches the implementation). */
        eof(): boolean;
        /** Method, not a property. */
        error(): boolean;
        getByte(): number;
        putByte(b: number): void;
        readBytes(max: number): Uint8Array;
        readAsString(max: number): string;
        writeBytes(bytes: Uint8Array): void;
        writeStr(str: string): void;
        getline(): string | null;
        fileno(): number;
        close(): void;
        seek(offset: number, whence: number): void;
        tell(): number;
    }
    const __stdin: StdFile;
    const __stdout: StdFile;
    const __stderr: StdFile;
    export { __stdin as in, __stdout as out, __stderr as err };
    /** Format and print to stdout. Width/precision (digits and `*`) capped at 1000000 (RangeError); %c honours width/flags like C. */
    function printf(fmt: string, ...args: unknown[]): void;
    /** Format to a string. Width/precision (digits and `*`) capped at 1000000 (RangeError); %c honours width/flags like C. */
    function sprintf(fmt: string, ...args: unknown[]): string;
    function puts(str: string): void;
    function getenv(name: string): string | undefined;
    function setenv(name: string, value: string): void;
    function unsetenv(name: string): void;
    /** The whole environment as a {name: value} record. */
    function getenviron(): Record<string, string>;
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
    function fdopen(fd: number, mode: string, error?: Error): StdFile | null;
    function tmpfile(): StdFile;
    /** strerror(errno). */
    function strerror(errno: number): string;
    /** JSON parse with extensions (Date serialization, ...). */
    function parseExtJSON(str: string): unknown;
    /** Internal: pretty-prints a value for print(). */
    function __printObject(obj: unknown): void;
    /** The standard Error constructor. */
    const Error: ErrorConstructor;
    const SEEK_SET: number;
    const SEEK_CUR: number;
    const SEEK_END: number;
}

/** os — QuickJS-compatible POSIX layer (fds, processes, signals, timers), only under --std; calls return a negative errno instead of throwing.
 *  os.Worker is the parallelism primitive: a separate runtime with its own heap, exchanging structured-clone messages. */
declare module "os" {
    /** The number of milliseconds since an arbitrary point. */
    function now(): number;
    /** The OS name; a string property, not a function. */
    const platform: string;
    function getpid(): number;
    /** [cwd, errorcode]. */
    function getcwd(): [string, number];
    /** 0 or a negative errno. */
    function chdir(path: string): number;
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
    /** [stat, errorcode]; times are milliseconds since epoch. */
    function stat(path: string): [OsStat, number];
    /** [stat, errorcode], not following symlinks; times in ms. */
    function lstat(path: string): [OsStat, number];
    function readdir(path: string): [string[], number];
    function readlink(path: string): [string, number];
    function realpath(path: string): [string, number];
    function rename(oldpath: string, newpath: string): number;
    function remove(path: string): number;
    function mkdir(path: string, mode?: number): number;
    function symlink(target: string, linkpath: string): number;
    function utimes(path: string, atime: number, mtime: number): number;
    /** fd, or a negative errno. */
    function open(path: string, flags: number, mode?: number): number;
    function close(fd: number): number;
    /** Byte count read, or a negative errno; `buffer` is a raw ArrayBuffer sliced by offset/length. */
    function read(fd: number, buffer: ArrayBuffer, offset: number, length: number, position?: number): number;
    /** Byte count written, or a negative errno; `buffer` is a raw ArrayBuffer sliced by offset/length. */
    function write(fd: number, buffer: ArrayBuffer, offset?: number, length?: number, position?: number): number;
    /** New offset, or a negative errno. */
    function seek(fd: number, position: number, whence: number): number;
    /** New fd, or a negative errno. */
    function dup(fd: number): number;
    function dup2(oldfd: number, newfd: number): number;
    /** [readFd, writeFd], or null. */
    function pipe(): [number, number] | null;
    function isatty(fd: number): boolean;
    function ttySetRaw(fd: number, raw: boolean): void;
    function ttyGetWinSize(fd: number): [number, number] | null;
    /** Runs a process and returns its exit status (negative = killed by that signal); block:false returns the pid. Legacy form: dyna:sys's Exec is canonical. */
    function exec(args: string[], options?: { blocking?: boolean; usePath?: boolean; file?: string; cwd?: string; stdin?: unknown; stdout?: unknown; stderr?: unknown; env?: Record<string, string>; uid?: number; gid?: number }): number;
    function waitpid(pid: number, options?: number): [number, number];
    function kill(pid: number, sig: number): number;
    /** The handler is invoked with no arguments; read the registered number from closure state. */
    function signal(signal: number, handler: () => void): void;
    function sleep(delay: number): void;
    function setTimeout(cb: (...args: unknown[]) => void, delay: number, ...args: unknown[]): number;
    function clearTimeout(id: number): void;
    function setInterval(cb: (...args: unknown[]) => void, delay: number, ...args: unknown[]): number;
    function clearInterval(id: number): void;
    function setReadHandler(fd: number, cb: (fd: number) => void): void;
    function setWriteHandler(fd: number, cb: (fd: number) => void): void;
    const O_RDONLY: number;
    const O_WRONLY: number;
    const O_RDWR: number;
    const O_APPEND: number;
    const O_CREAT: number;
    const O_EXCL: number;
    const O_TRUNC: number;
    /** Windows only; absent on POSIX builds. */
    const O_BINARY: number | undefined;
    /** Windows only; absent on POSIX builds. */
    const O_TEXT: number | undefined;
    const WNOHANG: number;
    const SIGABRT: number;
    const SIGALRM: number;
    const SIGCHLD: number;
    const SIGCONT: number;
    const SIGFPE: number;
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
        /** Inside a worker script: the port back to the parent; null on the main thread. */
        readonly parent: Worker | null;
    };
    interface Worker {
        /** Optional transfer list: buffers are copied to the receiver and detached on the sender; a SharedArrayBuffer is refused. */
        postMessage(value: unknown, transfer?: ArrayBuffer[] | ArrayBufferView[]): void;
        /** A live message port keeps the process alive even after the worker exits; set to null to release it. */
        onmessage: ((ev: { data: unknown }) => void) | null;
        onerror: ((err: unknown) => void) | null;
    }
}

/** dyna:bench — a micro-benchmark harness: bench() times a function in adaptive batches on the monotonic clock, table() prints the results. */
declare module "dyna:bench" {
    /** One result: opsPerSec = 1000/meanMs, rsd = std/mean. Samples are batches doubled until each lasts 0.1 ms, so tiny bodies are not measured against the clock tick. */
    interface BenchResult {
        name: string;
        iters: number;
        elapsedMs: number;
        opsPerSec: number;
        rsd: number;
        p50Ms: number;
        p99Ms: number;
        meanMs: number;
    }
    /** Runs fn for timeMs (default 500) after warmupMs (default 100) and records the result for table(). */
    function bench(name: string, fn: () => void, opts?: { timeMs?: number; warmupMs?: number; warmup?: number }): BenchResult;
    /** Tab-separated table of all bench() results this process (header + a row per run), retained for the process lifetime; no reset API. */
    function table(): string;
}

// ---- WHATWG globals. Known omissions: AbortSignal.any, Request.clone, Headers.getSetCookie ----

/** Identity for `using` disposal. */
interface SymbolConstructor {
    readonly dispose: symbol;
    readonly asyncDispose: symbol;
}

/** WHATWG AbortSignal: `aborted`, `reason`, and "abort" listeners. */
interface AbortSignal {
    readonly aborted: boolean;
    readonly reason: unknown;
    onabort: ((this: AbortSignal, ev: { type: string; target: AbortSignal }) => void) | null;
    addEventListener(type: "abort", listener: (ev: { type: string; target: AbortSignal }) => void): void;
    removeEventListener(type: "abort", listener: (ev: { type: string; target: AbortSignal }) => void): void;
    throwIfAborted(): void;
    /** @internal Fires the abort. Engine-internal mutator: use AbortController.abort() or AbortSignal.abort()/timeout() instead. */
    _abort(reason?: unknown): void;
}
interface AbortSignalConstructor {
    new (): AbortSignal;
    abort(reason?: unknown): AbortSignal;
    timeout(delayMs: number): AbortSignal;
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

type HeadersInit = Headers | string[][] | Record<string, string>;
/** WHATWG Headers: a case-insensitive multimap of header fields. */
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

/** WHATWG FormData: ordered name/value pairs for multipart and urlencoded bodies. */
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

/** Global WebSocket: thin alias over dyna:http WsClient (same constructor). */
declare const WebSocket: typeof import("dyna:http").WsClient;

/** Options for fetch() and new Request(). */
interface RequestInit {
    method?: string;
    headers?: HeadersInit;
    body?: unknown;
    signal?: AbortSignal | null;
    /** Milliseconds until the fetch fails; bounds the WHOLE exchange (connect + body). */
    timeout?: number;
    /** Connect hook as HTTPClient.onConnect: resolved IP; return false or throw to refuse. */
    onConnect?: (ip: string) => unknown;
}
/** WHATWG Request: an immutable description of one HTTP request. */
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

/** Options for new Response(). */
interface ResponseInit {
    status?: number;
    /** Defaults to "OK" when omitted (the WHATWG spec would default to ""). */
    statusText?: string;
    headers?: HeadersInit;
    url?: string;
}
/** WHATWG Response: status, headers and a body read once as text, JSON or bytes. */
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

/** The WHATWG URL class, identical to dyna:url's export (not a copy). */
declare const URL: typeof import("dyna:url").URL;
/** WHATWG query list, THE same class the dyna:url module exports. */
declare const URLSearchParams: typeof import("dyna:url").URLSearchParams;

/** The global fetch: the SAME function as dyna:http's — see its notes on redirects and dropped credentials. */
declare const fetch: typeof import("dyna:http").fetch;

/** Web-compat UTF-8 encoder; byte-identical to dyna:bytes' fromUtf8. */
declare class TextEncoder {
    constructor();
    readonly encoding: string;
    encode(input?: string): Uint8Array;
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number };
}
declare class TextDecoder {
    /** Only UTF-8 labels are accepted (default "utf-8"); any other label is a RangeError naming it. */
    constructor(encoding?: string, opts?: { fatal?: boolean; ignoreBOM?: boolean });
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    /** Decodes UTF-8. {stream: true} continues the decoder's stream: an incomplete trailing sequence is carried into the next call; a call without it is FINAL (tail becomes U+FFFD). */
    decode(buffer?: Uint8Array | ArrayBuffer, options?: { stream?: boolean }): string;
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
    /** Zips iterables into tuples; mode "shortest" (default), "longest" (pads undefined) or "strict" (length mismatch throws). */
    zip<T extends readonly (Iterable<unknown> | Iterator<unknown>)[]>(iterables: T, options?: { mode?: "shortest" | "longest" | "strict" }): IteratorObject<{ [K in keyof T]: T[K] extends Iterable<infer U> ? U : (T[K] extends Iterator<infer U2> ? U2 : unknown) }>;
    /** Zips an object whose VALUES are iterables into objects with the same keys. */
    zipKeyed<O extends Record<string, Iterable<unknown> | Iterator<unknown>>>(iterables: O, options?: { mode?: "shortest" | "longest" | "strict" }): IteratorObject<{ [K in keyof O]: O[K] extends Iterable<infer U> ? U : (O[K] extends Iterator<infer U2> ? U2 : unknown) }>;
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
    /** Sliding windows of size n. n === 0 yields len+1 empty windows; n < 0 throws RangeError. */
    aperture(n: number): IteratorObject<T[], TReturn, TNext>;
    splitEvery(n: number): IteratorObject<T[], TReturn, TNext>;
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

/** ES2021 explicit resource management (the engine honors `using`). */
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

/** Engine error for internal invariant failures; not for user code. */
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


/** Resolves after `ms` milliseconds without blocking the event loop (os.sleep is the blocking form). */
declare function sleep(ms: number): Promise<void>;

/** Structured deep copy preserving cycles (Date, Map, Set, RegExp, typed arrays, BigInt); functions throw. Same function as dyna:serialize's. */
declare function structuredClone(value: unknown): unknown;

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
    /** Everything after the first n elements. */
    drop(n: number): T[];
    /** The last n elements. */
    takeLast(n: number): T[];
    /** Everything except the last n elements. */
    dropLast(n: number): T[];
    /** A stable sort by a key function. Numeric keys compare numerically (NaN last); strings by UTF-8 byte order; numbers before strings. */
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
}

/** RegExp.prototype extensions. */
interface RegExp {
    /** Whether the v flag is set. */
    readonly unicodeSets: boolean;
}
/** dyna:async — promise combinators and bounded handoff structures: sleep, withTimeout, retry, Semaphore, Channel, pmap, Pool, debounce, throttle.
 *  Everything runs on the event-loop thread: this is CONCURRENCY-limited interleaving, not parallelism. Bad arguments throw synchronously. */
declare module "dyna:async" {

    /** Resolves after `ms` milliseconds (setTimeout). */
    function sleep(ms: number): Promise<void>;

    /** Rejects if `p` does not settle within `ms` (`reason` replaces the message); p's late rejection never surfaces as unhandled. */
    function withTimeout<T>(p: PromiseLike<T>, ms: number, reason?: string): Promise<T>;

    /** Calls `fn` until it resolves: 1 attempt + `retries` (default 3), waiting backoffMs * factor^attempt; the LAST error surfaces. */
    function retry<T>(fn: () => T | PromiseLike<T>, opts?: {
        retries?: number;
        backoffMs?: number;
        factor?: number;
        onRetry?: (err: unknown, attempt: number) => void;
    }): Promise<T>;

    /** Counting semaphore with direct hand-off (no queue jumping); acquire() resolves a release function, run(fn) wraps acquire/release. */
    class Semaphore {
        constructor(n: number);
        get available(): number;
        get waiting(): number;
        acquire(): Promise<() => void>;
        run<T>(fn: (...args: unknown[]) => T | PromiseLike<T>, ...args: unknown[]): Promise<T>;
        /** @internal Hands the token to the next waiter; call the release function from acquire() instead. */
        _release(): void;
    }

    /** The synchronous bounded FIFO (Channel is the async MPSC). */
    class Queue {
        /** The synchronous bounded FIFO: `push` throws when closed or full, `tryPush` returns false, `shift` returns `undefined` when empty, `close()` freezes it. */
        constructor(cap?: number);
        get length(): number;
        get closed(): boolean;
        /** Throws when closed or full (no silent drop). */
        push(v: unknown): this;
        /** False when closed or full. */
        tryPush(v: unknown): boolean;
        /** The next value, or undefined when empty (any state). */
        shift(): unknown;
        close(): void;
    }

    /** MPSC channel: send() backpressures at capacity; recv() resolves {value, done:false}, then {done:true} after close() and a full drain. */
    class Channel<T = unknown> {
        constructor(cap?: number);
        get length(): number;
        get closed(): boolean;
        send(v: T): Promise<void>;
        recv(): Promise<{ value: T; done: false } | { done: true }>;
        close(): void;
        /** @internal Admits blocked sends once a recv frees room. */
        _admit(): void;
        [Symbol.asyncIterator](): AsyncIterator<T>;
    }

    /** Maps `fn` over `items` with at most `concurrency` (default 8) in flight, results in ITEM order; the FIRST rejection rejects the whole pmap. */
    function pmap<T, R>(items: T[], fn: (item: T, index: number, items: T[]) => R | PromiseLike<R>,
                        opts?: { concurrency?: number }): Promise<R[]>;

    /** A long-lived scheduler with `n` submit slots (interleaving, not worker parallelism); a throwing job rejects its submit and frees its slot. */
    class Pool {
        constructor(n: number);
        get size(): number;
        get active(): number;
        get pending(): number;
        /** Queues a job; settles with its result once a slot ran it. */
        submit<T>(fn: () => T | PromiseLike<T>): Promise<T>;
        /** @internal Launches queued jobs into free slots. */
        _pump(): void;
    }

    /** Trailing debounce: the last call inside the `ms` window runs once at its edge; cancel() drops a pending call. */
    function debounce<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };

    /** Leading throttle with a trailing call: the first call runs now, the last call in the window runs at its edge. */
    function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };
}
