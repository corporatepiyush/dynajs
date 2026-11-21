/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:ssh
/** dyna:ssh — an SSH-2 client and server (RFC 4251-4254) in native C on the shared reactor, with OpenSSL for the primitives. The transport takes
 *  only the best algorithms and nothing else is negotiable: key exchange mlkem768x25519-sha256 (ML-KEM-768 + X25519, the shared secret
 *  SHA-256(K_pq || K_x25519) entering the exchange hash as a string), then sntrup761x25519-sha512 (also offered as
 *  sntrup761x25519-sha512@openssh.com: Streamlined NTRU Prime 761 + X25519, SHA-512(K_pq || K_x25519) as a string, SHA-512 exchange hash and
 *  key derivation; the sntrup761 code is this module's own constant-time implementation of the NTRU Prime round-3 KEM), then
 *  curve25519-sha256 (also offered as curve25519-sha256@libssh.org); host keys ssh-ed25519-cert-v01@openssh.com, ssh-ed25519 and
 *  sk-ssh-ed25519@openssh.com (verify only); ciphers chacha20-poly1305@openssh.com then aes256-gcm@openssh.com (AEAD: the MAC name-list is
 *  sent empty); compression "none". CBC/CTR, SHA-1 key exchange, DSA/ECDSA, non-AEAD MACs, compression, SSH-1 and the none cipher do not
 *  exist in the code. The client picks its first algorithm the server offers; the server picks the client's first algorithm it supports.
 *  Strict key exchange (kex-strict-c/s-v00@openssh.com, the Terrapin countermeasure) is required in both roles: a peer whose first KEXINIT
 *  lacks the other side's marker is refused, the first packet must be KEXINIT, any other message during the initial exchange is fatal, and
 *  sequence numbers restart at zero after every NEWKEYS. The client offers ext-info-c (RFC 8308) and records the server's server-sig-algs;
 *  the server sends EXT_INFO (server-sig-algs "ssh-ed25519,sk-ssh-ed25519@openssh.com") right after its first NEWKEYS when the client
 *  offered ext-info-c. Either side may re-key at any time; each side starts one after dyna.rekeyBytes bytes (both directions, default 1 GiB;
 *  checked on every sent and received packet, so a side overshoots the limit by at most one packet) or dyna.rekeySeconds (default 3600);
 *  channel data waits in its channel while a re-key runs and is sent after NEWKEYS, and the host key must stay
 *  the same as the first exchange's. Every fatal error sends SSH_MSG_DISCONNECT with the RFC 4253 reason code (2 protocol error, 3 key
 *  exchange failed, 5 MAC error, 7 service not available, 9 host key not verifiable, 11 by application, 14 no more auth methods, 15 illegal
 *  user name) and the message the rejection carries.
 *  Authentication is publickey only, in both roles: ssh-ed25519 keys in the OpenSSH format (openssh-key-v1, unencrypted, or encrypted by
 *  ssh-keygen with bcrypt-pbkdf and aes256-ctr or aes256-gcm@openssh.com: those ciphers decrypt the key file only and are never offered on
 *  the wire), sk-ssh-ed25519@openssh.com signatures (server side, user-presence flag required) and OpenSSH ed25519 certificates
 *  (ssh-ed25519-cert-v01@openssh.com, PROTOCOL.certkeys) signed by an ssh-ed25519 CA. There is no password, keyboard-interactive, hostbased
 *  or GSSAPI code. A certificate is accepted only when its CA signature verifies, its type is right (user for SSHServer logins, host for a
 *  server's host key), now is inside [valid_after, valid_before), its principals list names the user (or the host name the client dialled),
 *  and every critical option is understood: force-command (user certificates; the session's `command` becomes it) and source-address
 *  (user certificates; comma-separated CIDRs the client address must match). Any other critical option (verify-required included), and any
 *  critical option on a host certificate, refuses the certificate. The permit-pty extension is enforced (no pty-req without it); other
 *  extensions are ignored.
 *  The client checks the server's host key against an OpenSSH known_hosts file: plain and hashed (|1|salt|hash, HMAC-SHA1 by that format;
 *  used only to match a host name at rest) entries, comma-separated patterns with * ? and ! negation, [host]:port for a port other than 22,
 *  @revoked lines (a match refuses, for a host key or a certificate's key or CA) and @cert-authority lines (an ssh-ed25519 CA trusted for
 *  the matching hosts; the client offers the host-certificate algorithm only when such a line matches the host). An unknown host is
 *  refused by default. */
declare module "dyna:ssh" {
    /** dyna extension beyond the SSH RFCs: meta configuration in SSHConnectOptions.dyna. A strict bag (an unknown key is a TypeError naming
     *  the valid ones). It only disables costly behaviour or moves safety caps; the protocol defaults above never change with it.
     *  A non-number cap is a TypeError, a fraction or an out-of-range value a RangeError. */
    interface SSHMetaOptions {
        /** "strict" (default): a host absent from known_hosts is refused. "accept-new": an unknown host's key is appended to the
         *  knownHosts file as a HASHED entry ("|1|base64(salt)|base64(HMAC-SHA1(salt, host)) type base64", 20 random salt bytes, the
         *  host written "[host]:port" for a port other than 22; mode 0600 on creation) and trusted; a CHANGED or @revoked key is still
         *  refused. A host certificate is never appended: it needs a matching @cert-authority line. */
        hostKeyPolicy?: "strict" | "accept-new";
        /** Default false. true adds rsa-sha2-512 host keys (RSA PKCS#1 v1.5 over SHA-512, 2048..16384-bit moduli) after the ed25519 ones for
         *  a legacy server; ssh-rsa (SHA-1) is never offered. */
        allowRsaSha2_512?: boolean;
        /** Largest incoming packet_length accepted, 35000..16777216; default 262144. A bigger packet ends the connection (protocol error). */
        maxPacketBytes?: number;
        /** Per-channel receive window, 65536..1073741824; default 2097152. It bounds the bytes buffered per exec stream: the window is granted
         *  back only as the stdout/stderr readers drain their queues. */
        windowBytes?: number;
        /** Channels open at once on one connection, 1..4096; default 64. exec() past it rejects; a forwarded connection past it is closed. */
        maxChannels?: number;
        /** Connect + key exchange + authentication deadline in ms, 1..600000; default 30000. */
        handshakeTimeoutMs?: number;
        /** Bytes (in plus out) after which the client re-keys, 65536..2^32; default 2^30. */
        rekeyBytes?: number;
        /** Seconds after which the client re-keys, 1..86400; default 3600. */
        rekeySeconds?: number;
        /** Default false: a connection keeps the capacity its buffers grew to (socket input, packet output, each channel's outgoing
         *  queue) and reuses it, the fastest choice for steady transfers. true frees a buffer above 256 KiB as soon as it is empty, so an
         *  idle connection after a large burst gives that capacity back (measured: 4 MiB exec bursts left about 6.4 MiB per connection pair
         *  held, against about 0.3 MiB with it; idle forwarded TCP connections hold small buffers either way); it costs about 6..9% CPU on a sustained transfer between
         *  two dyna peers (buffers are re-grown after every drain). Visible in stats.bufferBytes. */
        shrinkBuffers?: boolean;
    }

    /** Options for SSHClient.connect; a strict bag (an unknown key is a TypeError naming the valid ones). */
    interface SSHConnectOptions {
        /** Host name or address (resolved off the event loop). Also the name looked up in known_hosts and the principal a host
         *  certificate must list. */
        host: string;
        /** Default 22. */
        port?: number;
        username: string;
        /** OpenSSH ssh-ed25519 private key text (string or bytes, <= 64 KiB); bad key/type/passphrase: sync TypeError.
         *  Required unless `agent`; with both, tried first and the agent only after the server refused it. */
        privateKey?: string | ArrayBuffer | ArrayBufferView;
        /** Passphrase of an encrypted key; bcrypt-pbkdf rounds > 4096 refused; without privateKey a TypeError. */
        passphrase?: string;
        /** ssh-agent socket path, or true for $SSH_AUTH_SOCK (never implicit; unset, too long or other type: sync TypeError). Keys queried
         *  before signing; all refused: reject "refused for every key tried"; bad agent reply: reject naming the path; hung: handshakeTimeoutMs. */
        agent?: string | true;
        /** An OpenSSH user certificate for privateKey (the "ssh-ed25519-cert-v01@openssh.com AAAA... [comment]" line ssh-keygen -s writes
         *  to key-cert.pub). When set, the client authenticates with the certificate instead of the bare key. A certificate for another
         *  key, a host certificate or a malformed line is a synchronous TypeError. */
        certificate?: string;
        /** Path of the known_hosts file; default $HOME/.ssh/known_hosts. A missing file means every host is unknown. */
        knownHosts?: string;
        /** ProxyJump (OpenSSH -J): a connected SSHClient to tunnel through. The new connection's transport is a direct-tcpip channel
         *  on `via` to host:port (as the jump host resolves it; no local DNS), so the jump server's forward policy applies. Everything
         *  else is end to end: the key exchange, the host key check (against THIS knownHosts entry for host:port, never the jump's),
         *  and authentication. Chains nest (via a client that itself has a via). Closing the inner client leaves the jump usable;
         *  the jump ending ends the inner client ("the jump connection closed"). Not an SSHClient, or not connected: a TypeError. */
        via?: SSHClient;
        /** dyna extension (see SSHMetaOptions). */
        dyna?: SSHMetaOptions;
    }

    /** Options for SSHClient.forwardLocal / forwardRemote; a strict bag. host/port name the TARGET (1..255 bytes; port 1..65535), the
     *  listening end is localHost/localPort (forwardLocal) or remoteHost/remotePort (forwardRemote), default "127.0.0.1" and 0 (an
     *  ephemeral port, read back from listenPort). The other side's pair (remoteHost on forwardLocal, ...) is a TypeError; a bad
     *  port is a RangeError. */
    interface SSHForwardOptions {
        host: string;
        port: number;
        localHost?: string;
        localPort?: number;
        remoteHost?: string;
        remotePort?: number;
    }

    /** One port forward on an SSHClient. Bytes are relayed in C between each TCP connection and its SSH channel, with backpressure both
     *  ways: the socket is read only while the channel's outgoing queue is below 256 KiB (read again at 64 KiB), and the channel window
     *  is granted back only as bytes reach the socket. Half-close passes through (a socket EOF sends CHANNEL_EOF; a CHANNEL_EOF shuts
     *  the socket's write side once the queued bytes are written), and a CHANNEL_CLOSE that arrives while bytes are still queued to the
     *  socket closes it only after they are written (at most dyna.maxChannels such draining sockets per connection, past that one is
     *  closed at once; a socket that never reads keeps its bytes until the SSH connection ends, which closes it). A forward stays alive while the connection does, even when the object is dropped;
     *  the connection ending closes it (and every relayed socket). */
    interface SSHForward {
        /** "local": this process listens and opens a direct-tcpip channel per accepted connection (OpenSSH -L). "remote": the server
         *  listens (tcpip-forward) and opens a forwarded-tcpip channel per connection; this side connects it to the target (-R). */
        readonly type: "local" | "remote";
        /** Where the listener is: the local bind address, or for "remote" the address string sent to the server. */
        readonly listenHost: string;
        /** The bound port (for "remote" with remotePort 0, the port the server allocated and returned in its reply). */
        readonly listenPort: number;
        readonly targetHost: string;
        readonly targetPort: number;
        /** Connections relayed so far. */
        readonly connections: number;
        /** Stops listening (a remote forward sends cancel-tcpip-forward); connections already relayed keep running. Idempotent. */
        close(): void;
        readonly closed: boolean;
    }

    /** What SSHExec.exit and SSHSession.done resolve with. It never rejects: a connection lost first resolves with exitCode null and
     *  `error` set. */
    interface SSHExitStatus {
        /** The exit-status sent, or null (a signal, or none sent). */
        exitCode: number | null;
        /** The exit-signal name without "SIG" ("TERM", "KILL", ...), or null. */
        signal: string | null;
        coreDumped: boolean;
        /** The exit-signal's error message, or "". */
        message: string;
        /** Why the channel ended abnormally (connection lost, client or server closed), or null for an orderly close. */
        error: string | null;
    }

    /** One "session" channel running one command (RFC 4254 exec). The streams are WHATWG ReadableStreams of Uint8Array chunks (one per
     *  SSH data packet) with a byte-length queuing strategy of dyna.windowBytes: the receive window is re-granted to the server (one
     *  WINDOW_ADJUST once half the window is free) only as the queues drain, so an unread stream stops the remote command instead of
     *  growing memory; stdout and stderr share the channel window, so read both (or cancel the one not wanted). The server's EOF closes
     *  both streams; a lost connection errors them. A cancelled stream drops further data for it and keeps granting its share of the
     *  window. The channel stays referenced by the connection until it closes, so `exit` settles even when the object is dropped. */
    interface SSHExec {
        readonly stdout: ReadableStream<Uint8Array>;
        /** Extended data type 1 (stderr); other extended data types are discarded. */
        readonly stderr: ReadableStream<Uint8Array>;
        /** Settles once the channel is closed by both sides; exit-status / exit-signal arrive before that. */
        readonly exit: Promise<SSHExitStatus>;
        /** Queues bytes (a string is UTF-8) for the command's stdin; resolves once they are handed to the transport within the server's window
         *  and max packet size, rejects once the channel closed first. More than 256 MiB queued rejects. After end() it rejects. */
        write(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** Sends EOF on stdin once the queued bytes are out. */
        end(): void;
        /** Sends CHANNEL_CLOSE (after the open completes when still opening); `exit` then settles. */
        close(): void;
        /** Sends a "signal" channel request (RFC 4254 6.9, no reply) with the name without "SIG" ("TERM", "INT", "HUP", ...; "SIGTERM" is
         *  accepted and sent as "TERM"). A name that is not 1..20 of [A-Z0-9] is a TypeError. The server may ignore it (OpenSSH sshd does
         *  for exec channels without a pty). */
        signal(name: string): void;
    }

    /** An authenticated SSH connection. A connected client holds the event loop (and itself) alive until close() or until the connection
     *  ends; close() sends DISCONNECT (by application) and settles every open channel with error "the client was closed". When the
     *  process exits with a connection still live (std.exit, an uncaught error), the runtime releases it and its SSHExec objects without
     *  calling back into JS; the exit code is the script's. */
    class SSHClient implements DynResource {
        /** new SSHClient() is a TypeError: use connect. */
        constructor();
        /** Connects, exchanges keys, verifies the host key, authenticates with the ed25519 key, and resolves with the client. Option errors
         *  throw synchronously; network, protocol, host-key and authentication failures reject with an Error whose message starts "SSH: ". */
        static connect(options: SSHConnectOptions): Promise<SSHClient>;
        /** Opens a session channel and runs `command`; resolves once the server accepted the exec request, rejects when the channel or
         *  the request is refused, the connection is not ready, or dyna.maxChannels are open. */
        exec(command: string): Promise<SSHExec>;
        /** Starts a client-initiated re-key now (no-op while one runs); TypeError before authentication. */
        rekey(): void;
        /** OpenSSH -L: listens on localHost:localPort now (synchronous; a bind error throws an Error starting "SSH: ") and, per accepted
         *  connection, opens a direct-tcpip channel to host:port carrying the peer's address as originator. A refused channel (the
         *  server's policy, or the target unreachable from the server) closes that connection with no bytes. Throws when the connection
         *  is not ready. */
        forwardLocal(options: SSHForwardOptions): SSHForward;
        /** OpenSSH -R: sends tcpip-forward for remoteHost:remotePort and resolves once the server accepts (remotePort 0: listenPort is
         *  the port the server chose); rejects with "SSH: " when the server refuses, or the connection ends first. Each connection the
         *  server reports (forwarded-tcpip, matched by the bound port) is connected from here to host:port; a failed connect refuses
         *  the channel. Where the server binds is its decision (OpenSSH: GatewayPorts; this module's server: publicForwardBinds). */
        forwardRemote(options: SSHForwardOptions): Promise<SSHForward>;
        /** The verified host key: {type, fingerprint "SHA256:<base64>"} (ssh-keygen -l format; for a host certificate, the type is
         *  "ssh-ed25519-cert-v01@openssh.com" and the fingerprint is the certified key's), or null before the key exchange. */
        readonly hostKey: { type: string; fingerprint: string } | null;
        /** The negotiated algorithms, or null before the first key exchange. kex is the canonical name ("sntrup761x25519-sha512" also
         *  when the @openssh.com alias was negotiated). */
        readonly algorithms: { kex: string; hostKey: string; cipherClientToServer: string; cipherServerToClient: string; strictKex: boolean } | null;
        /** The server's EXT_INFO server-sig-algs list, or null when it sent none. */
        readonly serverSignatureAlgorithms: string[] | null;
        /** Wire counters (bytes include packet framing and tags), the number of completed re-keys, and bufferBytes: the capacity of the
         *  buffers this connection holds now (socket input, packet output, pending packets, each open channel's outgoing queues). */
        readonly stats: { bytesIn: number; bytesOut: number; packetsIn: number; packetsOut: number; rekeys: number; openChannels: number; bufferBytes: number };
        /** The server's identification line (e.g. "SSH-2.0-OpenSSH_10.5"), or null. */
        readonly serverVersion: string | null;
        /** The last USERAUTH_BANNER text (at most 64 KiB), or null. */
        readonly banner: string | null;
        /** True while authenticated and not ended. */
        readonly connected: boolean;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** dyna extension beyond the SSH RFCs: SSHServerOptions.dyna, a strict bag of safety caps. Each default is a safe limit for an
     *  internet-facing server; raise one only on purpose. A non-number is a TypeError, a fraction or an out-of-range value a RangeError. */
    interface SSHServerMetaOptions {
        /** Connections open at once (any state), 1..65536; default 64. A connection accepted past it is closed at once and counted in
         *  stats.refused. */
        maxConnections?: number;
        /** Failed publickey attempts (a refused query, a bad signature, a refused key, a non-publickey method; "none" does not count)
         *  before DISCONNECT 14, 1..100; default 6. */
        maxAuthTries?: number;
        /** Channels open at once per connection (session and direct-tcpip alike), 1..4096; default 10. CHANNEL_OPEN past it gets OPEN_FAILURE 4
         *  (resource shortage); for a local forward, that connection is closed with no bytes. */
        maxChannels?: number;
        /** Per-channel receive window (the client's stdin buffered for the session.stdin reader), 65536..1073741824; default 2097152. */
        windowBytes?: number;
        /** Largest incoming packet_length accepted, 35000..16777216; default 262144. */
        maxPacketBytes?: number;
        /** Identification + key exchange + authentication deadline per connection in ms, 1..600000; default 30000. */
        handshakeTimeoutMs?: number;
        /** Disconnect an authenticated connection that received nothing for this many ms, 0..604800000; default 0 (off). */
        idleTimeoutMs?: number;
        /** Bytes (in plus out) after which the server re-keys a connection, 65536..2^32; default 2^30. */
        rekeyBytes?: number;
        /** Seconds after which the server re-keys a connection, 1..86400; default 3600. */
        rekeySeconds?: number;
        /** Default false. true frees each connection's buffers above 256 KiB as soon as they are empty (see SSHMetaOptions.shrinkBuffers):
         *  less memory per idle connection after a burst, some CPU per drain on sustained transfers. */
        shrinkBuffers?: boolean;
        /** Default false: every remote forward (tcpip-forward) binds 127.0.0.1 whatever address the client asked for (OpenSSH's
         *  GatewayPorts no), and the reply still echoes the client's address string. true binds the requested address: "", "*",
         *  "0.0.0.0" and "::" mean every interface, "localhost" stays 127.0.0.1 (GatewayPorts clientspecified). */
        publicForwardBinds?: boolean;
    }

    /** What SSHServerOptions.forward receives, once per direct-tcpip channel (type "local") and per tcpip-forward request ("remote"). */
    interface SSHForwardRequest {
        type: "local" | "remote";
        username: string;
        /** "local": the target the client wants this server to connect to. "remote": the address string and port the client asked this
         *  server to listen on (port 0: any). */
        host: string;
        port: number;
        /** "local": the originator address and port the client reported (informational, unverified); "remote": the client's address and 0. */
        originatorHost: string;
        originatorPort: number;
        remoteAddress: string;
        keyFingerprint: string;
    }

    /** What SSHServerOptions.authorize receives, after the client proved possession of the key (the signature verified). */
    interface SSHAuthRequest {
        username: string;
        /** The publickey algorithm: "ssh-ed25519", "sk-ssh-ed25519@openssh.com" or "ssh-ed25519-cert-v01@openssh.com". */
        keyType: string;
        /** SHA256 fingerprint of the key (for a certificate, of the certified key). */
        fingerprint: string;
        /** The key as an authorized_keys line without comment ("ssh-ed25519 AAAA..."); for a certificate, the certified key. */
        publicKey: string;
        /** True when the key is in authorizedKeys; always true for a certificate (one that fails any check against trustedUserCAKeys is
         *  refused before the hook is called, so the hook cannot override a certificate's CA, validity, principals or critical options). */
        listed: boolean;
        /** The parsed certificate (it passed every check), or null for a plain key. */
        certificate: SSHCertificateInfo | null;
        remoteAddress: string;
    }

    interface SSHCertificateInfo {
        type: "user" | "host";
        keyId: string;
        /** Number (exact below 2^53; Infinity at or above). */
        serial: number;
        principals: string[];
        /** Seconds since the epoch; validBefore is Infinity for "forever". */
        validAfter: number;
        validBefore: number;
        caFingerprint: string;
    }

    /** Options for SSHServer.listen; a strict bag (an unknown key is a TypeError naming the valid ones). Every error is a synchronous
     *  TypeError or RangeError naming the option. */
    interface SSHServerOptions {
        /** Address to bind; default "127.0.0.1" (pass "0.0.0.0" or "::" to listen on every interface). */
        host?: string;
        /** Default 0: an ephemeral port, read back from server.port. */
        port?: number;
        /** The host's OpenSSH ssh-ed25519 private key text, as a string or bytes (same rules as SSHConnectOptions.privateKey). */
        hostKey: string | ArrayBuffer | ArrayBufferView;
        hostKeyPassphrase?: string;
        /** An OpenSSH host certificate for hostKey ("ssh-ed25519-cert-v01@openssh.com AAAA..."); when set, the server also offers the
         *  ssh-ed25519-cert-v01@openssh.com host key algorithm (first) and sends the certificate to clients that pick it. */
        hostCertificate?: string;
        /** authorized_keys text: one "ssh-ed25519|sk-ssh-ed25519@openssh.com <base64> [comment]" per line; blank and # lines are skipped.
         *  A line with key options (from=, command=, ...) is a TypeError: express such policy in `authorize`. */
        authorizedKeys?: string;
        /** CA public keys in the same format; a user certificate signed by one of them is accepted under the rules in the module doc. */
        trustedUserCAKeys?: string;
        /** Optional decision hook. Without it, a key is accepted when `listed`. With it, every plain key whose signature verified, and
         *  every certificate that passed all certificate checks, is passed in and the hook decides: return true / false, or a Promise of one (the connection waits; a rejection or a non-boolean means false).
         *  A thrown error means false. Signature-less queries get PK_OK when the key is listed or (for a plain key) a hook is set. At least one of
         *  authorizedKeys, trustedUserCAKeys and authorize is required. */
        authorize?: (req: SSHAuthRequest) => boolean | Promise<boolean>;
        /** Called once per session channel, when the client sends "exec" or "shell" (the CHANNEL_SUCCESS reply is sent first). If it
         *  returns a Promise, the session exits with status 0 when it fulfils, or writes the reason to stderr and exits 1 when it rejects,
         *  unless exit()/kill()/close() came first. A synchronous throw does the same as a rejection. A sync return leaves the session open
         *  until exit(), kill() or close(). */
        onSession: (session: SSHSession) => void | Promise<void>;
        /** USERAUTH_BANNER text sent once per connection before the first authentication reply; at most 64 KiB. */
        banner?: string;
        /** Port forwarding policy. Without it every direct-tcpip channel gets OPEN_FAILURE 1 (administratively prohibited) and every
         *  tcpip-forward REQUEST_FAILURE. With it, each request is passed in and the hook decides: true / false, or a Promise of one
         *  (the channel stays pending, the global request replies stay in order); a throw, a rejection or a non-boolean means false. A
         *  user certificate without permit-port-forwarding is refused before the hook. Allowed "local" channels connect from here to
         *  host:port (a failed connect: OPEN_FAILURE 2 connect failed). Allowed "remote" requests listen (see
         *  SSHServerMetaOptions.publicForwardBinds), at most 16 per connection, and the reply carries the bound port when port 0 was
         *  asked; cancel-tcpip-forward stops one. Listeners and relayed sockets close when their connection ends. ProxyJump (OpenSSH -J)
         *  through this server is a "local" request. */
        forward?: (req: SSHForwardRequest) => boolean | Promise<boolean>;
        /** dyna extension (see SSHServerMetaOptions). */
        dyna?: SSHServerMetaOptions;
    }

    /** One session channel on an SSHServer. Requests the server honours before exec/shell: pty-req (refused for a certificate without
     *  permit-pty; at most one), env (only LANG and LC_* names of [A-Z0-9_], values up to 1024 bytes, at most 64), then exactly one exec or
     *  shell or subsystem (a name of 1..64 bytes; dyna:sftp serves "sftp"). After it: signal (calls onsignal), window-change (updates pty,
     *  calls onresize). x11-req, auth-agent-req and every other request get CHANNEL_FAILURE; port forwarding is decided by SSHServerOptions.forward. No process is spawned: the session is
     *  what onSession does with it. */
    interface SSHSession {
        readonly user: string;
        readonly type: "exec" | "shell" | "subsystem";
        /** The command to run: a certificate's force-command when present, else the exec command or the subsystem name; null for a plain shell. */
        readonly command: string | null;
        /** The command (exec) or subsystem name the client asked for, or null (shell). */
        readonly requestedCommand: string | null;
        /** The accepted env requests. */
        readonly env: Record<string, string>;
        /** The pty-req values (updated sizes arrive through onresize), or null. */
        readonly pty: { term: string; cols: number; rows: number; width: number; height: number } | null;
        readonly remoteAddress: string;
        readonly keyType: string;
        readonly keyFingerprint: string;
        readonly certificate: SSHCertificateInfo | null;
        /** The client's stdin (CHANNEL_DATA), with the same window-driven backpressure as SSHExec.stdout; closes on the client's EOF. */
        readonly stdin: ReadableStream<Uint8Array>;
        /** Settles once the channel is closed by both sides, with what this side sent (exitCode / signal), or error when the connection died. */
        readonly done: Promise<SSHExitStatus>;
        /** Queue stdout bytes (CHANNEL_DATA) / stderr bytes (EXTENDED_DATA 1); resolve once handed to the transport within the client's
         *  window, reject when the session's output is closed. More than 256 MiB queued per stream rejects. */
        write(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        writeStderr(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** Sends EOF once the queued output is out. */
        end(): void;
        /** Once the queued output is out: EOF, exit-status `code` (default 0, 0..2^32-1), then CHANNEL_CLOSE. Later calls are no-ops. */
        exit(code?: number): void;
        /** Like exit, with exit-signal `signal` (default "TERM"; "SIG" prefix stripped; 1..20 of [A-Z0-9]) and an optional message. */
        kill(signal?: string, message?: string): void;
        /** CHANNEL_CLOSE now, without exit status; queued output is dropped. */
        close(): void;
        /** Set to receive the client's "signal" requests (name without "SIG"). */
        onsignal: ((name: string) => void) | null;
        /** Set to receive "window-change" requests. */
        onresize: ((size: { cols: number; rows: number; width: number; height: number }) => void) | null;
    }

    /** A listening SSH server (publickey and certificate logins only). It holds the event loop alive until close(); close() stops
     *  accepting and ends every connection with DISCONNECT 11 "the server was closed" (open sessions' done settle with that error). */
    class SSHServer implements DynResource {
        /** new SSHServer() is a TypeError: use listen. */
        constructor();
        /** Binds, starts accepting, and returns the server. */
        static listen(options: SSHServerOptions): SSHServer;
        readonly port: number;
        readonly host: string;
        /** accepted / refused (past maxConnections or after close) connections so far; connections, authenticated and channels open now. */
        readonly stats: { accepted: number; refused: number; connections: number; authenticated: number; channels: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A key for SSHAgent.listen keys / add; strict bag. */
    interface SSHAgentKey {
        /** OpenSSH ssh-ed25519 private key text, as SSHConnectOptions.privateKey. */
        privateKey: string | ArrayBuffer | ArrayBufferView;
        /** Decrypts privateKey once; the agent keeps the decrypted key in memory (wiped on remove, expiry, close). */
        passphrase?: string;
        /** ssh-ed25519-cert-v01 USER certificate line for privateKey; held instead of the bare key. Host cert or other key: TypeError. */
        certificate?: string;
        /** Default "". */
        comment?: string;
        /** 1..31536000 s (ssh-add -t); default forever; checked on every request and list(). */
        lifetimeSeconds?: number;
    }

    /** One identity held by an SSHAgent. */
    interface SSHAgentIdentity {
        type: "ssh-ed25519" | "ssh-ed25519-cert-v01@openssh.com" | "sk-ssh-ed25519@openssh.com";
        /** "type AAAA..." line without comment. */
        publicKey: string;
        /** "SHA256:..." as ssh-keygen -l. */
        fingerprint: string;
        comment: string;
        /** Epoch ms, or null. */
        expiresAt: number | null;
        /** false for sk-ssh-ed25519 (listed; SIGN_REQUEST fails: no FIDO access, owner decision). */
        canSign: boolean;
    }

    /** SSHAgent.listen options; strict bag. */
    interface SSHAgentOptions {
        /** Unix socket to create: an existing path or one over 103/107 bytes (macOS/Linux) is a TypeError. Mode 0600 (umask 0177);
         *  unlinked by close(), runtime shutdown and process exit (not SIGKILL). */
        path: string;
        /** Keys held from the start (see add). */
        keys?: SSHAgentKey[];
        dyna?: {
            /** 1..65536, default 256; past it ADD fails (wire FAILURE, add() RangeError). */
            maxKeys?: number;
            /** 1..4096, default 64; one past it is closed at accept. */
            maxConnections?: number;
        };
    }

    /** ssh-agent (draft-miller-ssh-agent) for ssh-add/ssh: same-euid or root peers only; ed25519, its user certs, sk listed not signed.
     *  FAILURE for confirm, EXTENSION, unknown; msg <= 256 KiB; sign and lock KDF on the worker pool; no agent forwarding anywhere. */
    class SSHAgent implements DynResource {
        /** TypeError: use listen. */
        constructor();
        /** Creates the socket and accepts; holds the event loop until close(). */
        static listen(options: SSHAgentOptions): SSHAgent;
        /** Adds or replaces (same public key) a key; returns its identity. Locked: TypeError; past maxKeys: RangeError. */
        add(key: SSHAgentKey): SSHAgentIdentity;
        /** Removes by public key line; returns whether held. Locked or bad line: TypeError. */
        remove(publicKey: string): boolean;
        /** Drops every key; locked: TypeError. */
        removeAll(): void;
        /** Identities in add order (the wire order); [] while locked. */
        list(): SSHAgentIdentity[];
        readonly path: string | null;
        /** LOCK/UNLOCK over the wire (PBKDF2-HMAC-SHA512 x50000, 16-byte salt); locked lists and signs nothing. */
        readonly locked: boolean;
        /** refused: failed SIGNs and wrong UNLOCKs; offloaded: signatures and KDF runs done on the worker pool. */
        readonly stats: { keys: number; connections: number; accepted: number; refusedConnections: number; requests: number; signatures: number; refused: number; offloaded: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
