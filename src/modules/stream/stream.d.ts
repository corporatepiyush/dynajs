/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:stream
/** dyna:stream — thin sugar over WHATWG streams for bytes bigger than memory: fromBytes/fromFile/stdin are ReadableStreams, toFile/stdout/stderr are WritableStreams,
 *  lines()/ndjson() iterate chunk-safely in C, and toReadableStream/toWritableStream adapt any read(buf)/write(buf) object (subprocess pipes, HTTP getStream bodies).
 *  Compression is CompressionStream/DecompressionStream ("gzip", "deflate", "deflate-raw", "brotli", "zstd", "lz4"); copying is pipeTo/pipeThrough.
 *  v10b-19 BREAKING: pipe(), inflate(), deflate(), fromReadableStream(), fromWritableStream() and the ByteSource/ByteSink return types are gone. */
declare module "dyna:stream" {
    /** The duck-typed pull shape toReadableStream and lines()/ndjson() accept: read(buf) fills up to buf.length bytes and resolves the count, 0 = EOF; close() releases. */
    interface ByteSource {
        read(buf: Uint8Array): Promise<number>;
        close?(): void;
    }
    /** The duck-typed push shape toWritableStream accepts: write(view) resolves the count accepted (fewer is retried); flush() and close() are optional. */
    interface ByteSink {
        write(buf: ByteView): Promise<number>;
        flush?(): Promise<void>;
        close?(): void;
    }
    /** Options for `toFile`. */
    interface ToFileOptions {
        /** Append rather than truncate. */
        append?: boolean;
        /** Write buffer size, clamped to 4 KiB..64 MiB (default 128 KiB); allocated at the first write, so a never-written toFile holds none. */
        bufferSize?: number;
    }
    /** A byte ReadableStream over an in-memory COPY of `b` taken at the call (a string is encoded as UTF-8); chunks of at most 64 KiB; default and BYOB readers. */
    function fromBytes(b: Uint8Array | string): ReadableStream<Uint8Array>;
    /** A byte ReadableStream over a file. The path is checked at the call (a Path or a string without NUL, else TypeError); the file is opened LAZILY at the first read, so a
     *  missing file errors the stream (the read rejects with the errno error, e.g. code ENOENT). Sequential reads of at most 64 KiB, so FIFOs stream too; the fd closes at EOF,
     *  on cancel, on a read error and when the stream is collected. Piped into toFile (no prevent* option, no signal) the copy runs as a C loop with no promise per chunk. */
    function fromFile(path: import("dyna:file").Path | string): ReadableStream<Uint8Array>;
    /** A WritableStream of BufferSource or string (UTF-8) chunks over a file created LAZILY at the first non-empty write (truncate, or `{append: true}`): an unwritable path rejects
     *  that write and errors the stream. Chunks are written in C straight into the write buffer (a chunk of at least a quarter of it that would overflow goes out with the
     *  pending bytes in one writev, uncopied). close() flushes and closes the fd; abort() flushes what is buffered and closes; a write error is sticky. */
    function toFile(path: import("dyna:file").Path | string, opts?: ToFileOptions): WritableStream<ArrayBufferView | ArrayBuffer | string>;
    /** Options for `lines`. */
    interface LinesOptions {
        /** Only "utf-8" (the default) is supported; anything else is a TypeError; a value whose toString throws propagates that error. */
        encoding?: string;
        /** Yield string[] batches (one per source read) instead of single lines. */
        batch?: boolean;
    }
    /** Async iterable of lines, safe across ANY chunk boundary; strips CRLF, keeps empty lines, delivers an unterminated last line; `break` cancels the source.
     *  src is a ReadableStream of BufferSource/string chunks (locked by a reader that is released at the end; a locked stream is a TypeError) or an object with read(buf), which is read through toReadableStream (reads of at most 64 KiB, close() at EOF and on return(), a count outside [0, buf.length] or a non-number rejects next()); a live iterator holds no read buffer of its own.
     *  One C splitter (SIMD newline scan that resumes where the last chunk stopped, ASCII lines copied straight into 8-bit strings) serves both shapes. Per line, an already-buffered line
     *  is returned as an already-resolved promise and the source is read only when no complete line is buffered; next() while a read is pending is a TypeError.
     *  With `{batch: true}` each step yields string[]: every complete line from one source read (never empty, at most 65536), so a loop pays one promise per batch, not per line. */
    function lines(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts?: LinesOptions): AsyncIterableIterator<string>;
    function lines(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts: LinesOptions & { batch: true }): AsyncIterableIterator<string[]>;
    /** Async iterable of NDJSON values (sources and batching as for `lines`); blank lines are skipped; a malformed line rejects with its 1-based line number and iteration can resume after it.
     *  In batch mode the values before a malformed line arrive first as their own batch and the next step rejects with the error. */
    function ndjson(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts?: { batch?: false }): AsyncIterableIterator<unknown>;
    function ndjson(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts: { batch: true }): AsyncIterableIterator<unknown[]>;
    /** Options for `toReadableStream`. */
    interface ToReadableStreamOptions {
        /** Bytes per pull for a default reader (the buffer handed to src.read; a short read of under half of it is copied out at its length, a longer one keeps the buffer), 1..2^30; default 64 KiB. A BYOB reader's view is passed to src.read as is, whatever its size. */
        chunkSize?: number;
        /** Readable-side highWaterMark in bytes; default 0 (pull only on demand). */
        highWaterMark?: number;
    }
    /** Options for `toWritableStream`. */
    interface ToWritableStreamOptions {
        /** Chunks queued before backpressure; default 1. */
        highWaterMark?: number;
    }
    /** A WHATWG byte ReadableStream over a read(buf) object (a subprocess pipe, an HTTP getStream body, any duck source). Each pull calls src.read(view) on the BYOB view itself (no copy);
     *  read() resolving 0 closes the stream and src; cancel(), a rejected read() and a count outside [0, view.length] close src (the last two error the stream).
     *  Piped into a toWritableStream stream with no prevent* option and no signal, while both are idle, the pipe skips the per-chunk WHATWG machinery and runs a C pipe
     *  engine (128 KiB reads, short writes retried). At EOF src is closed, then the WritableStream closes (flush, then close() of the sink); on error both sides are closed and both streams error with it. */
    function toReadableStream(src: ByteSource, opts?: ToReadableStreamOptions): ReadableStream<Uint8Array>;
    /** A WHATWG WritableStream over a write(buf) object. Chunks are BufferSources or strings (UTF-8); a partial write() loops until the chunk is accepted (0 or more than offered is a TypeError).
     *  close() awaits sink.flush() when present, then sink.close(); abort() closes the sink. */
    function toWritableStream(sink: ByteSink, opts?: ToWritableStreamOptions): WritableStream<ArrayBufferView | ArrayBuffer | string>;
    /** This process's stdin as a byte ReadableStream: the SAME stream on every call until it closes, errors or is cancelled, then a fresh one (cancel settles a pending read done and leaves fd 0 open).
     *  A pipe, tty or socket is read when the event loop reports it readable (the JS thread never blocks, and a pending read keeps the loop alive); a regular file or a non-tty character device such as /dev/null is read directly,
     *  in C straight into the BYOB view or a fresh chunk (a chunk that fills under half of 64 KiB is copied to its exact size, otherwise it keeps the buffer, whose unread tail is zeroed).
     *  The fd is never switched to O_NONBLOCK (its open file description is shared with the parent). */
    function stdin(): ReadableStream<Uint8Array>;
    /** A new WritableStream over this process's stdout per call. Small writes are coalesced, like bun (owner decision D5):
     *  - A chunk under 16 KiB is copied into one process-wide buffer for fd 1 (at most 64 KiB, shared by every stdout() stream and every thread) and its write
     *    resolves at once, before the bytes reach the fd. It costs no allocation; an unused buffer costs no memory.
     *  - The buffer is written out when the next chunk would overflow it; when the event loop turns or goes idle (before it polls, so a timer, an awaited read
     *    or the end of the script flushes it); before a chunk of 16 KiB or more, which goes out with the buffered bytes in one writev, uncopied; on writer.close();
     *    before console.*, print, std.puts/printf/printObject, any std.out/std.err method, os.write/os.close/os.dup2 on fd 1 or 2, dyna:term output and dyna:log
     *    stderr output; before os.exec and dyna:sys Exec/Spawn fork; before an uncaught error or unhandled rejection is printed; at std.exit; and at process exit.
     *    A --timeout-ms hard stop (exit 113) flushes only when no other thread holds the buffer. Code that writes fd 1 or 2 by other means may overtake buffered bytes.
     *  - Order: bytes reach the fd in call order across stdout(), stderr(), console and std.out/std.err (C stdio output buffered by std.out is flushed before a
     *    chunk is buffered). Writing to stderr first flushes stdout and writing to stdout first flushes stderr, so the two never reorder on a shared terminal or a 2>&1 pipe.
     *    console.log itself stays unbuffered (written and flushed per call).
     *  - Errors stick to the fd for the rest of the process: a write error found by a flush (EPIPE once the reader is gone, ENOSPC, ...) rejects the next write,
     *    writer.close() and any later stdout() stream with TypeError "stdout: write failed: <strerror>", and the stream errors with it. An error that no write or
     *    close() reported is printed at exit as "TypeError: stdout: write failed: <strerror>" and the exit status becomes 1 (std.exit(n) keeps a non-zero n), as an
     *    uncaught rejected write did before D5. The buffered bytes of a failed flush are dropped.
     *  - On a non-blocking fd (shared with another process), EAGAIN while flushing the buffer waits with poll(POLLOUT); EAGAIN on a large chunk parks the rest of
     *    that chunk, copied, until the fd is writable, and the write settles when it is all written.
     *  close() leaves fd 1 open. */
    function stdout(): WritableStream<ArrayBufferView | ArrayBuffer | string>;
    /** stderr (fd 2) as a WritableStream, with stdout()'s rules: its own coalescing buffer and sticky error ("stderr: write failed: ..."); a stderr write flushes stdout first. */
    function stderr(): WritableStream<ArrayBufferView | ArrayBuffer | string>;
}

