/*
 * dyn-dict -- token-substitution dictionary codec. PURE C: no JSValue, no
 * JSContext. Compiles with -Isrc/core alone.
 *
 * THIS IS NOT LZ77 WITH A PRESET WINDOW. That is the other dictionary
 * mechanism, and dyn-compress already has it: `dyn_lz4_compress(..., dict, ...)`
 * seeds the match window with a prefix, which is the right tool when the
 * payload resembles a known BLOCK of text. This one replaces known PHRASES with
 * short codes, which is the right tool for short, highly templated payloads --
 * a JSON-RPC envelope, a log line, a protocol header -- where the payload is
 * far too small for LZ77 to have built a useful window by the time it ends.
 *
 * Both mechanisms exist because they win on different inputs, and neither
 * subsumes the other. The crossover is published in the header alongside the case
 * where each LOSES, per CLAUDE.md sec.4.
 *
 * THE COMPILED ARTEFACT IS AN AHO-CORASICK AUTOMATON over the phrase set
 * (src/core/dyn-ac.h), so one pass finds every phrase at every position and the
 * cost of that pass does not grow with the number of phrases. That is the
 * property being amortised, and it is why a dictionary is a capability rather
 * than a per-call argument.
 *
 * WIRE FORMAT -- self-describing, so a record carries what it needs to be
 * rejected rather than misread:
 *
 *     "DT"        2 bytes   magic
 *     version     1 byte    currently 1
 *     dict id     4 bytes   little-endian CRC-32C of the canonical phrase list
 *     raw length  varint    the decompressed size, for exact presizing
 *     items...              until the input is exhausted
 *
 *   item: varint code
 *           code == 0  literal run: varint length, then that many raw bytes
 *           code >= 1  phrase (code - 1) from the dictionary
 *
 * THE DICTIONARY ID IS THE POINT OF THE HEADER. A token-substituted record is
 * meaningless against a different phrase list, and decoding one anyway produces
 * plausible garbage rather than an error -- the codes are all still in range.
 * So the id is checked before a single byte is emitted, and a mismatch produces
 * NOTHING, not a partial or a wrong answer. dyn_lz4_compress's prefix mode
 * cannot do this, because a raw LZ4 block has nowhere to record it; that
 * asymmetry is why the two mechanisms are documented separately.
 *
 * UNTRUSTED INPUT. dyn_dict_decompress treats its buffer as hostile: every
 * varint is bounds-checked against the remaining input, every phrase code is
 * range-checked against the dictionary, every literal run is checked against
 * what is left, and the output is capped at DYN_MAX_OUTPUT. It has its own
 * fuzz target for the same reason the LZ4 decoder does.
 */
#ifndef DYN_DICT_H
#define DYN_DICT_H

#include <stddef.h>
#include <stdint.h>

#include "dyn-compress.h"   /* dyn_outbuf_t, DYN_MAX_OUTPUT */

#ifdef __cplusplus
extern "C" {
#endif

/* Largest phrase count. 16 bits of code space is far more than a templated
 * payload has distinct phrases, and it keeps the code varint to three bytes. */
#define DYN_DICT_MAX_PHRASES 65535

typedef struct dyn_dict dyn_dict_t;

/* Compile a dictionary. Phrases may be any bytes; an EMPTY phrase is rejected,
 * because it matches at every position and encodes nothing. Duplicate phrases
 * are accepted and the earlier index wins, so an id stays a function of the
 * list as given. Returns NULL on OOM or on an invalid phrase set. */
dyn_dict_t *dyn_dict_new(const uint8_t *const *phrases, const size_t *lens,
                         size_t n);

void dyn_dict_free(dyn_dict_t *d);

/* CRC-32C of the canonical encoding of the phrase list -- varint(len) then the
 * bytes, for each phrase in order. Two dictionaries with the same phrases in
 * the same order have the same id; any other pair does not. */
uint32_t dyn_dict_id(const dyn_dict_t *d);

size_t dyn_dict_count(const dyn_dict_t *d);

/* Compress into `o` (append; zero-initialise it first). Returns 0, or -1 on
 * OOM. Any input compresses -- worst case every byte becomes part of one
 * literal run -- but "compresses" is not "shrinks": on input containing none of
 * the phrases the output is the input plus the header and the run length. That
 * is the adversarial case and it is in the bench permanently. */
int dyn_dict_compress(dyn_dict_t *d, const uint8_t *src, size_t len,
                      dyn_outbuf_t *o);

/* Decompress into `o`. Returns 0 on success; -1 on a malformed record, a
 * dictionary-id mismatch, or OOM. On any failure `o` holds nothing usable --
 * check the return value, never the length. */
int dyn_dict_decompress(const dyn_dict_t *d, const uint8_t *src, size_t len,
                        dyn_outbuf_t *o);

/* The id a record was built against, without needing the dictionary that built
 * it -- so a caller can report WHICH dictionary is missing rather than just
 * that one is. Returns 0 on success, -1 if the buffer is not a dictionary
 * record at all. */
int dyn_dict_record_id(const uint8_t *src, size_t len, uint32_t *id);

#ifdef __cplusplus
}
#endif

#endif /* DYN_DICT_H */
