# CHANGELOG — work39 (dyna_text: text/codec module review)

Tree: `.agent-work/work39` (base commit = 96808bb, "base = master 2994938
(wave-3 suites)"). Owner brief: harsh black-box review + parametric assert
suites + fixes for **dyna:compress, dyna:csv, dyna:xml, dyna:yaml,
dyna:html, dyna:json, dyna:matcher, dyna:encoding**.

Build note: the tree's first `build_base.log` build had NO native modules
(default config; symptom `could not load module 'dyna:*'`). The working
baseline is `make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y` (snapshotted
`dynajs.pristine`); the final gate build is the same config WITH the four
fixes below. ASan variant in `dynajs-asan`.

## Suite: tests/agent/dyna_text/ (FIRST-CLASS DELIVERABLE)

- `h.js` — portable assert harness (strnum_bb pattern), extended with
  b64/hex byte helpers and `__norm` Uint8Array digests (fnv1a over hex).
- `gen_common.py` — oracle helpers: `__norm`-shaped expectation emitters
  (escaping verified byte-identical to node `JSON.stringify`), fnv1a,
  deterministic seeded blob corpus.
- Generators (python3 stdlib oracles, deterministic, no network, no node):
  - `gen_encoding.py` (613 cases) — RFC 4648 vectors + length sweeps 0..64K
    vs `base64`/`binascii`; ascii85 vs `a85encode/a85decode(adobe=False)`
    BOTH directions (incl. `'z'` folding + whitespace rows); base58/
    base58check/basex vs pinned python division-codec impls (4096/4097
    caps, alphabet validation, multi-alphabet matrix); LEB128 varints vs
    pinned python impl (BigInt beyond 2^53); 28-row malformed-input
    rejection matrix; JSON5 vs `json` on the JSON subset + documented
    superset (unquoted keys, hex, Infinity/NaN, comments, trailing commas,
    depth cap); StableStringify vs RFC 8785 canonical impl (incl. UTF-16
    key ordering, -0, NaN/Inf rejection); JSONPath per RFC 9535 examples;
    DetectEncoding (5 BOMs, UTF-8 validity, Shift-JIS probe, allowList).
  - `gen_json.py` (108) — RFC 6901 Pointer vs a pinned python resolver
    (section-5 examples, `~`-escape rules, array-index strictness incl.
    leading-zero/-/non-numeric token classification, depth/length caps) and
    RFC 6902 Patch official examples + documented contract (copy-on-write,
    clone-on-insert, move-prefix rule, non-plain pass-by-reference).
  - `gen_csv.py` (62) — RFC 4180 reader vs python `csv` (quoted fields,
    embedded commas/quotes/newlines/CRLF, BOM, unicode/astral, ragged rows,
    empty/header-only files, 1 MiB field); writer file-bytes vs python
    `csv.writer` minimal quoting; full load-modify-store lifecycle;
    window caps (100/1000); closed-resource discipline.
  - `gen_compress.py` (113) — gzip vs python zlib BOTH directions (python
    compresses -> gunzip; dynajs gzip 10-byte header pinned `1f 8b 08 00 /
    mtime 0 / OS ff` + CRC32/ISIZE trailer cross-checked vs `zlib.crc32`);
    zstd vs python 3.14 `compression.zstd` (PEP 784), level sweep 1..22,
    magic `28 b5 2f fd`; tar/zip archives BUILT by python `tarfile`/
    `zipfile` -> TarList/TarExtract/ZipList/ZipRead exact metadata + CRC32;
    TarPack/ZipPack structure (ustar magic at 257, safe-name refusals,
    store/deflate methods); Compressor/Dictionary class semantics
    (dictId, dictionary mismatch, use-after-close, 50x reuse);
    corrupt/truncate/garbage fuzz (bit-flips + truncations on strides,
    snappy/lz4 length-prefix lies).
  - `gen_xml.py` (99) — XMLParse vs python `xml.etree` on 24 well-formed
    docs (5 entities, numeric refs incl. astral, CDATA, comments, PI,
    DOCTYPE incl. internal subset, attr tab/newline normalization per XML
    1.0 3.3.3) + parse->stringify->parse stability + 14-row rejection
    matrix (all ET-rejected too) + SAX event streams with EVERY 1-char
    chunk split + write-in-handler/write-after-end discipline.
  - `gen_yaml.py` (88) — Parse vs PyYAML on the 1.1/1.2 core overlap (35
    docs: block/flow, quoted + escapes, literal/folded blocks, comments,
    unicode, doc markers); YAML 1.2 core divergences pinned (Norway
    problem: `no`/`yes`/`on` are strings; `0o17`=15; `012`=12;
    `1:30`/dates are strings; `1e3` resolves); refused-BY-NAME rows
    (anchors/aliases/tags/merge/directives/duplicate keys/multi-doc);
    ParseAll; Stringify round-trip invariants + exact pins + indent bounds.
  - `gen_html.py` (98) — token-level structure differential vs python
    `html.parser` driving a mini tree builder implementing the documented
    leniency (void elements, p auto-close incl. hr, li auto-close,
    stray-close ignored, lowercasing, valueless attrs = ""); entity
    semantics (named table, numeric incl. astral, out-of-range -> U+FFFD,
    unknown named stays literal); raw-text script/style; HTMLText raw-text
    skip; Selector matrix (tag/class/id/attr/child/descendant/groups/
    matches-combinator rule); Sanitizer (allow-list, protocol + control-
    char scheme checks, raw-text content dropped); Template (escape/raw/
    sections/invert/missing/comment/partial/fn refusals); Markdown.
  - `gen_matcher.py` (465) — Matcher vs python find loops in code-unit
    offsets (ascii/latin1/astral, overlapping), empty-pattern special case
    (firstIn 0 / test true / count+all empty), replaceAll non-overlapping,
    algo validation; MultiMatcher vs brute force (overlap + same-position;
    doc corpus exact order); Levenshtein vs python DP (max cutoff = exact
    while <= max else max+1); DiceCoefficient per documented formula
    (bigram multiset, whitespace strip, short-side rule); Diff reconstruction
    properties for Chars/Words/Lines.
  - `gen_asan.py` (1 probe, 3063 guarded hostile calls) — bounded
    buffer-boundary chaos: prefix/suffix/1-char-split mutations of
    construct-rich docs for html/xml/yaml, bit-flip + truncate loops for
    every decompressor, snappy copy-overrun + lz4 literal/match overrun
    blobs, gzip header lies, tar size lies, zip offset lies, cap-edge
    inputs, hostile Compressor reuse.
- `run.sh` / `run_one.sh` — timeout-wrapped, per-probe stdout+rc to
  SEPARATE `out/` files + summary.tsv, nonzero exit on any failure/crash/
  no-RESULT; `DYNAJS_BIN` selects the engine (used for ASan).
- `materialize.sh` — regenerates all 42 probes; committed source of truth =
  generators + harness (probes/ and out/ gitignored).
- The probes are dynajs-only (dyna:* modules); the reference-engine slot is
  filled by python3 stdlib oracles at materialize time — that IS the
  cross-vendor differential. **Totals: 42 probes, 4715 asserts, all green**
  on the final build AND under ASan.

## FINDINGS / FIXES (4 fixes, all in-tree, gates green after)

1. **dyna:json — Pointer depth cap unenforced (regression vs API.md).**
   API.md: "A pointer longer than 65536 bytes or deeper than 128 levels
   throws." The walk is iterative and had NO depth check — a 30000-level
   pointer walked fine (no crash, but the documented bound did not exist).
   FIX `src/dyna-json.c`: token count checked in `dyn_jp_walk` before any
   allocation; new `JP_TOODEEP` -> RangeError "pointer deeper than 128
   levels". Boundary pinned: 128 tokens OK, 129 throws (get AND has).
2. **dyna:xml — SAXParser swallowed content XMLParse rejects (pre-existing
   doc-vs-impl bug).** `write("<a/>x"); end()` silently dropped trailing
   text; `write("<a/><b/>")` accepted a second root; API.md says end()
   throws on trailing content and XMLParse rejects both. FIX
   `src/dyna-xml.c`: `dyn_sax_root_check()` mirrors the tree front end's
   document discipline (one root, no non-blank text outside it, blank text
   skipped, comments/PIs after the root stay legal; self-closed roots set
   root_closed). Error texts identical to XMLParse's.
3. **dyna:html — out-of-range numeric character references left literal
   (pre-existing doc-vs-impl bug; hostile-input relevant).** API.md:
   "numeric references out of range become U+FFFD". `&#x110000;`+ stayed
   LITERAL because the accumulator bailed with `return 0` on overflow — the
   code mapping `cp > 0x10FFFF` to U+FFFD was dead on that path. Worse, the
   uint32 accumulator could WRAP mod 2^32, aliasing a huge reference onto
   an arbitrary small code point (e.g. `&#x10000000A;`). FIX
   `src/dyna-html.c` `ht_entity()`: overflow checked BEFORE each multiply
   (base-aware bound); on overflow skip remaining digits and fall through
   to the U+FFFD mapping. Verified `&#x10FFFF;` still decodes;
   `&#x110000;`/`&#xFFFFFFFF;`/`&#1114112;` -> U+FFFD.
4. **dyna:html — Selector.first() returned null on no match (pre-existing
   doc-vs-impl bug).** API.md: "The first match, or `undefined` when none."
   FIX `src/dyna-html.c`: `JS_UNDEFINED` sentinel instead of `JS_NULL`.

## IMPL-DEFINED / DOCUMENTED DIVERGENCES (pinned in the suite)

- **dyna:yaml — TICKET: multi-line quoted scalars refused** (`"a\n  b"`,
  same for single quotes; "unterminated quoted scalar" at line 1). YAML
  8.1.2 folding is not implemented in the line-oriented scanner; block
  scalars `|`/`>` are the supported multi-line form. A proper fix needs a
  quoted-continuation joining pre-pass in `yml_split` — invasive, not a
  clean fix; pinned in y03 with a comment.
- dyna:yaml — YAML 1.2 core resolutions diverge from PyYAML 1.1 BY DESIGN
  per API.md ("core schema"): covered as rows, not bugs.
- dyna:csv — lenient recovery of `"x"garbage,2` differs from python csv
  (API.md documents "tolerant mode keeps the readable prefix"); a blank
  line parses as a padded row `["",""]` (python yields `[]`); the writer
  emits LF where RFC 4180 names CRLF (reader accepts both). Pinned.
- dyna:encoding — `Uvarint` overflow (>10 bytes) returns a NEGATIVE
  bytesRead sentinel `[0, -11]`; API.md documents only the truncated
  `[0, 0]` case. Pinned; flagged: caller-hostile shape, deserves an
  API.md row or a `[0,0]`-style contract (ticket for the owner).
- dyna:compress — `lz4Decompress(block, sizeHint)` ignores a mismatched
  hint; gzip multi-member streams refused; unzstd tolerates trailing bytes
  while gunzip refuses them; gzip level accepts 0 and 10 (documented range
  1..9) — lenient. Pinned.
- dyna:html — `trim:true` reads as "drop whitespace-ONLY text nodes", it
  does not trim mixed text (`<a>  hi  </a>` keeps `  hi  `); API.md wording
  is loose. Nesting throws AT 256 opens. Sanitizer lowercases surviving
  tags (browser-like). Pinned.
- dyna:xml — "nesting is capped at 256" throws AT 256 opens (255 deep OK).
  Pinned.
- Engine-wide, OTHER OWNER: the dynajs CLI treats a file as a module only
  when `import` precedes other top-level statements (`var x=1; import ...`
  fails "expecting '('"; node accepts imports anywhere at top level).
  Probes respect the heuristic; flagged here for the loader owner.

## GATES (final build = CONFIG_NATIVE_MODULES=y CONFIG_TLS=y + fixes)

- `make test`: **39/39 suites passed** (0 solo, 39 parallel) —
  gate_make_test.log.
- test262 (`run-test262 -c tools/test262.conf -a -T 8`): **60/83744
  errors**, fail file-set IDENTICAL to tools/test262_errors.txt (49 unique
  files / 60 error lines; verified with LC_ALL=C extraction, see
  t262_pin_files.txt vs t262_actual_files.txt) — gate_t262.log.
- ASan (`CONFIG_ASAN=y`, `dynajs-asan`): full dyna_text battery + hostile
  matrix = **42 probes, 4715 asserts, 0 failures, 0 sanitizer reports**
  (out_asan/summary.tsv, run_asan.log).
- dyna_text battery on the final non-ASan binary: 42 probes, 4713 asserts,
  all green (out/summary.tsv).

## Approaches TRIED / MISSED / POSTPONED

- TRIED: node as the second engine for the probes — impossible, dyna:*
  modules are dynajs-only; python3 stdlib adopted as the oracle instead
  (stricter for codecs: true cross-vendor, baked at materialize time).
- TRIED: python csv as oracle for lenient-recovery rows — differs by
  design; those rows pin the documented engine behavior instead.
- MISSED (not testable on this machine): brotli/snappy/lz4 have no python
  counterpart module (imports absent); covered by round-trips + spec
  structure checks (frame magic, varint length prefix) + fuzz. Residual
  risk: brotli via libcompression has no independent oracle here.
- POSTPONED (tickets above): YAML multi-line quoted scalars; Uvarint
  overflow contract; CLI import-position heuristic (other owner).

## REVIEW-FIXES (orchestrator round, work43 verified)

work43 independently verified the four fixes and found five review items;
all five are addressed here, and the gate list is EXPANDED with
`make test-native` (199 suites — `make test` alone misses the 160 module
suites).

1. BLOCKER (test-expectation update): FIX 4 changed `Selector.first()`'s
   no-match answer to `undefined` per API.md:824 / dynajs.d.ts:767
   ("node|undefined"); 4 internal suites still asserted null. Updated the
   assertions (minimal diff): tests/test_html.js ("or undefined when no
   match"), tests/test_p2_batch2.js (comment + assert), tests/test_p2_batch3.js,
   tests/test_cov_file_html_xml_yaml_json.js. `make test-native` added to
   the gate list from now on.
2. Ticket-or-mirror: MIRRORED. dyna-xml.c `dyn_sax_feed` end() now also
   refuses an unclosed element and a root-less document, with XMLParse's
   exact texts ("unclosed element", "no root element"; the SAX throw site
   prefixes "SAXParser: "). State was already tracked by
   dyn_sax_root_check (depth/root_closed). All FIVE of XMLParse's document
   checks are now mirrored by SAX:
     | feed                       | XMLParse                    | SAXParser end()              |
     | trailing text  "<a/>x"     | text outside the root       | text outside the root        |
     | second root    "<a/><b/>"  | a document has one root     | a document has one root      |
     | unclosed       "<a>hi"     | unclosed element            | unclosed element             |
     | no root        "<!--c-->"  | no root element             | no root element              |
     | stray close    "</a><b/>"  | close tag with no open tag  | close tag with no open tag   |
   5 new pinned probe rows (sax_end_*); xml family now 105 asserts.
   work43 additionally verified: the xml_scan/dyn_sax state is fully
   internal to dyna-xml.c (no other consumers), so no internal call site
   can regress; and dyna:json's cap is exact at 128/129 (pinned pdeep128/
   pdeep129 + phas_deep129). A qbc cold/warm check on the SAX feed path
   came back clean — recorded, not re-run here.
3. Warning: the digit-skip loop in ht_entity assigned an unused `int r`;
   restructured to continue/break conditions — build is warning-clean again.
4. Comment correction (work43 A/B): the OLD in-loop `cp > 0x10FFFF -> return
   0` fired before any 2^32 wrap was possible (max accumulator
   0x10FFFF*16+15 ~= 17.8M), so wraparound aliasing was NOT reachable; the
   real fixed defect was literal-instead-of-U+FFFD only. The ht_entity
   comment now says exactly that, and marks the pre-multiply multiply bound
   as defense-in-depth for future refactors. Behavior is unchanged from the
   first fix round: `&#x10FFFF;` decodes, `&#x110000;`+ -> U+FFFD.
5. materialize.sh now runs `python3 gen_asan.py` as well, and prints the
   exact reproduction lines for the committed summaries (./run.sh all =
   42 probes incl. asan; ASan pass via DYNAJS_BIN=...dynajs-asan
   OUTDIR=out_asan ./run.sh all). Lane cosmetics kept ("work39" is the
   tree name; the lane is dyna_text).

1b. (found by the expanded gate itself) tests/test_conformance.js's
   adversarial baseline had pinned the OLD SAX leniency
   ("sax/end-with-open-elements/accepted" -> !r.threw). Fix 2 (extended)
   changed exactly that, so the baseline was updated to pin the new
   refusal ("sax/end-with-open-elements/refused": SyntaxError
   /unclosed element/). Same legitimate class as (1): the recorded
   expectation moves with the documented-behavior fix.

REVIEW-FIX gates (final build, EXPANDED list, all green):
  - make test: 39/39 suites (gate3_make_test.log)
  - make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y test-native: 199/199 suites
    (gate3_test_native.log) -- now a standing gate for this lane
  - test262: 60/83744 errors, fail file-set IDENTICAL to
    tools/test262_errors.txt (gate2_t262.log; no src/ change after it)
  - dyna_text battery: 42 probes / 4721 asserts green on the final build
    (out/summary.tsv) and under ASan (out_asan/summary.tsv,
    dynajs-asan)
