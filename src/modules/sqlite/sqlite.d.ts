/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:sqlite
/** dyna:sqlite — an embedded SQLite handle over the linked libsqlite3 (the only module that pulls it in).
 *  Split out of dyna:net as a clean break: dyna:net no longer exports SQLite. Absent from builds without SQLite. */
declare module "dyna:sqlite" {
    /** SQLite options. */
    interface SQLiteOptions {
        readonly?: boolean;
        bigint?: boolean;
        /** Default true: a writable file database switches to WAL mode with synchronous=NORMAL when the file allows it (a lock held by another connection, or a filesystem without shared memory, keeps the rollback journal) (readers do not block the writer; a power loss can drop the last commits but never corrupts). false keeps SQLite's rollback journal and synchronous=FULL. In-memory and readonly opens are unchanged. A WAL database is the file plus its -wal file, so back it up with `VACUUM INTO`, not a file copy. */
        wal?: boolean;
    }
    /** A SQLite handle (defensive mode, trusted_schema off); values are bound, never interpolated; exec() returns rows changed; --timeout-ms interrupts a running statement. */
    class SQLite implements DynResource {
        constructor(path: string | import("dyna:file").Path, opts?: SQLiteOptions);
        /** Runs a statement and returns one object per row; duplicate column names collapse to the last one, so alias them in the SELECT. */
        query(sql: string, params?: unknown[]): Record<string, unknown>[];
        /** Runs a statement without result rows; returns the number of rows changed. */
        exec(sql: string, params?: unknown[]): number;
        /** Runs one statement once per row (each row a dense array of its parameters) inside a savepoint and returns the rows changed in total. All rows or none: a failing row (constraint, bad value, wrong parameter count) rolls the whole batch back and throws naming the row; an enclosing BEGIN stays open with its earlier work. Transaction control (BEGIN, COMMIT, SAVEPOINT, ...) and multi-statement text are refused. */
        execMany(sql: string, rows: unknown[][]): number;
        /** The rowid of the most recent successful INSERT. */
        readonly lastInsertRowId: number;
        /** The linked library's version, read at runtime. */
        readonly version: string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

