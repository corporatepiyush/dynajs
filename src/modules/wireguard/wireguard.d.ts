/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:wireguard
/** dyna:wireguard — WireGuard (Noise_IKpsk2, ChaCha20-Poly1305) over UDP, with a userspace IPv4/IPv6 + TCP (SACK, RFC 6528 ISN) stack or a kernel tun.
  *  X25519, AEAD batches and inner checksums run on the pool (a lone packet <=256 B is sealed inline: cheaper than a handoff). */
declare module "dyna:wireguard" {
    /** One peer; keys are base64 of 32 bytes. Unknown keys throw/reject TypeError naming the key. */
    interface WireGuardPeer {
        publicKey: string;
        presharedKey?: string;
        /** "a.b.c.d:port", an IPv4 literal (resolve names first; an IPv6 underlay is a TypeError); default none (learned from the first packet). */
        endpoint?: string;
        /** CIDRs routed to this peer, e.g. "10.0.0.2/32"; at least one. */
        allowedIPs: string[];
        /** Seconds between keepalives, 0..65535; default 0 (off). */
        persistentKeepalive?: number;
    }
    /** WireGuard.create options; unknown keys reject TypeError naming the key. */
    interface WireGuardOptions {
        /** Base64 of 32 bytes (X25519 private key; clamped as in RFC 7748). */
        privateKey: string;
        /** UDP port on 0.0.0.0, 0..65535; default 0 (ephemeral, see listenPort). */
        listenPort?: number;
        /** Tunnel addresses of this side ("10.0.0.1/24" or "fd00::1"); the prefix is ignored (routing is by allowedIPs). Required without tun. */
        address?: string[];
        peers?: WireGuardPeer[];
        /** Kernel tun interface name (utun<N> on macOS, any name on Linux); needs root or CAP_NET_ADMIN; dial/listen/udp then throw TypeError. */
        tun?: string;
        /** Tun MTU, 1280..65535; default 1420. */
        mtu?: number;
    }
    /** Byte stream over one tunnelled TCP connection. */
    interface WireGuardConn {
        /** Received bytes; a slow reader closes the TCP window (backpressure). cancel() aborts the connection. */
        readonly readable: ReadableStream<Uint8Array>;
        /** A write resolves once the stack accepted all its bytes; close() sends FIN after the queue drains; abort() sends RST. */
        readonly writable: WritableStream<ArrayBufferView | ArrayBuffer | string>;
        readonly remoteAddress: string;
        readonly remotePort: number;
        readonly localAddress: string;
        readonly localPort: number;
        /** RST now; the readable closes and the writable errors. Idempotent. */
        close(): void;
    }
    interface WireGuardListener {
        readonly port: number;
        /** Stops accepting; open connections stay. Idempotent. */
        close(): void;
    }
    /** UDP socket inside the tunnel (both families when both are configured). */
    interface WireGuardUdp {
        readonly port: number;
        /** Datagram of at most 65507 bytes to an IP literal; throws TypeError on a bad address or a closed socket, false when not routable. */
        send(data: ArrayBufferView | ArrayBuffer | string, host: string, port: number): boolean;
        close(): void;
    }
    interface WireGuardStats {
        peers: number;
        handshakesSent: number;
        handshakesCompleted: number;
        rxPackets: number;
        txPackets: number;
        rxDropped: number;
        txDropped: number;
        /** Inner packets whose L4 checksum the pool verified, so the stack skipped it. */
        checksumsVerified: number;
        poolJobs: number;
        /** Lone packets <=256 B (ACKs, keepalives) sealed or opened on the JS thread, plus any job the pool refused. */
        inlineJobs: number;
        tunRx: number;
        tunTx: number;
        tcpConnections: number;
        timerFires: number;
    }
    /** A WireGuard device on the shared reactor; it keeps the process alive until close(). */
    class WireGuard implements DynResource {
        /** Not constructible: TypeError; use WireGuard.create. */
        private constructor();
        /** Resolves once the UDP socket is bound and every peer's static key is derived (pool). Rejects TypeError on bad options, a bad
         *  key or a tun open failure ("WireGuard: tun: <stage>: <errno text>"); Error on a low-order peer key or a bind failure. */
        static create(opts: WireGuardOptions): Promise<WireGuard>;
        /** wg-quick INI ([Interface], [Peer]); keys are case-insensitive; DNS, Table, FwMark, PreUp/PostUp/PreDown/PostDown and SaveConfig are ignored.
         *  Throws SyntaxError "WireGuard.parseConfig: line N: ..." on an unknown key, a duplicate key or a bad number. */
        static parseConfig(text: string): WireGuardOptions;
        /** A fresh X25519 key pair (base64), generated on the pool. */
        static generateKeyPair(): Promise<{ privateKey: string; publicKey: string }>;
        readonly publicKey: string;
        readonly listenPort: number;
        /** Tun interface name, or null with the userspace stack. */
        readonly tun: string | null;
        /** TCP connect to an IP literal inside the tunnel; rejects Error "WireGuard: connect: <reason>" (no route to host, refused, timed out, closed). */
        dial(host: string, port: number): Promise<WireGuardConn>;
        /** Accepts on port (1..65535) on every configured family; handler runs once per connection. TypeError when the port is taken. */
        listen(port: number, handler: (conn: WireGuardConn) => void): WireGuardListener;
        /** Binds port (0 = ephemeral); handler gets each datagram. TypeError when the port is taken. */
        udp(port: number, handler: (data: Uint8Array, host: string, port: number) => void): WireGuardUdp;
        /** Resolves when the peer is usable; rejects TypeError on a bad or duplicate key, Error on a low-order key or removePeer/close first. */
        addPeer(peer: WireGuardPeer): Promise<void>;
        /** Drops the peer, its sessions and routes; false when unknown. */
        removePeer(publicKey: string): boolean;
        stats(): WireGuardStats;
        /** Closes every connection, socket and the device; pending promises reject Error "WireGuard: closed". Idempotent. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
