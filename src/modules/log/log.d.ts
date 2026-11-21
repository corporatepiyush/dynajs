/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:log
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
            /** Output path (string or Path), or a sink `(line) => void` receiving each formatted line unbuffered; rollover needs a path. Default: stderr. */
            dest?: string | import("dyna:file").Path | ((line: string) => void);
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
            /** File rollover (needs dest). Lazy: checked before each emit. Lines go to numbered files beside dest (`app.log` -> `app.1.log`, ...); `dest` itself exists only with `symlink`. */
            rollover?: {
                /** Rotate at this size: bytes, or "500k"/"10m"/"2g". */
                size?: number | string;
                /** Rotate on this interval: "daily", "hourly", or milliseconds. */
                frequency?: "daily" | "hourly" | number;
                /** Rotated files to keep (plus the active one). */
                count?: number;
                /** Keep `dest` as a symlink to the active file; a pre-existing symlink is replaced, never written through. readFile follows it, so dest reads the active file. */
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
