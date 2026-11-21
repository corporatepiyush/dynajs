/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:redis
/** dyna:redis — a Redis client (RESP2/RESP3, TLS, pub/sub, pipelined FIFO commands).
 *  Split out of dyna:net as a clean break: dyna:net no longer exports Redis. Replies are untrusted: every decoder bounds what a peer can make it allocate. */
declare module "dyna:redis" {
    /** Redis client options. */
    interface RedisOptions {
        host?: string;
        path?: string;
        port?: number;
        db?: number;
        username?: string;
        password?: string;
        /** Connect through TLS; certificates are verified against the system trust store with hostname checking (`ca` pins a CA PEM file). */
        tls?: boolean;
        /** CA certificate PEM file pinned for the TLS handshake; omitted uses the system trust store. */
        ca?: string;
        binary?: boolean;
        bigint?: boolean;
        /** Caps a bulk payload or streamed-string chunk total, and an aggregate or stream element count; default 64 MiB.
         *  RESP3 streamed replies (`$?` `;n` .. `;0`; `*?` `~?` `%?` `|?` .. `.`) decode as their fixed form; nesting max 31. */
        maxReplyBytes?: number;
        maxPending?: number;
        connectTimeoutMs?: number;
        commandTimeoutMs?: number;
    }
    /** A Redis client: commands return promises matched to replies by strict FIFO. Replies are UNTRUSTED: over-cap or malformed ones are refused by name and the connection torn down. */
    class Redis implements DynResource {
        constructor(opts?: RedisOptions);
        /** Sends one command (RESP3 when available). `HELLO` and `CLIENT REPLY` are refused: they change how many replies arrive, breaking positional matching. */
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        /** One round trip for many commands, one reply each; (P|S)SUBSCRIBE forms throw here (they answer per channel): issue them with command(). */
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
}
