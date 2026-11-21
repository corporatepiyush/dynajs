/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:net
/** dyna:net — TCP/UDP/unix sockets (with TLS), frame decoders, Happy Eyeballs connect and a token-bucket rate limiter.
 *  B1e clean break: HTTP (dyna:http), DNS (dyna:dns), the L4 proxy (dyna:proxy), addresses (dyna:netip) and metrics (dyna:metrics) are their own modules; dyna:net re-exports none of them. Network replies are untrusted: every decoder bounds what a peer can make it allocate. */
declare module "dyna:net" {
    /** Sockets: TCP servers and clients, UDP, frame decoders, connectHappy and RateLimiter. */

    /* ---- rate limiting ---- */
    /** A token bucket over a fixed-size, open-addressed table; the table grows only with `grow: true`. Keys are hashed with SipHash-1-3 under a random per-instance key, so keys cannot be crafted to collide into one probe cluster. */
    class RateLimiter {
        /** Token-bucket table keyed by string: `tokensPerSec` (alias `refill`) is required, `burst` (alias `capacity`) defaults to one second; 1024 fixed slots unless `grow: true`. */
        constructor(opts: { tokensPerSec?: number; refill?: number; burst?: number; capacity?: number; slots?: number; grow?: boolean });
        /** True when the key may proceed, consuming `cost` tokens (default 1); cost must be > 0. */
        allow(key: string, cost?: number): boolean;
        /** The key's current token count. */
        tokens(key: string): number;
        /** Clears one key, or the whole table when no key is given. */
        reset(key?: string): void;
        /** Running totals and table geometry. */
        readonly stats: { allowed: number; denied: number; slots: number; live: number; grew: number; tokensPerSec: number; burst: number };
    }


    /* ---- frame decoders ---- */
    /** A frame decoder spec from LengthFieldDecoder, DelimiterDecoder or FixedLengthDecoder. It holds configuration only: each
     *  connection it is attached to (the decoder option of the TCPServer constructor or TCPServer.connect, or TCPConn.setDecoder)
     *  gets its own decoder state, so one spec serves any number of connections. While a connection has a decoder, every received
     *  byte goes through it: the frame handler fires once per frame with a fresh Uint8Array (its own ArrayBuffer), never once per
     *  read, and the data handler does not fire. TLS connections decode the plaintext.
     *
     *  DEFAULT = Netty 4.1 (LengthFieldBasedFrameDecoder, DelimiterBasedFrameDecoder, FixedLengthFrameDecoder), read by read:
     *  - a decoder error is reported, not fatal: the error handler gets "frame decoder: " + Netty's exception message (e.g.
     *    "Adjusted frame length exceeds 1024: 2000 - discarded"), the offending bytes are skipped as Netty skips them (a too-long
     *    frame is discarded whole, also across later reads; a negative or too-short length field skips the header only) and the
     *    connection stays open. With no error handler the bytes are dropped silently.
     *  - failFast (default true, as Netty): report a too-long frame as soon as it is detected; false: once it has been discarded.
     *  - as in Netty's ByteToMessageDecoder, an error thrown by the decoder ends the decoding of the current read: frames after it
     *    in the same read are decoded when the next read arrives, or at the peer's FIN (where a second error drops the rest).
     *    LineBasedFrameDecoder fires its errors without throwing, so it keeps decoding the same read.
     *  - DelimiterDecoder with exactly the two delimiters "\r\n" and "\n" (either order) runs Netty's LineBasedFrameDecoder, as
     *    Netty's DelimiterBasedFrameDecoder does: a line is too long when everything buffered without a newline (a trailing "\r"
     *    included) exceeds maxFrameLength, and its messages are "frame length (N) exceeds the allowed maximum (M)".
     *  - other delimiter sets: the delimiter giving the shortest frame wins, a tie goes to the earlier one in the list; a trailing
     *    partial delimiter is not counted against maxFrameLength (Netty 4.1 partialDelimiterLength), so a max-length frame whose
     *    delimiter is split across reads is delivered; overlapping sets (e.g. ["bb", "b"]) decide on what has arrived, so "1b" then
     *    "b2b" gives "1", "", "2" while "1bb2b" in one read gives "1", "2" (holdOverlapping changes that).
     *  - LengthField width 8 reads a signed long: a length with the top bit set is "negative pre-adjustment length field: N"; the
     *    adjusted length (raw + adjustment + offset + width, Java long arithmetic, so it can wrap negative) decides "too short" or
     *    "too long". A length above 2^53 is never usable as a frame size here (maxFrameLength <= 2^53), so after adjustment it is
     *    necessarily too long unless the adjustment brings it back into range (then it is a valid frame, as in Netty).
     *  - at the peer's FIN the frames already decoded are delivered, decoding resumes on what an error left buffered, then the
     *    connection closes; an incomplete trailing frame is dropped without an error (as Netty): a truncated stream and a clean
     *    close look alike, so a protocol that must tell them apart frames an explicit end marker. A TLS close_notify ends the
     *    stream the same way.
     *  Known deviations from Netty 4.1 in the default mode: none (checked against a literal per-read port of the 4.1 decoders).
     *
     *  EXTENSION beyond Netty (dyna-specific, the `meta` argument, all off by default): stopOnError, rejectUnsafeLength,
     *  holdOverlapping, resumeAfterError (see the meta interfaces). holdOverlapping + stopOnError makes a DelimiterDecoder's frames and final error
     *  independent of how TCP segmented the stream; stopOnError makes a LengthFieldDecoder's independent of it. Without
     *  stopOnError, Netty's error handling above (decoding ends with the read) keeps the events after an error read-dependent.
     *
     *  pause() inside a frame handler stops delivery at once: frames already decoded from the same read wait in the connection and
     *  resume() delivers them before reading again, so none is lost. On the io_uring backend pause() is a no-op (see TCPConn.pause).
     *  A frame spanning reads is copied once into the decoder's buffer and once into its Uint8Array; a frame inside one read is
     *  copied once. */
    interface FrameDecoder {
        readonly kind: "length" | "delimiter" | "fixed";
        /** The largest frame accepted; for "fixed", the frame length. */
        readonly maxFrameLength: number;
    }
    /** LengthFieldDecoder options: Netty's constructor parameters (a strict bag: an unknown key is a TypeError). */
    interface LengthFieldDecoderOptions {
        /** Bytes before the length field, 0..2^53; default 0 (lengthFieldOffset). */
        offset?: number;
        /** Length field size in bytes: 1, 2, 3, 4 or 8 (RangeError otherwise); default 4. 1..4 are unsigned, 8 is a signed long. */
        width?: 1 | 2 | 3 | 4 | 8;
        /** Byte order of the length field; default "be". */
        endian?: "be" | "le";
        /** lengthAdjustment: frameLength = raw + adjustment + offset + width. |adjustment| <= 2^53; default 0. */
        adjustment?: number;
        /** Bytes dropped from the front of each frame, 0..2^53; more than the frame length is an error (the frame is skipped); default 0. */
        initialBytesToStrip?: number;
        /** The largest whole frame (header included, before stripping), 1..2^53, at least offset + width (RangeError otherwise);
         *  default 1048576. */
        maxFrameLength?: number;
        /** Netty failFast; default true. */
        failFast?: boolean;
    }
    /** dyna-specific extension for LengthFieldDecoder (not in Netty; a strict bag, all default false). */
    interface LengthFieldDecoderMeta {
        /** The first error is fatal: it is reported (at detection, whatever failFast says), nothing after it is decoded, and the
         *  connection closes once the frames decoded before it are delivered. */
        stopOnError?: boolean;
        /** A raw length field above 2^53 (including width-8 values Netty reads as negative) is its own error, "length field N
         *  exceeds 2^53 (rejectUnsafeLength)", instead of Netty's signed-long arithmetic. */
        rejectUnsafeLength?: boolean;
        /** Decode the bytes left in the read right after an error instead of waiting for the next read or end of input (Netty's
         *  callDecode stops at a thrown TooLongFrameException, so by default the bytes after an error wait, which can deadlock a
         *  request/response peer that sends nothing more). The errors and frames are those of Netty's decode with the stop removed.
         *  Ignored under stopOnError. */
        resumeAfterError?: boolean;
    }
    /** DelimiterDecoder options: Netty's constructor parameters (a strict bag). */
    interface DelimiterDecoderOptions {
        /** One delimiter or a list of 1..64; each is 1..4096 bytes (a string is UTF-8). Required. */
        delimiter: BytesInput | BytesInput[];
        /** Drop the delimiter from the delivered frame; default true. */
        stripDelimiter?: boolean;
        /** The largest frame without its delimiter, 1..2^53; default 1048576. */
        maxFrameLength?: number;
        /** Netty failFast; default true. */
        failFast?: boolean;
    }
    /** dyna-specific extension for DelimiterDecoder (not in Netty; a strict bag, all default false). */
    interface DelimiterDecoderMeta {
        /** The first error is fatal (reported at detection), then the connection closes; see LengthFieldDecoderMeta.stopOnError. */
        stopOnError?: boolean;
        /** For delimiter sets where one delimiter can begin inside another (e.g. ["bb", "b"]): a frame is delivered only when no
         *  longer delimiter still arriving could win, so frames equal Netty on the whole stream at once; the peer's FIN resolves a
         *  held frame (as Netty's decodeLast would on the coalesced stream). Turns off the LineBasedFrameDecoder path. */
        holdOverlapping?: boolean;
        /** See LengthFieldDecoderMeta.resumeAfterError. */
        resumeAfterError?: boolean;
    }
    /** Length-prefixed frames (Netty LengthFieldBasedFrameDecoder). A plain function: `new` is a TypeError. */
    function LengthFieldDecoder(opts?: LengthFieldDecoderOptions, meta?: LengthFieldDecoderMeta): FrameDecoder;
    /** Delimiter-terminated frames (Netty DelimiterBasedFrameDecoder / LineBasedFrameDecoder). A plain function. */
    function DelimiterDecoder(opts: DelimiterDecoderOptions, meta?: DelimiterDecoderMeta): FrameDecoder;
    /** Every `length` bytes is a frame, 1..2^53 (Netty FixedLengthFrameDecoder). A plain function. */
    function FixedLengthDecoder(length: number): FrameDecoder;
    /* ---- sockets and proxying ---- */
    /** A TCP connection handed to a handler; write is refused on a TLS connection before the handshake, and refused with a TypeError past the connection's high-water mark. */
    interface TCPConn {
        write(data: BytesInput): void;
        /** Bytes queued for this connection that the kernel has not taken (0 on a closed connection). */
        readonly bufferedAmount: number;
        close(): void;
        /** Stops reading: the receive is cancelled, so no data event fires and the kernel's buffer fills until the peer's sends block (TCP backpressure). A FIN or reset is not seen until resume().
         *  A paused connection is exempt from idleTimeoutMs. Idempotent; a no-op on a closed connection; a TypeError on a TLS connection before its handshake completed.
         *  On the opt-in io_uring backend (IO_URING=1 builds) pause() is a no-op: cancelling a multishot receive there drops a completion already queued, so streams() has no read backpressure on that backend. */
        pause(): void;
        /** Re-arms reading; on TLS it first delivers plaintext already decrypted when the pause landed. Idempotent; a no-op on a closed connection. */
        resume(): void;
        /** True between pause() and resume() on an open connection. */
        readonly paused: boolean;
        /** Half-close: shutdown(SHUT_WR) once nothing is queued (a TypeError while bytes are queued: wait for drain). The peer reads EOF; this side can still read. TLS too (since v10b-26a; it used to close() the socket, and a close() with unread bytes in the receive buffer, such as a late TLS 1.3 session ticket, sends RST, which drops the ciphertext still in the send buffer: the peer lost the tail of a stream whose writer.close() had resolved). No close_notify is sent: the TLS stream ends with the TCP FIN. */
        end(): void;
        /** WHATWG streams over THIS connection; once called, its data and drain events go to the streams instead of the TCPHandlers (connect/close still fire). Call it from the connect handler. Throws when called twice or on a closed connection.
         *  readable: a byte stream; each received chunk is enqueued and the connection pause()s once desiredSize <= 0, and a pull resume()s it, so a slow reader pushes back on the peer. cancel() stops reading (the connection is paused).
         *  writable: a chunk (BufferSource or string) is written in pieces of at most min(writableHighWaterMark, the connection's highWaterMark) bytes, each waiting for drain while bufferedAmount would pass that size, so a write never hits the high-water refusal; a write resolves once its last piece is queued.
         *  close() waits for the queue to drain, then end()s; abort() closes the connection. A failed send closes the connection and errors the writable ("the connection closed").
         *  The peer's FIN closes the readable; without allowHalfOpen the connection then closes once its queue drains, with it this side keeps writing until writable.close(). The connection closes when the readable is done (EOF or cancel) and the writable is closed.
         *  A close of the connection (peer reset, close(), idle timeout) closes the readable and errors the writable; closing the TCPServer itself drops its connections without events, so their streams stay pending.
         *  streams() is byte-mode only: on a connection with a frame decoder it is a TypeError (frames go to the frame handler). */
        /** Attaches a frame decoder to THIS connection (see FrameDecoder); from then on its bytes go to the frame handler, starting with
         *  any bytes the kernel already holds (pause() in the connect handler, setDecoder, then resume() to switch mid-stream without
         *  a data event). One decoder per connection for its lifetime. TypeError when the connection is closed, when dec is not a
         *  FrameDecoder, when a decoder is already set, after streams(), or when the handlers have no frame function.
         *  connectHappy takes no decoder option: call setDecoder from its connect handler. */
        setDecoder(dec: FrameDecoder): void;
        streams(opts?: TCPStreamsOptions): { readable: ReadableStream<Uint8Array>; writable: WritableStream<ArrayBufferView | ArrayBuffer | string> };
    }
    /** Options for TCPConn.streams(). */
    interface TCPStreamsOptions {
        /** Readable queue size in bytes before the connection pauses; default 65536 (0 = pause after every chunk until a read). */
        readableHighWaterMark?: number;
        /** Largest piece written at once and the queued-bytes level a write waits below, 1..2^30; default 65536, capped at the connection's highWaterMark. */
        writableHighWaterMark?: number;
        /** Keep the connection open for writing after the peer's FIN (default false: close once the queue drains). */
        allowHalfOpen?: boolean;
    }
    /** TCP server and client event handlers. */
    interface TCPHandlers {
        /** Fires when a connection lands; on failure conn is null and err names the reason. */
        connect?: (conn: TCPConn | null, err: string | null) => void;
        /** Fires with a copy of the received bytes. */
        data?: (conn: TCPConn, bytes: Uint8Array) => void;
        /** Fires exactly once per emptying of the connection's outbound queue after a write left bytes queued; the producer's signal to resume. */
        drain?: (conn: TCPConn) => void;
        close?: (conn: TCPConn) => void;
        /** Fires once per frame while the connection has a decoder (in place of data), with a fresh Uint8Array. Required when a decoder is set. */
        frame?: (conn: TCPConn, frame: Uint8Array) => void;
        /** Fires once per decoder error ("frame decoder: <Netty's message>"), in order with the frames: after the frames decoded before it. By default the connection stays open and decoding goes on (Netty); with the decoder's stopOnError meta option the connection closes after it. */
        error?: (conn: TCPConn, message: string) => void;
    }
    /** Server TLS: cert/key required; requestCert turns on mTLS (needs ca). */
    /** Server TLS: cert/key/ca are PEM file paths (string or Path). */
    interface TCPServerTLSOptions {
        cert: string | import("dyna:file").Path;
        key: string | import("dyna:file").Path;
        alpn?: string;
        ca?: string | import("dyna:file").Path;
        requestCert?: boolean;
    }
    /** Client TLS: the verified name defaults to host; cert/key give mTLS; `ca` pins a CA; rejectUnauthorized:false disables verification. */
    interface TCPClientTLSOptions {
        ca?: string | import("dyna:file").Path;
        servername?: string;
        alpn?: string | string[];
        minVersion?: string;
        rejectUnauthorized?: boolean;
        cert?: string | import("dyna:file").Path;
        key?: string | import("dyna:file").Path;
    }
    /** Options for TCPServer.connect; with a unix-socket `path`, TLS needs an explicit servername or rejectUnauthorized:false. */
    interface TCPConnectOptions {
        host?: string;
        port?: number;
        path?: string;
        connectTimeoutMs?: number;
        maxConnections?: number;
        idleTimeoutMs?: number;
        tls?: boolean | TCPClientTLSOptions;
        /** Outbound queue cap per connection in bytes (1..2^30); a write past it is refused with a TypeError until drain fires. */
        highWaterMark?: number;
        /** Frame decoder for the connection (see FrameDecoder); the handlers then need a frame function (TypeError otherwise). */
        decoder?: FrameDecoder;
    }
    /** A TCP server, and (via the static connect) a TCP client; runs on the shared io reactor. */
    class TCPServer implements DynResource {
        constructor(opts?: { port?: number; path?: string; maxConnections?: number; idleTimeoutMs?: number; tls?: TCPServerTLSOptions; /** outbound queue cap per connection in bytes (1..2^30), default 4 MiB */ highWaterMark?: number; /** frame decoder for every accepted connection (each gets its own state); start() then needs a frame handler */ decoder?: FrameDecoder });
        /** Connects to a peer and returns the client resource; events arrive through TCPHandlers. The client lives while connecting or connected even if the handle is dropped; once its connection ends it no longer holds the event loop. */
        static connect(opts: TCPConnectOptions, handlers?: TCPHandlers): TCPServer;
        /** Binds and accepts; a port of 0 resolves into `.port`. A started server (TCP, TLS or IPC) stays alive until close(), as in node, even when its handle is dropped: it is a GC root from start() to close() and keeps accepting; an unstarted or closed server is collectable. */
        start(handlers?: TCPHandlers): void;
        /** The bound port. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A bound UDP socket; binds in the constructor. `host` defaults to every IPv4 interface; a host containing ':' ("::", "::1", "fe80::1%lo0") binds an IPv6 socket, dual-stack (IPV6_V6ONLY off), so one bound to "::" also hears IPv4 peers as "::ffff:a.b.c.d". */
    class UDPSocket implements DynResource {
        constructor(opts?: { port?: number; host?: string });
        /** Arms the receive path; `message(data, from)` receives a copy of each datagram; `from.address` is numeric ("127.0.0.1", "::1", "::ffff:127.0.0.1") and passes straight back to send(). A started socket stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(handlers?: { message?: (data: Uint8Array, from: { address: string; port: number }) => void }): void;
        /** Sends a datagram to host:port, where host is an IPv4 or IPv6 literal (no name lookup); an IPv6 host needs a socket bound to an IPv6 host (TypeError otherwise), and such a socket sends to IPv4 literals v4-mapped. Returns the bytes sent. A failed sendto(2) throws an InternalError "UDPSocket: send failed: <strerror> (<CODE>)" carrying .errno and .code (EMSGSIZE for a datagram over the OS limit, 65507 bytes on IPv4 and net.inet.udp.maxdgram, default 9216, on macOS; EAGAIN/ENOBUFS when the send buffer is full: the datagram is dropped, not queued; EHOSTUNREACH/ENETUNREACH; EPIPE, which macOS can return for a send to the host's own LAN address while the Application Firewall blocks all incoming connections). A failed send does not close or wedge the socket: later sends and the receive path keep working. */
        send(data: BytesInput, host: string, port: number): number;
        /** The bound port. Port 0 takes an ephemeral port no other socket holds; an explicit port binds with SO_REUSEADDR. On Linux a UDP socket with SO_REUSEADDR does not count as holding its port during ephemeral selection, so a port-0 socket bound that way could be handed a live socket's port and take its unicast traffic. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Happy Eyeballs (simplified RFC 8305): both address families race, first success wins; fallbackMs is the whole-race deadline, counted from the end of name resolution. A host name is resolved off the event loop, so the call returns at once and a failed lookup reaches the `connect` handler as "connectHappy: DNS resolution failed"; an address literal is used as is. `tls` options are required for TLS endpoints. */
    function connectHappy(host: string, port: number, opts?: { fallbackMs?: number; tls?: boolean | TCPClientTLSOptions }, handlers?: TCPHandlers): TCPServer;

    /** Process-wide count of throws swallowed from net event handlers (TCP connect/data/close/drain, UDP message, DNSResolver query callback): the only diagnostic for them. */
    function swallowedHandlerThrows(): number;
}

