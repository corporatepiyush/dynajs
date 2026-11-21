/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:dns
/** dyna:dns — a UDP DNS client (resolver) and server. Answers are untrusted: every decoder bounds what a peer can make it allocate.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:dns" {
    /** One decoded answer record: `name`, `type` and `ttl` are always present; the payload field depends on the type (A/AAAA -> address, CNAME/NS/PTR -> target, MX -> priority+exchange, TXT -> chunks). */
    interface DNSRecord {
        readonly name: string;
        readonly type: number;
        readonly ttl: number;
        readonly address?: string;
        readonly target?: string;
        readonly priority?: number;
        readonly exchange?: string;
        readonly chunks?: string[];
    }
    /** A UDP DNS client; answers are matched by connected socket, CSPRNG query ID and echoed question.
        A GC root while any query is in flight (a dropped handle still settles); collectable once idle. */
    class DNSResolver implements DynResource {
        /** timeoutMs must fit uint32 (else RangeError). `{ttl}` seconds (at most 86400) enables the answer cache: hits return a fresh deep copy; no single-flight. */
        constructor(opts?: { server?: string; port?: number; timeoutMs?: number; /** answer-cache cap in seconds (1..86400); absent/0 disables caching */ ttl?: number });
        /** Queries a name; `type` is the record type (A is 1). The callback receives (err, records). */
        query(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): void;
        /** Resolves decoded answers (type defaults to A), following a CNAME chain inside the answer (8 hops): records are the final name's of `type`; a trailing callback gets (err, records). */
        lookup(name: string, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): Promise<DNSRecord[]> | undefined;
        lookup(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): Promise<DNSRecord[]> | undefined;
        /** Sugar over lookup(name, 5): the chain targets. */
        resolveCname(name: string): Promise<string[]>;
        /** Sugar over lookup(name, 15): {priority, exchange} pairs. */
        resolveMx(name: string): Promise<{ priority: number; exchange: string }[]>;
        /** Sugar over lookup(name, 16): one chunk array per record. */
        resolveTxt(name: string): Promise<string[][]>;
        /** Sugar over lookup(name, 2): the nameservers. */
        resolveNs(name: string): Promise<string[]>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A UDP DNS server with an amplification cap and a per-source token bucket. */
    class DNSServer implements DynResource {
        constructor(opts?: { port?: number; host?: string });
        /** Starts answering; the handler maps (name, type) to an address string, or null for no answer. A started server stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(handler: (name: string, type: number) => string | null): void;
        /** The bound port (resolved when constructed with port 0). */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

}
