/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:smtp
/** dyna:smtp — SMTP client and server (RFC 5321): STARTTLS/implicit TLS, SASL AUTH, PIPELINING, 8BITMIME, SMTPUTF8, BDAT/BINARYMIME, DSN.
 *  Parse, codec and SASL run on the pool (--io-threads 0: inline on the JS thread, top of the callback stack), TLS records on the main thread: client ~63 us/msg plain, ~62-100 us/msg STARTTLS (vs exim). */
declare module "dyna:smtp" {
    /** SmtpClient options; unknown keys throw TypeError. */
    interface SmtpOptions {
        host: string;
        /** Default 587, or 465 with tls "implicit". */
        port?: number;
        /** Default "starttls" (required: fails when not offered); "opportunistic" falls back to plaintext; "none" never upgrades. */
        tls?: "starttls" | "opportunistic" | "implicit" | "none";
        /** CA PEM file pinned for TLS; default the system trust store with hostname checking. */
        ca?: string;
        /** TLS SNI and certificate name; default host. */
        servername?: string;
        /** EHLO domain or address literal; default "localhost". */
        name?: string;
        /** AUTH identity; needs pass or token. */
        user?: string;
        pass?: string;
        /** OAuth 2.0 bearer token: OAUTHBEARER, else XOAUTH2. */
        token?: string;
        authzid?: string;
        /** Allowed SASL mechanisms; default every one except CRAM-MD5. Preference: SCRAM-SHA-256, -512, -1, PLAIN, LOGIN, CRAM-MD5. */
        mechanisms?: ("SCRAM-SHA-256" | "SCRAM-SHA-512" | "SCRAM-SHA-1" | "PLAIN" | "LOGIN" | "CRAM-MD5" | "OAUTHBEARER" | "XOAUTH2")[];
        /** Allow PLAIN, LOGIN, OAUTHBEARER, XOAUTH2 without TLS; default false (the session fails with SMTP_AUTH). */
        insecureAuth?: boolean;
        /** Connect, greeting, EHLO, TLS and AUTH deadline; default 10000. */
        connectTimeoutMs?: number;
        /** Wait for any reply; default 300000 (RFC 5321 4.5.3.2 floor for DATA). */
        commandTimeoutMs?: number;
        /** An idle connection sends QUIT after this; default 200. */
        idleTimeoutMs?: number;
        /** Concurrent connections, 1..64; default 1. Messages queue FIFO. */
        maxConnections?: number;
        /** QUIT and reconnect after this many messages; 0 = unlimited; default 100.
         *  A 421 on a connection that delivered a message requeues the in-flight one first in the queue; else it rejects. */
        maxMessagesPerConnection?: number;
        /** Highest SCRAM iteration count accepted, 4096..1000000; default 1000000. */
        scramMaxIterations?: number;
    }
    /** RFC 3461 DSN parameters; the send rejects with SMTP_LOCAL unless the server offers DSN. */
    interface SmtpDsn {
        ret?: "FULL" | "HDRS";
        /** ASCII, at most 100 octets; xtext-encoded on the wire. */
        envid?: string;
        notify?: ("NEVER" | "SUCCESS" | "FAILURE" | "DELAY")[];
        /** Send ORCPT=rfc822;<address> for ASCII recipients. */
        orcpt?: boolean;
    }
    /** One message: data is the full RFC 5322 message; bare CR/LF become CRLF and lines are dot-stuffed.
     *  Under BDAT with BODY=BINARYMIME the octets go exactly as given (RFC 3030). */
    interface SmtpMessage {
        /** Envelope sender; "" is the null reverse-path. */
        from: string;
        /** 1..1000 envelope recipients. */
        to: string | string[];
        data: string | Uint8Array | ArrayBuffer;
        /** Use BDAT when the server offers CHUNKING; BINARYMIME is used (with it) for NUL, bare CR/LF, lines over 998, 8-bit without 8BITMIME. */
        bdat?: boolean;
        dsn?: SmtpDsn;
    }
    interface SmtpRecipientError {
        recipient: string;
        code: number;
        /** RFC 3463 "class.subject.detail", or null. */
        enhanced: string | null;
        text: string;
    }
    /** Resolved when the server accepted the message for at least one recipient. */
    interface SmtpResult {
        accepted: string[];
        rejected: SmtpRecipientError[];
        code: number;
        enhanced: string | null;
        text: string;
    }
    /** Rejection value. SMTP_LOCAL refuses before sending (8-bit body without 8BITMIME, SIZE, line > 998, bad address). */
    interface SmtpError extends Error {
        code: "SMTP_REJECTED" | "SMTP_LOCAL" | "SMTP_AUTH" | "SMTP_TLS" | "SMTP_CONNECTION" | "SMTP_TIMEOUT" | "SMTP_CLOSED";
        smtpCode?: number;
        enhanced?: string | null;
        response?: string;
        rejected?: SmtpRecipientError[];
    }
    /** Counters; percentiles over the last 1024 samples in microseconds. */
    interface SmtpStats {
        messages: number;
        jobs: number;
        connections: number;
        queued: number;
        /** Pool handoff latency (submit to start, finish to callback). */
        handoffP50Us: number;
        handoffP99Us: number;
        /** Main-thread time per message in this module's callbacks (TLS record work excluded). */
        mainP50Us: number;
        mainP99Us: number;
    }
    /** SMTP client with a FIFO queue over up to maxConnections sessions. Every failure rejects with SmtpError. */
    class SmtpClient implements DynResource {
        constructor(opts: SmtpOptions);
        /** Queues one message; throws TypeError/RangeError on bad arguments, rejects with SmtpError. */
        send(msg: SmtpMessage): Promise<SmtpResult>;
        /** Sends QUIT on every session once the queue drains; later send() throws. */
        quit(): Promise<void>;
        stats(): SmtpStats;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** SmtpServer options; unknown keys throw TypeError, out-of-range numbers RangeError. */
    interface SmtpServerOptions {
        /** Bind address; default "127.0.0.1". */
        host?: string;
        /** Default 25, or 465 with tls "implicit"; 0 = ephemeral (read .port after listen()). */
        port?: number;
        /** Domain in the greeting and EHLO reply; default "localhost". */
        name?: string;
        /** Default "starttls" when cert is given, else "none"; STARTTLS discards plaintext pipelined after the command (RFC 3207). */
        tls?: "none" | "starttls" | "implicit";
        /** PEM file paths; required unless tls is "none" (TypeError). */
        cert?: string;
        key?: string;
        /** Refuse MAIL before STARTTLS (530 5.7.0). */
        requireTls?: boolean;
        /** Refuse MAIL before AUTH (530 5.7.0); TypeError without onAuth. */
        requireAuth?: boolean;
        /** Allow AUTH without TLS; default false (538 5.7.11). */
        insecureAuth?: boolean;
        /** Offered SASL mechanisms; default all below; CRAM-MD5 is a RangeError. AUTH is offered only with onAuth. */
        mechanisms?: ("SCRAM-SHA-256" | "SCRAM-SHA-512" | "SCRAM-SHA-1" | "PLAIN" | "LOGIN" | "OAUTHBEARER" | "XOAUTH2")[];
        /** EHLO extensions not to offer, e.g. ["CHUNKING"]; dropping CHUNKING drops BINARYMIME; unknown names RangeError. */
        disable?: ("PIPELINING" | "8BITMIME" | "SMTPUTF8" | "DSN" | "CHUNKING" | "BINARYMIME" | "SIZE" | "ENHANCEDSTATUSCODES")[];
        /** SIZE limit in octets, 1..2^40; default 10485760 (552 past it). */
        maxMessageSize?: number;
        /** Per transaction, default 100 (452 past it). */
        maxRecipients?: number;
        /** Bad commands before 421, default 10. */
        maxErrors?: number;
        /** Messages before 421 4.7.0, default 100. */
        maxMessagesPerSession?: number;
        /** Failed AUTH before 421 4.7.0, default 3. */
        maxAuthFailures?: number;
        /** Concurrent sessions; one more gets 421 4.7.0 and is closed; default 100. */
        maxConnections?: number;
        /** Idle limits in ms, 1..3600000, default 300000; 421 then close. */
        commandTimeoutMs?: number;
        dataTimeoutMs?: number;
        /** onChunk size bound in octets, 1024..16 MiB; default 65536. */
        chunkSize?: number;
        onHelo?: (session: SmtpSession, helo: { name: string; ehlo: boolean }) => SmtpVerdict | Promise<SmtpVerdict>;
        /** PLAIN/LOGIN carry password, OAuth token; SCRAM returns SmtpScramCreds (false denies). */
        onAuth?: (session: SmtpSession, auth: SmtpAuthAttempt) => SmtpVerdict | SmtpScramCreds | Promise<SmtpVerdict | SmtpScramCreds>;
        onMail?: (session: SmtpSession, mail: SmtpMailFrom) => SmtpVerdict | Promise<SmtpVerdict>;
        onRcpt?: (session: SmtpSession, rcpt: SmtpRcptTo) => SmtpVerdict | Promise<SmtpVerdict>;
        /** Body bytes as they arrive (unstuffed, <= chunkSize, the buffer is handed over); the next chunk waits for a returned promise. A throw makes onData's reply 451. */
        onChunk?: (session: SmtpSession, chunk: Uint8Array) => void | Promise<void>;
        /** After the last body octet; data holds the whole body unless onChunk is set. */
        onData?: (session: SmtpSession, msg: SmtpEnvelope) => SmtpVerdict | Promise<SmtpVerdict>;
    }
    /** One connection; the same object is passed to every hook of the session. */
    interface SmtpSession {
        id: number;
        remoteAddress: string;
        remotePort: number;
        tls: boolean;
        helo: string | null;
        /** Set after a successful AUTH. */
        user: string | null;
    }
    /** undefined/true accept; false refuses (550 5.7.1; RCPT 550 5.1.1; AUTH 535 5.7.8); a throw or rejection gives 451 4.3.0 (454 4.7.0 for AUTH). */
    type SmtpVerdict = void | boolean | { code: number; message: string; enhanced?: string };
    interface SmtpAuthAttempt {
        mechanism: "PLAIN" | "LOGIN" | "XOAUTH2" | "OAUTHBEARER" | "SCRAM-SHA-1" | "SCRAM-SHA-256" | "SCRAM-SHA-512";
        username: string;
        authzid: string | null;
        password?: string;
        token?: string;
    }
    /** RFC 5802 stored credentials; storedKey and serverKey are the digest length (20, 32, 64); salt 1..256 bytes. The proof is checked on the pool. */
    interface SmtpScramCreds {
        salt: BytesInput;
        iterations: number;
        storedKey: BytesInput;
        serverKey: BytesInput;
    }
    interface SmtpMailFrom {
        address: string;
        body?: "8BITMIME" | "BINARYMIME";
        smtputf8: boolean;
        ret?: "FULL" | "HDRS";
        envid: string | null;
    }
    interface SmtpRcptTo {
        address: string;
        /** RFC 3461 NOTIFY value as sent, e.g. "SUCCESS,FAILURE". */
        notify: string | null;
        orcpt: string | null;
    }
    interface SmtpEnvelope extends Omit<SmtpMailFrom, "address"> {
        from: string;
        to: SmtpRcptTo[];
        /** Body octets (unstuffed). */
        size: number;
        data?: Uint8Array;
    }
    /** Counters; percentiles in microseconds over the last 1024 samples. */
    interface SmtpServerStats {
        connections: number;
        sessions: number;
        /** Connections refused by maxConnections. */
        refused: number;
        messages: number;
        jobs: number;
        /** Hook throws, rejections and invalid verdicts. */
        hookErrors: number;
        handoffP50Us: number;
        handoffP99Us: number;
        /** Main-thread time per message in this module's callbacks, hooks excluded. */
        mainP50Us: number;
        mainP99Us: number;
    }
    /** SMTP server; command parse, unstuffing, SASL and SCRAM run as pool jobs, hooks on the main thread.
     *  Main thread incl. TLS records and reactor, N=2048: ~33 us/msg plain, ~42 STARTTLS (1 KiB); ~51 / ~77 (64 KiB). */
    class SmtpServer implements DynResource {
        constructor(opts: SmtpServerOptions);
        /** Binds and accepts; returns the port; throws InternalError when the bind fails, TypeError when called twice. */
        listen(): number;
        readonly port: number;
        stats(): SmtpServerStats;
        /** Stops accepting; open sessions get 421 4.3.2 and are closed. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
