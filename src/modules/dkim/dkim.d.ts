/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:dkim
/** dyna:dkim — DKIM signing and verification (RFC 6376, RFC 8463 Ed25519, RFC 8301 no rsa-sha1).
 *  The message is copied at call; parse, canonicalization, hashing and crypto run on the shared pool (inline under --io-threads 0); option errors throw TypeError/RangeError synchronously. */
declare module "dyna:dkim" {
    /** "simple" or "relaxed" for header/body; one word means header with body "simple". */
    type DkimCanonicalization = "relaxed/relaxed" | "relaxed/simple" | "simple/relaxed" | "simple/simple" | "relaxed" | "simple";
    interface DkimSignOptions {
        /** d=; dot-separated labels of letters, digits, "-", "_" or UTF-8 (RFC 8616 U-labels), no empty label, at most 256 bytes. */
        domain: string;
        /** s=; printable ASCII without ";" or space, at most 256 bytes. */
        selector: string;
        /** PEM private key (PKCS#8 or PKCS#1; RSA >= 1024 bits or Ed25519), or a 32-byte Ed25519 seed. */
        privateKey: string | ByteView;
        /** Must match the key type when given (else the promise rejects); default from the key. */
        algorithm?: "rsa-sha256" | "ed25519-sha256";
        /** Default "relaxed/relaxed". */
        canonicalization?: DkimCanonicalization;
        /** Header names to sign (1..128, must include From); every present instance is signed bottom-up.
         *  Default: from to cc subject date message-id reply-to in-reply-to references mime-version content-type content-transfer-encoding sender. */
        headers?: string[];
        /** Add one extra h= entry per listed name so a later added instance breaks the signature; default false. */
        oversign?: boolean;
        /** i=; must be within domain at verify time. */
        identity?: string;
        /** Emit l= with the canonical body length; default false. */
        bodyLength?: boolean;
        /** t= in Unix seconds; default now; null omits t=. */
        timestamp?: number | null;
        /** x= = t (or now) + expiresIn seconds, >= 1. */
        expiresIn?: number;
    }
    /** Lookup of "<selector>._domainkey.<domain>": string (one record), string[] (its chunks), string[][] (resolveTxt shape, first record), null = no record.
     *  A rejection whose message has "rcode 3", NXDOMAIN or ENOTFOUND is permerror; any other is temperror. */
    type DkimLookup = ((name: string) => string | string[] | string[][] | null | Promise<string | string[] | string[][] | null>) | { resolveTxt(name: string): Promise<string[][]> };
    interface DkimVerifyOptions {
        /** Required, no default resolver: a function or any object with resolveTxt() (e.g. a dyna:dns DNSResolver), else TypeError (thrown).
         *  Lookups for all signatures are started before any is awaited. */
        lookup: DkimLookup;
        /** Unix seconds for the x= check; default now. */
        now?: number;
        /** Signatures evaluated top-down (1..64, default 5); later ones report neutral; at most 64 are reported. */
        maxSignatures?: number;
        /** RSA keys below this are permerror (512..16384, default 1024 per RFC 8301). */
        minRsaBits?: number;
    }
    /** One DKIM-Signature field, in header order. pass/fail/neutral/temperror/permerror (RFC 8601 result names). */
    interface DkimResult {
        status: "pass" | "fail" | "neutral" | "temperror" | "permerror";
        domain?: string;
        selector?: string;
        algorithm?: "rsa-sha256" | "ed25519-sha256";
        identity?: string;
        signedHeaders?: string[];
        bodyLength?: number;
        timestamp?: number;
        expires?: number;
        /** Key record t=y. */
        testing?: boolean;
        reason?: string;
    }
    /** Resolves the DKIM-Signature field (folded at 76 columns, CRLF-terminated) to prepend to the message.
     *  Bare CR or LF counts as CRLF; at most 1000 header fields; rejects on a malformed header, a missing From or a bad key. */
    function sign(message: BytesInput, opts: DkimSignOptions): Promise<string>;
    /** Resolves one result per signature ([] when unsigned); never rejects for message content: a malformed header or over 1000 fields is one permerror.
     *  Key records over 64 KiB and tag-lists over 64 tags are permerror; p= is SubjectPublicKeyInfo or bare RSAPublicKey (RSA), 32 raw bytes (Ed25519). */
    function verify(message: BytesInput, opts: DkimVerifyOptions): Promise<DkimResult[]>;
}
