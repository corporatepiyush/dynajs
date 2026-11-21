/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:crypto
/** dyna:crypto — AEAD (AES-GCM, ChaCha20-Poly1305), signatures (RSA, ECDSA, Ed25519), key exchange, password hashing, HMAC/KDFs, X.509, JWT (HOTP/TOTP live in dyna:otp).
 *  Secrets compare in constant time. The OpenSSL-backed names (AEAD classes, RSA/ECDSA/ECDH/X509, Ed25519/X25519, Scrypt) exist only when crypto is built with the OpenSSL library (CONFIG_OPENSSL; tools/modules.tsv lists pkg openssl for crypto, with no dependency on the "tls" module): feature-detect via the namespace. */
declare module "dyna:crypto" {
    /** keyed/derive options for the BLAKE one-shots re-exported here. */
    interface Blk2Opts {
        key?: BytesInput;
        length?: number;
    }
    /** BLAKE3: key exactly 32 bytes; {context}/{deriveKey} = derive-key. */
    interface Blk3Opts {
        key?: BytesInput;
        length?: number;
        context?: string;
        deriveKey?: string;
    }
    /** A PEM key pair. */
    interface KeyPair {
        privateKey: string;
        publicKey: string;
    }

    /** OpenBSD $2b$ bcrypt. The password KDFs (Bcrypt.hash/verify, Argon2id.hash/verify, PBKDF2, Scrypt) return Promises: the arguments are checked and the password and salt copied on the calling thread, then the derivation runs as one job on the shared io pool while the event loop keeps serving (with --io-threads 0 it runs inline and the promise is already settled when the call returns). Argument errors (a wrong type, a value out of range, an unknown option key, a malformed PHC string) throw TypeError or RangeError synchronously, before any work is queued; only a failure of the derivation itself (out of memory, a parameter set the engine rejects) rejects the promise. N concurrent calls run up to the pool width at once, each holding its own working memory (Argon2id: `memory` KiB, Scrypt: 128*r*N bytes) until it settles; `dyna.maxConcurrent` (KDFMetaOptions) lowers that for the memory-hard ones. A script that hits --timeout-ms while a derivation runs is stopped within the hard-stop grace (exit 113): the job cannot be interrupted mid-derivation, so exit no longer waits out the 10 s teardown for it; the watchdog caps the drain of cancelled offloaded work at the grace window (tests/sys/test_sys_audit_w2_native_1.js pins it). There is no synchronous form; HKDF, which is cheap, stays synchronous. */
    namespace Bcrypt {
        /** Hashes a password with a fresh salt; rounds 4..31 (else RangeError); a password over bcrypt's 72-byte limit is a RangeError, never truncated. */
        function hash(password: string, rounds?: number): Promise<string>;
        /** Constant-time verify; accepts $2a$/$2b$/$2y$ prefixes; the hash's cost is capped at 20. */
        function verify(password: string, hash: string): Promise<boolean>;
    }

    /** dyna meta configuration of the memory-hard KDFs (Argon2id.hash/verify, Scrypt), an extension beyond the RFCs. A strict bag: an unknown key is a TypeError. `maxConcurrent` (an integer 1..2^31-1, else RangeError) makes this call wait, after its arguments are checked and before any memory is allocated, until fewer than `maxConcurrent` memory-hard derivations of this thread (Argon2id and Scrypt, capped or not) are in flight; waiting calls start in arrival order as slots free. Unset, a call is bounded only by the io pool width. Each Worker counts its own. */
    interface KDFMetaOptions {
        maxConcurrent?: number;
    }

    /** Argon2id v0x13 (RFC 9106). hash() and verify() return Promises and run on the shared io pool (see Bcrypt). */
    namespace Argon2id {
        /** Argon2id options: memory in KiB, salt at least 8 bytes; `encoded` selects hash()'s return form. */
        interface Argon2idOpts {
            iterations?: number;
            memory?: number;
            parallelism?: number;
            hashLen?: number;
            /** Default true: hash() returns the PHC string `$argon2id$v=19$m=..,t=..,p=..$salt$hash`; false returns the raw tag bytes. */
            encoded?: boolean;
            /** dyna meta configuration: see KDFMetaOptions. */
            dyna?: KDFMetaOptions;
        }
        /** PHC string form (default): the salt and every parameter ride in the string, so storing it is the whole contract. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded?: true }): Promise<string>;
        /** Raw tag form with { encoded: false }. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded: false }): Promise<Uint8Array>;
        /** Verifies a PHC string (untrusted input). Caps: iterations <= 16, parallelism <= 16, memory <= 1048576 (1 GiB) KiB; more is a RangeError. */
        function verify(hashString: string, password: BytesInput, opts?: { dyna?: KDFMetaOptions }): Promise<boolean>;
        /** Raw form: recomputes with the same parameters and compares in constant time. */
        function verify(password: BytesInput, salt: ByteView, expectedHash: ByteView, opts?: Argon2idOpts): Promise<boolean>;
    }

    /** RSA keys and signatures: PKCS#1 v1.5, RSASSA-PSS and RSA-OAEP. */
    namespace RSA {
        /** Defaults to 2048 bits. */
        function generate(bits?: 2048 | 3072 | 4096): KeyPair;
        /** md is "sha1"|"sha256"|"sha384"|"sha512" (case-insensitive). */
        function sign(md: string, privateKey: string, msg: BytesInput): Uint8Array;
        /** True when the signature checks against the public key. */
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView): boolean;
        /** RSASSA-PSS. saltLen: a byte count, "max", or "auto" (default; interoperates with OpenSSL); mgf1Hash defaults to md. */
        function signPSS(md: string, privateKey: string, msg: BytesInput,
                         opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): Uint8Array;
        /** RSASSA-PSS (RFC 8017): the same digest table and the same 2048-bit key floor as `sign`/`verify`, with the padding that carries a randomized salt. */
        function verifyPSS(md: string, publicKey: string, msg: BytesInput, sig: ByteView,
                           opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): boolean;
        /** RSA-OAEP: seal to a public key, open with the private one. md defaults to "sha256"; `label` must round-trip exactly; 2048-bit key floor. */
        namespace OAEP {
            function seal(publicKey: string, plaintext: BytesInput,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
            function open(privateKey: string, sealed: ByteView,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
        }
    }

    /** X.509 certificate parsing and self-signed generation. */
    namespace X509 {
        /** The parsed fields of a certificate. */
        interface X509Info {
            subject: string;
            issuer: string;
            serialNumber: string;
            version: number;
            /** OpenSSL ASN1_TIME text (e.g. "Sep 15 19:48:26 2026 GMT"). */
            notBefore: string;
            notAfter: string;
            fingerprint: string;
            sans: { dns: string[]; ip: string[]; email: string[] };
        }
        /** Parses a PEM string or DER bytes; malformed input is refused. */
        function parse(cert: BytesInput): X509Info;
        /** Builds a v3 self-signed SHA-256 certificate (days defaults to 365); `subject` is a bare common name, NOT a DN ("/CN=x/O=y" becomes the CN verbatim); `sans` are classified by shape (email, IP, DNS); unsupported extensions are refused by name. With `sans` omitted (and no subjectAltName extension), a hostname- or IP-shaped subject is also the one SAN, since TLS clients match SANs, not the CN; `sans: []` means none. */
        function generateSelfSigned(opts: {
            key: string;
            subject?: string;
            days?: number;
            sans?: string[];
            extensions?: { name: string; value: string; critical?: boolean }[];
        }): string;
    }

    /** ECDSA raw R||S or DER signatures. */
    namespace ECDSA {
        /** The curve defaults to P-256. */
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** md "sha1"|"sha256"|"sha384"|"sha512"; format "raw" (default) or "der". */
        function sign(md: string, privateKey: string, msg: BytesInput, opts?: { format?: "raw" | "der" }): Uint8Array;
        /** True for a valid raw or DER signature. A raw signature of the wrong length is a plain false. */
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView, opts?: { format?: "raw" | "der" }): boolean;
    }

    /** ECDH key agreement over X9.63. */
    namespace ECDH {
        /** The curve defaults to P-256. */
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** The raw shared secret; a small-order peer point is refused. */
        function derive(privateKey: string, peerPublicKey: string): Uint8Array;
    }

    /** Ed25519 one-shot signatures; raw 32-byte keys. */
    function Ed25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    /** A 64-byte signature over the whole message. Ed25519 is one-shot by construction. */
    function Ed25519Sign(privateKey: ByteView, message: BytesInput): Uint8Array;
    /** True when the signature is valid. A wrong-size signature returns false, indistinguishable from any other forgery. */
    function Ed25519Verify(publicKey: ByteView, message: BytesInput, signature: ByteView): boolean;

    /** X25519 key agreement; raw 32-byte keys. A small-order (incl. all-zero) peer key is a TypeError. */
    function X25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    /** The 32-byte shared secret. */
    function X25519Derive(privateKey: ByteView, peerPublicKey: ByteView): Uint8Array;

    /** PEM <-> raw converters between 32-byte raw keys and PKCS#8 / SPKI PEM (byte-exact with openssl genpkey). */
    function Ed25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    /** The bridge between the module's 32-byte raw keys and the PEM world (PKCS#8 `-----BEGIN PRIVATE KEY-----`, SPKI `-----BEGIN PUBLIC KEY-----`). */
    function Ed25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };
    function X25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    function X25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };

    /** AES-GCM AEAD; key exactly 16/24/32 bytes (else TypeError), nonce exactly 12 bytes (else TypeError). */
    class AESGCM implements DynResource {
        /** Binds the key once. AES-NI / ARMv8 crypto extensions are selected by the backend at runtime. */
        constructor(key: ByteView);
        /** Encrypts and appends the 16-byte tag; nonce must be exactly 12 bytes. */
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        /** Decrypts and authenticates; a forged tag throws `authentication failed`. */
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        /** Seals into the CALLER's buffer (no allocation), returns bytes written; {tagOut} detaches the 16-byte tag, {prefixNonce:true} emits nonce||ciphertext||tag. */
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        /** The open twin: {tag} reads a detached tag, {prefixNonce:true} reads the leading nonce from the blob; a forged tag throws like open(). */
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        /** Seals with a fresh CSPRNG 12-byte nonce and returns it beside ciphertext||tag; random nonces bound one key to ~2^32 messages. */
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** ChaCha20-Poly1305 AEAD; key exactly 32 bytes, nonce exactly 12 bytes (else TypeError). */
    class ChaCha20Poly1305 implements DynResource {
        /** `seal`, `open`, `sealInto`, `openInto`, `close`, `dispose` and `closed` behave exactly as `AESGCM`'s, with the same 12-byte nonce, 16-byte tag, and throw-on-forgery rule. */
        constructor(key: ByteView);
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Streaming HMAC with a derived key schedule reused across calls. */
    class Hmac implements DynResource {
        /** Derives the block-sized key schedule once; every later `sign`/`verify` reuses it. */
        constructor(algorithm: string, key: BytesInput);
        /** A complete MAC; the object is ready for the next message. */
        sign(msg: BytesInput): Uint8Array;
        /** A complete MAC; the object is ready for the next message (finalise resets). */
        signHex(msg: BytesInput): string;
        /** Streaming absorb; returns this. */
        update(msg: BytesInput): this;
        /** Finishes the accumulated stream. */
        digest(): Uint8Array;
        /** Finish the accumulated stream. */
        digestHex(): string;
        /** Returns the HMAC to its initial state, discarding any update() prefix; the key schedule is kept. */
        reset(): void;
        /** Constant-time compare; tag may be raw bytes or a hex string. */
        verify(msg: BytesInput, tag: BytesInput): boolean;
        readonly algorithm: string;
        readonly digestSize: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** One-shot HMAC. */
    function HMAC(algorithm: string, key: BytesInput, data: BytesInput): Uint8Array;
    /** The one-shot HMAC; `HMACHex` returns the same digest as lowercase hex. */
    function HMACHex(algorithm: string, key: BytesInput, data: BytesInput): string;

    /** The unkeyed digests and checksums of dyna:hash, re-exported (same functions) so PKCE challenges and fingerprints need no second import. */
    function MD5(data: BytesInput): Uint8Array;
    function MD5Hex(data: BytesInput): string;
    function SHA1(data: BytesInput): Uint8Array;
    function SHA1Hex(data: BytesInput): string;
    function SHA224(data: BytesInput): Uint8Array;
    function SHA224Hex(data: BytesInput): string;
    function SHA256(data: BytesInput): Uint8Array;
    function SHA256Hex(data: BytesInput): string;
    function SHA384(data: BytesInput): Uint8Array;
    function SHA384Hex(data: BytesInput): string;
    function SHA512(data: BytesInput): Uint8Array;
    function SHA512Hex(data: BytesInput): string;
    function CRC32(data: BytesInput): number;
    function CRC32C(data: BytesInput): number;
    /** Same as dyna:hash Adler32 / CDCChunks (dyna:crypto re-exports the hash table). */
    function Adler32(data: BytesInput): number;
    function CDCChunks(data: BytesInput, opts?: { min?: number; avg?: number; max?: number }): Uint32Array;
    function XXHash32(data: BytesInput, seed?: number): number;
    function XXHash64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    /** XXH3-64 (default secret); same shapes as XXHash64. Not a security primitive. */
    function XXH3_64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    function SHA3_224(data: BytesInput): Uint8Array;
    function SHA3_224Hex(data: BytesInput): string;
    function SHA3_256(data: BytesInput): Uint8Array;
    function SHA3_256Hex(data: BytesInput): string;
    function SHA3_384(data: BytesInput): Uint8Array;
    function SHA3_384Hex(data: BytesInput): string;
    function SHA3_512(data: BytesInput): Uint8Array;
    function SHA3_512Hex(data: BytesInput): string;
    function Keccak256(data: BytesInput): Uint8Array;
    function Keccak256Hex(data: BytesInput): string;
    function SHAKE128(data: BytesInput, length?: number): Uint8Array;
    function SHAKE128Hex(data: BytesInput, length?: number): string;
    function SHAKE256(data: BytesInput, length?: number): Uint8Array;
    function SHAKE256Hex(data: BytesInput, length?: number): string;
    /** Keyed/derive forms take an options object second: BLAKE2 keys are 0..64 (b) / 0..32 (s) bytes, BLAKE3 keys exactly 32; {context} derives a 32-byte key. */
    function BLAKE3(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE3(data: BytesInput, opts?: Blk3Opts): Uint8Array;
    function BLAKE3Hex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE3Hex(data: BytesInput, opts?: Blk3Opts): string;
    function BLAKE2b(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2b(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2bHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2bHex(data: BytesInput, opts?: Blk2Opts): string;
    function BLAKE2s(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2s(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2sHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2sHex(data: BytesInput, opts?: Blk2Opts): string;
    function Murmur3_128(data: BytesInput, seed?: number): Uint8Array;
    function Murmur3_128Hex(data: BytesInput, seed?: number): string;

    /** RFC 5869 HKDF extract-and-expand. Defaults: hash "sha256", length 32; an absent `salt` is the all-zero salt of hash length. */
    function HKDF(opts: { hash?: string; key: BytesInput; salt?: BytesInput; info?: BytesInput; length?: number }): Uint8Array;

    /** RFC 8018 PBKDF2: hash "sha256", iterations 100000, length 32 by default; `salt` is REQUIRED (TypeError); iterations x output blocks capped at 2^24. Returns a Promise and runs on the shared io pool (see Bcrypt). */
    function PBKDF2(opts: { hash?: string; password: BytesInput; salt: BytesInput; iterations?: number; length?: number }): Promise<Uint8Array>;

    /** RFC 7914 scrypt; budgets (RangeError): memory 128*r*(N+p+2) <= 1 GiB, work N*r*p <= 2^26. Returns a Promise and runs on the shared io pool (see Bcrypt). */
    function Scrypt(password: BytesInput, salt: BytesInput, opts?: { N?: number; r?: number; p?: number; keyLen?: number; dyna?: KDFMetaOptions }): Promise<Uint8Array>;

    /** OS-entropy CSPRNG bytes: the door for security material; use dyna:random's seeded Random for reproducible or fast simulation. */
    function RandomBytes(count?: number): Uint8Array;

    /** Constant-time comparison; different lengths return false (oauth2.secureCompare is the string-friendly form).
     *  Module rule: key/secret arguments must be a string, ArrayBuffer or byte view and are never stringified; anything else is a TypeError. */
    function TimingSafeEqual(a: ByteView, b: ByteView): boolean;

    /** JWS signing; HS/RS/ES algorithms. */
    function JWTSign(payload: unknown, key: BytesInput, opts?: { alg?: string }): string;

    /** Verifies a JWT: signature, header alg in the required `algorithms` allowlist (`none` always refused; every entry must be a string, wherever the match is), key family matching the alg; `crit` headers refused.
     *  exp/nbf/aud/iss are NOT validated here: use dyna:oauth2's verifyJWT for claims. Pass T to type the payload. */
    function JWTVerify<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[] }): T;
}
