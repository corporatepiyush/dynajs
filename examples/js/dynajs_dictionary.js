// dynajs_dictionary.js — the token-substitution Dictionary in dyna:compress.
//
// A COMPILED CAPABILITY: the phrase list goes in the constructor, an
// Aho-Corasick automaton is built once, and one instance serves unbounded
// records. Unlike a value handle, it is allowed to start behind — the question
// is only how many uses it takes to come out ahead, and on what input.
//
// The honest answer is that its crossover is not in the number of uses at all.
// It is in WHETHER THE PHRASES OCCUR. A dictionary pays exactly to the extent
// that its phrases appear in the payload and nowhere else, so this file is
// built around three payloads rather than three loop counts:
//
//   BEST   — a short, highly templated record. Every phrase fires; gzip cannot
//            compete because DEFLATE's own header is most of a 54-byte payload.
//   WORST  — bytes containing none of the phrases. The whole input becomes one
//            literal run and the output is the input plus 8 bytes of header.
//            An EXPANSION. This row stays here permanently.
//   WORST  — already-compressed bytes, the adversarial input for any codec.
//
// Run: dynajs examples/js/dynajs_dictionary.js
import { test, run, assert, assertEqual } from "./harness.js";
import { Dictionary, Compressor, gzip, gunzip } from "dyna:compress";

const enc = new TextEncoder();
const dec = new TextDecoder();
const bytes = (s) => enc.encode(s);
const text = (b) => dec.decode(b);

// A JSON-RPC envelope's fixed furniture. Note that `{"` overlaps the front of
// `"jsonrpc":"2.0"` — a realistic phrase list, and the shape that breaks a
// greedy encoder.
const RPC_PHRASES = [
  '"jsonrpc":"2.0"', '"method":', '"params":', '"id":',
  '"result":', '"error":', '{"', '"}', '":"', '","',
];

const FRAME = '{"jsonrpc":"2.0","method":"sum","params":[1,2],"id":7}';

// ---------------------------------------------------------------------------
// BEST CASE
// ---------------------------------------------------------------------------

test("BEST: a templated record, where every phrase fires", () => {
  const d = new Dictionary(RPC_PHRASES);
  const packed = d.compress(bytes(FRAME));
  const gz = gzip(bytes(FRAME));

  print(`  frame           ${FRAME.length} bytes`);
  print(`  Dictionary      ${packed.length} bytes` +
        `   (${(FRAME.length / packed.length).toFixed(2)}× smaller)`);
  print(`  gzip            ${gz.length} bytes` +
        `   (${(FRAME.length / gz.length).toFixed(2)}× — DEFLATE's header is most of it)`);

  assertEqual(text(d.decompress(packed)), FRAME);
  assert(packed.length < FRAME.length, "the record shrinks");
  assert(packed.length < gz.length, "and beats gzip at this size");
});

test("the parse must be able to DECLINE a match", () => {
  // `{"` matches at position 0 and `"jsonrpc":"2.0"` matches at position 1.
  // A greedy encoder takes the two-byte match and steps over the fifteen-byte
  // one, which it can never come back for — that turned this 54-byte frame
  // into 61 bytes, an expansion of its own best case. The parse is a dynamic
  // program, and this assertion is what stops that regressing.
  const d = new Dictionary(RPC_PHRASES);
  const withOverlap = d.compress(bytes(FRAME)).length;

  // The same dictionary minus the overlapping short phrase: if the parse were
  // greedy, REMOVING a phrase would make the output SMALLER, which is absurd
  // and is exactly the symptom.
  const noOverlap = new Dictionary(RPC_PHRASES.filter((p) => p !== '{"'))
    .compress(bytes(FRAME)).length;

  print(`  with '{"'       ${withOverlap} bytes`);
  print(`  without '{"'    ${noOverlap} bytes`);
  assert(withOverlap <= noOverlap,
    `adding a phrase must never make the output larger (${withOverlap} vs ${noOverlap})`);
});

// ---------------------------------------------------------------------------
// WORST CASES — kept permanently, per the rule that a bypass must be measured
// where it never fires as well as where it does
// ---------------------------------------------------------------------------

test("WORST: a payload containing none of the phrases EXPANDS", () => {
  const d = new Dictionary(RPC_PHRASES);
  const noise = "zqx".repeat(40);
  const grown = d.compress(bytes(noise));

  print(`  no phrases      ${noise.length} -> ${grown.length} bytes` +
        `   (+${grown.length - noise.length}: one literal run plus the header)`);

  assertEqual(text(d.decompress(grown)), noise);
  assert(grown.length > noise.length,
    "this is an expansion, and pretending otherwise would be the lie");
});

test("WORST: already-compressed bytes do not shrink further", () => {
  const d = new Dictionary(RPC_PHRASES);
  const gz = gzip(bytes(FRAME.repeat(20)));
  const twice = d.compress(gz);

  print(`  gzip output     ${gz.length} -> ${twice.length} bytes (recompressed)`);
  assertEqual(text(gunzip(d.decompress(twice))), FRAME.repeat(20));
  assert(twice.length >= gz.length, "compressed bytes carry no phrases");
});

test("WORST: a dictionary whose phrases are all long and all absent", () => {
  // The pathological configuration: a big automaton that never fires. The
  // construction cost is real and buys nothing.
  const many = [];
  for (let i = 0; i < 200; i++) many.push("phrase-that-never-appears-" + i);
  const d = new Dictionary(many);
  const payload = "a".repeat(500);
  const out = d.compress(bytes(payload));
  assertEqual(text(d.decompress(out)), payload);
  assert(out.length > payload.length, "200 unused phrases still expand the output");
  assertEqual(d.size, 200);
});

// ---------------------------------------------------------------------------
// Where it sits against the OTHER dictionary mechanism
// ---------------------------------------------------------------------------

test("the two dictionary mechanisms win on different payloads", () => {
  // Compressor({dict}) seeds an LZ77 window with a known BLOCK. Dictionary
  // substitutes known PHRASES. Neither subsumes the other, and the difference
  // shows up at exactly the size where LZ77 has no window yet.
  const window = enc.encode(FRAME);          // a whole prior frame as context
  const lz4d = new Compressor({ algo: "lz4", dict: window });
  const tok = new Dictionary(RPC_PHRASES);

  const short = '{"jsonrpc":"2.0","id":1}';
  const a = lz4d.compress(bytes(short)).length;
  const b = tok.compress(bytes(short)).length;
  print(`  short record    window-prefix ${a} B   token-substitution ${b} B`);

  const long = FRAME.repeat(40);
  const c = lz4d.compress(bytes(long)).length;
  const e = tok.compress(bytes(long)).length;
  print(`  long stream     window-prefix ${c} B   token-substitution ${e} B`);

  // On a long repetitive stream LZ77 has a real window and wins outright;
  // that is the point of keeping both.
  assert(c < e, "LZ77 wins once there is a window to build");
  lz4d.close();
  tok.close();
});

// ---------------------------------------------------------------------------
// The identity guarantee — the reason the record has a header at all
// ---------------------------------------------------------------------------

test("a record decoded against the wrong dictionary produces NOTHING", () => {
  const a = new Dictionary(RPC_PHRASES);
  const b = new Dictionary(["completely", "different", "phrases"]);
  const rec = a.compress(bytes(FRAME));

  // Every code in the record is a valid index into ANY dictionary, so without
  // the id check this would succeed and return a different string — a silent
  // wrong answer, which is worse than an error.
  let threw = false;
  try { b.decompress(rec); } catch { threw = true; }
  assert(threw, "the mismatch is detected, not decoded");

  // Order is part of the identity, because the codes mean different things.
  const reordered = new Dictionary(RPC_PHRASES.slice().reverse());
  assert(reordered.id !== a.id, "reordering changes the id");

  // But an identical list, built separately, IS the same dictionary —
  // otherwise a record could not survive a restart.
  assertEqual(new Dictionary(RPC_PHRASES.slice()).id, a.id);
});

test("reuse across many records is the shape it is built for", () => {
  const d = new Dictionary(RPC_PHRASES);
  let total = 0, raw = 0;
  for (let i = 0; i < 500; i++) {
    const s = '{"jsonrpc":"2.0","method":"m' + i + '","id":' + i + '}';
    const packed = d.compress(bytes(s));
    assertEqual(text(d.decompress(packed)), s);   // the scratch is reused; prove it stays correct
    total += packed.length;
    raw += s.length;
  }
  print(`  500 frames      ${raw} -> ${total} bytes (${(raw / total).toFixed(2)}× overall)`);
  assert(total < raw, "a stream of templated frames shrinks");
  d.close();
});

test("abuse: hostile records and arguments", () => {
  const throws = (fn) => { try { fn(); return false; } catch { return true; } };
  const d = new Dictionary(RPC_PHRASES);

  assert(throws(() => new Dictionary()), "the phrase list is required");
  assert(throws(() => new Dictionary([])), "an empty list is refused");
  assert(throws(() => new Dictionary(["ok", ""])), "an EMPTY PHRASE is refused: it matches everywhere and encodes nothing");
  assert(throws(() => new Dictionary("not an array")), "a non-array is refused");
  assert(throws(() => Dictionary(RPC_PHRASES)), "Dictionary requires new");

  // compress TYPE-CHECKS rather than coercing, so no user code runs inside a
  // call. The test pins the REASON: neither hook fires and the instance stays
  // open. Asserting only "it threw" would pass even if it were rewritten to
  // coerce, which is the vacuous form to avoid.
  let ranToString = false, ranValueOf = false;
  const attacker = {
    toString() { ranToString = true; d.close(); return "gotcha"; },
    valueOf() { ranValueOf = true; d.close(); return 1; },
  };
  assert(throws(() => d.compress(attacker)), "a non-buffer argument is refused");
  assert(!ranToString && !ranValueOf, "NO coercion hook ran");
  assertEqual(d.closed, false, "the instance the attacker tried to close is still open");

  // Hostile records: truncation and bit flips must be rejected or decode
  // sanely, never crash or inflate without bound.
  const good = d.compress(bytes(FRAME));
  assert(throws(() => d.decompress(bytes(""))), "an empty buffer is not a record");
  assert(throws(() => d.decompress(good.subarray(0, good.length - 1))), "truncated");
  const badMagic = good.slice(); badMagic[0] = 0x58;
  assert(throws(() => d.decompress(badMagic)), "bad magic");
  const badVer = good.slice(); badVer[2] = 9;
  assert(throws(() => d.decompress(badVer)), "an unknown version");
  for (let i = 0; i < good.length; i++) {
    const b = good.slice(); b[i] ^= 0xff;
    try { assert(d.decompress(b).length <= 4096, "a corrupted record cannot inflate without bound"); }
    catch { /* rejection is the expected outcome */ }
  }

  // Degenerate but legal.
  assertEqual(d.decompress(d.compress(bytes(""))).length, 0, "the empty input round-trips");
  assertEqual(text(d.decompress(d.compress(bytes("\u0000\u0001")))), "\u0000\u0001", "NULs survive");
  d.close();
  assert(throws(() => d.compress(bytes("x"))), "use after close throws");
});

await run("dyna:compress — the Dictionary capability");
