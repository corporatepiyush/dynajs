/**
 * DynaJS ambient type declarations.
 *
 * Covers: every `dyna:*` native module as `declare module "dyna:..."`, the
 * WHATWG globals the engine ships (fetch, Request, Response, Headers,
 * FormData, AbortController, AbortSignal, TextEncoder/TextDecoder,
 * URL/URLSearchParams, structuredClone, sleep,
 * performance, console, print, timers), the `std`/`os` modules available
 * with `--std`, the core prototype extensions (Array, String, Number,
 * Object, Date, RegExp) as `declare global` interface augmentation, and the
 * one JS-side source module (src/pool.js) under its file-path pattern --
 * see the note on that block.
 *
 * Pure types only: no runtime code, no implementations.
 *
 * Reference it from tsconfig/jsconfig:
 *   { "compilerOptions": { "types": ["../types/dynajs"] } }
 *
 * ------------------------------------------------------------------
 *  std/os: the --std compatibility layer
 * ------------------------------------------------------------------
 *
 *  With `--std` the engine also exposes QuickJS's classic `std`/`os`
 *  modules. They are a COMPATIBILITY layer, never the canonical door,
 *  and they follow different conventions (errno tuples instead of
 *  throws, `os.platform` is a string property, not a function):
 *
 *  | Job            | Canonical door            | std/os equivalent (different names AND conventions) |
 *  |----------------|---------------------------|------------------------------------------------------|
 *  | Filesystem     | dyna:file                 | os.stat/readdir/rename/mkdir/symlink/realpath (errno tuples; std.open for handles) |
 *  | Environment    | dyna:sys env/getEnv/setEnv| std.getenv/setenv/getenviron                          |
 *  | Spawn          | sys.Exec (result object)  | os.exec + os.waitpid (pid + negative-errno conventions) |
 *  | Timers         | global setTimeout/...     | os.setTimeout/setInterval (same names, os-gated)      |
 *  | Clocks         | dyna:time now-family      | os.now (ms, arbitrary origin), os.sleep (blocking ms) |
 *  | Platform       | sys.platform() (function) | os.platform (string PROPERTY)                         |
 *  | print/eval     | global print / std.evalScript | --                                                |
 *
 * ------------------------------------------------------------------
 *  Prototype extensions are OPTIONAL
 * ------------------------------------------------------------------
 *
 *  The `declare global` interface augmentations below (Array, String,
 *  Number, Object, Date, RegExp, Map, Set, Iterator helpers beyond the
 *  ES2025 standard set) describe the DEFAULT engine. Booted with
 *  `--no-prototypes` (or DYNAJS_NO_PROTOTYPES=1) none of them are
 *  installed -- standard ES builtins, the WHATWG globals and every
 *  dyna:* module are unaffected. Code that must run under the opt-out
 *  must not call these members; the flag exists for embedders who want
 *  a strict-baseline context.
 *
 * ------------------------------------------------------------------
 *  Globals
 * ------------------------------------------------------------------
 *
 *  Beyond the WHATWG set declared below, the engine ships these
 *  globals: atob/btoa (WHATWG latin-1 codecs -- see their WARNING),
 *  queueMicrotask, Iterator (with the ES2025 iterator-helper methods
 *  plus engine extras), Float16Array, SuppressedError,
 *  DisposableStack/AsyncDisposableStack (explicit resource management;
 *  `using` is honored), InternalError, and the Lens optics type.
 *  WeakRef and FinalizationRegistry also exist at runtime and are typed
 *  by lib.es2021 (included via lib es2023), so they are deliberately
 *  not re-declared here. `__loadScript` exists but is @internal.
 */

/* ------------------------------------------------------------------ *
 *  Shared helper types
 * ------------------------------------------------------------------ */

/** A byte-addressed view: a typed array of 1-byte elements, a DataView, or an ArrayBuffer. */
type ByteView = Uint8Array | Int8Array | Uint8ClampedArray | DataView | ArrayBuffer;

/** Input accepted wherever raw bytes are read: text encoded as UTF-8, or a byte view. */
type BytesInput = string | ByteView;

/** 4-byte-element typed arrays accepted by the f32 SIMD kernels.
 *  Int32Array/Uint32Array are accepted as a BIT-CAST reinterpretation
 *  the kernels read the raw 32-bit patterns as IEEE floats, so
 *  passing integer arrays only makes sense when you stored bit patterns
 *  on purpose (e.g. quantized/model weights). The output kernels write
 *  bit patterns into whatever array you hand them. */
type F32Like = Float32Array | Int32Array | Uint32Array;

/** Any typed array, all element widths. DataView and ArrayBuffer are NOT
 *  included: the byte-filling natives (e.g. Random.fill) refuse them. */
type AnyTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

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
 *
 *  METHOD <-> FREE-FUNCTION MAP (read this before hunting for an
 *  alias). The class methods and the free functions below are the SAME
 *  implementations reached through two doors, and the names drift on
 *  purpose where history fixed them:
 *
 *      method Bytes.equals(v)   <-> free equal(a, b)
 *      method Bytes.includes(n) <-> free contains(buf, n)
 *      method Bytes.fill(...)   <-> free fill(buf, ...)  (different RETURN:
 *                                  the method returns the handle, the free
 *                                  function the view it filled)
 *      every other method       <-> the same lowerCamel name as a free
 *                                  function taking the buffer first
 *                                  (compare, indexOf, lastIndexOf, count,
 *                                  toUtf8, readUint8, writeBigUint64LE, ...)
 *
 *  Deliberately NOT mirrored as free functions: startsWith/endsWith/
 *  readBytes/writeBytes (handle-shaped call sites) and indexOfAny/slice
 *  (method-only).
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
        /** Variadic form; a leading array and further views mix freely. */
        static concat(...views: ByteView[]): Bytes;
        /** The byte count. */
        get length(): number;
        /** True when no byte has the high bit set; computed once at construction. */
        get isAscii(): boolean;
        /** True when the bytes are well-formed UTF-8; computed at construction.
         *  Getter-vs-method note (resolved): the same predicate is the
         *  isValidUtf8 GETTER on Text now too, plus the free function
         *  isValidUtf8(data) -- one shared scan, three spellings. */
        get isValidUtf8(): boolean;
        /** The backing Uint8Array. */
        get array(): Uint8Array;
        /** A new Bytes handle that is a view sharing the owner's ArrayBuffer. */
        slice(start?: number, end?: number): Bytes;
        /** Lexicographic byte comparison; -1, 0, or 1. */
        compare(other: ByteView): number;
        /** True when the other view has identical length and bytes. */
        equals(other: ByteView): boolean;
        /** First position of a byte value (0..255) or byte view; -1 when absent.
         *  `fromIndex` starts the search there (negative clamps to 0). */
        indexOf(needle: number | ByteView, fromIndex?: number): number;
        /** Last position of the needle; the empty needle matches at `length`.
         *  With `fromIndex`, the match must start at or before it (the search
         *  is backward); negative clamps to 0. */
        lastIndexOf(needle: number | ByteView, fromIndex?: number): number;
        /** True when the needle occurs. */
        includes(needle: number | ByteView): boolean;
        /** Number of non-overlapping occurrences; the empty needle counts
         *  `length + 1`. The scan starts at `fromIndex` (negative clamps to 0),
         *  so the empty needle then counts `length - fromIndex + 1`. */
        count(needle: number | ByteView, fromIndex?: number): number;
        /** First position at or after `fromIndex` holding any byte of the
         *  `chars` view, or -1. */
        indexOfAny(chars: ByteView, fromIndex?: number): number;
        /** True when the needle (byte value or view) sits exactly at
         *  `fromIndex` (String.prototype.startsWith semantics; default 0). */
        startsWith(needle: number | ByteView, fromIndex?: number): boolean;
        /** True when the needle ends exactly at `end` (exclusive; String
         *  semantics; default `length`) -- a match must END there. */
        endsWith(needle: number | ByteView, end?: number): boolean;
        /** A FRESH Uint8Array copying buf[off .. off+len); RangeError out of
         *  bounds. A copy by design (the class is a copied byte buffer);
         *  `slice` is the sanctioned VIEW. */
        readBytes(off: number, len: number): Uint8Array;
        /** Copies `src` at `off`; overlap-safe when `src` aliases this buffer;
         *  returns the byte count; RangeError if the write would pass the end. */
        writeBytes(off: number, src: ByteView): number;
        /** BEHAVIOR CHANGE: returns the HANDLE (chainable), not the
         *  underlying Uint8Array -- recover the view with `.array`, or use
         *  the free `fill(buf, ...)` which still returns the view. */
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
        constructor(s: string);
        /** True when any code unit is above U+00FF. */
        get isWide(): boolean;
        /** The wrapped string. */
        get value(): string;
        /** True when the string's UTF-8 encoding is well-formed (a lone
         *  surrogate is the only way a JS string is not).
         *  Method-vs-getter note: resolved in phase 1 -- this member
         *  is a GETTER, the same predicate as Bytes (bytes.isValidUtf8) and
         *  the free function isValidUtf8(data). The historical METHOD
         *  spelling `text.isValidUtf8()` no longer applies -- a JS property
         *  is one slot per name, so the accessor replaced the method
         *  (calling it now throws "not a function", exactly like
         *  bytes.isValidUtf8() always has). */
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
    /** First position of the needle (byte value or view), or -1; the search
     *  starts at `fromIndex` (negative clamps to 0). */
    function indexOf(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** Last position of the needle; with `fromIndex` the match must start at
     *  or before it (the search is backward); negative clamps to 0. */
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
    /** Styles `text` with the named ANSI style (or array of styles), Node's util.styleText signature.
     *  Colors also take dynamic forms: "256:<0-255>" / "bg256:<0-255>" (256-color),
     *  "#rrggbb" / "bg#rrggbb" and "rgb:r,g,b" / "bgRgb:r,g,b" (truecolor, 0-255 each).
     *  Emission auto-dims: with $NO_COLOR set and non-empty, or a non-TTY stdout, the escapes
     *  are dropped and the text is returned unchanged ($FORCE_COLOR overrides both; "0"/"false"
     *  refuse). Style NAMES are validated either way: an unknown one always throws. */
    function StyleText(style: string | string[], text: string): string;
    /** The list of styles the engine can apply. */
    function Styles(): string[];
    /** True when the stream with the given fd (default stdout) is a TTY. */
    function IsTTY(fd?: number): boolean;
    /** The terminal column count. */
    function Columns(): number;
    /** The terminal color depth: 0 (none), 4 (16 colors), 8 (256), or 24 (truecolor). */
    function ColorDepth(fd?: number): number;

    /** Writes `message` to stdout and reads one line from stdin. An empty answer,
     *  or EOF, yields `opts.default` (or "" without one). The terminator and a
     *  CRLF's CR are dropped first; an answer longer than 1 MiB (counted after
     *  that drop) throws RangeError and the rest of the refused line is drained.
     *  `opts` is a strict bag: null/undefined counts as absent, any other
     *  non-object throws. */
    function prompt(message: string, opts?: { default?: string }): string;
    /** Writes `message` to stdout and reads one line: "y"/"yes" (ASCII
     *  case-insensitive) answers true, everything else (including EOF) false. */
    function confirm(message: string): boolean;
    /** Renders an arrow-key menu (raw mode) and returns the chosen option string,
     *  or null when cancelled (Escape, Ctrl-C) or stdin reaches EOF. Raw mode is
     *  entered before the menu is drawn and leaving it discards no input. */
    function select(message: string, options: string[]): string | null;
    /** Reads one key event from stdin in raw mode; null at EOF. An event never
     *  consumes a byte its `sequence` does not report: a byte that cannot extend
     *  the current event (a non-continuation after a truncated UTF-8 lead, input
     *  past the 15-byte sequence cap) is delivered as the NEXT event's input.
     *  Timing rule: a lone ESC is told apart from a sequence head only by a 50 ms
     *  quiet window (the same window bounds gaps inside a sequence); identical
     *  bytes decode identically while the gaps between them stay inside it. */
    function keypress(): {
        /** "up", "enter", "a", ... -- null for an unrecognized sequence. */
        name: string | null;
        /** The input the event consumed (UTF-8 decoded; malformed runs
         *  appear as U+FFFD). */
        sequence: string;
        ctrl: boolean;
        meta: boolean;
        shift: boolean;
    } | null;

    /** A single-line progress bar with a time-based ETA. */
    class ProgressBar {
        constructor(opts: { total: number });
        /** Redraws the bar at `n` of `total`; reaching `total` finishes the line
         *  and any later update is refused. */
        update(n: number): this;
    }

    /** A single-line spinner over the frames "|/-\". */
    class Spinner {
        constructor(opts?: { text?: string });
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

    /** A command-line parser: options, arguments, subcommands, help.
     *  Every options bag is STRICT: an unknown key throws a TypeError naming
     *  the key and the valid set. Only a null/undefined bag counts as absent;
     *  any other non-object bag throws. */
    class Command {
        constructor(name?: string);
        get name(): string;
        /** Describes this command for help output. */
        describe(text: string): this;
        /** Registers a named option. `env` names an environment variable used as
         *  the option's value when the CLI does not supply it (precedence per
         *  option: CLI beats env beats default). Env is consulted lazily -- for
         *  an option the CLI supplies the variable is neither read nor validated,
         *  so a poisoned one cannot refuse or replace a CLI value. A number CLI
         *  value and a number env value share one strict finite-decimal grammar
         *  ("8", "-.5", "1e3"; surrounding ASCII whitespace ignored); "NaN",
         *  "Infinity" and "0x10" are refused, not silently coerced. */
        option(flags: string, description?: string, opts?: { type?: "boolean" | "string" | "number"; required?: boolean; variadic?: boolean; default?: unknown; env?: string }): this;
        /** Registers a positional argument. */
        argument(name: string, description?: string): this;
        /** Registers a subcommand. */
        command(sub: Command): this;
        /** Permits unknown options instead of refusing them. */
        allowUnknown(v: boolean): this;
        /** Registers the handler a leaf parse invokes with (options, args); its
         *  return value lands on the parse result's `result`. */
        action(fn: (options: Record<string, unknown>, args: string[]) => unknown): this;
        /** With a string, sets the version and enables the automatic `--version`
         *  flag (chainable). With no argument, returns the version ("" when unset). */
        version(v: string): this;
        version(): string;
        /** Parses an argument vector (defaults to scriptArgs).
         *  Returns { options: Record<string, unknown>, arguments: string[],
         *  command: string | null } -- pass T to narrow it. Each option resolves
         *  CLI-first: a CLI-supplied option uses its parsed value(s) and its `env`
         *  variable is never consulted; otherwise env, then default, apply. */
        parse<T = { options: Record<string, unknown>; arguments: string[]; command: string | null }>(args?: string[]): T;
        /** Prints the help text. */
        help(): string;
    }
}

/* ================================================================== *
 *  dyna:compress
 *  Every options bag is STRICT: an unknown key throws a TypeError naming
 *  the key and the valid set (e.g. `unknown option "levle" (valid: level)`).
 *  A null/primitive bag counts as absent; symbol and inherited keys are
 *  not visible to the check.
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
    /** level 1..12 (acceleration); values outside the range are clamped by the codec. */
    function lz4Compress(data: BytesInput, opts?: { level?: number; dict?: ByteView }): Uint8Array;
    /** Raw LZ4 block decompression; `dict` must be the dictionary used at compress time. */
    function lz4Decompress(data: ByteView, opts?: { dict?: ByteView }): Uint8Array;
    /** LZ4 frame format with optional content checksum. */
    function lz4Frame(data: BytesInput, opts?: { level?: number; checksum?: boolean }): Uint8Array;
    /** LZ4 frame decompression; a bad checksum or structure is refused. */
    function lz4Unframe(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;
    /** RFC 1952 gzip framing; below 6 fixed-Huffman, 6+ dynamic-Huffman. */
    function gzip(data: BytesInput, level?: number): Uint8Array;
    /** gzip with the level in an options object (same encoder as the
     *  positional form; both arities are accepted). */
    function gzip(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Full RFC 1951 inflate with trailer validation. */
    function gunzip(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;

    /** Worst-case compressed size of `n` input bytes under one codec, so an
     *  output buffer can be allocated once. gzip/lz4/lz4frame report the
     *  bound their encoder actually reserves; zstd/snappy/brotli report the
     *  linked codec's own bound. */
    function compressBound(n: number, opts: { algo: CompressorAlgo }): number;

    /** ustar archive entry for packing. */
    interface TarEntry {
        name: string;
        data?: ByteView;
        /** "file" or "directory"; this writer emits no link metadata, so a
         *  link type is refused. Absent: "directory" iff there is no data. */
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

    /** Zip entry for packing: per-entry method/mtime/mode/comment are
     *  optional; absent method inherits the pack-level opts.method, absent
     *  mtime/mode/comment mean none/default/commentless. mtime is unix
     *  seconds (0 = none, stored as DOS datetime); mode is a unix file mode
     *  (default 0644, 0755 for directory names); comment is a central-directory
     *  comment (truncated at 1024 bytes). method "deflate" is ADVISORY: when
     *  deflating would not shrink the member it is silently stored instead
     *  (the member's `method` field reports what was actually written).
     *  Fields are own properties of the entry object. */
    interface ZipEntry {
        name: string;
        data: ByteView;
        method?: "deflate" | "store";
        mtime?: number;
        mode?: number;
        comment?: string;
    }
    /** Central-directory listing record (mode/comment included; mtime is
     *  unix seconds, 0 = none). */
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
    /** Central-directory listing. */
    function ZipList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): ZipRecord[];
    /** Extracts one member by exact name; CRC/size mismatches are refused. */
    function ZipRead(bytes: ByteView, name: string, opts?: { allowUnsafeNames?: boolean }): Uint8Array;
    /** Entry by-index in central-directory order; out of range is a RangeError. */
    function ZipReadAt(bytes: ByteView, index: number, opts?: { allowUnsafeNames?: boolean }): ZipMember;
    /** Options bag of ZipExtractAll. Unknown keys throw a TypeError naming the
     *  key and the valid set. */
    interface ZipExtractOptions {
        /** Destination directory (string or Path); absent = no writes. */
        dir?: string | import("dyna:file").Path;
        /** Opt-in escape, never implied: unsafe names (with `..`) are then
         *  written OUTSIDE `dir`. Without it such archives are refused. */
        allowUnsafeNames?: boolean;
    }
    /** Every member with its bytes; with a destination (a string/Path second
     *  argument, or `dir` in the options bag -- an object argument is always
     *  the bag, anything ambiguous is refused) each file is also written under
     *  it (mkdir -p) and the same records are returned. The write path never
     *  follows a symlink under the destination (a symlink planted there is
     *  refused, not written through), files are created with the default mode
     *  (0666 & ~umask -- a member's stored mode is only reported, never
     *  applied), and an existing regular file is replaced by a fresh one.
     *  Iteration is this array (ZipList is likewise an array). */
    function ZipExtractAll(bytes: ByteView, dir?: string | import("dyna:file").Path | ZipExtractOptions, opts?: ZipExtractOptions): ZipMember[];
    /** Raw DEFLATE (RFC 1951, no framing) compression; level 1..12.
     *  Same encoder core as ZipPack members: deflate(data) equals the member
     *  bytes of a DEFLATED ZipPack entry at level 1. For incompressible data
     *  ZipPack falls back to STORED (the member bytes are then the raw input),
     *  so the equality holds for deflated members only. */
    function deflate(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Raw DEFLATE decompression; trailing bytes are refused. */
    function inflate(data: ByteView, opts?: { asString?: boolean }): Uint8Array | string;

    /** The codec names Compressor accepts. */
    type CompressorAlgo = "gzip" | "lz4" | "lz4frame" | "zstd" | "brotli" | "snappy";
    /** A compiled codec: configuration and scratch owned once, reused across calls.
     *  Choose the class over the one-shot functions (zstd/gzip/lz4...) when you
     *  compress many buffers with the same configuration, above all when a
     *  pre-trained `dict` is in play -- the dictionary is compiled once here and
     *  `dictId` pins it beside the data. One-shot calls stay the right tool for
     *  single buffers. */
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
    /** TOML 1.0 parsing and serialization. Date-time values parse as their
     *  raw RFC 3339 STRINGS: local date-times carry no offset, so a
     *  unix conversion would be ambiguous and no {dates} option exists. */
    namespace TOML {
        /** Parses a full TOML 1.0 document; key collisions and leading zeros are refused. */
        function parse(text: string): Record<string, unknown>;
        /** Serializes a plain object root; NaN/Infinity render as nan/inf/-inf. */
        function stringify(value: unknown): string;
    }
    /** Classic INI reading and writing. */
    namespace INI {
        /** Reads [section] headers, key=value pairs, key[]=v lists, and bare keys as true. */
        function parse(text: string): Record<string, unknown>;
        /** Serializes an object to INI text: top-level scalars first,
         *  then one [section] block per object value. Strings go bare or
         *  double-quoted; numbers/booleans emit bare and parse back as
         *  strings (INI has no numeric/boolean type). Deeper nesting,
         *  arrays and null are refused. */
        function stringify(record: Record<string, unknown>): string;
    }
    /** dotenv grammar .env parsing and loading. */
    namespace Env {
        /** Parses KEY=value records; lines without `=` are skipped. */
        function parse(text: string): Record<string, string>;
        /** Parses a .env file AND assigns it into the process environment
         *  . Defaults: assign true, override false (existing entries
         *  win), expand false ($NAME/${NAME} expansion against the
         *  environment, where earlier keys of the file are visible). The
         *  option bag is strict: booleans only, and an unknown key throws a
         *  TypeError naming the key and the valid set . Returns the
         *  record. */
        function load(path: string, opts?: { assign?: boolean; override?: boolean; expand?: boolean }): Record<string, string>;
        /** Serializes a record to .env text; every value must be a
         *  string. Values that cannot sit bare are double-quoted with the
         *  escapes parse() expands. */
        function stringify(record: Record<string, string>): string;
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
// The Ed25519*/X25519*/Scrypt/Ed25519PemToRaw/Ed25519PemFromRaw/
// X25519PemToRaw/X25519PemFromRaw functions, the AESGCM/ChaCha20Poly1305
// classes and the RSA/X509/ECDSA/ECDH namespaces exist only in CONFIG_TLS=y
// builds; in a default build those names are absent from the module at
// runtime.
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

    /** OpenBSD $2b$ bcrypt. */
    namespace Bcrypt {
        /** Hashes a password (max 72 bytes) with a fresh salt; rounds 4..31. */
        function hash(password: string, rounds?: number): string;
        /** Constant-time verify; accepts $2a$/$2b$/$2y$ prefixes; the hash's cost is capped at 20. */
        function verify(password: string, hash: string): boolean;
    }

    /** Argon2id v0x13 (RFC 9106). */
    namespace Argon2id {
        /** Options; memory in KiB, salt at least 8 bytes. `encoded` selects
         *  the return/storage form of hash()/hashAsync() (default true).
         *  STRICT: unknown keys throw a TypeError naming the key and
         *  the valid set. */
        interface Argon2idOpts {
            iterations?: number;
            memory?: number;
            parallelism?: number;
            hashLen?: number;
            /** Default true: hash() returns the PHC string
             *  $argon2id$v=19$m=..,t=..,p=..<b64 salt>$<b64 hash>; false
             *  keeps the raw tag bytes (the pre-CY-4 form). */
            encoded?: boolean;
        }
        /** PHC string form (default). The salt and every parameter ride in
         *  the string, so storing it is the whole contract. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded?: true }): string;
        /** Raw tag form with { encoded: false }. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded: false }): Uint8Array;
        /** PHC form: every parameter comes from the string; a tampered
         *  parameter refuses (TypeError) or mismatches (false). */
        function verify(hashString: string, password: BytesInput): boolean;
        /** Raw form: recomputes with the same parameters and compares in
         *  constant time (opts still override the derivation parameters). */
        function verify(password: BytesInput, salt: ByteView, expectedHash: ByteView, opts?: Argon2idOpts): boolean;
        /** Same core on the shared offload pool (needs dyna:net; the names are
         *  absent from the module without it). Settles inline below the
         *  asyncStats offloadMin gate. hashAsync settles the PHC string by
         *  default ({encoded:false} the raw bytes); verifyAsync takes either
         *  the PHC pair or the raw triple. */
        function hashAsync(password: BytesInput, salt: ByteView, opts?: Argon2idOpts): Promise<string | Uint8Array>;
        function verifyAsync(hashString: string, password: BytesInput): Promise<boolean>;
        function verifyAsync(password: BytesInput, salt: ByteView, expectedHash: ByteView, opts?: Argon2idOpts): Promise<boolean>;
        /** { inline, offloaded, offloadMin } call counters for the async pair. */
        function asyncStats(): { inline: number; offloaded: number; offloadMin: number };
    }

    /** RSA keys and signatures: PKCS#1 v1.5, RSASSA-PSS and RSA-OAEP. */
    namespace RSA {
        function generate(bits?: 2048 | 3072 | 4096): KeyPair;
        /** md is "sha1"|"sha256"|"sha384"|"sha512" (case-insensitive). */
        function sign(md: string, privateKey: string, msg: BytesInput): Uint8Array;
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView): boolean;
        /** RSASSA-PSS. saltLen: byte count (0 = deterministic),
         *  "max" (modulus-filling), or "auto" (default: sign uses the
         *  maximum, verify recovers it -- OpenSSL's own default, so
         *  signatures interop byte-for-byte with `openssl dgst
         *  -sigopt rsa_padding_mode:pss`). mgf1Hash defaults to md. */
        function signPSS(md: string, privateKey: string, msg: BytesInput,
                         opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): Uint8Array;
        function verifyPSS(md: string, publicKey: string, msg: BytesInput, sig: ByteView,
                           opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): boolean;
        /** RSA-OAEP encryption: seal to a public key, open with the
         *  private one. md defaults to "sha256" (pass "sha1" for the legacy
         *  interop default); mgf1Hash defaults to md; label is the optional
         *  associated bytes and must round-trip exactly. The same
         *  2048-bit key floor as sign/verify applies. */
        namespace OAEP {
            function seal(publicKey: string, plaintext: BytesInput,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
            function open(privateKey: string, sealed: ByteView,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
        }
    }

    /** X.509 certificate parsing and self-signed generation. */
    namespace X509 {
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
        /** Builds a v3 self-signed certificate signed with SHA-256.
         *  sans: each entry is classified BY SHAPE -- contains "@" -> email,
         *  an address inet_pton parses (v4 or v6) -> IP, else a DNS name --
         *  and emitted as the v3 subjectAltName extension, which is what
         *  hostname validation reads (a cert without it fails every verifier).
         *  extensions: OpenSSL-config-syntax extension entries; the supported
         *  names are exactly subjectAltName, keyUsage, extendedKeyUsage,
         *  basicConstraints, subjectKeyIdentifier, authorityKeyIdentifier --
         *  anything else is refused by name, never ignored. An entry naming
         *  subjectAltName together with sans is refused as ambiguous. */
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
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** md "sha1"|"sha256"|"sha384"|"sha512"; format "raw" (default) or "der". */
        function sign(md: string, privateKey: string, msg: BytesInput, opts?: { format?: "raw" | "der" }): Uint8Array;
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView, opts?: { format?: "raw" | "der" }): boolean;
    }

    /** ECDH key agreement over X9.63. */
    namespace ECDH {
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
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

    /** PEM <-> raw key converters: the bridge between the module's
     *  32-byte raw keys and the PEM world (PKCS#8 "PRIVATE KEY" / SPKI
     *  "PUBLIC KEY", byte-exact against openssl genpkey output). A private
     *  PEM yields both raw halves; a public PEM, publicKey only. PemFromRaw
     *  writes exactly the halves given. */
    function Ed25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    function Ed25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };
    function X25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    function X25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };

    /** AES-GCM AEAD; key 16, 24, or 32 bytes. */
    class AESGCM implements DynResource {
        constructor(key: ByteView);
        /** Encrypts and appends the 16-byte tag; nonce must be exactly 12 bytes. */
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        /** Decrypts and authenticates; a forged tag throws `authentication failed`. */
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        /** Into form: writes into the CALLER's Uint8Array (a subarray
         *  writes its slice), no allocation; returns bytes written. opts
         *  {tagOut} detaches the 16-byte tag into that buffer (out is pure
         *  ciphertext); opts {prefixNonce:true} emits the one-blob
         *  nonce||ciphertext||tag layout (the nonce leads `out`). */
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        /** The open twin: opts {tag} reads a detached tag (sealed is pure
         *  ciphertext); {prefixNonce:true} reads the leading nonce from the
         *  blob (pass undefined for the nonce argument). A forged tag throws
         *  exactly like open(). */
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        /** Seals with a fresh CSPRNG 12-byte nonce and returns it beside the
         *  ciphertext||tag, so the pair is stored together. Random 96-bit
         *  nonces bound a key to ~2^32 messages (SP 800-38D s8). */
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
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
        constructor(algorithm: string, key: BytesInput);
        /** A complete MAC; the object is ready for the next message. */
        sign(msg: BytesInput): Uint8Array;
        signHex(msg: BytesInput): string;
        /** Streaming absorb; returns this. */
        update(msg: BytesInput): this;
        /** Finishes the accumulated stream. */
        digest(): Uint8Array;
        digestHex(): string;
        /** Returns the HMAC to its initial state, discarding any update()
         *  prefix; the key schedule is kept. Parity with Hasher.reset. */
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
    function HMACHex(algorithm: string, key: BytesInput, data: BytesInput): string;

    /** The unkeyed digest surface, re-exported from dyna:hash (same names,
     *  same signatures, same implementations -- aliases over one table), so
     *  the crypto module answers S256-style PKCE challenges and fingerprint
     *  pins without a second import. Checksums ride along because the source
     *  table is dyna:hash's whole one-shot surface and the alias set must
     *  never drift from it. */
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
    /** XXH3-64 (xxHash spec 0.8, the default secret); same shapes as
     *  XXHash64. Not a security primitive. */
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
    /** keyed/derive forms: the second argument may be an options object
     *  instead of a length. BLAKE2 keys are 0..64 (b) / 0..32 (s) bytes, and
     *  an EMPTY key is legal = unkeyed (RFC 7693). BLAKE3 keys are exactly
     *  32 bytes; {context}/{deriveKey} run the derive-key mode, whose output
     *  is exactly 32 bytes (`length` is refused there). Unknown keys are
     *  rejections. */
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

    /** RFC 5869 extract-and-expand key derivation. */
    /** STRICT: the opts bag rejects unknown keys (valid: key, hash,
     *  salt, info, length). */
    function HKDF(opts: { hash?: string; key: BytesInput; salt?: BytesInput; info?: BytesInput; length?: number }): Uint8Array;

    /** RFC 8018 PBKDF2. */
    /** STRICT: the opts bag rejects unknown keys (valid: password,
     *  hash, salt, iterations, length). */
    function PBKDF2(opts: { hash?: string; password: BytesInput; salt?: BytesInput; iterations?: number; length?: number }): Uint8Array;

    /** RFC 7914 scrypt. */
    /** STRICT: the opts bag rejects unknown keys (valid: N, r, p,
     *  keyLen). */
    function Scrypt(password: BytesInput, salt: BytesInput, opts?: { N?: number; r?: number; p?: number; keyLen?: number }): Uint8Array;

    /** OS entropy; the CSPRNG path, not the seeded PRNG.
     *  Which random when: security/token material -> this function;
     *  reproducible or fast simulation -> dyna:random's Random (seeded
     *  xoshiro256**); a throwaway shuffle/sample on an array ->
     *  the Array.prototype extensions (they use a global RNG). */
    function RandomBytes(count?: number): Uint8Array;

    /** Constant-time comparison; different lengths return false.
     *  oauth2.secureCompare is the string/bytes convenience form of this
     *  same primitive -- either is fine, keep secrets out of
     *  plain `===`. */
    function TimingSafeEqual(a: ByteView, b: ByteView): boolean;

    /** RFC 4226 HOTP; digits 6..8, algo any Hmac name. */
    function HOTPGenerate(secret: BytesInput, counter: number, opts?: { digits?: number; algo?: string }): string;

    /** RFC 6238 TOTP; atSec is explicit so results are testable. */
    function TOTPGenerate(secret: BytesInput, opts?: { atSec?: number; period?: number; digits?: number; algo?: string }): string;

    /** HOTP verification: recomputes the expected code and compares
     *  constant-time. A malformed code (wrong length, non-digits) is FALSE,
     *  the same answer a wrong guess gets -- never a throw. */
    function HOTPVerify(secret: BytesInput, counter: number, code: string, opts?: { digits?: number; algo?: string }): boolean;

    /** TOTP verification with clock-skew tolerance: {window} (default
     *  0 = exact) checks every counter atSec/period - window .. + window;
     *  each comparison is constant-time. */
    function TOTPVerify(secret: BytesInput, code: string, opts?: { atSec?: number; period?: number; digits?: number; algo?: string; window?: number }): boolean;

    /** JWS signing; HS/RS/ES algorithms. */
    function JWTSign(payload: unknown, key: BytesInput, opts?: { alg?: string }): string;

    /** JWT verification; `algorithms` is a required allowlist. Pass T to
     *  type the verified payload (the runtime returns whatever the token
     *  carried; without T the payload reads as unknown). */
    function JWTVerify<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[] }): T;
}

/* ================================================================== *
 *  dyna:csv
 *  Every options bag is STRICT: an unknown key throws a TypeError naming
 *  the key and the valid set (e.g. `unknown option "delimeter" (valid:
 *  delimiter, quote, hasHeader, strict)`).
 * ================================================================== */
declare module "dyna:csv" {
    /** Parses CSV text in memory (no temp file). A leading UTF-8 BOM is stripped; unlike CSVFile.read, an empty text is the empty table (no throw). */
    /** Rows are shaped to the first parsed row's width (short rows pad with "", long rows truncate); rows in syntax errors are 1-based over the text. */
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
    /** Serializes rows to CSV text in memory; every line ends with \n. */
    /** string[][] rows are written verbatim (a header is the caller's first row: stringify([t.headers, ...t.rows]) round-trips parse). */
    /** Record rows derive the columns from the FIRST row's own keys in insertion order (JS ordering: integer-like keys first); missing key -> "", extra key ignored; a header line is written unless hasHeader:false (no effect on array rows). Mixing the two forms is refused. */
    function stringify(rows: string[][] | Record<string, unknown>[], opts?: {
        delimiter?: string;
        quote?: string;
        /** Default true; false suppresses the derived header of object rows. */
        hasHeader?: boolean;
    }): string;
    /** A file-backed table whose every method load-modify-stores the bound file. */
    class CSVFile implements DynResource {
        constructor(path: import("dyna:file").Path);
        /** Creates the file from headers and optional rows. */
        create(opts: { headers: string[]; rows?: (string | number)[][]; overwrite?: boolean }): { path: import("dyna:file").Path; rows: number };
        /** Loads the file; options offset/limit/columns. delimiter/quote change the input dialect (read-only support: mutators rewrite the canonical comma/quote pair). */
        read(opts?: { offset?: number; limit?: number; columns?: string[]; strict?: boolean; delimiter?: string; quote?: string }): { headers: string[]; rows: string[][]; totalRows: number };
        /** Appends rows (positional arrays or objects keyed by header). */
        /** A positional row longer than the header is truncated to it. */
        /** A bare array argument is ONE positional row (multi-row adds go through {rows}); unknown option keys are rejected. */
        /** The bare form carries no options bag: module defaults apply and any further argument is accepted and IGNORED (documented, not an error). */
        addRow(opts: { rows: (string | number)[][] | Record<string, unknown>[]; strict?: boolean; durable?: boolean } | (string | number)[]): { added: number; totalRows: number };
        /** Sets one cell. */
        updateCell(opts: { row: number; column?: string; columnIndex?: number; value: string; strict?: boolean; durable?: boolean }): { row: number; column: string; value: string };
        /** Removes the data row at `row`. */
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
        /** Streaming row iteration: async-iterable batches
         *  `for await (const b of file.rows({batch}))`. Each batch is
         *  {headers, rows, offset, totalRows} with at most batch rows (default
         *  1000, 1..100000); only one batch is materialised at a time.
         *  Aligned with dyna:stream (manual async iterator: next/return/
         *  [Symbol.asyncIterator], promises of {value, done}).
         *  Time/memory tradeoff: each next() re-reads and re-parses the file
         *  to slice its batch (snapshot-per-batch), so peak memory is O(batch)
         *  rows but total parse work grows with batches x file: prefer the
         *  largest batch a consumer can hold (batch=10000 measured ~9x faster
         *  end-to-end than batch=1000 on a 200k-row file, at somewhat higher
         *  peak memory), or read() when the table fits in memory. */
        rows(opts?: { batch?: number; delimiter?: string; quote?: string; strict?: boolean }): AsyncIterableIterator<{ headers: string[]; rows: string[][]; offset: number; totalRows: number }>;
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
    /** Exact arbitrary-precision decimal; IEEE 754-2008 decimal128 context (34 digits).
     *
     *  Rounding defaults are DELIBERATELY split: arithmetic that must be
     *  exact (add/sub/mul/mod) accepts `opts` and ignores it; `round()` defaults
     *  to halfEven (the statistically-neutral banker's rounding, IEEE's own
     *  default), while `toFixed()` defaults to halfUp (the human-facing, invoice
     *  style). Both accept an explicit RoundingMode -- pass one whenever the
     *  default matters. */
    /** STRICT: unknown KEYS throw (valid: precision, rounding).
     *  Known-but-ignored keys stay accepted -- add/sub/mul/mod/divmod are
     *  exact, so a `precision` there is the documented accepted-and-ignored
     *  form, and sqrt/exp/ln/log10 likewise ignore `rounding`. */
    interface DecimalOptions {
        precision?: number;
        rounding?: RoundingMode | string;
    }
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
        /** The square root, CORRECTLY ROUNDED to the context: always
         *  half-even, `rounding` accepted and ignored (python's decimal does
         *  the same). A negative value throws RangeError. */
        sqrt(opts?: DecimalOptions): Decimal;
        /** e^x, correctly rounded half-even. Overflows (RangeError) when the
         *  ideal result passes Emax = 999999, i.e. x >= 10^6*ln10 -- decided
         *  against the threshold carried at precision+40 digits (the
         *  threshold is irrational; an input would need more than
         *  precision+40 significant digits to sit closer to it than the
         *  constant is carried). Results the context cannot represent
         *  (adjusted exponent below Etiny = -999999-precision+1, less the
         *  round-to-zero half step) are 0, not an error; the subnormal band
         *  above that is quantized to the Etiny exponent, python-style. */
        exp(opts?: DecimalOptions): Decimal;
        /** The natural log, correctly rounded half-even. Zero or a negative
         *  value throws RangeError (this module has no -Infinity). */
        ln(opts?: DecimalOptions): Decimal;
        /** The base-10 log, correctly rounded half-even; exact for powers of
         *  ten. Zero or a negative value throws RangeError. */
        log10(opts?: DecimalOptions): Decimal;
        /** The integer toward -Infinity. Exact; no rounding context. */
        floor(): Decimal;
        /** The integer toward +Infinity. Exact; no rounding context. */
        ceil(): Decimal;
        /** The integer toward zero. Exact; no rounding context. */
        trunc(): Decimal;
        /** The exact truncated-division pair [quotient, remainder]: q toward
         *  zero, r with the dividend's sign, a == q*b + r on the nose.
         *  Division by zero throws; opts accepted and ignored. */
        divmod(x: Decimal | string | number, opts?: DecimalOptions): [Decimal, Decimal];
        /** Truncates toward zero as a bigint. A fractional part, or a
         *  magnitude at or past 2^63 (the engine's C bridge is an int64),
         *  throws RangeError. */
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
        constructor(minorUnits: number, currency: string, opts?: { minorDigits?: number });  /* minorUnits and minorDigits must be integers; the code is upper-cased; opts must be an object when given */
        /** The human decimal form ("-1,234.56"). EXACT: the text is
         *  parsed digit by digit (the stored value never touches a double);
         *  ',' grouping is structural (first group 1-3 digits, the rest
         *  exactly 3), and a value FINER than the currency's minor unit is
         *  refused -- rounding a fraction of a cent away is what this type
         *  prevents. Trailing zeros are TEXT, not finer value: "1.500" on
         *  USD is exactly 150 minor units and is admitted, while "1.005"
         *  (a nonzero digit past the scale) throws. The same scale rule
         *  governs all three factories. */
        static fromString(str: string, currency: string, opts?: { minorDigits?: number }): Money;
        /** An exact Decimal. `d * 10^minorDigits` must be an integer:
         *  a Decimal whose VALUE is finer than the minor unit is refused,
         *  not rounded (trailing zeros at or past the scale are exact and
         *  fine -- `new Decimal("1.500")` is 150 on USD). */
        static fromDecimal(d: Decimal, currency: string, opts?: { minorDigits?: number }): Money;
        /** The raw minor-unit count as an integer number, a bigint, or an
         *  amount string (the latter two are exact past 2^53). The string
         *  takes the same amount grammar as `fromString` (optional `,`
         *  grouping, optional `.` fraction) with the value required to be
         *  whole minor units: "1,234" and "5.0" are 1234 and 5, "5.5"
         *  throws. */
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
        /** The minor-unit integer as a Number: exact through 2^53 minor
         *  units, and past that the double rounds (the store does not --
         *  use toString()/toDecimal() to read an exact value). */
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
    /** Base64/percent-encoding vocabulary (/ ) -- which door for
     *  which payload:
     *
     *  | Payload                        | Door                                   |
     *  |--------------------------------|----------------------------------------|
     *  | raw bytes -> base64 text       | Base64Encode/Base64Decode (here)       |
     *  | URL-safe base64 (JWT, PKCE)    | Base64URLEncode/Base64URLDecode (here) |
     *  | JS string -> base64 (UTF-8)    | String.prototype.encodeBase64          |
     *  | latin-1 web-compat atob/btoa   | globals (WARNING: NOT UTF-8, see there)|
     *  | form/query percent-encoding    | dyna:url formEncode/formDecode         |
     *  | component percent-encoding     | url.encodeURIComponentStrict / global encodeURIComponent |
     */

    /** Lowercase hex string, SIMD-accelerated. */
    function HexEncode(data: BytesInput): string;
    /**: hex text into a caller-owned buffer; returns the 2*n ASCII
     *  bytes written at offset 0. `out` needs 2*n bytes or RangeError (and
     *  nothing is written on that path). Input/output overlap refuses. */
    function hexEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Returns a Uint8Array; throws SyntaxError on odd length or an invalid digit. */
    function HexDecode(text: string): Uint8Array;
    /** Decodes hex into a caller-owned buffer; returns the byte count written.
     *  `out` needs at least text.length/2 bytes or RangeError. */
    function hexDecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 base64 (`+/`, padded).
     *  WARNING: this is a BYTE-level codec. The global atob/btoa are
     *  WHATWG latin-1 STRING codecs and give different answers for the same
     *  input: btoa("é") is "6Q==" (one latin-1 byte) while
     *  Base64Encode(utf8 bytes of "é") is "w6k=". Mixing the two families
     *  silently corrupts data -- see the vocabulary table in the block header. */
    function Base64Encode(data: BytesInput): string;
    /**: base64 text into a caller-owned buffer; returns the byte count.
     *  `out` needs 4*ceil(n/3) bytes or RangeError. */
    function base64EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Byte-level RFC 4648 decode; see the Base64Encode WARNING before mixing
     *  this with atob/btoa output. */
    function Base64Decode(text: string): Uint8Array;
    /** Decodes base64 into a caller-owned buffer; returns the byte count.
     *  `out` needs at least 3*(text.length/4) bytes or RangeError. */
    function base64DecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 section 5 base64url (no padding). */
    function Base64URLEncode(data: BytesInput): string;
    /**: unpadded base64url text into a caller-owned buffer; returns the
     *  byte count. `out` needs 4*ceil(n/3) bytes or RangeError; up to 3 pad
     *  bytes PAST the count are unspecified scratch (shared core), never
     *  past the bound. */
    function base64UrlEncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base64URLDecode(text: string): Uint8Array;
    /** base64url into a caller-owned buffer; `out` needs at least
     *  3*((text.length+3)/4) bytes or RangeError. */
    function base64UrlDecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 base32, `=` padded. */
    function Base32Encode(data: BytesInput): string;
    /**: base32 text into a caller-owned buffer; `out` needs
     *  ((n+4)/5)*8 bytes or RangeError. */
    function base32EncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base32Decode(text: string): Uint8Array;
    /** base32 into a caller-owned buffer; `out` needs (text.length/8)*5 bytes
     *  or RangeError (the codec requires a padded, length % 8 == 0 string). */
    function base32DecodeInto(text: string, out: Uint8Array): number;
    /** Extended-hex base32. */
    function Base32HexEncode(data: BytesInput): string;
    /**: base32hex text into a caller-owned buffer; capacity as
     *  base32EncodeInto. */
    function base32HexEncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base32HexDecode(text: string): Uint8Array;
    /** base32hex into a caller-owned buffer; capacity as base32DecodeInto. */
    function base32HexDecodeInto(text: string, out: Uint8Array): number;
    /** Adobe-less ascii85 with the `z` shorthand. */
    function Base85Encode(data: BytesInput): string;
    /**: ascii85 text into a caller-owned buffer; `out` needs
     *  ((n+3)/4)*5 bytes or RangeError. */
    function base85EncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base85Decode(text: string): Uint8Array;
    /** ascii85 into a caller-owned buffer; `out` needs at least
     *  4*text.length bytes or RangeError (whitespace-safe bound). */
    function base85DecodeInto(text: string, out: Uint8Array): number;
    /** Bitcoin base58; leading zero bytes become leading `1`s. */
    function Base58Encode(data: BytesInput): string;
    /**: base58 text into a caller-owned buffer; returns the count.
     *  Zero-allocation (the buffer is its own workspace). `out` needs
     *  (n*8)/5+1 bytes or RangeError (n == 0 writes nothing). Input over
     *  4096 bytes refuses: the codec's cost is quadratic. */
    function base58EncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base58Decode(text: string): Uint8Array;
    /** base58 into a caller-owned buffer; `out` needs at least
     *  text.length bytes or RangeError. No BaseXDecodeInto exists: the
     *  division codec allocates internally anyway (documented). */
    function base58DecodeInto(text: string, out: Uint8Array): number;
    /** Base58 with a double-SHA256 checksum appended. */
    function Base58CheckEncode(data: BytesInput): string;
    /**: base58check text into a caller-owned buffer; `out` needs
     *  ((n+4)*8)/5+1 bytes or RangeError. */
    function base58CheckEncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base58CheckDecode(text: string): Uint8Array;
    /** base58check into a caller-owned buffer; capacity as base58DecodeInto. */
    function base58CheckDecodeInto(text: string, out: Uint8Array): number;
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
    /** Writes the varint at `offset` (default 0) and returns the NEW END
     *  offset. No reallocation: a buffer too small at `offset` throws
     *  RangeError and nothing is written. At most 10 bytes per value. */
    function appendUvarint(buf: Uint8Array, value: number | bigint, offset?: number): number;
    /** The decoded value at `offset` (default 0) WITHOUT the [value, bytesRead]
     *  tuple: Number when it fits exactly (<= 2^53-1), BigInt above. A
     *  truncated stream returns 0 (the value half of uvarint's [0, 0]); a
     *  64-bit overflow throws RangeError; an offset BEYOND the end is
     *  RangeError, while offset == length is an empty stream (returns 0). */
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

    /** JSON5 superset parsing; depth capped at 256.
     *  Placement note: the JSON5 family lives HERE (JSON5Parse/
     *  JSON5Stringify), not in dyna:json; dyna:json owns JSON Pointer/Patch
     *  and will gain JSONC.parse (comments + trailing commas) in phase 1.
     *  Canonical/stable JSON stringify also lives here (StableStringify,
     *  RFC 8785) -- see the note in the dyna:json block. */
    function JSON5Parse(text: string): unknown;
    /** JSON5 output with unquoted keys and NaN/Infinity literals; indent clamped to 0-10. */
    function JSON5Stringify(value: unknown, opts?: { indent?: number }): string;
    /** RFC 8785 canonical JSON; NaN/Infinity rejected. The canonical form is always compact, so `indent` is accepted and ignored. */
    function StableStringify(value: unknown, opts?: { indent?: number }): string;

    /** Compiled RFC 9535 JSONPath expression, reusable across queries.
     *  JSONPath vs JSON Pointer: JSONPath (here) is the PATTERN
     *  query language (wildcards, filters, `$..x`); dyna:json's Pointer
     *  (RFC 6901) is exact addressing of one node and the RFC 6902 patch
     *  path syntax. Use Pointer when you know the address, JSONPath when
     *  you are searching. */
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
    /**: the keyed/derive options object. BLAKE2b keys run 0..64 bytes,
     *  BLAKE2s 0..32; an EMPTY key is legal and means unkeyed (RFC 7693 --
     *  it is the parameter block's key_length field, not a flag). */
    interface Blk2Opts {
        key?: BytesInput;
        length?: number;
    }
    /** BLAKE3 keys are exactly 32 bytes. {context} (or its alias
     *  {deriveKey}) runs the derive-key mode: the context string is hashed
     *  under DERIVE_KEY_CONTEXT, and that 32-byte hash keys a
     *  DERIVE_KEY_MATERIAL pass; the output is exactly 32 bytes and
     *  `length` is refused there. */
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
    /** BLAKE3 Merkle-tree hash; length 1..2^20 bytes.
     *  the opts-object second argument carries {key} / {context}. */
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
    /** 64-bit xxHash as a 16-character hex string by default (a number cannot
     *  hold 64 bits exactly); {as:"bigint"|"bytes"} for the exact value or its
     *  little-endian bytes. */
    function XXHash64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    /** XXH3-64 (xxHash spec 0.8, the default secret); the modern one-shot
     *  cousin of XXHash64, NOT a security primitive. */
    function XXH3_64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;

    /** Streaming digest over the module's whole one-shot digest table:
     *  md5|sha1|sha224|sha256|sha384|sha512|sha3_224|sha3_256|sha3_384|
     *  sha3_512|keccak256|shake128|shake256|blake2b|blake2s|blake3 (the
     *  same strcmp names as the one-shot functions). {length} sizes the
     *  extensible-output algorithms (shake128/shake256/blake3 1..2^20,
     *  blake2b 1..64, blake2s 1..32); the fixed-digest algorithms reject it. */
    class Hasher implements DynResource {
        constructor(algorithm: string, opts?: { length?: number });
        /** Absorbs bytes; returns this for chaining. */
        update(data: BytesInput): this;
        /** Finalizes a copy; the stream stays usable. */
        digest(): Uint8Array;
        digestHex(): string;
        /** Finalizes a copy and writes the bytes into a Uint8Array the CALLER
         *  owns (no allocation); returns the byte count written. RangeError
         *  when buf.byteLength < digestSize; TypeError once closed. The
         *  hasher stays usable afterwards, exactly like digest(). */
        digestInto(buf: Uint8Array): number;
        /** Returns the hasher to its initial state. */
        reset(): void;
        readonly algorithm: string;
        readonly digestSize: number;
        /** Releases the native state early (the GC finalizer is the backstop).
         * Idempotent; every method and getter throws TypeError afterwards. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
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
        /** Byte copy to a new File; refuses an existing destination unless
         *  `{overwrite: true}` (same contract as the free `copyFile`). */
        copyTo(dest: Path, opts?: { overwrite?: boolean }): File;
        reader(opts?: { bufferSize?: number }): FileReader;
        writer(opts?: { bufferSize?: number; preallocate?: number; append?: boolean }): FileWriter;
        toString(): string;
        toJSON(): string;
    }

    /** Buffered sequential reader over a strictly-opened fd.
     *
     *  Two families of reads, by contract: `read`/`readLine`/`readAll` are
     *  the STRING paths (UTF-8 decoded, invalid bytes become U+FFFD);
     *  `readInto`/`readBytes` are the BYTE paths (raw file bytes, no decode,
     *  no repair -- a multi-byte sequence split at a buffer edge arrives
     *  split). */
    class FileReader implements DynResource {
        constructor(path: Path, opts?: { bufferSize?: number });
        /** Up to `n` bytes as a string, "" at EOF; `n` omitted reads all. */
        read(n?: number): string;
        /** The next line without its trailing newline; null at a clean EOF. */
        readLine(): string | null;
        /** The rest of the file. */
        readAll(): string;
        /** Copy up to `buf.length` raw bytes into `buf` (any TypedArray or
         *  DataView, its own view bounds honored); returns bytes copied,
         *  0 = EOF (also 0 for a zero-length buffer). BYTES primitive: no
         *  UTF-8 repair; loops internal fills when `buf` exceeds the read
         *  buffer. */
        readInto(buf: ByteView): number;
        /** Up to `n` raw bytes as a fresh Uint8Array, "" equivalent is the
         *  empty array at EOF; `n` omitted reads all. The byte twin of
         *  `read(n)`, same accumulation cap (DYN_MAX_INPUT). */
        readBytes(n?: number): Uint8Array;
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

    /** Kernel-event file watching (kqueue/inotify), one classifier for both.
     *  event shape: { path, kind } with kind one of "change" | "add" |
     *  "addDir" | "unlink" | "unlinkDir", delivered to BOTH surfaces -- the
     *  start() callback and the async iteration. */
    class Watcher implements DynResource {
        constructor(path: Path, opts?: { recursive?: boolean; debounceMs?: number; ignore?: string[] });  /* opts must be an object when given: a string/number throws TypeError */
        /** Arm the watch; `path` is a plain string, not a Path.
         *  A (re)start reopens the event stream a stop() ended. */
        start(cb?: (event: { path: string; kind: "change" | "add" | "addDir" | "unlink" | "unlinkDir" }) => void): void;
        /** stop: halt the watch and end the event stream with the
         *  Channel close contract -- events ALREADY buffered still drain
         *  through next() in arrival order, then { done: true }, so
         *  stop-then-iterate always terminates. Idempotent; does not close,
         *  and the watcher can start() again. */
        stop(): void;
        /** One async pull: a Promise of { value, done } (the house manual-
         *  iterator shape). A parked pull resolves done on stop()/close(). */
        next(): Promise<{ value: { path: string; kind: string }; done: false } | { value: undefined; done: true }>;
        /** What a for-await `break` calls: end the iteration NOW (buffered
         *  events are dropped) and stop the watch -- the unconditional
         *  cleanup doctrine (cf. pg.queryIter's return()). */
        return(): Promise<{ value: undefined; done: true }>;
        /** The watcher itself, so `for await (const ev of w)` works. */
        [Symbol.asyncIterator](): AsyncIterator<{ path: string; kind: string }>;
        stats(): { entries: number; directories: number; events: number; truncated: boolean; debounceMs: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A compiled glob pattern over the same matcher glob() walks with.
     *
     *  `matches`/`filter`/`Glob.match` are purely LEXICAL (whole-path string
     *  match, no filesystem access): '/' is an ordinary byte to the wildcards
     *  (`*`, `**` and `?` all cross it), a star run may match the EMPTY
     *  string -- but the literals around it stay literal, so the pattern
     *  "a", two stars, slash "c" DOES match "a/c" (the run matches empty),
     *  while "a", slash, two stars, slash, "c" does NOT (its two slashes are
     *  literal and "a/c" carries one) -- and the walk-only leading-dot rule
     *  does NOT apply here ("*idden" matches ".hidden"). Backslash is a
     *  literal byte, not an escape. The per-segment semantics of `glob()`
     *  come from the walk, not from this matcher. */
    class Glob {
        constructor(pattern: string);
        /** Lexical match only, no filesystem access. */
        matches(path: Path): boolean;
        /**: the one-shot spelling of `new Glob(pattern).matches(path)`
         *  (same matcher, two spellings). `path` is a Path or a plain string
         *  -- note a Path contributes its NORMALISED bytes ("" becomes ".")
         *  while a string is used verbatim. */
        static match(path: Path | string, pattern: string): boolean;
        /** The glob() walk with an optional cwd; returns matching Paths. */
        expand(cwd?: Path): Path[];
        /** The subset of the array matching the pattern. */
        filter(paths: Path[]): Path[];
        readonly pattern: string;
        readonly hasWildcard: boolean;
    }

    /** Whole file as a string; strict open first. Invalid UTF-8 becomes
     *  U+FFFD -- pass `{bytes: true}` or use `readBytes` for binary data. */
    function readFile(path: Path, opts?: { bytes?: boolean }): string | Uint8Array;
    /** Whole file as a Uint8Array; the bytes alias of `readFile(path,
     *  {bytes: true})`. Identical to `File.readBytes`. */
    function readBytes(path: Path): Uint8Array;
    /** The copy off the loop thread (>= 1 MiB offloads to the pool);
     *  same refusals as `copyFile`, as a rejection. */
    function copyFileAsync(from: Path, to: Path, opts?: { overwrite?: boolean }): Promise<number>;
    /** Write string or bytes; O_CREAT, truncate unless append; returns the byte count. */
    function writeFile(path: Path, data: BytesInput, opts?: { append?: boolean }): number;
    /** Async read; {bytes:true} resolves a Uint8Array. */
    function readFileAsync(path: Path, opts?: { bytes?: boolean }): Promise<string | Uint8Array>;
    /** Async write; the payload is copied before the call returns. */
    function writeFileAsync(path: Path, data: BytesInput, opts?: { append?: boolean }): Promise<number>;
    /** The inline/offloaded counters and the 1 MiB thresholds. */
    function asyncStats(): { inline: number; offloaded: number; readMin: number; writeMin: number; copyMin: number };

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

    /** Walk the filesystem matching *, **, ?, [...]; returns sorted Paths.
     *
     *  Grammar (runtime-verified): `*` matches any run within ONE path
     *  segment; `**` matches any number of segments INCLUDING none (so a
     *  two-star segment between slashes may match zero segments, and as the
     *  last segment it also emits directories); `?` is one character;
     *  `[abc]`/`[a-z]` are character classes with `[!...]`/`[^...]` negation.
     *  Brace expansion `{a,b}` is NOT supported.
     *  Dotfiles: a wildcard segment does not match a name starting with `.`
     *  unless the pattern segment starts with `.` too (minimatch rule);
     *  `**` skips dotfile entries outright.
     *  Symlinks: `**` never traverses a symlinked directory (cycle safety;
     *  the link itself is still emitted as a leaf); wildcard segments DO
     *  follow symlinks to directories. There is NO `followSymlinks` option.
     *  Options: `{cwd}` roots the walk (default: the process cwd) -- the
     *  only option; options bags are strict (unknown keys throw TypeError).
     *  For a pure lexical test with no filesystem access use the Glob class
     *  below -- note its matcher is whole-path lexical and does NOT apply
     *  the per-segment/leading-dot rules. */
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
    /** An element node in the dyna:xml-compatible tree shape. This is the
     *  SHARED document type: dyna:html parses it (HTMLParse), dyna:scrape
     *  consumes it (Extractor.run, Crawl's parse callback) -- one tree shape
     *  across both modules, not prose. */
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
    function MarkdownToHTML(text: string, opts?: { allowRawHTML?: boolean }): string;

    /** A compiled CSS selector over the parsed tree. */
    class Selector {
        constructor(text: string);
        /** Every matching element. */
        all(doc: HTMLElement | HTMLElement[]): HTMLElement[];
        /** The first match, or null when none. */
        first(doc: HTMLElement | HTMLElement[]): HTMLElement | undefined;
        /** True when the single node matches (no combinators allowed). */
        matches(node: HTMLElement): boolean;
    }

    /** An allow-list HTML sanitizer. */
    class Sanitizer {
        /** An allow-list is required; there is no default policy. */
        constructor(opts: { allow: Record<string, unknown>; protocols?: Record<string, string[]> });
        clean(html: string): string;
    }

    /**: rewrites URL-bearing attributes IN PLACE through
     *  `fn(url, tag, attr)` -- the crawler's canonicalize step. Single-URL
     *  sinks (href, src, action, formaction, cite, data, poster,
     *  background, ping, xlink:href, longdesc) pass the whole value to fn;
     *  srcset splits into candidates and rewrites each URL (descriptors
     *  preserved, rejoined with ", "). fn returning undefined or null keeps
     *  the original; any other return replaces it (coerced with ToString).
     *  Returns the number of attributes actually replaced (srcset
     *  candidates count each). Tag and attr names reach fn lower-cased.
     *  A fn that throws mid-walk leaves the earlier rewrites applied and
     *  its exception propagates. */
    function rewriteLinks(doc: HTMLElement | HTMLElement[], fn: (url: string, tag: string, attr: string) => unknown): number;

    /** A compiled template with escaping. */
    class Template {
        constructor(source: string, opts?: { escape?: boolean });
        /** Renders with the given data scope. */
        render(data?: unknown): string;
    }
}

/* ================================================================== *
 *  dyna:http
 *  Every options/handler bag is STRICT: an unknown key throws a TypeError
 *  naming the key and the valid set (e.g. `unknown option "workerz"
 *  (valid: port, workers, backlog, requestTimeoutMs, host, routes)`).
 *  Covers the server/App ctors, route value specs, static/upload/proxy,
 *  ws/sse/WsClient handler bags, CookieSerialize opts, and the fetch/
 *  Request/Response init bags (fetch throws synchronously). A null/
 *  primitive bag counts as absent; symbol and inherited keys are not
 *  visible to the check.
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
    function Negotiate(header: string, candidates: string[]): string | null;
    /** Content negotiation by media-type token. */
    /** Picks the best supported token by q-value; "*" selects the first candidate. */
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

    /** A blocking HTTP client over one connection.
     *  Choosing an HTTP client: global `fetch` for one-shot async
     *  calls (WHATWG shape, redirects followed, RequestInit.timeout honored);
     *  HTTPClient for blocking get/post/request with explicit
     *  setTimeout/disconnect control and a maxBody cap; scrape's
     *  Fetcher when robots policy, retries, backoff and caching matter
     *  (crawler workloads). */
    class HTTPClient implements DynResource {
        constructor(maxBody?: number);
        /** GET; returns {status, statusText, ok, headers, body}. */
        get(url: string, headers?: Record<string, string>): HTTPResponse;
        post(url: string, body?: BytesInput, headers?: Record<string, string>): HTTPResponse;
        request(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): HTTPResponse;
        getAsync(url: string, headers?: Record<string, string>): Promise<HTTPResponse>;
        postAsync(url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        requestAsync(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** Connect hook (P3): set per client; called with the RESOLVED IP
         *  between TCP connect and the first request byte. Return false
         *  (or throw) to refuse the connection for this request; a
         *  non-function value is ignored. */
        onConnect?: (ip: string) => unknown;
        setTimeout(ms: number): void;
        disconnect(): void;
        /**: ONE exchange (no redirect chasing, like get) with the
         *  body left on the socket: the returned StreamedResponse's
         *  read(buf) pulls bytes as they are consumed. Framing
         *  (Content-Length, chunked, read-to-EOF) matches get()'s
         *  buffered reader; maxBody is enforced on the stream; a
         *  truncated body rejects the read by name. The caller owns the
         *  stream: close() (or `using`) releases the socket. */
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
        headers: Record<string, string>;
        body: string;
    }

    /**: the getStream response -- the head readable off the SAME
     *  object that streams the body. `read`/`close` are dyna:stream's
     *  ByteSource shape (duck-typed; this module neither imports nor
     *  links dyna:stream), so pipe(), lines(), ndjson() and inflate()
     *  consume the response directly. */
    interface StreamedResponse {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        /** The final url of the exchange (one hop: no redirect chasing,
         *  exactly like get()). */
        url: string;
        contentType: string;
        /** Fills the caller's buffer with up to buf.length body bytes and
         *  resolves with the count; 0 = end of body. Rejects on
         *  truncation (connection ended before a declared
         *  Content-Length), on the client's max body being crossed, and
         *  on malformed chunked framing -- the stream is then closed and
         *  every later read rejects the same error. */
        read(buf: Uint8Array): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A route table: path -> literal body, or {status, contentType, body}. Static only.
     *
     *  Choosing an HTTP server: App is the routed application
     *  server (rpc/static/proxy/upload/ws/sse endpoints, single event
     *  loop); HTTPServer is the thread-pool server for static route
     *  tables (a request handled per worker thread); HTTPServerAsync is
     *  the single-threaded reactor for many mostly-idle connections.
     *  Start with App; drop to the others for their specific shapes. */
    type StaticRoutes = Record<string, string | { status?: number; contentType?: string; body?: string }>;

    /** A thread-pool HTTP server serving static routes. */
    class HTTPServer implements DynResource {
        constructor(opts?: {
            port?: number;
            host?: string;
            workers?: number;
            backlog?: number;
            requestTimeoutMs?: number;
            routes?: StaticRoutes;
        });
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
        constructor(opts?: {
            port?: number;
            host?: string;
            backlog?: number;
            idleTimeoutMs?: number;
            maxConns?: number;
            routes?: StaticRoutes;
        });
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
        constructor(opts?: { port?: number; host?: string; idleTimeoutMs?: number; maxConns?: number; compress?: boolean; metrics?: boolean;
            /** listen(2) backlog, forwarded to the listen call at start().
             *  Integer 1..65535; default 1024. */
            backlog?: number;
            /** Validated and reported (integer 1..64, the thread-pool
             *  HTTPServer's range) so a config shared with HTTPServer does
             *  not drop the key. App is a single-reactor server: today every
             *  handler runs on the one loop thread, so this sizes nothing.
             *  Default 1. */
            workers?: number; });
        /** Registers a strict JSON-RPC 2.0 endpoint. */
        rpc(path: string, methods: Record<string, (...args: unknown[]) => unknown>): this;
        /**: an rpc handler may ALSO return a dyna:stream ByteSource
         *  (any object with read(buf) -> Promise<number>) -- or
         *  { stream, contentType?, status? } -- instead of a JSON value.
         *  That response then opts out of the JSON-RPC envelope: the App
         *  answers with the handler's status/content type and
         *  `Transfer-Encoding: chunked`, pumping read() one chunk at a
         *  time; a 0 ends the body (the source's close() runs), anything
         *  thrown mid-stream truncates the connection. HTTP/1.0 peers
         *  get a 500 (no chunked framing below 1.1), a peer refusing
         *  identity encoding a 406, and a ByteSource inside a BATCH an
         *  internal error (a batch is one response). */

        /** Serves a static document root at a URL prefix. */
        static(prefix: string, root: import("dyna:file").Path, opts?: { maxFileSize?: number; allow?: string[] }): this;
        /** Proxies a URL prefix to a host/port. */
        proxy(prefix: string, opts: { host?: string; port?: number }): this;
        /** Registers an upload endpoint; the handler receives the saved path
         * and {size, contentType} once the body is fully written. */
        upload(path: string, opts: { dir?: import("dyna:file").Path; maxFileSize?: number; allow?: string[] }, handler: (savedPath: string, meta: { size: number; contentType: string }) => void): this;
        /** Registers a WebSocket endpoint; handlers receive the connection. */
        ws(path: string, handlers: { open?: (socket: WsConn) => void; message?: (socket: WsConn, data: string | Uint8Array, isBinary: boolean) => void; close?: (socket: WsConn, code: number, reason: string) => void }): this;
        /** Registers a server-sent events endpoint; open and close receive
         * the stream. */
        sse(path: string, handlers: { open?: (stream: SseConn) => void; close?: (stream: SseConn) => void }): this;
        /**: registers a dynamic route for one method. `pattern` is a
         *  path (no query/fragment) whose segments are literal text, `:param`
         *  (exactly one path segment, non-empty, captured percent-decoded
         *  into `req.params`) or a TRAILING `*rest` (the path remainder,
         *  slashes included; "" on the bare prefix). Names match
         *  [A-Za-z0-9_]+ and must be unique in the pattern (at most 16
         *  named captures); at most one
         *  wildcard, which must be last. An invalid pattern throws a
         *  TypeError at registration naming the problem. A trailing slash
         *  is not significant ("/v/" and "/v" match each other). Matching
         *  is raw-path (params decode, static segments compare as sent) and
         *  first-registered wins. `handler(req)` returns the response:
         *  a string is 200 text/plain, a byte view 200 application/
         *  octet-stream, an object with an own `status` number or `body`
         *  is an envelope {status?, body?, contentType?} (a string body
         *  defaults to text/plain, an absent body to the envelope's content
         *  type), anything else JSON-encodes at 200. Undefined is a 500.
         *  A thenable return settles before the response sends; a throw or
         *  rejection is a 500 carrying the (JSON-escaped) message. When a
         *  pattern matches but no route's method does, the answer is 405.
         *  Returns `this`. */
        get(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for POST. */
        post(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PUT. */
        put(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PATCH. */
        patch(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for DELETE (the method name cannot be a keyword). */
        del(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: appends middleware, run in registration order before the
         *  matched dynamic handler with the SAME AppRequest object the
         *  handler receives (annotate it to pass state along). Returning an
         *  object with a `response` property short-circuits: that value is
         *  sent like a handler's return (a thenable response is awaited).
         *  Any other return continues the chain. Middleware is SYNC: a
         *  thenable return is a 500 (put async work in the handler). A
         *  throw ends the chain with a 500 (the message JSON-escaped).
         *  Middleware runs for matched DYNAMIC routes only -- typed routes
         *  (rpc/static/proxy/upload/ws/sse) and unmatched paths are not
         *  gated by it. Returns `this`. */
        use(fn: (req: AppRequest) => { response?: unknown } | void): this;
        start(): void;
        get port(): number;
        /** The ctor's workers option (default 1). See the ctor note: it is
         *  validated and echoed, and sizes nothing today. */
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

    /**: the request context handed to dynamic-route handlers and
     *  to `app.use` middleware. */
    interface AppRequest {
        /** The request method as sent; route matching is exact and
         *  case-sensitive (HEAD is not aliased to GET). */
        method: string;
        /** The request path, raw (percent-encoding as received), query
         *  stripped. */
        path: string;
        /**: `:param` captures (one path segment each, percent-decoded)
         *  and the trailing `*rest` capture (the path remainder, slashes
         *  included, decoded). A pattern with no params yields {}. */
        params: Record<string, string>;
        /** The query string: keys and values percent-decoded ('+' decodes
         *  to space), a bare key is present with value "", a repeated key
         *  keeps the LAST occurrence. */
        query: Record<string, string>;
        /** Request headers; names lower-cased. Only the comma-joinable list
         *  fields and cookie/set-cookie may repeat (any other duplicate is a
         *  400 before dispatch): repeats join with ", ", Cookie joins with
         *  "; ", Set-Cookie keeps the first copy (its values cannot be
         *  combined). */
        headers: Record<string, string>;
        /** The raw request body as a string. */
        body: string;
    }

    /** A WebSocket client. */
    class WsClient implements DynResource {
        constructor(url: string, handlers: { open?: (ws: WsClient) => void; message?: (ws: WsClient, data: string | Uint8Array, isBinary: boolean) => void; close?: (ws: WsClient, code: number, reason: string) => void });
        send(data: BytesInput): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** WHATWG fetch subset: no `redirect`/`credentials`/`integrity`; redirects are followed automatically and there is no cookie jar. */
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
 *  Every options bag is STRICT: an unknown key throws a TypeError naming
 *  the key and the valid set (e.g. `unknown option "portz" (valid: tls,
 *  ca, host, path, username, password, port, db, binary, bigint,
 *  maxReplyBytes, maxPending, connectTimeoutMs, commandTimeoutMs)`).
 *  Covers the Redis/PostgreSQL/DNSResolver/DNSServer/TCPProxy ctor bags,
 *  pg query()'s {maxRows}, TCPServer/connect/connectHappy options, their
 *  `tls` sub-bags, and the connect/WS handler bags. Params objects,
 *  upstream lists and command args are DATA (keys are never checked).
 *  A null/primitive bag counts as absent; symbol and inherited keys are
 *  not visible to the check.
 * ================================================================== */
declare module "dyna:net" {
    /** Networking: addresses and CIDR prefixes, sockets, DNS, protocol clients, rate limiting and metrics, plus the shared HTTP surface re-exported from dyna:http. */

    /* ---- shared HTTP surface, re-exported from dyna:http ----
     * every name below IS the dyna:http object (same identity, not a
     * second definition -- runtime-verified). dyna:net re-exports them so a
     * one-import script can `import {fetch, ...} from "dyna:net"`; the
     * canonical home for the HTTP surface stays dyna:http. */
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

    /** A compiled CIDR prefix: parsed and masked once at construction, then cheap to test.
     *  The name says "Prefix", this is CIDR networking (contains/overlaps/masked/bits) --
     *  nothing to do with dyna:structures.Trie, which is a string trie. */
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
        /** A direct-mapped token-bucket table. `tokensPerSec` (or its alias
         *  `refill`) is required; `burst` (alias `capacity`) defaults to one
         *  second of traffic -- an alias pair with different values throws.
         *  The table is FIXED by default (the anti-amplification bound);
         *  `grow: true` opts into doubling past a 3/4 load factor, up to the
         *  hard ceiling of 2^20 slots, past which colliding keys share slots
         *  as always (stats.grew counts doublings). the options bag is
         *  strict -- an unknown key throws a TypeError naming the key and
         *  the valid set. */
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

    /** A fixed registry of counters, gauges and histograms with a Prometheus text scrape. */
    const Metrics: {
        /** Increments a counter; a negative or NaN increment is refused. */
        counter(name: string, value?: number, labels?: Record<string, string>): void;
        /** Sets a gauge to a value. */
        gauge(name: string, value: number, labels?: Record<string, string>): void;
        /** Records an observation into the 5ms..1s buckets. */
        /** Records an observation into the series' buckets: 5ms..1s
         *  by default, or the `opts.buckets` edges given at registration --
         *  1..6 finite, positive, strictly increasing seconds. Edges are
         *  fixed once a series exists; later calls ignore them. the
         *  opts bag is strict -- an unknown key throws a TypeError naming
         *  the key and the valid set. */
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
        /** `{ttl}` (seconds) enables the per-resolver answer cache; absent or
         *  0 disables it. A successful non-empty answer caches under
         *  `lowercase(name)|type` for min(smallest RR TTL, ttl) seconds (the
         *  cap clamps to 86400); a TTL-0 or empty answer is never cached.
         *  Every lookup over a cached entry gets a FRESH deep copy of the
         *  answer -- mutating a returned array or record can never poison a
         *  later hit -- and hits settle asynchronously exactly like wire
         *  answers. There is no single-flight de-duplication: concurrent
         *  lookups of an uncached name each put a query on the wire. */
        constructor(opts?: { server?: string; port?: number; timeoutMs?: number; /** answer-cache cap in seconds (1..86400); absent/0 disables caching */ ttl?: number });
        /** Queries a name; `type` is the record type (A is 1). The callback receives (err, records). */
        query(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): void;
        /** Promise form: resolves with the decoded answers (type defaults to A), rejects with an Error. The callback form still works: a trailing function receives (err, records). With `{ttl}` caching on, a cache hit serves a fresh deep copy of the cached answer without a wire query (see the ctor). */
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
        /** Connect through TLS (the handshake runs on the aio engine). */
        tls?: boolean;
        /** CA certificate PEM file path pinned for the TLS handshake;
         *  omitted (or null) uses the system trust store. */
        ca?: string;
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
        /** Connect through TLS (the startup handshake is encrypted). */
        tls?: boolean;
        /** CA certificate PEM file path pinned for the TLS handshake;
         *  omitted (or null) uses the system trust store. */
        ca?: string;
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
        /** Streaming rows via a cursor: constant memory for any result size.
         *  queryIter is NATIVE (a DECLARE CURSOR/FETCH FORWARD driver
         *  in dyna:net; BEGIN ... CLOSE + COMMIT/ROLLBACK cleanup is
         *  unconditional, so a break mid-stream always ends the
         *  transaction). opts: {batch} rows per FETCH FORWARD (default
         *  1000), {maxRows} total rows (default 0 = unbounded -- the
         *  safety cap for huge results). Construction is LAZY (async-
         *  generator semantics): no IO until the first next(); a closed
         *  connection rejects there. The stream is an object with
         *  next()/return()/[Symbol.asyncIterator] (there is no C-level
         *  async generator), so for-await and manual iteration both work.
         *  src/pool.js no longer needs importing for this: its installer
         *  sees the native method and stands down. */
        queryIter(sql: string, params?: unknown[], opts?: { batch?: number; maxRows?: number }): AsyncIterableIterator<Record<string, unknown>>;
        /** The server parameters from startup (server_version, client_encoding, ...). */
        readonly parameters: Record<string, string>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /* PgPool/RedisPool are NOT exports of this module (importing them from
     * "dyna:net" yields undefined). They are pure-JS classes in the source
     * tree, imported by file path and declared in the src/pool.js block
     * below. */

    /** SQLite options. */
    interface SQLiteOptions {
        readonly?: boolean;
        bigint?: boolean;
    }
    /** A SQLite database handle; every value is bound, never interpolated. */
    class SQLite implements DynResource {
        constructor(path: string, opts?: SQLiteOptions);
        /** Runs a statement and returns one object per result row. */
        /** Duplicate column names collapse to the last one — alias them in the SELECT. */
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
    /** Client TLS: the name verified defaults to the host; cert/key present the client certificate (mTLS). */
    interface TCPClientTLSOptions {
        ca?: string;
        servername?: string;
        alpn?: string | string[];
        minVersion?: string;
        rejectUnauthorized?: boolean;
        cert?: string;
        key?: string;
    }
    /** Options for TCPServer.connect. */
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

    /** Happy Eyeballs (RFC 8305, simplified): both address families race in parallel -- not the RFC's staggered Resolution Delay -- and the first success wins; fallbackMs is the whole-race deadline. */
    function connectHappy(host: string, port: number, opts?: { fallbackMs?: number }, handlers?: TCPHandlers): TCPServer;

    /** How many net event-handler throws (TCP connect/data/close, UDP message) have been swallowed process-wide. */
    function swallowedHandlerThrows(): number;
}


/* ================================================================== *
 *  src/pool.js  (JS-side module, imported by file path)
 * ================================================================== */
/**
 * PgPool and RedisPool are NOT builtins: they are pure-JS orchestration over
 * the dyna:net native clients, shipped as a source file next to the engine.
 * Import them by their real specifier, relative to the importing file:
 *
 *   import { PgPool, RedisPool } from "./src/pool.js";   // or ../src/pool.js
 *
 * Importing them from "dyna:net" compiles under the old declarations but is
 * `undefined` at runtime -- exactly the drift this dedicated block removes.
 * The ambient module name is TypeScript's one-wildcard pattern form: a single
 * leading wildcard matches every relative spelling whose tail is
 * /src/pool.js ("./src/pool.js", "../src/pool.js", "<root>/src/pool.js").
 * A plain relative name ("./src/pool.js") is not legal in an ambient module
 * declaration.
 */
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
        stmtCacheMax?: number;
        maxPending?: number;
    }
    /** A bounded pool of PostgreSQL clients; query/pipeline auto acquire-release. */
    class PgPool {
        constructor(opts?: PgPoolOptions);
        readonly stats: { total: number; free: number; used: number; waiting: number; closed: boolean };
        query(sql: string, params?: unknown[], opts?: { maxRows?: number }): Promise<unknown>;
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
    /** A bounded pool of Redis clients. */
    class RedisPool {
        constructor(opts?: RedisPoolOptions);
        readonly stats: { total: number; free: number; used: number; waiting: number; closed: boolean };
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        pipeline(commands: (string | number | Uint8Array | ArrayBuffer)[][]): Promise<unknown[]>;
        close(): Promise<void>;
    }
}

/* queryIter is declared ON the class in the dyna:net block above (
 * one block per module, so readers of dyna:net see the whole surface).
 * it is a NATIVE method there; importing src/pool.js is no longer
 * needed for it. pool.js's installer checks `!PostgreSQL.prototype.queryIter`,
 * so on a native build the JS fallback stands down and the pools simply
 * delegate any queryIter use to the native driver. */

/* ================================================================== *
 *  dyna:json
 * ================================================================== */
declare module "dyna:json" {
    /** This module owns JSON Pointer (RFC 6901) and JSON Patch (RFC 6902).
     *  Two neighbours live elsewhere on purpose: canonical/stable
     *  JSON stringify is encoding.StableStringify (RFC 8785) in dyna:encoding,
     *  and the JSON5 family is encoding.JSON5Parse/JSON5Stringify there too;
     *  JSONC.parse (comments + trailing commas) joins THIS module in phase 1.
     *  JSONPath queries (RFC 9535) are encoding.JSONPath -- see its
     *  JSONPath-vs-Pointer note. */

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

    /** NDJSON (newline-delimited JSON), buffer-at-once: the whole
     *  text is parsed (or every item serialized) before anything is
     *  returned. The single STREAMING form is `stream.ndjson(src)` in
     *  dyna:stream -- this module deliberately does not grow a second
     *  streaming entry point. The line rules are identical in both:
     *  lines split on `\n` with one trailing `\r` stripped, and a line
     *  that is empty or only whitespace is skipped. */
    namespace Ndjson {
        /** Parses every line into a value; blank (whitespace-only) lines
         *  are skipped. A malformed line throws a SyntaxError naming its
         *  1-based FILE line number -- blank lines count toward the
         *  number, which is a position in the text, not an index into
         *  the result. */
        function parse(text: string): unknown[];
        /** One compact JSON document per line, trailing newline included.
         *  An item with no JSON form (undefined, a function, a symbol)
         *  throws a TypeError naming its 1-based index rather than
         *  emitting an empty line; a BigInt propagates JSON.stringify's
         *  own TypeError. */
        function stringify(items: unknown[]): string;
    }
}

/* ================================================================== *
 *  dyna:log
 * ================================================================== */
declare module "dyna:log" {
    /** Leveled structured logging: JSON lines (default) or a human text format, to stderr or a (rolling) file. */
    type LogLevel = "trace" | "debug" | "info" | "warn" | "error" | "fatal" | "silent";
    class Logger {
        /** Options bags are strict -- an unknown key throws a TypeError
         *  naming the key and the valid set (in the ctor bag, the rollover
         *  sub-bag, and child's second argument). */
        constructor(opts?: {
            level?: LogLevel;
            name?: string;
            timestamp?: "epoch" | "iso" | false;
            base?: Record<string, unknown>;
            /** Output path, or a custom sink `(line: string) => void`
             *  receiving the exact formatted line, trailing newline
             *  included, the instant it is built (unbuffered: flush() is a
             *  no-op; a throwing sink propagates). Rollover needs a path.
             *  Default: stderr. */
            dest?: string | ((line: string) => void);
            /**: emit sampling -- every Nth attempt that passes the
             *  level gate is written. Deterministic (counter mod N, per
             *  logger; a child restarts at zero). Default 1 = everything. */
            sample?: number;
            /** Create dest's parent directories. */
            mkdir?: boolean;
            /** Batch file output: true => 64 KiB; a number is a byte size (1..1048576). */
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
                /** Keep `dest` itself as a symlink to the active file. */
                symlink?: boolean;
            };
        });
        /** Each emits one line below the configured level. Shapes: (msg), (fields, msg), (err, msg), (err, fields, msg). A caller key colliding with a frame key (time/level/name/pid/hostname/msg/err) is dropped. */
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

    /**
     * Returns a function that prints only when DEBUG matches. Comma-separated
     * patterns, last match wins: a leading '-' negates, one '*' may sit
     * anywhere (`DEBUG=*,-app:secret`). Multiple call arguments join with a
     * space.
     */
    function Debug(namespace: string): (...args: unknown[]) => void;
}

/* ================================================================== *
 *  dyna:matcher
 * ================================================================== */
declare module "dyna:matcher" {
    /** A compiled single-pattern matcher. */
    class Matcher {
        constructor(pattern: string, opts?: { algo?: "kmp" | "bmh" | "boyer-moore" });
        /** Code-unit offset of the first match, or -1. `fromIndex` follows
         *  String.prototype.indexOf conventions: negative is 0, past the end
         *  is -1, an empty pattern answers min(fromIndex, length). */
        firstIn(text: string, fromIndex?: number): number;
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
        /** Non-overlapping left-to-right replacement over the whole pattern
         *  set, in one pass. Hits are consumed in allIn's order (by end
         *  position; longest first at one end), and a hit is replaced only
         *  when it starts at or after the previous replacement's end.
         *  `repl` is a replacement string, or repl(match, patternIndex)
         *  receiving the same two fields allIn reports; its result is
         *  ToString'd (String.replace conventions). */
        replaceAllIn(text: string, repl: string | ((match: string, index: number) => string)): string;
        readonly size: number;
        readonly states: number;
    }

    /** Exact edit distance in code points, Myers bit-parallel below 64. */
    function Levenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Bigram multiset similarity in [0, 1]. */
    function DiceCoefficient(a: string, b: string): number;
    /** Jaro similarity with Winkler's common-prefix boost (l capped at 4,
     *  scale 0.1, applied when the Jaro score exceeds 0.7): [0, 1].
     *  Code-point operands, like Levenshtein. */
    function JaroWinkler(a: string, b: string): number;
    /** Restricted Damerau-Levenshtein (the OPTIMAL STRING ALIGNMENT variant:
     *  a transposition may not overlap another edit) in code points. Same
     *  { max } contract as Levenshtein: exact while <= max, max + 1 beyond. */
    function DamerauLevenshtein(a: string, b: string, opts?: { max?: number }): number;
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

/* WHERE STATISTICS LIVE (/ ) — the three layers and their
 * input shapes, so a caller picks the right door first:
 *
 *   Array.prototype (extension): sum/mean/min/max/median/product over
 *     number[] — convenience.
 *   mathx.stats (this module): sum/mean/variance/stddev/median/quantile/
 *     min/max/cov/corr — Float64Array fast path, number[] and array-likes;
 *     the MISSING HALF: variance/quantile/corr/cov.
 *   DataFrame (dyna:dataframe): VARIANCE/STDDEV/SKEW/KURTOSIS/CORR/COV/
 *     QUANTILE(S)/DESCRIBE/HISTOGRAM — columnar + masks, the full set.
 *
 * Cross-references for the deliberate overlaps:
 *   - mathx.linspace (n points, INCLUSIVE ends) vs Number.range (EXCLUSIVE
 *     end): linspace for a closed grid of known count, Number.range for an
 *     open stepper.
 *   - mathx histogram: none — equal-width binning lives on
 *     DataFrame.HISTOGRAM; do not look for it here.
 *   - stats.quantile/median use the exact DataFrame.QUANTILE/MEDIAN
 *     convention (R-7) and agree bit-for-bit on the same values; stats.corr
 *     THROWS on zero variance where DataFrame.CORR answers NaN.
 *   - Array.prototype.sum/mean are plain accumulators over number[];
 *     mathx.stats.sum is Neumaier-compensated (exact on [0.1]*1e7).
 */
declare module "dyna:mathx" {
    /** Mathematical constants, written with enough digits for one correctly-rounded conversion.
     *
     *  Where statistics live: scalar/array helpers here
     *  (cumsum/cumprod/diff, bits); columnar statistics -- including the
     *  histogram (HISTOGRAM/HISTOGRAM_NORMALIZED, equal-width bins) -- live on
     *  dyna:dataframe; Array.prototype carries the number[] conveniences
     *  (sum/mean/min/max/median/product). mathx deliberately has no histogram
     *  and no bare-Float64Array stats layer of its own. */
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
    /** The gap to the next representable double away from zero; bare eps is eps(1).
     *  At realmax there is no next double, so eps(realmax) is the ulp of the top
     *  binade, 2^971 (matching MATLAB). */
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

    /** Both arguments within the int64 range; wider BigInts are refused. */
    function gcd(a: number | bigint, b: number | bigint): bigint;
    /** Both arguments within the int64 range; wider BigInts are refused. */
    function lcm(a: number | bigint, b: number | bigint): bigint;
    /** n! exactly, capped at 10000. */
    function factorial(n: number): bigint;
    /** BigInt only; the magnitude as unsigned. The magnitude must be below
     *  2^64: wider BigInts are refused rather than silently truncated. */
    function abs(n: bigint): bigint;
    /** Minimum bits to represent the magnitude; bitLen(0n) is 0. The magnitude
     *  must be below 2^64: wider BigInts are refused rather than truncated. */
    function bitLen(n: bigint): number;
    /** Set bits in the magnitude. The magnitude must be below 2^64: wider
     *  BigInts are refused rather than truncated. */
    function popcount(n: bigint): number;

    /** Binomial coefficient, built multiplicatively. */
    function nchoosek(n: number, k: number): number;
    /** Every permutation, reverse lexicographic, at most 8 elements. */
    function perms(v: number[]): number[][];
    /** Rational approximation by continued fractions within relative tolerance. */
    function rat(x: number, tol?: number): [number, number];

    /** n points inclusive of both ends; the last point is exactly b.
     *  linspace is INCLUSIVE at the end; the Number.range prototype
     *  extension is EXCLUSIVE -- pick per the boundary you mean. */
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
        /** The named twin of rotateLeft: k reduces modulo the width, so a
         *  negative k rotates left (-k), exactly as rotateLeft mirrors. */
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

    /** Reductions over a bare Float64Array (fast path, read directly) or
     *  number[] / array-likes (per element). Contracts that differ from the
     *  neighbours, on purpose:
     *  - sum is Neumaier-compensated: exact where Array.prototype.sum and
     *    DataFrame.SUM drift (sum([0.1] x 1e7) === 1000000 exactly).
     *  - median/quantile use the DataFrame.QUANTILE R-7 convention
     *    (pos = q*(n-1), linear between closest ranks) but THROW RangeError
     *    on an empty input where DataFrame answers undefined; a NaN
     *    element answers NaN where DataFrame's mask drops it
     *    (stats.median([1,NaN,3]) is NaN, DataFrame.MEDIAN is 2).
     *    Interpolated quantiles are bit-identical to DataFrame.QUANTILE
     *    and within 1 ULP of an exact R-7 reference (FMA contraction).
     *  - corr throws RangeError on zero variance / empty input where
     *    DataFrame.CORR answers NaN; NaN INPUTS answer NaN like everywhere.
     *  - min/max follow Math.min/Math.max: NaN poisons, empty is
     *    +Infinity/-Infinity, -0 wins min and +0 wins max.
     *  - a partial sum that overflows to Infinity is reported as NaN
     *    (the compensation term goes Inf-Inf), not Inf. */
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
        /** R-7 / "linear" between closest ranks, DataFrame.QUANTILE's exact
         *  convention; q must be in [0, 1] and the array non-empty
         *  (RangeError otherwise); a NaN element answers NaN. */
        function quantile(a: ArrayLike<number>, q: number): number;
        /** Math.min semantics: NaN poisons; empty is +Infinity. */
        function min(a: ArrayLike<number>): number;
        /** Math.max semantics: NaN poisons; empty is -Infinity. */
        function max(a: ArrayLike<number>): number;
        /** Sample covariance (n-1) by default; opts.pop divides by n.
         *  Length mismatch throws RangeError. */
        function cov(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** Pearson r, clamped to [-1, 1]; length mismatch or zero variance
         *  throws RangeError; NaN inputs answer NaN. */
        function corr(a: ArrayLike<number>, b: ArrayLike<number>): number;
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
 *  Every options bag is STRICT: an unknown key throws a TypeError naming
 *  the key and the valid set (e.g. `unknown option "nItr" (valid: scoring,
 *  seed, nIter, shuffle, k, folds)`). Covers estimator ctor bags
 *  (LogisticRegression, the tree/booster family, SVC, GaussianMixture),
 *  every fit()'s trailing {sampleWeight} bag, and the model-selection
 *  bags (trainTestSplit/kFold/stratifiedKFold/crossValScore/gridSearch/
 *  randomSearch). Positional ctors (LinearRegression, KMeans, PCA, NB, KN*,
 *  DBSCAN, scalers, Pipeline) take no bag. A null/primitive bag counts as
 *  absent; symbol and inherited keys are not visible to the check.
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

    /** Closed-form ordinary least squares. A CSR X accumulates nonzero pairs
     *  of each row's summed reading -- bit-identical coefficients to the
     *  dense fit of the same matrix. */
    class LinearRegression implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): LinearRegression;
        static load(path: import("dyna:file").Path): LinearRegression;
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        decisionFunction(X: Matrix | CSR, rows?: number, cols?: number): number[] | number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): number;
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): number[][];
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
        /**: writes the transformed rows into `out` and returns the
         *  number of rows. Never allocates; RangeError when `out` is shorter
         *  than the transform (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): number;
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
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
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
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNRegressor;
        static load(path: import("dyna:file").Path): KNRegressor;
        fit(X: Matrix, y: Target, rows?: number, cols?: number): this;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): number[] | Float64Array;
        /**: writes the predictions into `out` and returns the number of
         *  rows written. Never allocates; RangeError when `out` is shorter
         *  than the prediction (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X (earlier rows are written before
         *  later ones are read). */
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
        /**: writes the transformed rows into `out` and returns the
         *  number of rows. Never allocates; RangeError when `out` is shorter
         *  than the transform (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): number;
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
        /**: writes the transformed rows into `out` and returns the
         *  number of rows. Never allocates; RangeError when `out` is shorter
         *  than the transform (nothing is written); `out` must be exactly a
         *  Float64Array and must not alias X. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): number;
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
    /** A compressed sparse row matrix (scipy's layout): `values`/`columns`
     *  hold the nonzeros row by row, `rowPointers` has `rows + 1` boundaries.
     *
     *  Where a CSR is accepted: every `predict`/`predictProba`/`predictInto`
     *  (and SVC's `decisionFunction`) takes one on every estimator,
     *  and LinearRegression/LogisticRegression also `fit` one. Fits,
     *  `apply()` and the scaler/PCA `transform()`s stay dense-only and throw
     *  a TypeError naming `toDense()`. A predict expands ONE row at a time
     *  into a cols-sized scratch and runs the same kernel the dense form
     *  runs, so a sparse and a dense call on the same data return the same
     *  BIT-IDENTICAL answers (one computation), and LinearRegression.fit
     *  reads rows through the same summed reading with the dense arm's
     *  rounding, so a sparse fit is bit-equal to the dense fit of the same
     *  matrix (LogisticRegression's fit gradient is equivalence-class equal,
     *  not guaranteed bit-equal); the whole matrix is never expanded. Duplicate column
     *  indices within a row SUM (scipy's rule) in storage order -- in
     *  `row()`, `toDense()`, every predict and both sparse fits alike -- and
     *  column order within a row is free. A row whose SUM overflows the
     *  finite range is refused exactly where its dense reading is. */
    class CSR implements DynResource {
        constructor(values: number[] | Float64Array, columns: number[] | Int32Array, rowPointers: number[] | Int32Array, cols: number);
        /** Drops exact zeros from a dense matrix. Like the constructor,
         *  refuses a non-finite value (RangeError naming the cell): a CSR
         *  never carries one. */
        static fromDense(X: Matrix, rows?: number, cols?: number): CSR;
        /** The dense form; throws if it does not fit in memory. Duplicate
         *  column indices in a row sum. */
        toDense(): number[][];
        /** Row i as a dense Array. Duplicate column indices sum. Throws if the
         *  dense row does not fit in memory. */
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

    /** The estimators and transformers dyna:ml accepts as Pipeline stages
     *  . Duck-typed stages that merely implement fit/transform also
     *  work at runtime, but the declared contract is these classes. */
    type PipelineStage = LinearRegression | LogisticRegression | KMeans | SVC
        | GaussianMixture | GaussianNB | DecisionTreeClassifier | DecisionTreeRegressor
        | RandomForestClassifier | RandomForestRegressor | GradientBoostingRegressor
        | GradientBoostingClassifier | XGBRegressor | XGBClassifier | PCA
        | KNClassifier | KNRegressor | DBScan | StandardScaler | MinMaxScaler;

    /** A composition of feature stages and a final estimator. */
    class Pipeline implements DynResource {
        constructor(stages: PipelineStage[]);
        fit(X: Matrix, y: Target): this;
        predict(X: Matrix): number[];
        predictProba(X: Matrix): number[][];
        transform(X: Matrix): number[][];
        /** The i-th stage (negative counts from the end). */
        stage(i: number): PipelineStage;
        readonly length: number;
        readonly fitted: boolean;
        readonly estimator: PipelineStage;
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
    /**: `nJobs` is validated (positive integer; ToInt64 semantics --
     *  2.5 truncates to 2, huge values are accepted) and then run
     *  SEQUENTIALLY: an os.Worker cannot receive the estimator factory, so
     *  parallelism is reserved, not faked. `onProgress({done, total})` fires
     *  after each fold. */
    function crossValScore(estimatorFactory: () => { fit(X: Matrix, y: Target): unknown; predict(X: Matrix): number[]; close(): void }, X: Matrix, y: Target, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): number[];
    /** Exhaustive parameter search. */
    function gridSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): { best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] };
    /** Random parameter search over nIter sampled points. */
    function randomSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { nIter?: number; k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): { best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] };

    /** Replaces every non-finite entry with its column's finite mean. */
    function imputeMean(X: Matrix, rows?: number, cols?: number): number[][];
    /** Removes rows holding a non-finite value; `kept` lists the survivors. */
    function dropMissing(X: Matrix, y?: Target, rows?: number, cols?: number): { X: number[][]; y: Float64Array | undefined; kept: number[] };
}
/* ================================================================== *
 *  dyna:random
 * ================================================================== */
declare module "dyna:random" {
    /** Which random when: THIS module for reproducible seeded or
     *  fast non-security randomness (xoshiro256**); crypto.RandomBytes for
     *  anything security-adjacent (CSPRNG); the Array.prototype
     *  shuffle/sample extensions for a throwaway shuffle (they draw from a
     *  global RNG, not from any Random instance). */
    /** A seedable xoshiro256** PRNG; a given seed is deterministic and reproducible. */
    class Random {
        /** seed is coerced via ToInt64 (any JS value: "42" gives the 42 stream, "pigs"/null/{}
         *  coerce to 0, 42n gives the same stream as 42); non-coercible values (a Symbol) throw.
         *  Omitted/undefined draws from OS entropy. */
        constructor(seed?: number | bigint);
        /** A full 64-bit draw, always BigInt. */
        nextU64(): bigint;
        /** The top 53 bits as an exact Number in [0, 2^53). */
        nextU53(): number;
        /** A double in [0, 1). */
        nextFloat(): number;
        /** Uniform in [0, bound) by rejection sampling; the result type mirrors the argument.
         *  Unbiased: draws below the 2^64 mod bound threshold are discarded and
         *  redrawn, so there is no modulo bias.
         *  A Number bound must be an integer in [1, 2^53]; a BigInt bound may span the full u64 range. */
        nextBounded(bound: number): number;
        nextBounded(bound: bigint): bigint;
        /** Fills any byte-width typed array with fresh random bytes and returns `this`.
         *  A DataView or bare ArrayBuffer is refused (TypeError). The optional
         *  window is in ELEMENTS (TypedArray convention): offset defaults to 0,
         *  length to the rest of the array; out-of-bounds windows throw RangeError.
         *  A refused call consumes no draws. */
        fill(typedArray: AnyTypedArray, offset?: number, length?: number): this;
        /** n fresh random bytes as a new Uint8Array; n must be an integer in
         *  [0, 2^30] (1 GiB cap, refused before any allocation) and bytes(0)
         *  consumes no draws. */
        bytes(n: number): Uint8Array;
        /** Normal(mu, sigma) by Marsaglia's polar method -- rejection on the unit
         *  disk, no trig, and NO cached second draw, so getState()/setState()
         *  keep their exact 32-byte format and sequences replay losslessly.
         *  mu defaults to 0, sigma to 1; sigma must be finite >= 0
         *  (sigma = 0 is the point mass at mu and consumes no draws). */
        normal(mu?: number, sigma?: number): number;
        /** Exponential(lambda) by inverse CDF (-log(1-u)/lambda); lambda must be
         *  a finite number > 0, default 1. */
        exponential(lambda?: number): number;
        /** Exact Poisson(lambda): Knuth's product method for lambda < 30, an
         *  exact lgamma-based centered inverse-CDF walk (single draw,
         *  O(sqrt(lambda))) for lambda in [30, 2^31-1]. Returns a non-negative
         *  integer; lambda is required and outside [0, 2^31-1] throws RangeError. */
        poisson(lambda: number): number;
        /** In-place Fisher-Yates over the INSTANCE stream (a seeded shuffle the
         *  Array.prototype extensions cannot give); returns `this`. Accepts
         *  arrays and typed arrays; holes move as holes (never densify);
         *  frozen/sealed collections are refused with a TypeError. Exactly
         *  len-1 draws for len >= 2. */
        shuffle(arr: AnyTypedArray | unknown[]): this;
        /** n elements drawn WITHOUT replacement (partial Fisher-Yates over an
         *  index table; the source is never mutated, exactly n draws). n must
         *  be an integer in [0, len]. A typed array yields its own type. */
        sample(arr: AnyTypedArray | unknown[], n: number): AnyTypedArray | unknown[];
        /** One uniform element (a single rejection-sampled index); an empty
         *  array is a RangeError. Array holes read as undefined. */
        choice(arr: AnyTypedArray | unknown[]): unknown;
        /** Advances the state by 2^128 draws WITHOUT generating them -- the
         *  canonical xoshiro256** jump (constants verified in-tree against the
         *  transition matrix). Two same-seed generators separated by a jump
         *  produce disjoint streams. Mutates in place, returns `this`. */
        jump(): this;
        /** longJump: the 2^192-advance variant, for 2^64 independent lanes. */
        longJump(): this;
        /** A 32-byte opaque snapshot of the generator state (four LE u64 words). */
        getState(): Uint8Array;
        /** Restores a state captured by getState(); an all-zero state is the xoshiro fixed point and is refused. */
        setState(state: Uint8Array): void;
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
        /** Compiles the schema once into a native node tree.
         *
         *  Draft support: the compiled tree implements Draft 2020-12
         *  core keywords ONLY (including unevaluatedProperties/
         *  unevaluatedItems and relative $ref pointers). There is no
         *  draft-dispatch code inside, so no {draft} option exists -- draft-07
         *  documents need upgrading by hand (draft-07's exclusiveMinimum as a
         *  boolean, "id", $ref-in-place semantics). */
        compile(schema: unknown): CompiledSchema;
        /** Compiles and caches on the schema object; accepts an already-compiled schema. */
        validate(schema: unknown | CompiledSchema, instance: unknown): SchemaResult;
        /** Registers a custom "format" validator, wiring it into the
         *  "format" keyword of every schema that names it. The registry is
         *  live at validate() time: it affects already-compiled schemas.
         *  Scope is PER RUNTIME (a Worker owns its own registry; the key is
         *  minted per access, so runtimes never share atoms). The validator
         *  receives the instance string and its return value is coerced to
         *  boolean; a throw aborts validate. Assertion applies to STRING
         *  instances only; unregistered names stay annotation-only.
         *  Duplicate names are refused (immutable registry). */
        registerFormat(name: string, fn: (value: string) => unknown): void;
    };
}

/* ================================================================== *
 *  dyna:scrape
 *  Every options bag is STRICT: an unknown key throws a TypeError naming
 *  the key and the valid set (e.g. `unknown option "agnt" (valid: agent)`).
 *  Covers Robots, the Extractor ctor + field specs + run, Fetcher, and
 *  Crawl. A null/primitive bag counts as absent; symbol and inherited
 *  keys are not visible to the check.
 * ================================================================== */
declare module "dyna:scrape" {
    /** robots.txt parsing (RFC 9309). */
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

    /** Field extraction spec: a selector per field. GC-managed: no close surface. */
    class Extractor {
        /** `spec` maps field names to { sel, attr?, all?, required?, trim?,
         *  source?, default?, as? } with as: "number" | "url" | "json";
         *  `source` reads raw child source (script/style/ld+json). */
        constructor(spec: Record<string, unknown>, opts?: { text?: (node: import("dyna:html").HTMLElement) => string });
        /**
         * Runs against a parsed document (the dyna:html HTMLElement tree --
         * HTMLParse's output, shared type with dyna:html);
         * options.base resolves `as: "url"`.
         */
        run(doc: import("dyna:html").HTMLElement | import("dyna:html").HTMLElement[], opts?: { base?: string }): { ok: boolean; value: Record<string, unknown>; missing: string[] };
    }

    /** Polite HTTP retrieval with robots policy, retries and backoff. */
    class Fetcher implements DynResource {
        constructor(opts: {
            agent: string;
            /** Optional: omitted, the module constructs its own
             *  HTTPClient (dyna:net) with this fetcher's maxBodyBytes cap,
             *  so a one-page fetch needs no hand-built client. Pass one to
             *  inject -- a mock client drives the policy in tests. */
            client?: unknown;
            /**: a proxy URL (non-empty string). Validated, stored and
             *  echoed by stats(). The built-in transport opens direct
             *  connections and does not route through it; a transport that
             *  honors a proxy is an injected `client` (which carries its
             *  own transport configuration). */
            proxy?: string;
            /**: a CA bundle path/PEM (non-empty string). Same
             *  contract as `proxy`: validated, stored and echoed by
             *  stats(); the built-in transport uses the platform trust
             *  store. */
            ca?: string;
            /**: client-side connection pool size, integer 1..64
             *  (default 4). Validated, stored and echoed by stats(); the
             *  built-in transport opens one connection per request. */
            poolSize?: number;
            /** Extra request headers; User-Agent always stays the crawler's own.
             *  Credential keys (Authorization/Cookie/Proxy-Authorization/Set-Cookie)
             *  are refused -- they would leak to redirect targets. */
            headers?: Record<string, string>;
            /** Conditional GETs against stored validators. Default true. */
            revalidate?: boolean;
            /** robots.txt refresh interval in ms; 0 re-fetches per request.
             *  A failed refresh keeps serving the last-known-good copy.
             *  Default 24h. */
            robotsTtlMs?: number;
            /** Follow a redirect that moves https to http. Default false. */
            allowInsecureDowngrade?: boolean;
            robots?: boolean;
            minDelayMs?: number;
            retries?: number;
            maxRedirects?: number;
            maxBodyBytes?: number;
            allowPrivateHosts?: boolean;
        });
        /** GET a URL; returns the HTTP response with url/fromCache/notModified added. */
        get(url: string): FetcherResponse;
        /**: `get` for one-page fetches without constructing a Crawl,
         *  settled as a Promise: the same policy pass (robots gate, delay
         *  floor, retries, redirects, caps) runs, a transport throw becomes
         *  a REJECTION, never a sync throw, and a policy skip (robots)
         *  resolves with the status-0 response `get` returns. */
        getAsync(url: string): Promise<FetcherResponse>;
        stats(): {
            fetched: number; skippedByRobots: number; retried: number;
            throttledMs: number; bytes: number; revalidated: number; savedBytes: number;
            /**: the stored simple options, echoed (null when unset;
             *  poolSize defaults to 4). Never silently ignored. */
            proxy: string | null; ca: string | null; poolSize: number;
        };
        /** + CC-2-scrape: fetch `url` under the same policy as get
         *  (robots gate, per-host delay floor, retries with Retry-After and
         *  jittered backoff, redirect chase with the private-host and
         *  https-downgrade gates, maxBodyBytes, the SSRF name gate) and
         *  return the body as a streaming FetcherStream instead of a
         *  string. The transport is this module's own (the injected
         *  client's request() buffers by contract, so a stream through it
         *  could only be faked): plain http, https under CONFIG_TLS with
         *  the platform trust store, `Accept-Encoding: identity`, fixed
         *  15s connect/recv timeouts. NOT carried over from get():
         *  revalidate/conditional-GET and the response decorations
         *  (X-Robots-Tag, Link canonical) -- a stream is consumed, not
         *  parsed for directives. */
        getStream(url: string): FetcherStream;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A fetched page. On revalidation status stays 304 with the cached body
     *  and the STORED content-type replays with it. */
    interface FetcherResponse {
        status: number;
        headers: Record<string, string>;
        contentType: string;
        body: string;
        url: string;
        fromCache: boolean;
        notModified: boolean;
        /** X-Robots-Tag directives honored for this fetcher's agent
         *  (noindex/nofollow/none expanded, bot-scoped rules filtered). */
        robotsDirectives?: string[];
        /** Link: <url>; rel="canonical" (RFC 8288), resolved against the
         *  request url, when the response names one. */
        canonicalUrl?: string;
    }

    /** The getStream response: the HTTP head readable off the SAME object
     *  that streams the body (+ CC-2-scrape). `read`/`close` are
     *  exactly dyna:stream's ByteSource shape -- any object with a callable
     *  `read` is a ByteSource there, so pipe(), lines() and ndjson()
     *  consume this object directly; dyna:scrape neither imports nor
     *  depends on dyna:stream. */
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
        /** Fills the caller's buffer with up to buf.length body bytes and
         *  resolves with the count; 0 = end of body. Rejects on truncation
         *  (connection ended before a declared Content-Length), on
         *  maxBodyBytes overflow, and on malformed chunked framing -- the
         *  stream is then closed and every later read rejects the same
         *  error. */
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
        /** The Extractor's extracted fields for this page (Extractor.run's
         *  `.value` record). */
        value: Record<string, unknown>;
        /** Merged X-Robots-Tag + meta-robots directive list, when either exists. */
        robots?: string[];
    }

    /** Bounded traversal over a Fetcher plus an Extractor. Relative hrefs in
     *  linkField resolve against the page that emitted them. */
    class Crawl implements DynResource {
        constructor(fetcher: Fetcher, opts?: {
            maxPages?: number; maxDepth?: number; sameHost?: boolean;
            linkField?: string; baseField?: string;
            /** rel=canonical dedup: duplicate pages emit but queue no links. */
            canonicalField?: string;
            /** Parallel array of link rel strings; nofollow slots are skipped. */
            relField?: string;
            /** Extractor field naming the page's <meta name="robots"
             *  content="...">: its nofollow gates link following, and the
             *  directives join page.robots. */
            robotsField?: string;
            /** pages fetched in parallel (1..16, default 1). At 1 the
             *  crawl drives one page per next() like a sync iterator; above 1
             *  it keeps that many fetches in flight and next() returns a
             *  Promise<IteratorResult>, so drain it with for await...of. The
             *  per-host politeness floor is unchanged either way. */
            concurrency?: number;
        });
        /** Seeds the crawl with an http(s) url; the iterator yields pages.
         *  `parse` is the document parser, normally dyna:html's HTMLParse
         *  (the shared HTMLElement tree). */
        start(seed: string, extractor?: Extractor, parse?: (html: string) => import("dyna:html").HTMLElement[]): this;
        /** One page per call: {value, done} at concurrency 1, a
         *  Promise<IteratorResult> above it. */
        next(): IteratorResult<CrawlPage> | Promise<IteratorResult<CrawlPage>>;
        /** The crawl's page stream: returns the crawl itself, which is the
         *  [Symbol.iterator] -- one lazily fetched page per next(). Pages
         *  are NOT collected eagerly; drain it with for...of. */
        pages(): IterableIterator<CrawlPage>;
        [Symbol.iterator](): Iterator<CrawlPage>;
        /** Installed on every crawl: for await...of awaits each next() result
         *  through the real async protocol, which is what a concurrent
         *  (concurrency > 1) crawl's promises flow through. */
        [Symbol.asyncIterator](): AsyncIterator<CrawlPage>;
        /**: the crawl's resumable state as a versioned JSON string:
         *  frontier (remaining urls with depths, in order), visited dedup
         *  keys, bounds (page/depth limits and concurrency), counters and
         *  field names. Throws TypeError on a
         *  crawl that was never started. NOT part of the state: in-flight
         *  fetches (a page mid-fetch is dropped on resume), the fetcher's
         *  own robots/delay/validator caches, and every JS object (the
         *  fetcher/extractor/parser are arguments of resume, not data). */
        serialize(): string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
        /**: rebuild a crawl from serialize output. `fetcher`,
         *  `extractor` and `parse` are supplied fresh (they are live JS
         *  objects, deliberately not serialized); bounds, field names,
         *  frontier, visited set and counters come from the state, so the
         *  remaining page set -- and its order -- matches the donor's.
         *  Throws TypeError on any malformed or wrong-version state; a
         *  state whose page budget is exhausted resumes as a finished
         *  crawl. */
        static resume(fetcher: Fetcher, state: string, extractor?: Extractor,
                      parse?: (html: string) => import("dyna:html").HTMLElement[]): Crawl;
    }

    /** Sitemap reader: the URLs Robots.sitemaps hands out, parsed.
     *  A namespace, not a class -- parse/list are stateless. */
    namespace Sitemap {
        /** Parse sitemap XML into the <loc> list, in document order. A
         *  <sitemapindex> parses to its child sitemap URLs. Comments, PIs,
         *  DOCTYPE, CDATA, attributes and the default namespace are
         *  skipped; the five named entities and numeric refs decode inside
         *  <loc>; element names match case-insensitively. Structural
         *  breakage throws SyntaxError with the byte offset; non-XML input
         *  throws TypeError; an empty urlset is an empty list. Bounds:
         *  16 MiB, 50000 entries, 2048-byte <loc> text (the spec's url
         *  budget). */
        function parse(xml: string): string[];
        /** Fetch + parse + flatten. A <sitemapindex> is followed ONE level:
         *  each child is fetched and its list appended (a child that is
         *  itself an index has its entries returned as-is). `source` is
         *  any object with get(url) -- a Fetcher, or a mock -- or an
         *  options bag for building one (the Fetcher ctor's keys); omit it
         *  for the module's own fetcher, agent "dyna-sitemap/1.0".
         *  Non-2xx answers throw RangeError; duplicates are preserved. */
        function list(url: string, source?: Fetcher | {
            agent?: string; headers?: Record<string, string>;
            minDelayMs?: number; allowPrivateHosts?: boolean;
            maxBodyBytes?: number; retries?: number;
        }): string[];
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
    /** Ascending sort by precedence; does not mutate the input. */
    function sort(versions: string[]): string[];
    function major(v: string): number;
    function minor(v: string): number;
    function patch(v: string): number;
    function prerelease(v: string): (string | number)[] | null;
    /** Bumps the version; release major|minor|patch|premajor|preminor|prepatch|prerelease|release.
     *  'release' strips the prerelease; it returns null when the input has none (node-semver pins null). */
    function inc(version: string, release: string, identifier?: string): string | null;
    /** One-shot range match; a range string is parsed per call. */
    function satisfies(version: string, range: string): boolean;
    function maxSatisfying(versions: string[], range: string): string | null;
    function minSatisfying(versions: string[], range: string): string | null;
    /** The highest-precedence component that differs:
     *  "major" | "minor" | "patch" | "prerelease" | "build", or null when
     *  identical. Node-semver's documented contract; unlike node 7.8.5's
     *  implementation we never return pre-prefixed names, and a build-only
     *  difference is reported as "build" (node's eq() cannot see build). */
    function diff(a: string, b: string): "major" | "minor" | "patch" | "prerelease" | "build" | null;
    /** compare(), then, on a precedence tie, the build identifiers decide
     *  (absent build sorts below present; numeric ids compare numerically,
     *  numeric < alphanumeric, else lexically). */
    function compareBuild(a: string, b: string): -1 | 0 | 1;
    /** Whether the two ranges intersect -- node-semver's
     *  comparator-pairwise rule, including its ">=0.0.0"-as-ANY
     *  normalization. Each argument is a range string or a Range. */
    function intersects(rangeA: string | Range, rangeB: string | Range): boolean;

    /** A compiled range expression. */
    class Range {
        constructor(rangeString: string);
        /** Whether the version matches the compiled range. */
        test(version: string): boolean;
        /** The versions that match, preserving input order. */
        filter(versions: string[]): string[];
        maxSatisfying(versions: string[]): string | null;
        minSatisfying(versions: string[]): string | null;
        /** Whether this range intersects `other` (a range string or Range):
         *  node-semver's comparator-pairwise rule, including its
         *  ">=0.0.0"-as-ANY normalization. */
        intersects(other: string | Range): boolean;
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
        /** Any integer Number or BigInt: beyond int64 the value takes
         *  the exact decimal path; minimal two's complement per X.690 8.3.2,
         *  64-content-byte cap both ways. Decode returns Number up to 2^53,
         *  an exact BigInt beyond. */
        int(value: number | bigint): ASN1Node;
        bool(value: boolean): ASN1Node;
        null(): ASN1Node;
        octets(bytes: ByteView): ASN1Node;
        bitString(bytes: ByteView, unused: number): ASN1Node;
        oid(str: string): ASN1Node;
        utf8(str: string): ASN1Node;
        /** IA5String (tag 22): ASCII only; non-ASCII refuses at encode. */
        ia5String(str: string): ASN1Node;
        /** BMPString (tag 30): UCS-2 big-endian; code points above U+FFFF and
         *  lone surrogates refuse at encode. */
        bmpString(str: string): ASN1Node;
        /** UniversalString (tag 28): UCS-4 big-endian; lone surrogates refuse
         *  at encode. */
        universalString(str: string): ASN1Node;
        printable(str: string): ASN1Node;
        utcTime(str: string): ASN1Node;
        generalizedTime(str: string): ASN1Node;
        context(tag: number, content: ByteView): ASN1Node;
        contextC(tag: number, children: ASN1Node[]): ASN1Node;
    };

    /** MessagePack encoding; refuses symbols and functions.
     *  Untrusted surface: MsgPackDecode and CBORDecode are hardened
     *  for hostile input -- nesting is depth-capped and lengths are
     *  validated against the input bounds before allocation, so a malformed
     *  or hostile payload refuses instead of exhausting memory. */
    function MsgPackEncode(value: unknown): Uint8Array;
    function MsgPackDecode(bytes: ByteView): unknown;
    /** RFC 8949 CBOR with the same walker, bounds and refusals (see the
     *  MsgPackDecode untrusted-surface note). */
    function CBOREncode(value: unknown): Uint8Array;
    function CBORDecode(bytes: ByteView): unknown;
    /** CBOR with map keys sorted byte-wise (deterministic form). */
    function CBORCanonical(value: unknown): Uint8Array;
    /** Canonical CBOR run through XXH64, returned as 16 lowercase hex chars. */
    function ValueHash(value: unknown): string;
    /** Deep clone preserving cycles, shared references, Date, RegExp, Map,
     *  Set, ArrayBuffers and typed arrays; functions and accessor properties
     *  refused. */
    function structuredClone(value: unknown): unknown;
}

/* ================================================================== *
 *  dyna:simd
 * ================================================================== */
declare module "dyna:simd" {
    /** Width prefix rule: plain names run the Float32Array kernels
     *  (the `f32` default width; any 4-byte typed array is accepted as a
     *  BIT-CAST reinterpret -- see the F32Like note). `f64*` names run
     *  Float64Array; `i32*` names run Int32Array and are STRICT: the element
     *  type is verified by class, a same-stride Float32Array/Uint32Array is
     *  rejected, never reinterpreted. Reductions (sum/dot/norm/mean/variance
     *  ...) accumulate in vector lanes and may fuse multiply-adds, so they
     *  match a sequential scalar loop to a RELATIVE tolerance, not bitwise;
     *  every n >= 1 is safe (the kernels guard their seed loads) and NaN in
     *  the input propagates to NaN through every reduction and in-place
     *  transform. */
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
    /** gemv with the 6-arg legacy form: y = beta*y + A*x. */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** gemv with the 7-arg form (gemm parity): y = alpha*A*x + beta*y;
     *  beta == 0 skips the y read. Dispatched on the argument count. */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, alpha: number, beta: number): F32Like;
    function gemvT(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    function gemm(c: F32Like, a: F32Like, b: F32Like, m: number, n: number, k: number, alpha: number, beta: number): F32Like;

    /** Statistics: reductions composed from the sum/dot kernels, so
     *  they inherit the relative-tolerance regime; NaN propagates. Empty
     *  input throws RangeError (mean/variance are undefined there). */
    /** (offset,length) windows: every vector kernel below (all except gemv/gemvT/gemm/
     *  topkIndices, whose range is owned by dims/k) accepts a trailing
     *  (offset, length) window or {offset, length|limit} bag (limit aliases
     *  length, never both): the same slice applies to every array arg without
     *  a subarray() copy. Out of range is a RangeError. Examples:
     *  sum(a, 8, 16), sum(a, {offset: 8, length: 16}). */
    /** Arithmetic mean = sum(a)/n. */
    function mean(a: F32Like, offset?: number, length?: number): number;
    function mean(a: F32Like, opts?: { offset?: number; length?: number; limit?: number }): number;
    /** Two-pass centered population variance: mean, then dot(a-mean, a-mean)/n. */
    function variance(a: F32Like): number;
    /** In-place z-score a[i] = (a[i]-mean)/std; returns a (joins softmax in
     *  the in-place single-array family). A constant array has no meaningful
     *  z-score: the output is all-NaN when the centering cancels exactly,
     *  else ±1-shaped rounding noise. */
    function normalize(a: F32Like): F32Like;
    /** Arithmetic mean over Float64Array = f64Sum(a)/n. */
    function f64Mean(a: Float64Array): number;
    /** Two-pass centered population variance over Float64Array. */
    function f64Variance(a: Float64Array): number;
    /** Arithmetic mean over Int32Array: exact int64 accumulation inside the
     *  kernel (cannot overflow for any array that fits memory), exact as a
     *  double while |sum| <= 2^53, one double rounding for the divide. */
    function i32Mean(a: Int32Array): number;

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

    /** Inclusive prefix scan, in place (the output type follows the input). */
    function cumsum(a: Int32Array): Int32Array;
    function cumsum(a: Float32Array): Float32Array;
    function cummax(a: Int32Array): Int32Array;
    function cummax(a: Float32Array): Float32Array;
}

/* ================================================================== *
 *  dyna:stream
 * ================================================================== */
declare module "dyna:stream" {
    /**
     * A pull-based byte producer (plan; deliberately NOT WHATWG
     * ReadableStream). `read` fills UP TO `buf.length` bytes into the
     * CALLER's buffer and resolves with the count actually read; `0` means
     * EOF. `close`/`dispose`/`[Symbol.dispose]`/`closed` come from the
     * shared native-resource surface, so `using` works.
     *
     * ANY object with a compatible `read` is a ByteSource: `pipe`, `lines`,
     * `ndjson` and the codec wrappers are duck-typed, which is how other
     * modules' bodies (fetch responses, spawn output) plug in without this
     * module knowing them.
     */
    interface ByteSource {
        /** Fills up to buf.length bytes; resolves with bytes read, 0 = EOF. */
        read(buf: Uint8Array): Promise<number>;
        /** Deterministic release. A source that is closed before the first
         *  `read` never touches the disk (file sources open lazily). */
        close(): void;
        /** True once released. */
        readonly closed: boolean;
        [Symbol.dispose](): void;
    }

    /**
     * A pull-based byte consumer. Buffered implementations accept the whole
     * view and resolve with the count accepted (a duck-typed sink may accept
     * fewer; `pipe` loops over the remainder).
     */
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
        /** Called after each accepted chunk with the chunk's byte count.
         *  A throw rejects the pipe and closes both sides. */
        onChunk?: (bytes: number) => void;
    }

    /** Options for `toFile`. */
    interface ToFileOptions {
        /** Append rather than truncate. */
        append?: boolean;
        /** Sink buffer size, clamped to 4 KiB..64 MiB (default 128 KiB). */
        bufferSize?: number;
    }

    /**
     * Pipes `src` into `dst`: repeated `read` into a private 128 KiB chunk
     * and `write` of the filled prefix (short writes are retried), until
     * EOF. Resolves with the TOTAL bytes transferred. On failure the promise
     * rejects with the failure AND both sides are closed (best effort; their
     * close errors are swallowed). On success neither side is closed -- the
     * caller owns both. Note `toFile` sinks buffer: close (or flush) the
     * sink after the pipe to persist the tail.
     */
    function pipe(src: ByteSource, dst: ByteSink, opts?: PipeOptions): Promise<number>;

    /**
     * A ByteSource over an in-memory copy of `b` (a string is encoded
     * UTF-8). The bytes are copied, so later mutation of `b` is invisible.
     */
    function fromBytes(b: Uint8Array | string): ByteSource;

    /**
     * A ByteSource over a file, opened LAZILY at the first `read` -- a bad
     * path REJECTS there rather than throwing here; a source that is only
     * closed never touches the disk. Sequential: read(2) semantics, so
     * non-seekable files (FIFOs) stream too.
     */
    function fromFile(path: import("dyna:file").Path | string): ByteSource;

    /**
     * A buffered ByteSink over a file, created LAZILY at the first `write`
     * (truncated, or appended with `{append: true}`). A bad path rejects the
     * write. Buffered: `write` resolves once bytes are accepted into the
     * buffer -- fd errors surface on the write that triggers the flush or on
     * `flush()`; `close()` flushes best-effort but cannot report. A failed
     * write makes the sink sticky: later `write`/`flush` reject with the
     * same errno.
     */
    function toFile(path: import("dyna:file").Path | string, opts?: ToFileOptions): ByteSink;

    /** Options for `lines`. */
    interface LinesOptions {
        /** Only "utf-8" (the default) is supported: it is the one encoding
         *  whose chunk-boundary state carry this splitter can promise
         *  (continuation bytes are >= 0x80, so a partial sequence can never
         *  contain the 0x0a split byte). Anything else is refused. */
        encoding?: string;
    }

    /**
     * Splits a ByteSource into lines as an async iterable of strings
     * (`for await (const line of lines(src))`). Chunk-safe by construction:
     * a line split across ANY read boundary -- including a source yielding
     * one byte at a time -- is carried intact; a partial multi-byte UTF-8
     * sequence waits at the buffer end for its continuation. `"\r\n"` and
     * `"\r"`-at-EOL are stripped; empty lines are preserved; a final line
     * without a trailing newline is still delivered. Invalid UTF-8 decodes
     * with U+FFFD replacement (FileReader.readLine parity). Lines longer
     * than DYN_MAX_INPUT (1 GiB) are refused with a RangeError.
     *
     * `break`ing a for-await loop calls `return()`, which closes the source.
     */
    function lines(src: ByteSource, opts?: LinesOptions): AsyncIterableIterator<string>;

    /**
     * Newline-delimited JSON over a ByteSource as an async iterable of the
     * parsed values. Blank/whitespace-only lines are skipped; a malformed
     * line rejects with a SyntaxError naming the 1-based LINE NUMBER
     * (`ndjson: line 7: ...`); the iterator stays usable -- the next
     * `next()` resumes after the bad line. A final line without a trailing
     * newline is still parsed.
     */
    function ndjson(src: ByteSource): AsyncIterableIterator<unknown>;

    /** Streaming codecs accepted by `inflate`/`deflate`. */
    type StreamCodec = "gzip" | "zstd" | "brotli" | "lz4" | "deflate";

    /**
     * A compressing ByteSink: `write` accepts RAW bytes, compressed bytes
     * flow into the wrapped sink. Buffered codecs emit on their own
     * schedule, so `write` resolves once the raw bytes are accepted, not
     * when bytes reach the wrapped sink.
     *
     * TEARDOWN: the stream tail needs an async write, so always end a
     * compressing sink with `await sink.finish()` -- it finalizes the
     * stream, flushes, closes the wrapped sink, and resolves with the
     * total RAW bytes compressed. The inherited close()/dispose() (and
     * `using`) only release the wrapper: an unfinished stream's tail is
     * lost (truncated output), so do not `using` a compressing sink.
     */
    interface DeflateByteSink extends ByteSink {
        /** Finalizes the stream, pushes the tail, flushes and closes the
         *  wrapped sink; resolves with the total raw bytes compressed. */
        finish(): Promise<number>;
    }

    /** Options for `deflate`. `level` semantics per codec: zstd 1..22
     *  (default 3, clamped), lz4 1..12 (default 1), gzip/deflate/brotli
     *  fixed-quality (accepted and ignored). */
    interface DeflateOptions {
        codec: StreamCodec;
        level?: number;
    }

    /** Options for `inflate`. */
    interface InflateOptions {
        codec: StreamCodec;
    }

    /**
     * Decompressing ByteSource over `src`: `read(buf)` fills the caller's
     * buffer with decompressed bytes; 0 = end of stream (the framing
     * trailers/checksums are verified before EOF is reported). Closing the
     * wrapper releases it; close the underlying source separately if you
     * own it.
     *
     * Platform honesty: zstd needs libzstd (CONFIG_ZSTD) and brotli needs
     * libcompression or libbrotli; where a codec's library is absent the
     * factory throws a named "not compiled in" error. gzip/deflate use
     * libcompression's raw-deflate streaming and are macOS-only in this
     * build. lz4 (hand-rolled frame around the in-repo LZ4 block codec)
     * works everywhere and cross-decodes with dyna:compress lz4Frame.
     */
    function inflate(src: ByteSource, opts: InflateOptions): ByteSource;

    /**
     * A compressing ByteSink over `sink`: compressed output flows into
     * `sink` as you write raw bytes. See DeflateByteSink for the finish()
     * teardown contract.
     */
    function deflate(sink: ByteSink, opts: DeflateOptions): DeflateByteSink;
}


/* ================================================================== *
 *  dyna:structures
 * ================================================================== */
declare module "dyna:structures" {
    /** Adjacency-list graph over integer node ids. */
    class Graph {
        /** The options bag is strict -- an unknown key throws a
         *  TypeError naming the key and the valid set. */
        constructor(opts?: { directed?: boolean; weighted?: boolean });
        static deserialize(bytes: Uint8Array | ArrayBuffer): Graph;
        addNode(): number;
        /** Adds a directed (or both-direction) edge; nodes grow on demand. */
        addEdge(u: number, v: number, w?: number): this;
        /** Targets of u's outgoing edges. */
        neighbors(u: number): number[];
        /**: writes u's out-neighbors into `out` and returns the
         *  degree. Never allocates. RangeError when `out` is shorter than the
         *  degree (nothing is written); extra capacity is left untouched. */
        neighborsInto(u: number, out: Int32Array): number;
        /**: the adjacency as CSR, node-ordered:
         *  edges[offsets[u] .. offsets[u+1]] === neighbors(u). Copies once
         *  into two fresh Int32Arrays; offsets has n+1 entries. */
        exportCSR(): { offsets: Int32Array; edges: Int32Array };
        hasEdge(u: number, v: number): boolean;
        readonly nodeCount: number;
        readonly edgeCount: number;
        bfs(src: number): number[];
        dfs(src: number): number[];
        /** visitor form: calls visit(node) in bfs order and stops the
         *  walk the moment it returns true (a falsy or undefined return keeps
         *  going). Returns the number of nodes visited, counting the one visit
         *  returned true for. Allocates nothing per node; a visit() throw
         *  unwinds the traversal and propagates. */
        bfs(src: number, opts: { visit: (node: number) => boolean | void }): number;
        /** visitor form of dfs: same contract as the bfs overload,
         *  visiting in dfs order (smaller ids first). */
        dfs(src: number, opts: { visit: (node: number) => boolean | void }): number;
        /** Shortest distances; a single distance to dst when given. */
        dijkstra(src: number): number[];
        dijkstra(src: number, dst: number): number;
        bellmanFord(src: number): number[];
        topologicalSort(): number[];
        /** visitor form: visits in topological order, early-exits on a
         *  true return, returns the visited count. A cycle reached before the
         *  walk stops still throws RangeError. */
        topologicalSort(opts: { visit: (node: number) => boolean | void }): number;
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
        /** The options bag is strict -- an unknown key throws a
         *  TypeError naming the key and the valid set. */
        constructor(capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: V) => void });
        /**: T flows through -- `LRU.deserialize<MyT>(bytes)` or an
         *  assignment annotated `LRU<MyT>` needs no cast. */
        static deserialize<T>(this: new (capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: T) => void }) => LRU<T>, bytes: Uint8Array | ArrayBuffer): LRU<T>;
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
        /**: T flows from the comparator, the `this` class, or an explicit
         *  type argument -- no cast at the persistence boundary. */
        static deserialize<T>(bytes: Uint8Array | ArrayBuffer, cmp: (a: T, b: T) => number): Heap<T>;
        static deserialize<T>(this: new (comparator?: (a: T, b: T) => number) => Heap<T>, bytes: Uint8Array | ArrayBuffer): Heap<T>;
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
        static deserialize<T>(this: new () => MinMaxHeap<T>, bytes: Uint8Array | ArrayBuffer): MinMaxHeap<T>;
        push(priority: number, value?: V): this;
        popMin(): V | undefined;
        popMax(): V | undefined;
        peekMin(): V | undefined;
        peekMax(): V | undefined;
        readonly size: number;
        serialize(): Uint8Array;
    }

    /** Set of numbers in sorted order (B+tree-backed, same core as BTree). */
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

    /** Map from numeric key to JS value, keys sorted (B+tree-backed, same core as BTree).
     *  BTree vs SortedMap (runtime-verified): same method surface, but
     *  their serialize() formats are DISTINCT -- SortedMap.deserialize refuses
     *  a BTree record and vice versa, so the two are NOT interchangeable at
     *  the persistence boundary. SortedMap has no [Symbol.iterator]; BTree
     *  does. Pick one per codebase and stay with it. */
    class SortedMap<V = unknown> {
        constructor();
        static deserialize<T>(this: new () => SortedMap<T>, bytes: Uint8Array | ArrayBuffer): SortedMap<T>;
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

    /** Ordered map on numeric keys as a B-tree of order 32.
     *  BTree vs SortedMap: distinct serialize formats (cross-deserialize
     *  refuses); BTree is the iterable one ([Symbol.iterator] over [key, value]). */
    class BTree<V = unknown> {
        constructor();
        static deserialize<T>(this: new () => BTree<T>, bytes: Uint8Array | ArrayBuffer): BTree<T>;
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
        static deserialize<T>(this: new () => Deque<T>, bytes: Uint8Array | ArrayBuffer): Deque<T>;
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
        static deserialize<T>(this: new () => List<T>, bytes: Uint8Array | ArrayBuffer): List<T>;
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
        static deserialize<T>(this: new (capacity: number) => RingBuffer<T>, bytes: Uint8Array | ArrayBuffer): RingBuffer<T>;
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
        static deserialize<T>(this: new () => Multimap<T>, bytes: Uint8Array | ArrayBuffer): Multimap<T>;
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
        static deserialize<T>(this: new () => Table<T>, bytes: Uint8Array | ArrayBuffer): Table<T>;
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
        static deserialize<T>(this: new () => RangeMap<T>, bytes: Uint8Array | ArrayBuffer): RangeMap<T>;
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
        static deserialize<T>(this: new () => IntervalTree<T>, bytes: Uint8Array | ArrayBuffer): IntervalTree<T>;
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
    /** A NUL-bearing name is refused (no environment name can contain one). */
    function getEnv(name: string): string | undefined;
    /** Overwrites an entry; refuses empty names, `=` in a name, or NUL anywhere. */
    function setEnv(name: string, value: string): void;
    /** The process argument vector (argv[0] first). */
    function args(): string[];
    function cwd(): string;
    /** A NUL-bearing path is refused rather than chdir'd by its prefix. */
    function chDir(path: string): void;
    /** "darwin", "linux", or the uname(2) sysname lowercased for the other
     * platforms a build can run on ("freebsd", "openbsd", "netbsd",
     * "dragonfly", "sunos"). "unknown" only when uname() itself fails. */
    function platform(): string;
    /** The machine hardware name from uname(2), with aliases normalised:
     * aarch64 -> "arm64", amd64 -> "x86_64"; other machine strings
     * (i386/i686/riscv64/...) pass through lowercased. */
    function arch(): string;
    /** The five standard utsname fields: {sysname, nodename, release,
     * version, machine} -- all strings; nodename matches hostName(). */
    function uname(): { sysname: string; nodename: string; release: string; version: string; machine: string };
    /** The real user id. */
    function getuid(): number;
    /** The real group id. */
    function getgid(): number;
    /** setuid(2). Takes effect only with privilege; without it the kernel
     * refuses and this throws a clean Error with code "EPERM" (errno 1).
     * id must be a non-negative integer (TypeError/RangeError otherwise). */
    function setUid(id: number): void;
    /** setgid(2). Same discipline as setUid: clean {code: "EPERM"} Error
     * when the kernel refuses an unprivileged change. */
    function setGid(id: number): void;
    /** CPU time consumed by THIS process: {user, system} in fractional
     * seconds, from getrusage(RUSAGE_SELF). Monotonic non-decreasing. */
    function cpuUsage(): { user: number; system: number };
    /** The full getrusage(RUSAGE_SELF) struct as a flat object. user/system
     * are fractional seconds (exactly cpuUsage()'s values); maxrss is
     * normalised to BYTES (memoryUsage().peakRss is the same number).
     * idrss/isrss are historical (Linux reports 0); counter semantics for
     * minflt..nivcsw are kernel-specific -- treat as advisory. */
    function rusage(): { user: number; system: number; maxrss: number; idrss: number; isrss: number; minflt: number; majflt: number; nswap: number; inblock: number; oublock: number; msgsnd: number; msgrcv: number; nsigs: number; nvcsw: number; nivcsw: number };
    function pid(): number;
    function hostName(): string;
    function homeDir(): string;
    /** {model, cores?, threads, mhz?, features} of the selected SIMD dispatch. */
    function cpuInfo(): { model: string; cores?: number; threads: number; mhz?: number; features: string[] };
    function memInfo(): { total: number; free: number; available: number };
    /** [1, 5, 15]-minute load averages. */
    function loadAvg(): [number, number, number];
    function uptime(): number;
    /** Volume statistics for the filesystem CONTAINING path (a file answers
     * with its volume, not its own size); refuses NUL-bearing paths. */
    function diskUsage(path: string): { total: number; free: number; available: number };
    /** The engine's live allocation counters plus OS peakRss, plus the
     * module-native ledger: nativeSize is libc memory dyna:* modules hold
     * outside the JS heap (resource boxes + counted-allocator payloads;
     * unconverted modules' payload buffers are NOT counted), nativeLimit is
     * the cap setNativeMemoryLimit() installed, 0 = uncapped. The remaining
     * *Count/*Size fields are the engine's full breakdown (strings, props,
     * shapes, arrays). */
    function memoryUsage(): { mallocCount: number; mallocSize: number; memoryUsedCount: number; memoryUsedSize: number; objCount: number; objSize: number; strCount: number; strSize: number; propCount: number; shapeCount: number; arrayCount: number; peakRss: number; nativeSize: number; nativeLimit: number };
    /** Cap module-native memory at `bytes` (JS_SetMemoryLimit for the libc
     * half). 0, the default, is uncapped. Allocation past the cap is refused:
     * the call throws out-of-memory instead of the process growing. Never
     * frees anything. `bytes` must be a finite number -- a NaN/Infinity would
     * coerce to 0 and silently disarm the cap. */
    function setNativeMemoryLimit(bytes: number): void;

    /** Run a program with argv, no shell anywhere. Names, args, env and cwd
     *  refuse NUL-bearing strings rather than act on their truncated prefix.
     *
     *  sys.Exec vs os.exec: sys.Exec is CANONICAL -- it returns a
     *  result object {code, signal, stdout, stderr, timedOut} and throws on
     *  failure to spawn. os.exec (the --std QuickJS compat module) is the
     *  pid/errno-shaped legacy door with different conventions; prefer
     *  sys.Exec in new code. */
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
        /** Drop the child's credentials after the exec-side chdir, gid before
         * uid; both must be non-negative integers fitting uid_t/gid_t. */
        uid?: number;
        gid?: number;
    }
    function Exec(command: string, args?: string[], options?: ExecOptions): ExecResult;
    /** Resolve a program name against PATH; null when not found. A
     * NUL-bearing name is refused rather than resolved by its prefix. */
    function Which(name: string): string | null;

    /* ---- Spawn: the async child with streaming stdio ------------ */

    /** One output side (stdout or stderr): a dyna:stream-compatible
     *  ByteSource (duck-typed `read(buf) -> Promise<number>`; fills UP TO
     *  buf.length, resolves the count, 0 = EOF). Drained continuously into
     *  an internal buffer bounded by `maxPipe`; when the buffer is full the
     *  watch comes off and the child BLOCKS on the kernel pipe until a
     *  read() drains room -- true backpressure, nobody is killed. One
     *  read() at a time per pipe; a second concurrent read throws. */
    interface SpawnPipe {
        read(buf: Uint8Array): Promise<number>;
        /** Stop draining. A pending read() is rejected, not left hanging. */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }

    /** stdin as a ByteSink-shaped write side, available only when the
     *  child's stdin is a pipe (opts.stdin: "pipe", or opts.input). write()
     *  ACCEPTS the whole view into a queue and resolves with its length
     *  (the drain happens on WRITE readiness); flush() resolves when the
     *  queue has reached the child; close() sends EOF after the queue
     *  drains. A write that hits EPIPE (reader gone) rejects flush() and
     *  makes later writes reject: bytes that can never be written are not
     *  silently accepted. */
    interface SpawnStdin {
        write(buf: BytesInput): Promise<number>;
        flush(): Promise<void>;
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }

    interface SpawnOptions {
        cwd?: string;
        env?: Record<string, string>;
        /** Written to the child's stdin, then EOF (implies stdin: "pipe"). */
        input?: BytesInput;
        /** The child's stdin: "ignore" (the default -- /dev/null, so a
         *  child that reads stdin gets EOF and never hangs), "pipe"
         *  (p.stdin becomes writable), or "inherit". */
        stdin?: "pipe" | "ignore" | "inherit";
        /** SIGTERM at the deadline, SIGKILL after the 2 s Exec grace;
         *  wait() reports timedOut: true. */
        timeoutMs?: number;
        /** The per-pipe drain-ahead bound where backpressure kicks in
         *  (default 8 MiB, the Exec maxBuffer default). */
        maxPipe?: number;
        /** Same discipline as Exec's uid/gid. */
        uid?: number;
        gid?: number;
        /** false skips empty PATH elements (the execvp cwd search). */
        allowPathCwd?: boolean;
    }

    interface SpawnResult {
        /** Exit code, or null when a signal killed the child (Exec's rule). */
        code: number | null;
        /** "SIGTERM", "SIGKILL", ... or null for a normal exit. */
        signal: string | null;
        /** True when opts.timeoutMs fired. */
        timedOut: boolean;
    }

    /** The async child process: argv only, no shell -- the same
     *  execve discipline as Exec (the PARENT resolves PATH; command, args,
     *  env and cwd refuse NUL-bearing strings rather than act on their
     *  truncated prefix), but NOTHING blocks the JS thread: stdout/stderr
     *  are watched on the shared async reactor, and the child's exit is
     *  reaped from the engine's event loop, so a script that spawns and
     *  never awaits still runs the child to completion before exiting.
     *
     *  Requires `new` (a class, like the other C constructors here).
     *
     *  Lifetime: the object, each stdio view, and each operation whose state
     *  is parked in C (a pending read, flush or wait) hold a reference. A
     *  view pulled into a local, an awaited read and a pending wait()
     *  therefore all outlive a dropped Spawn and settle normally -- a parked
     *  wait() still resolves when the child exits. The child GROUP is
     *  SIGKILLed and reaped, and everything dropped, when close()/dispose()
     *  runs or when the last reference goes away (an abandoned Spawn's
     *  finalizer is one way to reach that); close() additionally settles a
     *  parked wait() with the SIGKILL it delivered, promptly. No zombies. */
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
        /** Promise<{code, signal, timedOut}>; settles within about one
         *  event-loop tick of the child's death. One wait() at a time;
         *  a second concurrent wait() throws; after exit the result is
         *  replayed. */
        wait(): Promise<SpawnResult>;
        /** Signal the child GROUP (the pid in case setsid failed): a
         *  number (1..31) or a "SIGxxx"/"xxx" name. Default SIGTERM.
         *  Returns true when the child was still running. */
        kill(signal?: number | string): boolean;
        /** Deterministic teardown: SIGKILL the group, reap, drop every
         *  resource, settle a parked wait() with the SIGKILL. */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }
}

/* ================================================================== *
 *  dyna:time
 * ================================================================== */
declare module "dyna:time" {
    /** Which clock when -- the engine's five time sources:
     *
     *  | Clock                | Kind       | Unit/Shape                  | Use for |
     *  |----------------------|------------|-----------------------------|---------|
     *  | time.now()           | wall       | {sec, nsec} split           | second-precision timestamps, RFC 3339 formatting |
     *  | time.nowSec()        | wall       | number whole seconds        | unix seconds without the sub-second half (now().sec) |
     *  | time.nowUnixNano()   | wall       | bigint ns since Unix epoch  | exact elapsed math across processes |
     *  | time.nowNanos()      | wall       | bigint ns since Unix epoch  | alias of nowUnixNano (the duration-side spelling) |
     *  | time.nowMillis()     | wall       | number ms                   | Date.now() equivalent from the same clock |
     *  | time.monotonicNano() | MONOTONIC  | bigint ns (arbitrary origin)| measuring durations; never compare across processes |
     *  | Date.now()/performance.now() | wall/mono | number ms             | web-compat shims (same truths, less precision) |
     *
     *  (`os.now` under --std is an ms clock with an arbitrary origin -- see
     *  the compat matrix in the file header.)
     *
     *  parseDuration speaks NANOSECONDS -- see its note below. */

    const Nanosecond: number;
    const Microsecond: number;
    const Millisecond: number;
    const Second: number;
    const Minute: number;
    const Hour: number;

    /** Parses "300ms", "-1.5h", "2h45m", "0"; returns a Number or BigInt of NANOSECONDS.
     *  Units: "ns", "us", "ms", "s", "m", "h" (compound forms like "2h45m";
     *  "d" is NOT accepted). Magnitudes below 2^53 ns return a `number`, at
     *  or above 2^53 ns a `bigint` (e.g. parseDuration("200000h") is
     *  720000000000000000n). A value that overflows int64 nanoseconds --
     *  or any unparsable input -- throws "invalid duration". */
    function parseDuration(str: string): number | bigint;
    /** parseDuration as MILLISECONDS, always a `number`. The ns parse is
     *  exact (int64); the result is the correctly-rounded double of
     *  ns/1e6 -- exact whenever that quotient is a safe integer
     *  (parseDurationMs("200000h") is 7.2e11 exactly, though parseDuration
     *  needed a BigInt for the same input). Past 2^53 IN THE RESULT the
     *  usual double rounding applies; parseDuration stays the exact path.
     *  Same grammar, same "invalid duration" SyntaxError. */
    function parseDurationMs(str: string): number;
    /** parseDuration as SECONDS, always a `number`; the same precision rule
     *  as parseDurationMs with ns/1e9. */
    function parseDurationSecs(str: string): number;
    /** The inverse of parseDuration; 0 is "0s" ("2h45m" round-trips as "2h45m0s"). */
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

    /** `new Duration({ milliseconds: ms })` as a call: the constructor's exact
     *  field semantics (integral; non-integers truncate toward zero; NaN and
     *  Infinity coerce to 0; negative allowed). */
    function durationMs(ms: number | bigint): Duration;
    /** `new Duration({ seconds: s })` as a call: the seconds fold into the
     *  Duration's millisecond field exactly as the constructor folds them, so
     *  fractional input truncates to whole seconds FIRST (durationSecs(1.9)
     *  is 1s, not 1.9s -- pass durationMs for sub-second precision). An s
     *  whose millisecond fold would leave int64 throws RangeError. */
    function durationSecs(s: number | bigint): Duration;

    /** {sec, nsec} from CLOCK_REALTIME (wall time). */
    function now(): { sec: number; nsec: number };
    /** Whole seconds since the Unix epoch (wall time) -- now().sec as one number. */
    function nowSec(): number;
    /** BigInt nanoseconds since the Unix epoch (wall time). */
    function nowUnixNano(): bigint;
    /** Alias of nowUnixNano: the same clock under the duration-side name. */
    function nowNanos(): bigint;
    /** Milliseconds since the Unix epoch (wall time; time.nowMillis() and
     *  Date.now() agree up to the call interval). */
    function nowMillis(): number;
    /** BigInt nanoseconds from CLOCK_MONOTONIC (arbitrary origin; durations
     *  only, never compare across processes). */
    function monotonicNano(): bigint;

    /** RFC 3339; nsec emitted only when non-zero (trailing fraction zeros
     *  trimmed); utc defaults true, and `utc: false` appends the LOCAL offset.
     *  Every typed slot is typed as declared, never coerced: a non-number
     *  ("5", true, null, an object, a BigInt) throws TypeError, a non-integer
     *  (2.5, NaN, Infinity) throws RangeError, and a whole number with no
     *  int64 representation (1e300) throws RangeError "out of range" --
     *  `sec`, `nsec` and `offsetMinutes` alike; `utc` takes a boolean.
     *  The second argument is the ONE overloaded slot: an object there is the
     *  options bag (that is how the bag form is spelled), so an object is
     *  never read as a legacy nsec; every other value is the legacy nsec and
     *  refuses exactly what the bag's nsec refuses.
     *  pass a bag instead of (nsec, utc) to render at a FIXED UTC
     *  offset -- `{ offsetMinutes }` in -1439..1439 (RFC 3339's +/-23:59),
     *  emitted as "Z" at zero and "+HH:MM"/"-HH:MM" otherwise, the exact
     *  shapes parseRFC3339 accepts back. STRICT bag: the valid keys
     *  are `nsec` and `offsetMinutes`; an unknown key throws a TypeError
     *  naming the key and the valid set. The bag form is EXACTLY two
     *  arguments -- a third positional throws TypeError instead of being
     *  silently ignored (the (nsec, utc) tail is the legacy overload only). */
    function formatRFC3339(sec: number, nsec?: number, utc?: boolean): string;
    function formatRFC3339(sec: number, opts: { nsec?: number; offsetMinutes?: number }): string;
    /** Go-style layout tokens 2006 Jan Mon 01 02 15 04 05, plus 's %z
     *  (the UTC offset: "Z" at zero, "+HH:MM"/"-HH:MM" otherwise). `opts` is
     *  the STRICT bag { offsetMinutes }: the fixed offset the fields and %z
     *  render at (default 0 = UTC), number-typed and whole like
     *  formatRFC3339's. A bag must be an object (or absent) --
     *  a string/number in the options position throws TypeError. */
    function formatUnix(sec: number, layout: string, opts?: { offsetMinutes?: number }): string;
    /** Strict RFC 3339 parse; returns {sec, nsec}. */
    function parseRFC3339(str: string): { sec: number; nsec: number };
    /** Unix seconds (UTC); an out-of-range month carries into the year. */
    function date(y: number, mo: number, d: number, h?: number, mi?: number, s?: number): number;
    /** {year, month, day, hour, min, sec, weekday, yday}; weekday 0 = Sunday. */
    function fromUnix(sec: number): { year: number; month: number; day: number; hour: number; min: number; sec: number; weekday: number; yday: number };

    /** A compiled Go-style layout (tokens as formatUnix, %z included).
     *  `opts` is the STRICT bag { offsetMinutes }: the fixed UTC
     *  offset the fields and %z render at; parse() reads fields at that same
     *  offset unless the input's own %z supplies one (which wins). A bag
     *  must be an object (or absent) -- a string/number in the options
     *  position throws TypeError. */
    class Format {
        constructor(layout: string, opts?: { offsetMinutes?: number });
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
        /** The PlainDate.until fold on the date parts (whole months first,
         *  day-of-month clamped, then remaining days) with the time-of-day
         *  remainder folded into the Duration's milliseconds. Days count
         *  whole 24-hour periods from the shifted, day-clamped point, so
         *  a.add(a.until(b)) === b exactly. A result may be MIXED-SIGN
         *  (months > 0, negative ms) when b falls on the very day the month
         *  shift lands on but earlier in it -- correct under add/subtract,
         *  but toString() refuses it like every mixed-sign Duration. */
        until(other: PlainDateTime): Duration;
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
        wkst?: number | "MO" | "TU" | "WE" | "TH" | "FR" | "SA" | "SU";
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
        /** STRICT: the opts bag is {dtstart} only; unknown keys throw. */
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
        /** STRICT: the opts bag is {now} only; unknown keys throw
         *  (a typo'd `now` used to silently race the real clock). */
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
    /** The joiner for the two splitters (PlainDateTime.toPlainDate()/
     *  toPlainTime()): a pure field copy, so
     *  toPlainDateTime(dt.toPlainDate(), dt.toPlainTime()) === dt always. */
    function toPlainDateTime(date: PlainDate, time: PlainTime): PlainDateTime;
}

/* ================================================================== *
 *  dyna:uring
 * ================================================================== */
// Linux + CONFIG_IO_URING=y builds only; the module does not exist otherwise.
declare module "dyna:uring" {
    /** Whole file as a string via the io_uring bulk reader (Linux only). */
    function readFile(path: import("dyna:file").Path): string;
    /** Whole file as a string via the blocking pread(2) reference reader. */
    function readFileSync(path: import("dyna:file").Path): string;
    /** Whole file as a fresh Uint8Array via the io_uring bulk reader:
     *  the exact bytes, no UTF-8 decode. Same Path contract and refusals as
     *  readFile; empty file = zero-length array. Linux only. */
    function readFileBytes(path: import("dyna:file").Path): Uint8Array;
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
        /** Parses without throwing: null for EVERY failure the
         *  constructor would throw for (unparsable input, bad base, input
         *  over 65536 bytes). A non-string argument is still the
         *  constructor's TypeError. Returns the plain URL class even under
         *  a subclass (WHATWG URL.parse never constructs the receiver). */
        static parse(input: string, base?: string): URL | null;
        /** parse() as a predicate. */
        static canParse(input: string, base?: string): boolean;
        /** Resolves `rel` against `base` and returns the resolved href
         *  (e.g. URL.join("https://a.com/x/y", "../z") === "https://a.com/z").
         *  THROWS TypeError on an unparseable base or reference -- in a join
         *  that is a caller bug, not a probe. */
        static join(base: string, rel: string): string;
        readonly href: string;
        readonly protocol: string;
        readonly username: string;
        readonly password: string;
        /**: settable. A value carrying `:port` splits there; a
         *  forbidden host code point (`/`, `?`, `#`) or an empty value makes
         *  the assignment a no-op, and a host-less URL has no host to set. */
        host: string;
        /**: settable (the port is untouched; `B.TEST` is lowercased,
         *  and the assignment is a no-op on input a host cannot hold). */
        hostname: string;
        /**: settable. Digits only -- anything else is a no-op; the empty
         *  string clears the port. */
        port: string;
        /**: settable. The value is parsed in path state, so a `?` in it
         *  starts the query and a `#` starts the fragment; a relative value is
         *  rooted at `/`, spaces are percent-encoded, and `.`/`..` resolve. */
        pathname: string;
        /**: settable. One leading `?` is a delimiter, not content; a
         *  `#` inside the value starts the fragment (the parse runs in query
         *  state), and the empty string clears the query. */
        search: string;
        /**: settable. One leading `#` is a delimiter; a later `#` is
         *  fragment content (nothing percent-encodes it); the empty string
         *  clears the fragment. */
        hash: string;
        readonly origin: string;
        /**: settable -- replaces the whole query with
         *  `new URLSearchParams(value)`'s serialization, so a string, a pair
         *  array and a record all work, and `""` clears the query. */
        searchParams: URLSearchParams;
        toJSON(): string;
        toString(): string;
    }
    /** WHATWG query list. A BOUND instance (from url.searchParams) writes
     *  through to the URL's query slot; a standalone one owns its query.
     *  Iteration (BEHAVIOR CHANGE): entries/keys/values return
     *  IterableIterators over a live index into the pair list (WHATWG
     *  shape); the former ARRAY returns moved to entriesArray()/
     *  keysArray()/valuesArray() -- code that indexed or measured the old
     *  arrays must switch to the twins. [Symbol.iterator] on the instance
     *  is the entries iterator, so for..of, spread, Array.from and
     *  `new Map(sp)` work directly. */
    class URLSearchParams {
        constructor(init?: string | URLSearchParams | Array<[string, string]> | Record<string, string>);
        readonly size: number;
        append(name: string, value: string): void;
        delete(name: string): void;
        get(name: string): string | null;
        getAll(name: string): string[];
        /** With the optional value only pairs matching BOTH count (WHATWG has(name, value)). */
        has(name: string, value?: string): boolean;
        set(name: string, value: string): void;
        sort(): void;
        toString(): string;
        forEach(callback: (value: string, key: string, params: URLSearchParams) => void): void;
        [Symbol.iterator](): IterableIterator<[string, string]>;
        /** Iterator over [name, value] pairs (WHATWG parity; live between
         *  steps, latched done once exhausted). */
        entries(): IterableIterator<[string, string]>;
        /** Iterator over names. */
        keys(): IterableIterator<string>;
        /** Iterator over values. */
        values(): IterableIterator<string>;
        /** The pre-UL-3 array forms, for code that indexed or measured the
         *  results. */
        entriesArray(): Array<[string, string]>;
        keysArray(): string[];
        valuesArray(): string[];
    }
    /** IDNA 2008 (UTS #46) mapping; options.transitional selects transitional
     *  processing. STRICT: the options bag is {transitional} only;
     *  unknown keys throw -- the two modes disagree on exactly the names
     *  where the option matters, so a typo must not silently pick one. */
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
    /** Random version-4 UUID. Unknown option keys are refused (TypeError). */
    function v4(opts?: { as?: "string" }): string;
    /** The 16 bytes a v4 string would encode, for DB-key paths. */
    function v4(opts: { as: "bytes" }): Uint8Array;
    /** Time-ordered version-7 UUID (48-bit ms + RFC 9562 same-ms counter): ids generated later in the process sort after earlier ones. */
    function v7(opts?: { as?: "string" }): string;
    /** The 16 bytes a v7 string would encode (same counter logic). */
    function v7(opts: { as: "bytes" }): Uint8Array;
    /** RFC 9562 custom: the caller supplies ALL 16 bytes; only the
     *  version nibble (byte 6 high <- 1000) and the variant bits (byte 8 top
     *  two <- 10) are overwritten, everything else survives. The fill view is
     *  copied, never mutated; wrong length is a RangeError. */
    function v8(opts: { fill: ByteView }): string;
    /** -1 | 0 | 1 comparing the canonical 16-byte forms (memcmp semantics):
     *  the sort order v7 ids already have. All four accepted string forms are
     *  form-agnostic here. TypeError on a malformed or non-string argument. */
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
    /** URL-safe ID over the default 64-symbol alphabet; size 1..4096. */
    function NanoID(size?: number): string;
    /** The same generator over a caller-supplied alphabet of 2..256 ASCII symbols. */
    function NanoIDAlphabet(alphabet: string, size?: number): string;
    /** A 26-character Crockford base32 ULID; atMillis must fit 48 bits.
     *  {monotonic: true}: same-millisecond calls +1 the 80 random bits under a
     *  process-wide mutex (workers share the state), so rapid same-ms calls are
     *  strictly ascending; a new millisecond re-seeds from entropy, and a
     *  backwards clock or an atMillis below the floor HOLDS the previous
     *  millisecond (the v7 clamp). Default is fresh entropy per call: NOT
     *  monotonic. */
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

/* ================================================================== *
 *  dyna:oauth2 — optional OAuth 2.0 helpers (RFC 6749 + 6750 + 7636)
 *  Pure utilities; no IO, no JWT dependency. JWT via dyna:crypto when
 *  CONFIG_TLS=y, otherwise those helpers throw "not compiled in".
 * ================================================================== */
declare module "dyna:oauth2" {
    /** 43..128 unreserved chars; 32 octet CSPRNG -> 43 char base64url */
    function generateCodeVerifier(): string;
    function isValidCodeVerifier(s: string): boolean;
    /** S256 = BASE64URL(SHA256(verifier)), plain = verifier; S256 default */
    function generateCodeChallenge(verifier: string, method?: "S256" | "plain"): string;
    function verifyCodeChallenge(verifier: string, challenge: string, method: string): boolean;
    /** CSPRNG bytes -> base64url; default 32 bytes */
    function generateState(bytes?: number): string;
    /** Constant-time compare over UTF-8 bytes or raw ByteView bytes.
     *  The bytes/string convenience form of dyna:crypto's TimingSafeEqual --
     *  same primitive, same truth table. */
    function secureCompare(a: BytesInput, b: BytesInput): boolean;
    /** Client Basic auth per RFC 6749: form-encodes id:secret then base64 */
    function buildClientAuthHeader(clientId: string, clientSecret?: string): string;
    /** JWT access-token verification (needs CONFIG_TLS=y, otherwise throws). Checks signature via dyna:crypto then exp/nbf/aud/iss/scope.
     *  Pass T to type the verified payload (the runtime returns whatever the
     *  token carried; without T the payload reads as unknown). */
    function verifyJWT<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[]; aud?: string | string[]; iss?: string; requiredScope?: string[]; clockSkewSec?: number }): T;
    /** NQCHAR scope tokens, space-delimited, per RFC 6749 A.4 */
    function parseScope(scope: string): string[];
    function formatScope(scopes: string[]): string;
    /** "Bearer <b64token>" with b64token validation */
    function buildBearerHeader(token: string): string;
    function parseBearerHeader(header: string): string | null;
    /** headers.authorization/query/body; allowQuery/allowBody opt-in (default false) */
    function parseBearerFromRequest(req: { headers?: Record<string,string>; query?: string; body?: string; allowQuery?: boolean; allowBody?: boolean }): string | null;
    function isValidBearerToken(token: string): boolean;
    /** WWW-Authenticate: Bearer realm, error, error_description, error_uri, scope. error is REQUIRED (RFC 6750 ). */
    function buildWWWAuthenticate(opts: { realm?: string; error: string; errorDescription?: string; error_description?: string; errorUri?: string; error_uri?: string; scope?: string }): string;
    /** Verbatim exact except loopback port-ignore per RFC 8252 */
    function isValidRedirectUri(candidate: string, registered: string[]): boolean;
    /** Build ?response_type=code&client_id&redirect_uri&scope&state&code_challenge; auto-generates state if absent (always returned) */
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
    function parseTokenResponse(body: string): TokenResponse;
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

    /** Parses a document; options trim (default true), entities
     * "strict"|"keep", and multiple: true returns EVERY root element
     * as an array (parity with HTMLParse's fragment array) instead of
     * demanding exactly one. Document-level strictness is unchanged either
     * way: malformed input throws, and a document with no element at all
     * throws in both modes. The options bag is strict -- an unknown
     * key throws a TypeError naming the key and the valid set. */
    function XMLParse(text: string, opts?: { trim?: boolean; entities?: "strict" | "keep"; multiple?: boolean }): XMLElement | XMLElement[];
    /** Serializes a node; indent 0..16 spaces; nesting beyond 256 throws. The options bag is strict (unknown key throws). */
    function XMLStringify(node: XMLElement, opts?: { indent?: number }): string;
    /** Collapses an element into a plain object keyed by element name. */
    function XMLToObject(node: XMLElement): Record<string, unknown>;

    /**: a streaming document writer for output too deep (or too long)
     *  for the tree + XMLStringify round-trip. No tree is ever built: the
     *  writer holds an incremental byte buffer and a heap stack of open
     *  element names, so cost tracks output size, not nodes. Elements must
     *  close innermost-first; one writer produces one document.
     *
     *  Depth is NOT the serializer's 256: the writer accepts nesting to
     *  65536 (a runaway guard; beyond it open() throws a RangeError).
     *  Names that are not valid XML name characters throw; the options bag
     *  is strict (unknown key throws a TypeError naming the key and the
     *  valid set). */
    class XmlWriter {
        constructor(opts?: { indent?: number });
        /** Writes `<name attrs>` and pushes the element. `attrs` values are
         *  escaped (quotes included); invalid names throw. */
        open(name: string, attrs?: Record<string, string>): void;
        /** Appends escaped text (& < > escaped; quotes are legal raw in
         *  text). */
        text(s: string): void;
        /** Writes `</name>` for the innermost open element; throws when
         *  none is open. */
        close(): void;
        /** The bytes so far, WITHOUT closing anything. */
        toString(): string;
        /** Closes every remaining open element (innermost-first) and
         *  returns the complete document. Idempotent: the buffer never
         *  truncates. */
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
    /**: one event of the pull form. Fields are present only where
     *  they mean something: `name` on open/close/pi, `attrs` on open,
     *  `text` on text/cdata/comment and for the PI's data. `offset` is
     *  the event's starting byte offset in the stream as fed (absolute:
     *  it accumulates across write() chunks and matches `write`'s string
     *  UTF-8 bytes or a byte view's indices). */
    interface SAXEvent {
        event: "open" | "close" | "text" | "cdata" | "comment" | "pi";
        name?: string;
        attrs?: Record<string, string>;
        text?: string;
        offset: number;
    }
    /** Streaming SAX parser; a token interrupted by a chunk boundary resumes.
     *
     *  MODE RULE (the documented interplay of the two forms): the
     *  form is fixed at construction by the ARGUMENT SHAPE.
     *    - A handlers object (even `{}`) is a PUSH parser: write()/end()
     *      scan synchronously, handlers fire during the scan, malformed
     *      input throws from write()/end() -- byte-for-byte the historic
     *      contract. next() on a push parser is legal but has nothing to
     *      hand out: write() already consumed the buffer.
     *    - NO handlers argument (or undefined/null) is a PULL parser:
     *      write() only buffers (bounded carry), next() scans on demand
     *      and returns one SAXEvent at a time (null when the input is
     *      drained -- feed more, or finalize with end()), and a pull
     *      parser's end() is LAZY: it only finalizes, so malformed input
     *      surfaces from next() instead (the error is sticky: every later
     *      next() re-throws it; the end-of-document checks run at the last
     *      next()). Mixing: a push parser's handlers fire whenever the
     *      scanner runs, including from a next() call; the two forms never
     *      double-deliver because push write() never leaves events
     *      pending. */
    class SAXParser {
        /** The handlers object doubles as the options bag: `trim` and `entities` are read from it. Omit it entirely for the pull form; a pull parser's options then come from the second argument. */
        constructor(handlers?: SAXHandlers & { trim?: boolean; entities?: "strict" | "keep" }, opts?: { entities?: "strict" | "keep" });
        /** Feeds a string or any byte view. */
        write(chunk: BytesInput): void;
        /** Finalizes the stream. Push parsers run their end-of-document checks here (trailing content throws); pull parsers defer them to the last next(). */
        end(): void;
        /** Pull form: scans forward and returns the next SAXEvent, or null while the input is drained (feed more, or the document is finished). Errors throw and are sticky. */
        next(): SAXEvent | null;
        /** Async-iterable wrapper: builds its own no-handler parser and drives it with every chunk `source` (any sync or async iterable of strings/byte views) yields, so
         *  `for await (const ev of SAXParser.iterate(src, opts))` surfaces one SAXEvent per step. Chunk boundaries may fall anywhere (a token interrupted mid-way resumes). Malformed input rejects the iteration. */
        static iterate(source: AsyncIterable<BytesInput> | Iterable<BytesInput>, opts?: { entities?: "strict" | "keep" }): AsyncGenerator<SAXEvent>;
    }
}

/* ================================================================== *
 *  dyna:yaml
 * ================================================================== */
declare module "dyna:yaml" {
    /** Parses exactly one document; multi-document input is refused.
     *
     *  Options: { maxDepth: 1..128 } lowers the nesting cap (a
     *  fuzzer-hardening bound; the 128 ceiling is a C-stack guard and
     *  cannot be raised from JS). { schema } accepts "core" (the only
     *  implemented schema) and refuses "full" BY NAME with a RangeError
     *  rather than silently parsing as core. There is deliberately no
     *  {maxAliases}: anchors and aliases are refused outright, which is
     *  stricter than any alias cap. The bag is fully strict: wrong types
     *  are TypeErrors and an unknown key throws one naming the key
     *  and the valid set . */
    function Parse(text: string, opts?: { maxDepth?: number; schema?: "core" | "full" }): unknown;
    /** Parses every `---`-separated document into an array. Same options as Parse. */
    function ParseAll(text: string, opts?: { maxDepth?: number; schema?: "core" | "full" }): unknown[];
    /** Serializes a value as a YAML document. The options bag is strict --
     *  an unknown key throws a TypeError naming the key and the valid set
     *  (unknown option "indnt" (valid: indent, width, sortKeys, flow)).
     *
     *  Options:
     *    { indent }   1..10 spaces (default 2).
     *    { width }    0 = never fold, which is the emitter's historic
     *                 behavior byte-for-byte, so the default output does
     *                 not change. Above 0: a STRING value whose single-line
     *                 form exceeds width is emitted as a double-quoted
     *                 scalar folded across lines with backslash
     *                 continuations -- each break contributes NOTHING to
     *                 the value, so the re-parse is exact (the reader
     *                 accepts precisely this continuation form). The
     *                 first line carries up to `width` columns of the
     *                 scalar's own form, continuation lines up to
     *                 `width - indent`; a budget under 16 clamps to 16
     *                 rather than breaking per byte; a word longer than
     *                 the whole budget is split mid-word with a
     *                 continuation (still exact) when no space falls in
     *                 the window. Columns are bytes of the UTF-8 form
     *                 (conservative for wide chars).
     *                 Keys are NEVER folded (a key must stay one line)
     *                 and flow style is never folded. Integer 0..65536;
     *                 wrong types are TypeErrors.
     *    { sortKeys } true = mapping keys sort by UTF-8 byte order
     *                 (strcmp), recursively, in block and flow style.
     *                 Default false = insertion order. Must be boolean.
     *    { flow }     true = every collection is emitted in flow style
     *                 (`[a, b]`, `{k: v}`), recursively, ", " separated;
     *                 empty collections stay `[]`/`{}`. Flow lines may
     *                 exceed width. Default false = block style. Must be
     *                 boolean. */
    function Stringify(value: unknown, opts?: {
        indent?: number;
        width?: number;
        sortKeys?: boolean;
        flow?: boolean;
    }): string;
    /** Line-at-a-time multi-document parsing: returns a SYNC
     *  iterator yielding the `---`-separated documents one at a time,
     *  with ParseAll's strictness -- same markers, same refusals, and an
     *  empty document between markers yields null exactly as ParseAll
     *  stores one. The input is the whole text (the line table is built
     *  up front, as for Parse); what the iterator buys is LAZY DOCUMENTS,
     *  not incremental feeding: only one parsed document is alive at a
     *  time, and malformed document N is reported AS document N.
     *  Per-document errors are SyntaxErrors carrying the 1-based document
     *  index and the line number within the whole text
     *  ("ParseStream: document 3: ... at line 17"); after one, the
     *  iterator is dead -- every further next() re-throws the same error.
     *  return() closes early (for-of break); next() after done stays
     *  done. Same options bag as Parse (errors are labeled "ParseStream"). */
    function ParseStream(text: string, opts?: { maxDepth?: number; schema?: "core" | "full" }): Iterator<unknown>;
}
/* ================================================================== *
 *  dyna:dataframe
 * ================================================================== */
declare module "dyna:dataframe" {
    /** Columnar tables over TypedArrays; string columns are dictionary-encoded.
     *
     *  Choosing a tabular surface: CSVFile (dyna:csv) is the FILE-BACKED
     *  load-modify-store table; DataFrame (here) is the in-memory columnar
     *  analytics engine (verbs, masks, group-bys); dyna:structures.Table is a
     *  sparse string-keyed row/column map for lookups, not analytics. */
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
        /** Header row plus one row per frame row, RFC 4180 quoting.
         *  {escapeFormulas: true} prefixes a "'" to cells starting with
         *  =/+/-/@ so spreadsheet apps do not execute them (CSV injection). */
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

        /** Elementwise verbs returning a Float64Array of ROWS entries.
         *  The binary family's second operand is a number or another
         *  COLUMN NAME (col-vs-col); RSUB/RDIV take a number only. */
        ABS(col: string): Float64Array;
        ABS(col: string, opts: { out: string }): this;
        ROUND(col: string): Float64Array;
        ROUND(col: string, opts: { out: string }): this;
        FLOOR(col: string): Float64Array;
        FLOOR(col: string, opts: { out: string }): this;
        CEIL(col: string): Float64Array;
        CEIL(col: string, opts: { out: string }): this;
        SQRT(col: string): Float64Array;
        SQRT(col: string, opts: { out: string }): this;
        LOG(col: string): Float64Array;
        LOG(col: string, opts: { out: string }): this;
        EXP(col: string): Float64Array;
        EXP(col: string, opts: { out: string }): this;
        SIGN(col: string): Float64Array;
        SIGN(col: string, opts: { out: string }): this;
        CLIP(col: string, lo: number, hi: number): Float64Array;
        CLIP(col: string, lo: number, hi: number, opts: { out: string }): this;
        FILL_NA(col: string, value: number): Float64Array;
        FILL_NA(col: string, value: number, opts: { out: string }): this;
        ADD(col: string, x: number | string): Float64Array;
        ADD(col: string, x: number | string, opts: { out: string }): this;
        SUB(col: string, x: number | string): Float64Array;
        SUB(col: string, x: number | string, opts: { out: string }): this;
        MUL(col: string, x: number | string): Float64Array;
        MUL(col: string, x: number | string, opts: { out: string }): this;
        DIV(col: string, x: number | string): Float64Array;
        DIV(col: string, x: number | string, opts: { out: string }): this;
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
        /** Equal-width bins over the observed range; edges has bins+1 entries.
         *  This is the engine's histogram surface -- dyna:mathx deliberately
         *  has none; see also HISTOGRAM_NORMALIZED below. */
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
        ROLLING_SUM(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_SUM(col: string, w: number, opts: { out: string }): this;
        ROLLING_MEAN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MEAN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MEAN(col: string, w: number, opts: { out: string }): this;
        ROLLING_MIN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MIN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MIN(col: string, w: number, opts: { out: string }): this;
        ROLLING_MAX(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MAX(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MAX(col: string, w: number, opts: { out: string }): this;
        ROLLING_VAR(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_VAR(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_VAR(col: string, w: number, opts: { out: string }): this;
        ROLLING_STD(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_STD(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_STD(col: string, w: number, opts: { out: string }): this;
        /** Exponential moving average; alpha in (0, 1]. */
        EMA(col: string, alpha: number, mask?: Uint8Array): Float64Array;
        EMA(col: string, alpha: number, mask: Uint8Array, opts: { out: string }): this;
        EMA(col: string, alpha: number, opts: { out: string }): this;
        PCT_CHANGE(col: string, periods?: number, mask?: Uint8Array): Float64Array;
        PCT_CHANGE(col: string, periods: number, mask: Uint8Array, opts: { out: string }): this;
        PCT_CHANGE(col: string, periods: number, opts: { out: string }): this;
        PCT_CHANGE(col: string, opts: { out: string }): this;
        ZSCORE(col: string, mask?: Uint8Array): Float64Array;
        ZSCORE(col: string, mask: Uint8Array, opts: { out: string }): this;
        ZSCORE(col: string, opts: { out: string }): this;
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


    /**: the runtime type test. True ONLY for frames this engine built
     *  (constructor or any verb returning a frame) -- a brand check on the
     *  instance, not duck typing: an object carrying `ROWS`/`COLUMNS`
     *  properties, a `DataFrame.prototype`-chained object without a brand,
     *  arrays and primitives are all false. */
    function isDataFrame(v: unknown): boolean;
    /** Builds a frame from an object mapping column name to a TypedArray or string array.
     *
     *  ZERO-COPY (runtime-verified): TypedArray columns are ALIASED, not
     *  copied -- mutating the source array after construction changes the
     *  frame (TO_COLUMNS()/COPY() hand out fresh copies). `number[]` columns
     *  are accepted but stored as STRING columns (dtype "str"): the
     *  numeric verbs refuse them. Use TypedArrays for numeric data. */
    const DataFrame: {
        new (columns: Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | number[] | string[]>): DataFrame;
    };
}
/* ================================================================== *
 *  WHATWG globals, std/os modules, and core prototype extensions
 * ================================================================== */

/* ---- std / os modules (available with --std) ---------------------- */

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
    /** Format and print to stdout. */
    function printf(fmt: string, ...args: unknown[]): void;
    /** Format to a string. */
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
    /** Byte count read, or a negative errno; `buffer` is a raw ArrayBuffer
     * sliced by offset/length. */
    function read(fd: number, buffer: ArrayBuffer, offset: number, length: number, position?: number): number;
    /** Byte count written, or a negative errno; `buffer` is a raw ArrayBuffer
     * sliced by offset/length. */
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
    /** Runs a process and returns its exit status (negative = terminated by
     *  -signal); with block:false returns the pid and does not wait.
     *  sys.Exec vs os.exec: this is the --std QuickJS classic door
     *  (pid/errno conventions, options for stdio piping); dyna:sys's Exec is
     *  canonical (result object, throws on spawn failure). */
    function exec(args: string[], options?: { blocking?: boolean; usePath?: boolean; file?: string; cwd?: string; stdin?: unknown; stdout?: unknown; stderr?: unknown; env?: Record<string, string>; uid?: number; gid?: number }): number;
    function waitpid(pid: number, options?: number): [number, number];
    function kill(pid: number, sig: number): number;
    /** The handler is invoked with no arguments; read the registered number from closure state. */
    function signal(signal: number, handler: () => void): void;
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
        /** Optional transfer list of ArrayBuffers/ArrayBufferView to
         *  detach (neuter) on the sender after a successful queue
         *  (copy+detach: the receiver gets its own copy). A DataView entry
         *  transfers its underlying buffer; a SharedArrayBuffer is refused
         *  (it cannot be detached); a validation, clone or queue failure
         *  detaches nothing. */
        postMessage(value: unknown, transfer?: ArrayBuffer[] | ArrayBufferView[]): void;
        /** A live message port keeps the process alive -- even after the
         * worker thread exits. Set to null to release it. */
        onmessage: ((ev: { data: unknown }) => void) | null;
        onerror: ((err: unknown) => void) | null;
    }
}

/* ================================================================== *
 *  dyna:bench
 *  Every options bag is STRICT: unknown keys throw a TypeError naming the key
 *  and the valid set. Timeless docs: no dates.
 * ================================================================== */
declare module "dyna:bench" {
    /** One benchmark result. opsPerSec = 1000/meanMs; rsd = std/mean;
     *  p50Ms/p99Ms over per-iteration latencies. */
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
    /** Runs fn repeatedly for timeMs (default 500, 1..60000) after warmupMs
     *  (default 100, 0..60000; warmup aliases warmupMs). Native monotonic
     *  clock; throwing fn propagates. Recorded for table(). */
    function bench(name: string, fn: () => void, opts?: { timeMs?: number; warmupMs?: number; warmup?: number }): BenchResult;
    /** Tab-separated table of all bench() results this process (header + one row per run). */
    function table(): string;
}

/* ---- WHATWG globals the engine ships ----------------------------- */

/** Identity for `using` disposal. */
interface SymbolConstructor {
    readonly dispose: symbol;
    readonly asyncDispose: symbol;
}

interface AbortSignal {
    readonly aborted: boolean;
    readonly reason: unknown;
    onabort: ((this: AbortSignal, ev: { type: string; target: AbortSignal }) => void) | null;
    addEventListener(type: "abort", listener: (ev: { type: string; target: AbortSignal }) => void): void;
    removeEventListener(type: "abort", listener: (ev: { type: string; target: AbortSignal }) => void): void;
    throwIfAborted(): void;
    /** @internal Fires the abort. Engine-internal mutator: call
     *  AbortController.abort() or AbortSignal.abort()/timeout() instead. */
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

/** Global WebSocket: thin alias over dyna:http WsClient (same constructor). */
declare const WebSocket: typeof import("dyna:http").WsClient;

interface RequestInit {
    method?: string;
    headers?: HeadersInit;
    body?: unknown;
    signal?: AbortSignal | null;
    /** Milliseconds until the fetch fails (runtime-verified honored --
     *  a server that accepts and never answers fails at ~timeout). Saves
     *  hand-rolling an AbortController; combine with `signal` if you also
     *  need caller-driven abort. */
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

/** WHATWG URL parser, global since the globals parity pack -- THE same
 *  class the dyna:url module exports (`globalThis.URL === (await
 *  import("dyna:url")).URL`), not a copy. Writable/enumerable/configurable
 *  data property, so scripts may shadow or `delete` it. */
declare const URL: typeof import("dyna:url").URL;
/** WHATWG query list, THE same class the dyna:url module exports. */
declare const URLSearchParams: typeof import("dyna:url").URLSearchParams;

/** The global fetch: the SAME function object as dyna:http's (identity
 *  runtime-verified), declared here once by reference so the two doors can
 *  never drift. WHATWG subset: no `redirect`/`credentials`/`integrity`;
 *  redirects are followed automatically and there is no cookie jar. */
declare const fetch: typeof import("dyna:http").fetch;

/** Web-compat encoder: byte-identical to dyna:bytes' fromUtf8 and
 *  new bytes.Text(s).toUtf8(). Prefer fromUtf8 in module code; TextEncoder
 *  exists so browser-shaped code runs unmodified. */
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
    /** Decodes the buffer as UTF-8.
     *  {stream: true} continues the decoder's stream: an incomplete
     *  trailing multi-byte sequence is carried (no U+FFFD, no fatal error)
     *  and prepended to the next call's input. A call without stream (or
     *  with stream: false) is FINAL: the carry is processed, an incomplete
     *  tail becomes U+FFFD (or throws under fatal), and the decoder resets.
     *  A stream's leading BOM is stripped once, even when its bytes complete
     *  in a later chunk; plain (non-stream) calls stay stateless. Unknown
     *  option keys throw TypeError. */
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

/** WHATWG latin-1 base64 codecs. WARNING: these are STRING codecs over
 *  LATIN-1, not UTF-8 -- btoa("é") is "6Q==" (one raw byte, chars above U+00FF
 *  throw), while the byte-level encoding.Base64Encode(utf8) of the same string
 *  is "w6k=". For JS strings use encoding.Base64* or
 *  String.prototype.encodeBase64/decodeBase64 (UTF-8); atob/btoa exist for
 *  web-compat code that already holds latin-1 strings. */
declare function atob(data: string): string;
declare function btoa(data: string): string;

/** The base iterator constructor. Every runtime iterator's prototype
 *  chain reaches Iterator.prototype, which carries the ES2025 iterator-helper
 *  methods plus engine extras -- so `[1,2].values().map(...).take(2)` works,
 *  as do the engine's lazies (`"str".lazy()`, `arr.lazy()`). */
interface IteratorConstructor {
    /** Wraps any iterable or iterator in the helper-equipped iterator. */
    from<T>(objects: Iterable<T> | Iterator<T>): IteratorObject<T>;
    /** Concatenates iterables into one iterator (ES2025 Iterator.concat). */
    concat<T>(...items: (Iterable<T> | Iterator<T>)[]): IteratorObject<T>;
    /** Zips iterables pairwise into [a, b] tuples; mode "shortest" (default),
     *  "longest" (pads with undefined) or "strict" (length mismatch throws).
     *  String inputs are refused. */
    zip<T extends readonly (Iterable<unknown> | Iterator<unknown>)[]>(iterables: T, options?: { mode?: "shortest" | "longest" | "strict" }): IteratorObject<{ [K in keyof T]: T[K] extends Iterable<infer U> ? U : (T[K] extends Iterator<infer U2> ? U2 : unknown) }>;
    /** Zips an object whose VALUES are iterables into objects with the same keys. */
    zipKeyed<O extends Record<string, Iterable<unknown> | Iterator<unknown>>>(iterables: O, options?: { mode?: "shortest" | "longest" | "strict" }): IteratorObject<{ [K in keyof O]: O[K] extends Iterable<infer U> ? U : (O[K] extends Iterator<infer U2> ? U2 : unknown) }>;
}
declare const Iterator: IteratorConstructor;

/** The helper-equipped iterator shape (merges the engine's runtime surface
 *  onto the lib's IteratorObject: every helper below lives on the intrinsic
 *  Iterator.prototype, so every runtime iterator carries them). */
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
    aperture(n: number): IteratorObject<T[], TReturn, TNext>;
    splitEvery(n: number): IteratorObject<T[], TReturn, TNext>;
    zipWith<U, R>(other: Iterable<U>, fn: (a: T, b: U) => R): IteratorObject<R, TReturn, TNext>;
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
    reduceBy<K extends string | number, R>(key: (value: T) => K, fn: (acc: R, value: T) => R, seed: R): Record<K, R>;
}

/** ES2025 half-precision (binary16) typed array; a standard %TypedArray%
 *  (BYTES_PER_ELEMENT 2, full prototype surface). */
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
    new (message?: string, error?: unknown, suppressed?: unknown): SuppressedError;
};

interface DisposableStack {
    /** Registers a resource; its dispose()/close() (when present) runs at
     *  stack disposal. */
    use<T>(value: T): T;
    /** Registers a non-resource value with an explicit disposer. */
    adopt<T>(value: T, onDispose: (value: T) => void): T;
    /** Registers a bare callback to run at disposal. */
    defer(onDispose: () => void): this;
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
    /** Registers a resource; its dispose()/close() (when present) runs at
     *  stack disposal. */
    use<T>(value: T): T;
    adopt<T>(value: T, onDispose: (value: T) => void): T;
    defer(onDispose: () => void | Promise<void>): this;
    move(): AsyncDisposableStack;
    disposeAsync(): Promise<void>;
    readonly disposed: boolean;
    [Symbol.asyncDispose](): Promise<void>;
}
declare const AsyncDisposableStack: {
    readonly prototype: AsyncDisposableStack;
    new (): AsyncDisposableStack;
};

/** Internal engine error class (thrown for internal invariant failures,
 *  e.g. the dyna:uring read path). Not for user code. */
interface InternalError extends Error {
    name: "InternalError";
}
declare const InternalError: {
    readonly prototype: InternalError;
    new (message?: string): InternalError;
};

/**
 * A native optics value: a reusable getter/setter pair over one focus
 * (property, array index, nested path, or a custom pair). All updates are
 * IMMUTABLE -- view/set/over never touch the source.
 *
 * Lens vs the Object.path family: Lens is for REUSABLE, composable optics and
 * `over` (transform at a focus); Object.path/get/set/assocPath are for ONE-OFF
 * paths with the object in hand.
 */
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
    /** Builds a custom lens from a getter and an immutable setter
     *  (set(value, source) returns the updated source); works with or without `new`. */
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

/** @internal Loads and evaluates a script file into global scope
 *  (engine bootstrap helper; use import or std.loadScript instead). */
declare function __loadScript(filename: string): unknown;


/** Resolves after `ms` milliseconds (fractions truncate; NaN/negative/absent
 *  count as 0; caps at 2^31-1 ms). The global spelling of `os.sleepAsync` --
 *  the same timer machinery, so other ready work (timers, IO) is serviced
 *  while it waits. */
declare function sleep(ms: number): Promise<void>;

/** Structured deep copy: plain objects, arrays, Date, Map, Set, RegExp,
 *  typed arrays/ArrayBuffer (fresh buffer, same bytes), BigInt, Symbol and
 *  cyclic references (the copy's cycle points into the copy). Functions
 *  cannot be cloned and throw. THE same function the dyna:serialize module
 *  exports (`globalThis.structuredClone === (await
 *  import("dyna:serialize")).structuredClone`). */
declare function structuredClone(value: unknown): unknown;

/* ---- core prototype extensions (project additions) --------------- */

interface String {
    /** A lazy iterator over the string's CODE POINTS (runtime-verified:
     *  a native method; ARGUMENTS ARE IGNORED; an astral character is one
     *  entry, unlike code-unit walks). The result is a full Iterator -- the
     *  helper methods (map/filter/take/toArray, ...) compose:
     *  `"a,b".lazy().map(s => s.toUpperCase()).toArray()`. Hazard: `lazy` is
     *  a writable prototype member on EVERY string/array, so code that
     *  stores its own `lazy` field collides (own properties shadow it). */
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
    /** Case-insensitive equality. */
    equalsIgnoreCase(other: string): boolean;
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
    /** A stable sort by a key function. */
    sortBy<K>(key: (v: T) => K): T[];
    /** The insertion index for a sorted array; `comparator` must match the array's sort order. */
    sortedIndexOf(value: T, comparator?: (a: T, b: T) => number): number;
    /** Groups elements by a key function into a Record. */
    groupBy<K extends string | number>(key: (v: T) => K): Record<K, T[]>;
    /** A fresh array in random order (draws from a GLOBAL RNG -- see the
     *  which-random-when note on dyna:random; use a Random instance for
     *  reproducible shuffles). */
    shuffle(): T[];
    /** n random elements without replacement (global RNG, as shuffle). */
    sample(n?: number): T[];
    /** The distinct elements, first occurrence kept; `map` picks the dedup key.
     *  Canonical spelling of the pair. */
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
    /** A lazy iterator over the array's elements (runtime-verified: a
     *  native method; ARGUMENTS ARE IGNORED -- there is no lazy-map form).
     *  The result composes with the Iterator helpers. Same shadowing hazard
     *  as String.prototype.lazy. */
    lazy(): Iterator<T>;
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
    /** An ARRAY of numbers [start, end) -- EXCLUSIVE of end (returns a real
     *  array, not a lazy iterator). For an INCLUSIVE grid of n points use
     *  mathx.linspace. */
    range(start: number, end?: number, step?: number): number[];
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
    /** Swaps keys and values. Canonical spelling of the pair. */
    invert(obj: object): Record<string, string>;
    /** LEGACY alias of invert; prefer invert. */
    invertObj(obj: object): Record<string, string>;
    objOf<K extends string, V>(key: K, value: V): Record<K, V>;
    /** An object of the picked keys. */
    pick(keys: string[], obj: object): Record<string, unknown>;
    /** An object without the given keys. */
    omit(keys: string[], obj: object): Record<string, unknown>;
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
    /** Shallow merge; later sources win. Canonical spelling of the pair
     *  (runtime-verified: merge ≡ mergeRight). mergeLeft is the
     *  FIRST-wins variant, not an alias. */
    mergeRight(a: object, b: object): Record<string, unknown>;
    /** LEGACY alias of mergeRight; prefer mergeRight. */
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
    pickAll(keys: string[], obj: object): Record<string, unknown>;
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
}

/* Legacy accessors live on Object.prototype, not the constructor. */
interface Object {
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
/* ================================================================== *
 *  dyna:async
 * ================================================================== */
declare module "dyna:async" {
    /* WHAT IS REAL, stated honestly: every fn here runs on the event-loop
     * thread. The engine's shared native offload pool executes C functions,
     * not JS, and os.Worker takes a module FILENAME whose messages cannot
     * carry functions -- so nothing in this module is PARALLEL. pmap/Pool
     * deliver CONCURRENCY-limited interleaving (at most N fns in flight,
     * async fns interleaving at their awaits); the rest are real bounded
     * handoff structures and clock-driven combinators. Argument refusals
     * throw SYNCHRONOUSLY (the house doctrine: the same shape the sync
     * surface throws, before anything is scheduled). */

    /** Resolves after `ms` milliseconds (setTimeout). */
    function sleep(ms: number): Promise<void>;

    /** Rejects if `p` does not settle within `ms`; resolves/rejects with
     *  p otherwise. The underlying promise is handled even when the timer
     *  wins -- its late rejection never surfaces as unhandled -- and the
     *  timer is cleared on settle. `reason` replaces the message. */
    function withTimeout<T>(p: PromiseLike<T>, ms: number, reason?: string): Promise<T>;

    /** Calls `fn` (async or sync) until it resolves or the retries run
     *  out: 1 initial attempt + `retries` retries (default 3), waiting
     *  backoffMs * factor^attempt (defaults 0 ms, 2) between tries.
     *  `onRetry(error, attempt)` observes each retry. The LAST error
     *  surfaces. */
    function retry<T>(fn: () => T | PromiseLike<T>, opts?: {
        retries?: number;
        backoffMs?: number;
        factor?: number;
        onRetry?: (err: unknown, attempt: number) => void;
    }): Promise<T>;

    /** Counting semaphore with DIRECT token hand-off (a release passes the
     *  token straight to the next waiter -- no newcomer can jump the
     *  queue). acquire() resolves with a release function; run(fn) wraps
     *  acquire/try/release. */
    class Semaphore {
        constructor(n: number);
        get available(): number;
        get waiting(): number;
        acquire(): Promise<() => void>;
        run<T>(fn: (...args: unknown[]) => T | PromiseLike<T>, ...args: unknown[]): Promise<T>;
    }

    /** The synchronous bounded FIFO (Channel is the async MPSC). */
    class Queue {
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

    /** MPSC channel: many senders, ONE consumer. send() backpressures (a
     *  promise) at capacity; recv() resolves {value, done:false}, then --
     *  after close() AND a full drain -- {done:true}. close() resolves a
     *  parked recv with done and REJECTS a send that was blocked on
     *  capacity (its value was never accepted). */
    class Channel<T = unknown> {
        constructor(cap?: number);
        get length(): number;
        get closed(): boolean;
        send(v: T): Promise<void>;
        recv(): Promise<{ value: T; done: false } | { done: true }>;
        close(): void;
        [Symbol.asyncIterator](): AsyncIterator<T>;
    }

    /** Map `fn` over `items` with at most `concurrency` invocations in
     *  flight (default 8), results in ITEM order. fn runs on THIS thread:
     *  async fns interleave at their awaits, sync fns run one at a time.
     *  The FIRST rejection rejects the whole pmap; in-flight items still
     *  run to completion and settle, their late results dropped and their
     *  late rejections handled (nothing surfaces twice). */
    function pmap<T, R>(items: T[], fn: (item: T, index: number, items: T[]) => R | PromiseLike<R>,
                        opts?: { concurrency?: number }): Promise<R[]>;

    /** A fixed set of `n` submit slots -- the same honest interleave as
     *  pmap, shaped as a long-lived scheduler. NOT worker parallelism: a
     *  throwing job rejects its submit, frees its slot, and the pool keeps
     *  pumping. */
    class Pool {
        constructor(n: number);
        get size(): number;
        get active(): number;
        get pending(): number;
        submit<T>(fn: () => T | PromiseLike<T>): Promise<T>;
    }

    /** Trailing debounce: the LAST call inside the `ms` window runs once,
     *  at the window's edge. cancel() drops a pending call. */
    function debounce<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };

    /** Leading throttle with a trailing call: the first call in the window
     *  runs now, the last call in the window runs at the edge. */
    function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };
}
