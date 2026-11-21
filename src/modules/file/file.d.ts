/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:file
/** dyna:file — the filesystem: Path values, whole-file and buffered IO, atomic writes, locks, watchers, globs, temp files, platform directories. */
declare module "dyna:file" {
    /** A value handle over one normalized path; immutable, refcounted. Every dyna:file entry point that takes a path accepts a Path or a string; a string is converted exactly as new Path(s) would (same normalization, same errors), and any other value is a TypeError naming the Path argument. */
    class Path {
        /** Joins and normalizes the segments; at least one segment is required. An absolute result then has its parent directory canonicalized through realpath(3): symlinks in the parent are followed (on macOS /tmp/x becomes /private/tmp/x); a parent that does not exist leaves the path as joined; the final component is never followed, except when the parent is "/" and the whole path resolves. dirname and every Path the module returns (cwd, home, temp, makeTempDir, glob, ...) get the same treatment; join() and resolve() are lexical, so new Path(p, "x") and p.join("x") differ when p ends in a symlink; relative paths are untouched. Kept deliberately: one realpath per absolute Path. */
        constructor(first: string | Path, ...segments: (string | Path)[]);
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
        /** join that resolves `.`/`..` LEXICALLY (absolute argument rebases); no filesystem/symlink access; NUL in a segment is a TypeError. The result drops a trailing slash. A relative result stays relative and is never rooted at "/" or the cwd: new Path("tools/x.js").resolve() is "tools/x.js", new Path("a").resolve("../../b") is "../b", an empty result is ".". (Before B11 a relative receiver came back rooted at "/".) */
        resolve(...segments: (string | Path)[]): Path;
        /** The relative path from `this` to `other`. */
        relativeTo(other: Path | string): Path;
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
        moveTo(dest: Path | string): this;
        /** Byte copy to a new File; refuses an existing destination unless `{overwrite: true}`. */
        copyTo(dest: Path | string, opts?: { overwrite?: boolean }): File;
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
        constructor(path: Path | string, opts?: { bufferSize?: number });
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
        constructor(path: Path | string, opts?: { bufferSize?: number; preallocate?: number; append?: boolean });
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

    /** Kernel-event file watching (kqueue/inotify); events { path, kind } reach both the start() callback and async iteration. Each `ignore` pattern is matched like Glob (braces expand; past 4096 alternatives the constructor throws RangeError) against the relative path and the bare name. */
    class Watcher implements DynResource {
        constructor(path: Path | string, opts?: { recursive?: boolean; debounceMs?: number; ignore?: string[] });  /* opts must be an object when given: a string/number throws TypeError */
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

    /** A compiled glob for LEXICAL whole-path matching (no filesystem access): braces expand as in glob() (a path matches if any alternative does; past 4096 alternatives the constructor throws RangeError), `*`, `**` and `?` all cross `/` and no leading-dot rule applies, unlike glob()'s walk; a `**` followed by `/` needs that slash, so the recursive `.tmp` pattern misses a top-level `a.tmp` that `*.tmp` matches. */
    class Glob {
        constructor(pattern: string);
        /** Lexical match only, no filesystem access. */
        matches(path: Path | string): boolean;
        /** One-shot `new Glob(pattern).matches(path)`; a Path contributes its normalised bytes, a string is used verbatim. */
        static match(path: Path | string, pattern: string): boolean;
        /** The glob() walk with an optional cwd; returns matching Paths. */
        expand(cwd?: Path | string): Path[];
        /** The subset of the array matching the pattern. */
        filter<T extends Path | string>(paths: T[]): T[];
        readonly pattern: string;
        /** True when the pattern has `*`, `?`, `[` or a brace group that expands to more than one alternative. */
        readonly hasWildcard: boolean;
    }

    /** Whole file as a string (invalid UTF-8 becomes U+FFFD); `{bytes: true}` returns a Uint8Array. Symlinks anywhere in the path are followed. */
    function readFile(path: Path | string, opts?: { bytes?: boolean }): string | Uint8Array;
    /** Whole file as a Uint8Array; same as `readFile(path, {bytes: true})`. */
    function readBytes(path: Path | string): Uint8Array;
    /** Write string or bytes; O_CREAT, truncate unless append; returns the byte count; undefined/null is a TypeError. A string is written as well-formed UTF-8: a lone surrogate becomes U+FFFD (EF BF BD), as TextEncoder does.
     *  `{atomic: true}` never exposes a half-written file: the data goes to a new temporary file beside the target, is fsync'd, then renamed over it (and the directory fsync'd), so a reader or a crash sees the old content or the new, never a mix.
     *  The result is a NEW inode (an open handle or hard link keeps the old content); an existing target's permission bits carry over, its owner does not; a symlinked path replaces the link's target, not the link. On failure the target is untouched and the temporary removed. With append: TypeError. */
    function writeFile(path: Path | string, data: BytesInput, opts?: { append?: boolean; atomic?: boolean }): number;
    /** Promise twins of readFile, readBytes, writeFile and the 14 path calls (stat..chmod, same results and error shapes), plus open(), reader() and writer(); transitional, F5 gives them the default names. Disk I/O and UTF-8 repair run on the I/O pool.
     *  Argument errors throw synchronously; I/O errors reject with the sync error shape; a pending request rejects at engine shutdown. 4 KiB read: 22 us awaited one by one (sync 14 us, empty pool hop 3.6 us), 13 us with 64 in flight; RSS +0.4 MiB, flat. */
    namespace async {
        /** A relative path resolves against the cwd when the pool runs the request, not at the call. With no I/O reactor the call rejects InternalError; no disk I/O runs on the JS thread. */
        function readFile(path: Path | string, opts?: { bytes?: boolean }): Promise<string | Uint8Array>;
        function readBytes(path: Path | string): Promise<Uint8Array>;
        /** Un-awaited requests run in parallel in no set order and the last to finish wins; an atomic write is always one whole payload, plain writes to one path may interleave. */
        function writeFile(path: Path | string, data: BytesInput, opts?: { append?: boolean; atomic?: boolean }): Promise<number>;
        /** Sync stat/lstat/exists stay (2x rule): warm-cache p50 sync 2 us vs async 6-16 us awaited (>=3x), 2-7 us each with 64 in flight; N=16..1M, peak RSS 7.3 MiB flat.
         *  A sync call blocks the JS thread on a slow or network filesystem; use async there. */
        function stat(path: Path | string): Promise<Stat>;
        function lstat(path: Path | string): Promise<Stat>;
        function exists(path: Path | string): Promise<boolean>;
        function readDir(path: Path | string): Promise<{ name: string; isDir: boolean; isFile: boolean; isSymlink: boolean }[]>;
        function makeDir(path: Path | string, opts?: { recursive?: boolean; mode?: number }): Promise<void>;
        function remove(path: Path | string): Promise<void>;
        function removeAll(path: Path | string): Promise<void>;
        function rename(from: Path | string, to: Path | string): Promise<void>;
        function copyFile(from: Path | string, to: Path | string, opts?: { overwrite?: boolean }): Promise<number>;
        function move(from: Path | string, to: Path | string): Promise<void>;
        function symlink(target: string, linkpath: Path | string): Promise<void>;
        function readLink(path: Path | string): Promise<string>;
        function realPath(path: Path | string): Promise<Path>;
        function chmod(path: Path | string, mode: number): Promise<void>;
        /** flags "r" (default) | "r+" | "w" | "w+" | "wx" | "a" | "a+", O_CLOEXEC always; mode 0..0o7777 (default 0o666, else RangeError) for a created file. */
        function open(path: Path | string, opts?: { flags?: "r" | "r+" | "w" | "w+" | "wx" | "a" | "a+"; mode?: number }): Promise<FileHandle>;
        /** An open fd from open(): its calls run one at a time on the I/O pool in call order; after close() every call rejects EBADF.
         *  Dropped unclosed, GC closes the fd on the JS thread; at engine shutdown queued calls reject and the fd closes after the in-flight one. */
        interface FileHandle {
            /** Up to n bytes (default 65536, 0..2^30 else RangeError), short only at EOF; pos reads there without moving the cursor. */
            read(n?: number, pos?: number): Promise<Uint8Array>;
            /** Fills view (copied in at completion: only what still fits a shrunk view; a detached view rejects TypeError); resolves the count. */
            read(view: ByteView, pos?: number): Promise<number>;
            /** Writes all of data (copied at the call); pos writes there without moving the cursor (O_APPEND handles write at the end). */
            write(data: BytesInput, pos?: number): Promise<number>;
            /** Sync lseek, whence "set" (default) | "cur" | "end"; returns the new offset; TypeError while a call is queued or in flight. */
            seek(offset: number, whence?: "set" | "cur" | "end"): number;
            /** F_FULLFSYNC on Darwin (fsync where refused), fsync elsewhere. */
            sync(): Promise<void>;
            stat(): Promise<Stat>;
            /** Runs after the queued calls; a second close() resolves at once. */
            close(): Promise<void>;
            readonly closed: boolean;
            [Symbol.asyncDispose](): Promise<void>;
        }
        /** Buffered FileReader twin; bufferSize as FileReader (128 KiB, clamped 4 KiB..64 MiB); open, fills and close run on the I/O pool.
         *  With nothing queued, a line or read(n) already buffered settles on the JS thread (tens of ns vs a ~4 us pool hop, measured). */
        function reader(path: Path | string, opts?: { bufferSize?: number }): Promise<AsyncFileReader>;
        /** Buffered FileWriter twin (mode 0644, truncates unless append); preallocate reserves bytes, failure ignored.
         *  With nothing queued, a write that fits the buffer is copied on the JS thread; flushes and larger writes run on the I/O pool. */
        function writer(path: Path | string, opts?: { bufferSize?: number; preallocate?: number; append?: boolean }): Promise<AsyncFileWriter>;
        /** Calls queue in order; strings repair invalid UTF-8 to U+FFFD; after close, reads reject EBADF and next/return resolve done. */
        interface AsyncFileReader {
            /** Up to n bytes (0..2^30 else RangeError) as a string, "" at EOF; n omitted reads all, over 2^30 rejects RangeError. */
            read(n?: number): Promise<string>;
            /** The rest of the file; over 2^30 bytes rejects RangeError. */
            readAll(): Promise<string>;
            /** The next line without \n or \r\n; null at a clean EOF; a line over 2^30 bytes rejects RangeError. */
            readLine(): Promise<string | null>;
            /** Fills view with raw bytes (copied in at completion, only what still fits; detached rejects TypeError); resolves the count, 0 = EOF. */
            readInto(view: ByteView): Promise<number>;
            /** Up to n raw bytes (0..2^30 else RangeError); n omitted reads all. */
            readBytes(n?: number): Promise<Uint8Array>;
            /** The next line as an iterator result; done at EOF. */
            next(): Promise<IteratorResult<string, undefined>>;
            /** Closes the reader (for-await break calls it); resolves done. */
            return(): Promise<IteratorResult<string, undefined>>;
            lines(): AsyncFileReader;
            [Symbol.asyncIterator](): AsyncFileReader;
            /** Runs after the queued calls; a second close() resolves at once. */
            close(): Promise<void>;
            readonly closed: boolean;
            [Symbol.asyncDispose](): Promise<void>;
        }
        /** Calls queue in order; buffered bytes are flushed at close, at runtime teardown and when collected; after close, calls reject EBADF. */
        interface AsyncFileWriter {
            /** Copies data at the call; resolves bytes accepted; data of at least bufferSize goes straight to the fd. */
            write(data: BytesInput): Promise<number>;
            /** Pushes buffered bytes to the fd. */
            flush(): Promise<void>;
            /** Flush then F_FULLFSYNC on Darwin (fsync where refused), fsync elsewhere. */
            sync(): Promise<void>;
            /** Flushes then closes after the queued calls; a second close() resolves at once. */
            close(): Promise<void>;
            readonly closed: boolean;
            [Symbol.asyncDispose](): Promise<void>;
        }
    }

    /** Follows the final component. */
    function stat(path: Path | string): Stat;
    /** Does not follow the final component. */
    function lstat(path: Path | string): Stat;
    /** Boolean; never throws; uses lstat, so a dangling symlink reports true. */
    function exists(path: Path | string): boolean;

    /** Sorted entries; `.`/`..` excluded. */
    function readDir(path: Path | string): { name: string; isDir: boolean; isFile: boolean; isSymlink: boolean }[];
    /** Create a directory; recursive creates missing parents. */
    function makeDir(path: Path | string, opts?: { recursive?: boolean; mode?: number }): void;
    /** Unlink a file or an empty directory. */
    function remove(path: Path | string): void;
    /** Recursive, symlink-safe removal; a missing path is a no-op. */
    function removeAll(path: Path | string): void;
    /** rename(2). */
    function rename(from: Path | string, to: Path | string): void;
    /** Byte copy through the kernel; refusing an existing destination is the default. Returns the source byte count. */
    function copyFile(from: Path | string, to: Path | string, opts?: { overwrite?: boolean }): number;
    /** rename(2), falling back to copy-then-unlink across filesystems; refuses a destination that resolves to the same file. */
    function move(from: Path | string, to: Path | string): void;
    /** MIME type from magic bytes, not the extension. */
    function sniffType(pathOrBytes: Path | string | ByteView): string;

    /** Creates the link; the link location is a Path or a string (converted like new Path), the target is stored verbatim; a target with an embedded NUL is a TypeError. */
    function symlink(target: string, linkpath: Path | string): void;
    /** The stored target verbatim, as a string. */
    function readLink(path: Path | string): string;
    /** The fully resolved path as a Path. */
    function realPath(path: Path | string): Path;
    /** Changes permissions. */
    function chmod(path: Path | string, mode: number): void;

    /** Walks the filesystem and returns sorted Paths: `*` stays within one segment, `**` spans any number (never through symlinked directories), `?`, `[a-z]`, `[!x]`.
     *  Braces expand first, as in bash: `{a,b}`, nested `{a,{b,c}}`, several groups and empty alternatives (`x{,.bak}`) each become a pattern and the results are merged, sorted and de-duplicated; a group with no top-level comma (`{x}`), an unbalanced `{` and `{1..3}` stay literal, a `{` inside `[..]` is literal (write `[{]` for a literal brace), and more than 4096 expansions throw RangeError.
 *  Wildcards skip dotfiles unless the segment starts with `.`; `{cwd}` roots the walk and the returned Paths are RELATIVE to it (re-join with cwd before stat/readFile). */
    function glob(pattern: string, opts?: { cwd?: Path | string }): Path[];

    /** The system temp directory as a Path. */
    function tempDir(): Path;
    /** mkdtemp under the temp dir; the prefix is a plain name fragment (a slash, a leading `.` or more than 200 bytes is a RangeError). */
    function makeTempDir(prefix?: string): Path;
    /** mkstemp; returns the path of the empty file. The file is created with O_CREAT|O_EXCL and mode 0600: the name did not exist before the call, so no existing file is ever opened, truncated or returned, and only the owner can read or write it. */
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
