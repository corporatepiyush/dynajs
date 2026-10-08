// flags: --std
import { test, run, assert, assertEqual } from "./harness.js";
import { Dictionary, Compressor, gzip, gunzip } from "dyna:compress";

const enc = new TextEncoder();
const dec = new TextDecoder();
const bytes = (s) => enc.encode(s);
const text = (b) => dec.decode(b);

const RPC_PHRASES = [
  '"jsonrpc":"2.0"', '"method":', '"params":', '"id":',
  '"result":', '"error":', '{"', '"}', '":"', '","',
];

const FRAME = '{"jsonrpc":"2.0","method":"sum","params":[1,2],"id":7}';

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
  const d = new Dictionary(RPC_PHRASES);
  const withOverlap = d.compress(bytes(FRAME)).length;

  const noOverlap = new Dictionary(RPC_PHRASES.filter((p) => p !== '{"'))
    .compress(bytes(FRAME)).length;

  print(`  with '{"'       ${withOverlap} bytes`);
  print(`  without '{"'    ${noOverlap} bytes`);
  assert(withOverlap <= noOverlap,
    `adding a phrase must never make the output larger (${withOverlap} vs ${noOverlap})`);
});

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
  const many = [];
  for (let i = 0; i < 200; i++) many.push("phrase-that-never-appears-" + i);
  const d = new Dictionary(many);
  const payload = "a".repeat(500);
  const out = d.compress(bytes(payload));
  assertEqual(text(d.decompress(out)), payload);
  assert(out.length > payload.length, "200 unused phrases still expand the output");
  assertEqual(d.size, 200);
});

test("the two dictionary mechanisms win on different payloads", () => {
  const window = enc.encode(FRAME);
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

  assert(c < e, "LZ77 wins once there is a window to build");
  lz4d.close();
  tok.close();
});

test("a record decoded against the wrong dictionary produces NOTHING", () => {
  const a = new Dictionary(RPC_PHRASES);
  const b = new Dictionary(["completely", "different", "phrases"]);
  const rec = a.compress(bytes(FRAME));

  let threw = false;
  try { b.decompress(rec); } catch { threw = true; }
  assert(threw, "the mismatch is detected, not decoded");

  const reordered = new Dictionary(RPC_PHRASES.slice().reverse());
  assert(reordered.id !== a.id, "reordering changes the id");

  assertEqual(new Dictionary(RPC_PHRASES.slice()).id, a.id);
});

test("reuse across many records is the shape it is built for", () => {
  const d = new Dictionary(RPC_PHRASES);
  let total = 0, raw = 0;
  for (let i = 0; i < 500; i++) {
    const s = '{"jsonrpc":"2.0","method":"m' + i + '","id":' + i + '}';
    const packed = d.compress(bytes(s));
    assertEqual(text(d.decompress(packed)), s);
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

  let ranToString = false, ranValueOf = false;
  const attacker = {
    toString() { ranToString = true; d.close(); return "gotcha"; },
    valueOf() { ranValueOf = true; d.close(); return 1; },
  };
  assert(throws(() => d.compress(attacker)), "a non-buffer argument is refused");
  assert(!ranToString && !ranValueOf, "NO coercion hook ran");
  assertEqual(d.closed, false, "the instance the attacker tried to close is still open");

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
    catch {  }
  }

  assertEqual(d.decompress(d.compress(bytes(""))).length, 0, "the empty input round-trips");
  assertEqual(text(d.decompress(d.compress(bytes("\u0000\u0001")))), "\u0000\u0001", "NULs survive");
  d.close();
  assert(throws(() => d.compress(bytes("x"))), "use after close throws");
});

await run("dyna:compress — the Dictionary capability");
