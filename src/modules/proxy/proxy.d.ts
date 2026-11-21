/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:proxy
/** dyna:proxy — TCPProxy, an L4 byte reverse proxy on the shared io reactor with no JS on the data path.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:proxy" {
    /** One upstream entry for TCPProxy. */
    interface TCPUpstream {
        host?: string;
        port: number;
    }
    /** TCPProxy counters. */
    interface TCPProxyStats {
        readonly live: number;
        readonly accepted: number;
        readonly refused: number;
        readonly idleClosed: number;
        readonly connectFailed: number;
        readonly bytesUp: number;
        readonly bytesDown: number;
    }
    /** An L4 byte reverse proxy with no JS on the data path. Defaults: maxConns 8192, connectTimeoutMs 10000, no idle timeout, listening on 0.0.0.0. */
    class TCPProxy implements DynResource {
        constructor(opts: { port: number; host?: string; upstream: TCPUpstream | TCPUpstream[]; maxConns?: number; idleTimeoutMs?: number; connectTimeoutMs?: number });
        /** Binds and starts forwarding; a port of 0 resolves into `.port`. A started proxy stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(): void;
        /** Live pairs and cumulative counters. */
        stats(): TCPProxyStats;
        /** The bound port. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

}
