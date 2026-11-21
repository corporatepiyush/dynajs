/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:async
/** dyna:async — promise combinators and bounded handoff structures: sleep, withTimeout, retry, Semaphore, Channel, pmap, Pool, debounce, throttle.
 *  Everything runs on the event-loop thread: this is CONCURRENCY-limited interleaving, not parallelism. Bad arguments throw synchronously.
 *  Native design (no JS objects per waiter): Semaphore waiters, Channel/Queue buffers, Channel blocked senders and Pool jobs live in power-of-two ring buffers inside the C object; acquire() hands out one callable native release object per grant; pmap keeps its results array and counters in one C record and subscribes to each job with a native promise reaction (no derived promise per item); debounce/throttle return one callable native object holding the timer, the pending call's receiver and arguments (a C array) and the throttle edge; cancel() and pending() live on its shared prototype, which inherits Function.prototype, so call/apply/bind work. Ring buffers grow by realloc (the wrapped head segment moves to the new end), so peak memory stays one buffer. The prototypes carry no internal helper methods. */
declare module "dyna:async" {

    /** On-demand concurrency without ...Async twins: `offload(fn, ...args)` calls the native dyna:* function `fn(...args)` and returns a Promise of its result. Each native names the stretch of its own work that is safe off the loop thread (it works on a private copy of its inputs and touches no JS value there); under offload that stretch runs on a worker thread while the event loop keeps serving, and the result is built back on the loop thread. A native without such a stretch still runs, on the loop thread, and its promise settles the same way. Called directly, the same function is unchanged: synchronous, same result, same errors. Offloadable today: every one-shot codec of dyna:compress (gzip, gunzip, zstd, unzstd, brotli, unbrotli, snappy, unsnappy, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, deflate, inflate; archives build JS per entry and stay on the loop); the dyna:crypto password KDFs (Bcrypt.hash/verify, Scrypt, PBKDF2, Argon2id.hash/verify), which already return Promises and run on the pool when called directly, so offload() of one adds nothing but resolves to the same value; every one-shot digest of dyna:hash and its Hex form (all of the module but the streaming Hasher and CDCChunks); dyna:file readFile, readBytes and writeFile (the open and read or write; the result string or bytes are built on the loop); dyna:csv parseDataFrame (the scan into typed columns and a string dictionary; the DataFrame is wrapped on the loop). Offloaded work runs as a job on the shared io pool sized by --io-threads (CPU work and file IO share it; no thread is created per call); with --io-threads 0 there is no pool and offloaded work runs inline on the loop thread, settling the promise the same way. The other parsers (JSON.parse, MessagePack, csv.parse, XML, YAML) and docdb queries are not offloadable: the parsers build JS values as they scan (measured: 40-80% of a parse is value building, interleaved with the scan), and a docdb query.s matches index live collection slots. A JS function (or a bound one) is a TypeError: running JS mid-call off the loop would interleave it with other callbacks. `fn` is called with no receiver, so a method (a docdb Collection query, CSVFile#toDataFrame) rejects with its own TypeError (e.g. "Collection object expected") and cannot be offloaded. Errors from `fn`, including argument errors, reject the promise. The input is copied when the call starts (and its ArrayBuffer cannot be transferred until the call ends), so changing the buffer afterwards does not change the result. Cost: about 7-10 us per call on top of the work (one pool hop each way), plus the input copy for a buffer input (about 20 us per MiB); it pays from about 50-100 us of work (gzip or SHA-256 of about 64 KiB). Very fast functions (XXH3_64, CRC32 at any size; any call under about 64 KiB) are better made directly, and BLAKE3 over 1 MiB already uses every core on its own. Error backtraces from the offloaded call stop at `fn`. */
    function offload<A extends unknown[], R>(fn: (...args: A) => R, ...args: A): Promise<R>;

    /** Resolves after `ms` milliseconds (setTimeout). */
    function sleep(ms: number): Promise<void>;

    /** Rejects if `p` does not settle within `ms` (`reason` replaces the message); p's late rejection never surfaces as unhandled. */
    function withTimeout<T>(p: PromiseLike<T>, ms: number, reason?: string): Promise<T>;

    /** Integer arguments must fit int32, as the original `(x | 0) === x` checks: Semaphore n, Pool n, Queue/Channel cap and pmap concurrency are integers 1..2147483647 (cap may also be Infinity), retry `retries` is 0..2147483647. Anything else is a TypeError (a non-number, NaN or out-of-range count) or a RangeError (an out-of-range cap), never truncated or wrapped. Timer delays (sleep, withTimeout, debounce/throttle ms, backoffMs) must be finite and non-negative (else TypeError); a delay above 2147483647 ms is clamped to 2147483647 by setTimeout. */
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

    /** MPSC channel: send() backpressures at capacity; recv() resolves {value, done:false}, then {done:true} after close() and a full drain. SINGLE consumer: a second concurrent for-await throws a TypeError at the loop. */
    class Channel<T = unknown> {
        constructor(cap?: number);
        get length(): number;
        get closed(): boolean;
        send(v: T): Promise<void>;
        recv(): Promise<{ value: T; done: false } | { done: true }>;
        close(): void;
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
    }

    /** Trailing debounce: the last call inside the `ms` window runs once at its edge; cancel() drops a pending call. */
    function debounce<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };

    /** Leading throttle with a trailing call: the first call runs now, the last call in the window runs at its edge. */
    function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };
}
