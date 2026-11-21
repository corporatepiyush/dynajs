/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block os
/** os — QuickJS-compatible POSIX layer (fds, processes, signals, timers), only under --std; calls return a negative errno instead of throwing.
 *  os.Worker is the parallelism primitive: a separate runtime with its own heap, exchanging structured-clone messages. */
declare module "os" {
    /** The number of milliseconds since an arbitrary point, microsecond resolution; the same clock as performance.now() and every timer deadline (macOS: mach_continuous_time, which keeps counting across sleep like CLOCK_MONOTONIC; elsewhere CLOCK_MONOTONIC). Each arming (setTimeout, setInterval re-arm, sleep) reads it fresh, so no timer fires before its delay as measured by this clock; a timer already due by the last reading fires without reading it again when no I/O, worker port or reactor is registered. */
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
    /** A File backed by the regular file at path, read lazily: nothing is read or mapped at open, each read (slice bytes, text(), arrayBuffer(), stream() 64 KiB chunks, FileReader) preads only the bytes it needs from the path of the descriptor opened at open (F_GETPATH or /proc/self/fd, else realpath), so streaming a large file keeps memory near one chunk. name = the basename, lastModified = mtime, type from options (normalized like Blob type).
     *  File API snapshot: the device, inode, size and mtime are recorded at open; a read after any of them changed (rewrite, append, truncate, replace) fails with a NotReadableError DOMException (text()/arrayBuffer()/bytes() reject, stream() errors, FileReader fires error) instead of returning the new bytes. Each read stats the path (the identity check); the read-only fd opened to take the snapshot stays open as the File's read fd, so open + first read cost one open() (at most 64 such fds across all Files; beyond that the snapshot fd closes and each read opens and closes its own), released when the File is collected; reads of 4 KiB or less within one aligned 64 KiB block go through a single per-thread 64 KiB block shared by all Files (freed when the last File is collected), so a live File costs no block of its own. TypeError for a missing file or a non-regular file. */
    function openAsBlob(path: string, options?: { type?: string }): File;
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
    /** Counters of THIS thread's event loop since start: blocking waits entered, timer callbacks, fd-handler callbacks, Worker messages delivered, cross-thread completions run, signal handlers run; `watched` is how many fd handlers sit in the kqueue/epoll set (0 below 16 handlers, where plain poll() is cheaper). `ioWaits` counts this thread's native I/O reactor calls into the kernel readiness API (kevent/epoll_wait/epoll_ctl/poll) and `ioChanges` the interest changes they carried: on kqueue, fd interest changes are queued and submitted with the next wait in one kevent call (one syscall per loop turn), so a change undone within the same turn costs nothing. On io_uring builds `ioWaits` and `ioChanges` are always 0: the io_uring reactor does not count its submissions. An idle process must not grow `waits`. */
    function loopStats(): { waits: number; timers: number; fdEvents: number; messages: number; posts: number; signals: number; watched: number; ioWaits: number; ioChanges: number };
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
        /** Optional transfer list: buffers are copied to the receiver and detached on the sender; a SharedArrayBuffer is refused. An array is written index by index with holes as undefined, so one with more than 2^24 holes beyond its real elements throws RangeError.
         *  ReadableStream, WritableStream and TransformStream are transferable: the sender's stream is locked and piped over a private native port pair, and the receiver gets a new stream
         *  wherever the original sat in `value` (in its arrays and objects; an object holding one is copied as a plain object). Map and Set are not structured-cloneable here at all (TypeError "unsupported object class", with or without streams in them), so a stream inside one is refused like any other unclonable value. A stream in `value` but not in the list is a TypeError ("unsupported object class"), as are a locked stream and one listed twice;
         *  a top-level "@@dyna.streams.envelope" key marks a message that carries streams (do not use it in your own messages), and "@@dyna.streams.ref" objects stand for the streams inside one.
         *  Nothing is locked if the post fails. Chunks are structured-cloned one per pull (pull-driven, as the spec's cross-realm transform); an Error reason crosses as its class and message.
         *  A receiver that exits without reading makes the sender's side error with "the other side of a transferred stream went away". */
        postMessage(value: unknown, transfer?: (ArrayBuffer | ArrayBufferView | ReadableStream | WritableStream | TransformStream)[]): void;
        /** A live message port keeps the process alive even after the worker exits; set to null to release it. */
        onmessage: ((ev: { data: unknown }) => void) | null;
        onerror: ((err: unknown) => void) | null;
    }
}
