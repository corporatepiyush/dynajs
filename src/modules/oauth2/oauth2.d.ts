/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:oauth2
/** dyna:oauth2 — OAuth 2.0 helpers (RFC 6749/6750/7636): PKCE, state, bearer parsing, redirect/scope checks, claim-validating verifyJWT (needs the optional "crypto" module, which links OpenSSL). No IO. */
declare module "dyna:oauth2" {
    /** 43..128 unreserved chars; 32 octet CSPRNG -> 43 char base64url */
    function generateCodeVerifier(): string;
    /** True for a PKCE code_verifier (43..128 unreserved characters, RFC 7636). */
    function isValidCodeVerifier(s: string): boolean;
    /** S256 = BASE64URL(SHA256(verifier)), plain = verifier; S256 default */
    function generateCodeChallenge(verifier: string, method?: "S256" | "plain"): string;
    /** Verifies a PKCE challenge; method "S256"|"plain" (else TypeError); constant-time. */
    function verifyCodeChallenge(verifier: string, challenge: string, method: string): boolean;
    /** CSPRNG bytes -> base64url; default 32 bytes */
    function generateState(bytes?: number): string;
    /** Constant-time compare over UTF-8 strings or raw bytes; the convenience form of crypto.TimingSafeEqual. */
    function secureCompare(a: BytesInput, b: BytesInput): boolean;
    /** Client Basic auth per RFC 6749 s2.3.1: "Basic " + base64(formEncode(id:secret)), prefix included. */
    function buildClientAuthHeader(clientId: string, clientSecret?: string): string;
    /** Verifies a JWT access token: alg allowlist (`none` refused), key family, then exp/nbf/aud/iss/scope; clockSkewSec 0..300; requireExp rejects a missing exp.
     *  The signature check is dyna:crypto's JWTVerify, reached through a bridge compiled on the first call in a realm and kept as the hidden global
     *  `__oauth_jwt_verify` (non-writable, non-configurable, non-enumerable); later calls reuse it (compiling it per call was ~70% of a verify).
     *  A writable or configurable property of that name is replaced on the first call; one already of the bridge's shape is used as the bridge. */
    function verifyJWT<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[]; aud?: string | string[]; iss?: string; requiredScope?: string[]; clockSkewSec?: number; requireExp?: boolean }): T;
    /** NQCHAR scope tokens, space-delimited, per RFC 6749 A.4 */
    function parseScope(scope: string): string[];
    /** Joins scopes with single spaces. */
    function formatScope(scopes: string[]): string;
    /** "Bearer <b64token>" with b64token validation */
    function buildBearerHeader(token: string): string;
    /** The token from an `Authorization: Bearer ...` header value, or null. */
    function parseBearerHeader(header: string): string | null;
    /** headers.authorization/query/body; allowQuery/allowBody opt-in (default false) */
    function parseBearerFromRequest(req: { headers?: Record<string,string>; query?: string; body?: string; allowQuery?: boolean; allowBody?: boolean }): string | null;
    /** RFC 6750 b64token: 1*b64char *"=", no length cap; strict: at most two "=" and 4096 bytes. */
    function isValidBearerToken(token: string, opts?: { strict?: boolean }): boolean;
    /** Bearer realm, error (REQUIRED, 1*NQSCHAR per RFC 6749 A.7), error_description (A.7), error_uri (A.8, no SP), scope, all quoted; TypeError otherwise.
        strict: error limited to 1-64 token68 chars. */
    function buildWWWAuthenticate(opts: { realm?: string; error: string; errorDescription?: string; error_description?: string; errorUri?: string; error_uri?: string; scope?: string; strict?: boolean }): string;
    /** Verbatim exact except loopback port-ignore per RFC 8252; every registered entry must be a string (TypeError naming the index), wherever the match is. */
    function isValidRedirectUri(candidate: string, registered: string[]): boolean;
    /** Builds the authorization URL (response_type=code), generating `state` when absent; the endpoint must be https (loopback excepted) unless allowInsecure. */
    function buildAuthorizationUrl(opts: { authorizationEndpoint: string; clientId: string; redirectUri: string; responseType?: "code"; scope?: string; state?: string; codeChallenge?: string; codeChallengeMethod?: "S256" | "plain"; allowPlain?: boolean; allowInsecure?: boolean; extraParams?: Record<string,string> }): { url: string; state: string };
    /** Parse ?code/state/error from redirect URL (query, not fragment for code flow); refuses implicit-flow fragments, code+error, duplicate params */
    function parseAuthorizationResponse(url: string): { code?: string; state?: string; error?: string; errorDescription?: string; errorUri?: string };
    /** application/x-www-form-urlencoded body builder; every value must be a string */
    function buildTokenRequestBody(params: Record<string,string>): string;
    /** Token response per RFC 6749: access_token + token_type Bearer required, b64token access_token */
    interface TokenResponse {
        access_token: string;
        token_type: string;
        expires_in?: number;
        refresh_token?: string;
        scope?: string;
        [key: string]: unknown;
    }
    /** Parses a token endpoint JSON body into a TokenResponse. */
    function parseTokenResponse(body: string): TokenResponse;
}
