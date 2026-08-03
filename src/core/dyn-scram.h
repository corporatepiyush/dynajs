/*
 * dyn-scram -- the CLIENT half of SCRAM-SHA-256 (RFC 5802 / RFC 7677). Pure C:
 * no sockets, no JS, so it can be driven against the RFC's own vectors.
 *
 * WITHOUT TLS THIS AUTHENTICATES THE CREDENTIAL, NOT THE SESSION. That is the
 * whole security statement and it should be read before using this:
 *
 *   - the password never crosses the wire, and a passive listener gets nothing
 *     replayable, because the proof is bound to both nonces;
 *   - the server is authenticated too, but ONLY if the caller checks the
 *     ServerSignature -- which is why dyn_scram_server_final exists as a step a
 *     caller cannot skip by accident rather than as an optional extra;
 *   - it does NOT stop a man in the middle. PostgreSQL's own documentation says
 *     so: a relay can pass the server's random value through and authenticate.
 *     Everything after the handshake is plaintext regardless;
 *   - a passive listener CAN mount an offline dictionary attack, bounded only
 *     by the iteration count the server chose.
 *
 * Channel binding is not implemented and cannot be: it needs a TLS channel to
 * bind to. The gs2 flag is therefore a truthful "n" (no support) rather than
 * the "y" a TLS-capable client would send, and `c=biws` is a constant.
 *
 * THE SERVER CHOOSES THE ITERATION COUNT, so it is an attacker-controlled
 * amount of work: RFC 5802 section 9 names it as a denial of service and libpq
 * does not bound it. This does.
 */
#ifndef DYN_SCRAM_H
#define DYN_SCRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_SCRAM_KEY_LEN     32   /* SHA-256 */
#define DYN_SCRAM_RAW_NONCE   18   /* PostgreSQL's SCRAM_RAW_NONCE_LEN */
#define DYN_SCRAM_NONCE_B64   24   /* base64 of 18 octets, no padding needed */
#define DYN_SCRAM_MAX_ITERS   1000000
#define DYN_SCRAM_MAX_SALT    1024
#define DYN_SCRAM_MAX_MSG     8192

#define DYN_SCRAM_OK           0
#define DYN_SCRAM_E_SYNTAX    -1   /* not a well-formed SCRAM message */
#define DYN_SCRAM_E_NONCE     -2   /* the server nonce does not extend ours */
#define DYN_SCRAM_E_ITERS     -3   /* iteration count absent, zero, or too big */
#define DYN_SCRAM_E_SALT      -4   /* salt absent, unparseable, or too long */
#define DYN_SCRAM_E_VERIFY    -5   /* ServerSignature mismatch: NOT our server */
#define DYN_SCRAM_E_SERVER    -6   /* the server sent e=<reason> */
#define DYN_SCRAM_E_SHORT     -7   /* output buffer too small */
#define DYN_SCRAM_E_STATE     -8   /* steps called out of order */
#define DYN_SCRAM_E_EXT       -9   /* a mandatory extension we cannot honour */

typedef struct {
    int step;
    char nonce[DYN_SCRAM_NONCE_B64 + 1];  /* ours, base64 */
    char *cfirst_bare;                    /* "n=,r=<nonce>" */
    char *sfirst;                         /* server-first, VERBATIM */
    uint8_t server_sig[DYN_SCRAM_KEY_LEN];/* what the final message must carry */
    char err[128];                        /* the server's e= reason, if any */
} dyn_scram_t;

/* Start the exchange. Writes the client-first-message ("n,,n=,r=<nonce>") and
 * returns its length. The username is deliberately EMPTY: PostgreSQL ignores
 * the SCRAM username and uses the one from the startup packet, so sending it
 * twice only creates a way for the two to disagree. */
int dyn_scram_client_first(dyn_scram_t *s, char *out, size_t outcap);

/* Consume the server-first-message and write the client-final-message. This is
 * where the password is used and where every bound on server-chosen values is
 * enforced. `password` must be NUL-terminated ASCII; SASLprep is the identity
 * on ASCII, so none is applied. */
int dyn_scram_server_first(dyn_scram_t *s, const char *msg, size_t len,
                           const char *password, char *out, size_t outcap);

/* Verify the server-final-message. A caller that skips this has thrown away
 * mutual authentication and kept only the password-hiding half. */
int dyn_scram_server_final(dyn_scram_t *s, const char *msg, size_t len);

void dyn_scram_free(dyn_scram_t *s);

const char *dyn_scram_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* DYN_SCRAM_H */
