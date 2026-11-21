/// <reference path="../src/types/prelude.d.ts" />
//@dts-block */src/pool.js
/** src/pool.js — PgPool and RedisPool: pure-JS pooling over the dyna:postgres and dyna:redis clients, shipped as a source file, NOT a builtin.
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
        with<T>(fn: (client: import("dyna:postgres").PostgreSQL) => Promise<T>): Promise<T>;
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

