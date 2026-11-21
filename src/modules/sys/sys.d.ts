/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:sys
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
    /** CPU time of the CALLING thread in fractional milliseconds (clock_gettime CLOCK_THREAD_CPUTIME_ID, nanosecond resolution on Linux and macOS): time a descheduled thread does not accrue, and helper threads are not counted. One call costs about 0.15 us on macOS, against about 0.4 us plus an object for cpuUsage(). Use it to time a function under a loaded host. */
    function threadCpuNow(): number;
    /** The full getrusage(RUSAGE_SELF) struct; user/system in seconds, maxrss normalised to BYTES; the fault/switch counters are kernel-specific. */
    /** Process-wide counters of the fork-join helper threads some natives use inside one call (Argon2id lanes, BLAKE3 over 128 KiB, lz4Frame over one 4 MiB block, DataFrame radix sorts from 98,304 rows): runs, serial runs, helpers spawned, spawn failures, helpers live now, the high-water mark, and the helper budget (CPUs - 1). `--io-threads 0` makes every such call serial. */
    function parStats(): { runs: number; serialRuns: number; spawned: number; spawnFailures: number; live: number; peak: number; budget: number };
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
    /** The engine's allocation counters, OS peakRss, and the native ledger: nativeSize (libc memory held by dyna:* modules; dyna:structures and other library-backed allocations count only while a limit is set) and nativeLimit (the cap in force, 0 = none). */
    function memoryUsage(): { mallocCount: number; mallocSize: number; memoryUsedCount: number; memoryUsedSize: number; objCount: number; objSize: number; strCount: number; strSize: number; propCount: number; shapeCount: number; arrayCount: number; peakRss: number; nativeSize: number; nativeLimit: number };
    /** Caps module-native memory at `bytes` (0 = uncapped); allocation past it throws out-of-memory. RRule and the Temporal classes are accounted like every other native class.
     *  Under an operator ceiling (--native-memory-limit) a script may only LOWER the cap; memory owned by linked libraries (OpenSSL, SQLite) is not counted.
     *  A typed-array result a module computes natively (Graph analytics and exportCSR weights, CsvIndex.column, mathx stats/geo arrays, DataFrame columns) moves off the native ledger when it is returned: it counts toward the engine heap (mallocSize, the engine memory limit) for its lifetime, never toward nativeSize, so collecting it never leaves the ledger high. */
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
        /** REPLACES the child's environment entirely (execve semantics), unless extendEnv is true. */
        env?: Record<string, string>;
        /** true: the child gets this process's environment with `env` laid over it (an `env` key wins; PATH and HOME survive), e.g. `Exec("git", ["log"], { env: { GIT_PAGER: "cat" }, extendEnv: true })`. Default false. Must be a boolean (TypeError otherwise). */
        extendEnv?: boolean;
        input?: BytesInput;
        timeoutMs?: number;
        /** Per-stream stdout/stderr cap in bytes (default 8 MiB); crossing it kills the child and throws a RangeError naming the stream.
         *  Big outputs (a whole-tree `git diff`, `git show` of a large file) pass it: `Exec("git", ["diff"], { maxBuffer: 256 << 20 })`; unbounded output belongs in Spawn, which streams. */
        maxBuffer?: number;
        encoding?: "utf8" | "bytes";
        /** Drops the child's credentials after chdir (gid before uid); non-negative integers. */
        uid?: number;
        gid?: number;
    }
    /** Runs a program to completion, BLOCKING the thread, and returns its ExecResult; throws when it cannot spawn.
     *  Child lifetime (Exec and Spawn alike): every child runs in its own session and process group (setsid), so a group signal reaches its
     *  descendants, and this process tracks each live child group until it reaps it. When this process ends it takes them down: on a normal
     *  exit and on an uncaught-error exit (atexit), and on SIGTERM, SIGINT or SIGHUP (a handler installed at the first child, only over the
     *  default disposition: a signal that is ignored, or that os.signal handles, is left alone), it sends each group SIGTERM, waits up to 2 s
     *  reaping, then SIGKILLs what is left, and a signal is then re-raised so this process still dies BY it. The --timeout-ms hard stop
     *  (exit 113) SIGKILLs the groups at once. Not covered: SIGKILL of this process, abort(), and children beyond 4096 live at once.
     *  There is no detached mode: a child meant to outlive its parent needs a daemonizing wrapper of its own. */
    function Exec(command: string, args?: string[], options?: ExecOptions): ExecResult;
    /** Resolves a program name against PATH; null when not found. */
    function Which(name: string): string | null;

    /* ---- Spawn: the async child with streaming stdio ------------ */

    /** One output pipe in ByteSource shape (read(buf) resolves a count, 0 = EOF). Buffered up to `maxPipe`, then the child BLOCKS: true backpressure. One read at a time.
     *  dyna:stream consumes it directly: `for await (const line of lines(p.stdout))`, `ndjson(p.stdout)`, `toReadableStream(p.stdout).pipeTo(toFile(path))`. */
    interface SpawnPipe {
        read(buf: Uint8Array): Promise<number>;
        /** `for await (const chunk of p.stdout)`: each chunk is a fresh Uint8Array copy (at most 64 KiB) in arrival order; the loop ends at EOF; `break` (or return/throw out of the loop) closes the pipe. */
        [Symbol.asyncIterator](): AsyncIterableIterator<Uint8Array>;
        /** Stop draining. A pending read() is rejected, not left hanging; bytes already buffered are dropped, so a later read() resolves 0 (EOF), also after a for-await `break`. */
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
        /** Replaces the child's environment, unless extendEnv is true (same rules as ExecOptions). */
        env?: Record<string, string>;
        /** true: this process's environment with `env` laid over it; default false. */
        extendEnv?: boolean;
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
     *  A dropped or closed Spawn SIGKILLs and reaps the child GROUP; pending reads and wait() still settle. No zombies.
     *  A read(), flush() or wait() still parked when the engine shuts down (an uncaught error ends the script) is rejected with "... aborted at
     *  engine shutdown", so the process exits with the script's error, never an abort. The child's lifetime against this process: see Exec. */
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
        /** Signals the child GROUP with a number (1..31) or name (default SIGTERM); true when the child was still running. After TERM/KILL/INT/HUP/QUIT, reads reach EOF once the child is reaped even if a surviving descendant holds the pipe. */
        kill(signal?: number | string): boolean;
        /** Deterministic teardown: SIGKILL the group, reap, release everything, settle a parked wait(). */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }
}
