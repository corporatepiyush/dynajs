/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:postgres
/** dyna:postgres — a PostgreSQL client over the extended protocol (SCRAM-SHA-256/MD5 auth, TLS, pipelining, a prepared-statement cache).
 *  Split out of dyna:net as a clean break: dyna:net no longer exports PostgreSQL. Server replies are untrusted and bounded. */
declare module "dyna:postgres" {
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
        /** numeric columns: "string" (default, exact server text) or a constructor called as new C(text), e.g. Decimal from dyna:decimal; NaN/Infinity become numbers; a throwing constructor rejects the query. */
        numeric?: "string" | (new (text: string) => unknown);
        /** "string" (default) or "date": date/timestamp/timestamptz become Date (UTC; microseconds truncated to ms; outside Date range: ISO string), infinity ±Infinity,
         *  interval { months, days, microseconds } (bigint past 2^53); text results need DateStyle ISO and IntervalStyle postgres, else stay strings. JS thread: C 23/25/11 ns per int/quoted-text/binary element; end to end +0.1..0.7 µs/row vs strings (int4[10], text[5], jsonb; 1e3-1e5 rows), under the ~3.6 µs pool hop. */
        dates?: "string" | "date";
        /** json/jsonb: "string" (default, server text) or "parse" (JSON.parse on the JS thread; numbers past 2^53 lose precision; malformed JSON rejects the query). */
        json?: "string" | "parse";
        /** "string" (default, PG text such as {1,"a b",NULL}) or "array": arrays of bool/bytea/int2/4/8/float4/8/oid/text/varchar/bpchar/numeric/date/timestamp(tz)/interval/uuid/json(b) become nested arrays
         *  (lower bounds dropped, max 6 dims; malformed text stays a string); elements decode like the column type (bigint, numeric, dates, json, bytes). JS thread: C 23.1/24.6/10.6 ns per int/quoted-text/binary element, +-3.64 µs/row (int4[10], text[5], jsonb) at 1e5 rows, under the ~3.6 µs pool hop. */
        arrays?: "string" | "array";
        /** "string" (default) or "object": int4/int8/num/ts/tstz/date ranges become { empty, lower, upper, lowerInclusive, upperInclusive } (unbounded side null; bounds decode like
         *  the subtype: bigint, numeric, dates), multiranges arrays of them (empty members dropped); malformed text stays a string. JS thread: C 13.4/88.0/3.2 ns per int4/quoted-tstz/binary range; end to end within ±0.1 µs/row of strings (int4range, tstzrange, int4multirange; 1e3-1e5 rows), under the ~3.6 µs pool hop. */
        ranges?: "string" | "object";
        /** Numeric IPv4/IPv6 address(es), a comma list paired with host (host then names TLS verification and the .pgpass match; tls needs host). libpq 18 hostaddr. */
        hostaddr?: string;
        /** libpq 18 target_session_attrs (default "any"; url key, PGTARGETSESSIONATTRS): read-write/read-only check ParameterStatus default_transaction_read_only + in_hot_standby, else SHOW transaction_read_only; primary/standby check
         *  in_hot_standby, else pg_is_in_recovery() (server < 9.0 = primary); a mismatch sends Terminate and tries the next host; prefer-standby then retries the list as any. Invalid: RangeError. */
        targetSessionAttrs?: "any" | "read-write" | "read-only" | "primary" | "standby" | "prefer-standby";
        /** libpq 18 load_balance_hosts (default "disable"; url key, PGLOADBALANCEHOSTS): "random" shuffles the host list once per client (Fisher-Yates, OS-entropy seed). Invalid: RangeError. */
        loadBalanceHosts?: "disable" | "random";
        /** libpq URI postgres[ql]://user:pass@host:port/db?k=v (≤8192 bytes; bad %-escape, %00, inner space, unknown key: TypeError); options win, then url, then env. Lists per libpq 18 34.1.1.3 (≤1024 hosts, else RangeError): host/hostaddr/port comma lists, 1 or N ports, empty = default, else libpq pqParseIntParam (blanks, sign; non-integer or outside 1..65535: RangeError); tried in order, next host on refused, per-host connect_timeout or 57P03, never after an auth or TLS error; all failed: queries reject (a dead client throws) with every host's reason.
         *  Keys host port user password dbname application_name sslrootcert sslnegotiation connect_timeout(s) target_session_attrs load_balance_hosts ssl=true sslmode: disable | require/verify-ca/verify-full (= tls: true, always verified); allow/prefer throw RangeError (downgradable to plaintext). */
        url?: string | URL;
        /** Default true: unset host/port/user/password/database come from PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE, then the password from PGPASSFILE or $HOME/.pgpass
         *  (libpq format; skipped unless a regular file without group/other bits, at most 1 MiB, no NUL; no host or a socket matches localhost). Read on the I/O pool before the connect (errors reject queries): open+read+match missing=1296 small=13424 lines10k=544390 ns. */
        env?: boolean;
        /** Default false: MD5/cleartext auth is refused on a plaintext connection unless true (accepted under `tls: true`). */
        insecureAuth?: boolean;
        /** Default 'prefer': SCRAM-SHA-256-PLUS with tls-server-end-point (RFC 5929) when TLS is on and the server offers it, else SCRAM-SHA-256; PLUS offered with an unusable cert hash (e.g. Ed25519) fails, as in libpq.
         *  'require' fails the connection unless PLUS is used (no TLS, no PLUS, cert hash unusable, or password/MD5/trust auth); 'disable' never binds. */
        channelBinding?: "disable" | "prefer" | "require";
        /** Highest protocol requested, default "3.0" as in libpq 18 (poolers and proxies may break on 3.2); a higher minProtocolVersion raises it. */
        maxProtocolVersion?: "3.0" | "3.2";
        /** Lowest protocol accepted, default "3.0"; a NegotiateProtocolVersion below it fails the connection. */
        minProtocolVersion?: "3.0" | "3.2";
        /** "postgres" (default): SSLRequest, then TLS; "direct": TLS at once (PostgreSQL 17+, needs tls: true, server must select ALPN "postgresql").
         *  ALPN "postgresql" is offered in both; cancel() negotiates TLS the same way (libpq 17), with no plaintext fallback. */
        sslNegotiation?: "postgres" | "direct";
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
        /** Same as new PostgreSQL({ ...opts, url }); a url in both places throws TypeError. A host starting with / is a socket directory. */
        constructor(url: string, opts?: PostgreSQLOptions);
        constructor(opts?: PostgreSQLOptions);
        /** Runs one statement (extended protocol when params are given); `opts.maxRows` rejects that query when exceeded, leaving the connection usable. */
        query(sql: string, params?: unknown[], opts?: { maxRows?: number }): Promise<unknown>;
        /** 1..65535 statements, one round trip, one Sync (one implicit transaction). Throws TypeError/RangeError on a bad list, InternalError when closed or over maxPending.
         *  Resolves to a result or Error per statement (first failure, then a skipped Error each); rejects on connection failure, close() or out of memory. */
        pipeline(statements: [sql: string, params?: unknown[]][]): Promise<({ rows: unknown[]; fields: unknown[]; command: string; rowCount: number } | Error)[]>;
        /** Asks the server to cancel the running query over a fresh connection, TLS as for the session (no plaintext fallback); best effort, no reply. */
        cancel(): void;
        /** Registers a notice, notification or error handler; returns the client. */
        on(event: "notice" | "notification" | "error", handler: (data: unknown) => void): this;
        /** True once the startup handshake has completed. */
        readonly ready: boolean;
        /** Negotiated protocol, null until ready; the cancel key is 4 bytes in "3.0", 4..256 in "3.2". */
        readonly protocolVersion: "3.0" | "3.2" | null;
        /** The statement cache's size and how often each arm was taken. */
        readonly statementCache: StatementCacheStats;
        /** Queries issued but not yet answered. */
        readonly pending: number;
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
        /** COPY ... FROM STDIN; string/bytes are copied at call, an (async) iterable of string|Uint8Array|ArrayBuffer is pulled while the socket queue is <= highWaterMark (bytes, 1..2^30, default 1 MiB).
         *  A source throw, rejection or bad chunk sends CopyFail and rejects with that error; the connection stays usable and later queries wait for the COPY; query() on COPY FROM STDIN sends CopyFail. */
        copyFrom(sql: string, source: string | Uint8Array | ArrayBuffer | Iterable<string | Uint8Array | ArrayBuffer> | AsyncIterable<string | Uint8Array | ArrayBuffer>, opts?: { highWaterMark?: number }): Promise<unknown>;
        /** COPY ... TO STDOUT as Uint8Array chunks; receiving pauses while > highWaterMark bytes are unread (TCP/Unix, not TLS); one next() at a time; return() discards the rest; done value = query result.
         *  Throws TypeError unless sql starts with COPY; a COPY that never streams (TO file/PROGRAM) rejects next() with TypeError; query() refuses TO STDOUT; CopyBoth fails the connection. */
        copyTo(sql: string, opts?: { highWaterMark?: number }): AsyncIterableIterator<Uint8Array>;
        /** The server parameters from startup (server_version, client_encoding, ...). */
        readonly parameters: Record<string, string>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    // PgPool/RedisPool are NOT exports of dyna:postgres or dyna:redis: they are pure-JS classes imported by file path (see the src/pool.js block).
}
