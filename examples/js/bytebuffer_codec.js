// bytebuffer_codec.js — a binary serialization codec: a growable byte writer /
// reader over a *resizable* ArrayBuffer, LEB128 var-ints, zig-zag signed
// var-ints, 64-bit BigInt integers, hand-rolled UTF-8, and a small schema layer
// (struct/array/primitives). Round-trips through Base64 and Hex too.
//
// Engine features exercised: resizable ArrayBuffer with length-tracking views,
// DataView (incl. setBigUint64/setBigInt64), Uint8Array.toBase64/fromBase64 and
// toHex/fromHex, BigInt, and code-point iteration for UTF-8.

import { test, run, assert, assertEqual, assertClose, assertThrows, deepEqual } from "./harness.js";

// --- UTF-8 (TextEncoder/TextDecoder are absent on this engine) ---------------

function utf8Encode(str) {
  const out = [];
  for (const ch of str) { // iterates by code point, not UTF-16 unit
    let cp = ch.codePointAt(0);
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return Uint8Array.from(out);
}

function utf8Decode(bytes, start, length) {
  let out = "";
  const end = start + length;
  let i = start;
  while (i < end) {
    const b = bytes[i++];
    let cp;
    if (b < 0x80) cp = b;
    else if ((b & 0xe0) === 0xc0) cp = ((b & 0x1f) << 6) | (bytes[i++] & 0x3f);
    else if ((b & 0xf0) === 0xe0) cp = ((b & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
    else cp = ((b & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
    out += String.fromCodePoint(cp);
  }
  return out;
}

// --- writer ------------------------------------------------------------------

export class ByteWriter {
  #ab; #view; #u8; #len = 0; #le;

  constructor({ littleEndian = true, initial = 64 } = {}) {
    // A resizable ArrayBuffer: the DataView / Uint8Array below are
    // length-tracking, so they follow every resize() automatically.
    this.#ab = new ArrayBuffer(initial, { maxByteLength: 1 << 28 });
    this.#view = new DataView(this.#ab);
    this.#u8 = new Uint8Array(this.#ab);
    this.#le = littleEndian;
  }

  #ensure(extra) {
    const need = this.#len + extra;
    if (need <= this.#ab.byteLength) return;
    let cap = this.#ab.byteLength;
    while (cap < need) cap *= 2;
    this.#ab.resize(cap); // views track the new length
  }

  u8(v) { this.#ensure(1); this.#view.setUint8(this.#len, v); this.#len += 1; return this; }
  u16(v) { this.#ensure(2); this.#view.setUint16(this.#len, v, this.#le); this.#len += 2; return this; }
  u32(v) { this.#ensure(4); this.#view.setUint32(this.#len, v, this.#le); this.#len += 4; return this; }
  i32(v) { this.#ensure(4); this.#view.setInt32(this.#len, v, this.#le); this.#len += 4; return this; }
  f32(v) { this.#ensure(4); this.#view.setFloat32(this.#len, v, this.#le); this.#len += 4; return this; }
  f64(v) { this.#ensure(8); this.#view.setFloat64(this.#len, v, this.#le); this.#len += 8; return this; }
  bigU64(v) { this.#ensure(8); this.#view.setBigUint64(this.#len, v, this.#le); this.#len += 8; return this; }
  bigI64(v) { this.#ensure(8); this.#view.setBigInt64(this.#len, v, this.#le); this.#len += 8; return this; }

  /** LEB128 unsigned var-int (supports 0 .. 2^53-1 without bitwise overflow). */
  varUint(n) {
    if (n < 0 || !Number.isSafeInteger(n)) throw new RangeError("varUint needs a safe non-negative integer");
    while (n >= 0x80) { this.u8((n & 0x7f) | 0x80); n = Math.floor(n / 128); }
    return this.u8(n);
  }

  /** Zig-zag signed var-int. */
  varInt(n) { return this.varUint(n >= 0 ? n * 2 : -n * 2 - 1); }

  bytes(u8arr) { this.varUint(u8arr.length); this.#ensure(u8arr.length); this.#u8.set(u8arr, this.#len); this.#len += u8arr.length; return this; }
  string(str) { return this.bytes(utf8Encode(str)); }

  toUint8Array() { return this.#u8.slice(0, this.#len); }
  toBase64() { return this.toUint8Array().toBase64(); }
  toHex() { return this.toUint8Array().toHex(); }
  get length() { return this.#len; }
}

// --- reader ------------------------------------------------------------------

export class ByteReader {
  #view; #u8; #pos = 0; #le;

  constructor(u8arr, { littleEndian = true } = {}) {
    this.#u8 = u8arr;
    this.#view = new DataView(u8arr.buffer, u8arr.byteOffset, u8arr.byteLength);
    this.#le = littleEndian;
  }

  static fromBase64(s, opts) { return new ByteReader(Uint8Array.fromBase64(s), opts); }
  static fromHex(s, opts) { return new ByteReader(Uint8Array.fromHex(s), opts); }

  #need(n) {
    if (this.#pos + n > this.#u8.length) {
      throw new RangeError(`unexpected end of buffer at ${this.#pos} (+${n} > ${this.#u8.length})`);
    }
  }

  u8() { this.#need(1); const v = this.#view.getUint8(this.#pos); this.#pos += 1; return v; }
  u16() { this.#need(2); const v = this.#view.getUint16(this.#pos, this.#le); this.#pos += 2; return v; }
  u32() { this.#need(4); const v = this.#view.getUint32(this.#pos, this.#le); this.#pos += 4; return v; }
  i32() { this.#need(4); const v = this.#view.getInt32(this.#pos, this.#le); this.#pos += 4; return v; }
  f32() { this.#need(4); const v = this.#view.getFloat32(this.#pos, this.#le); this.#pos += 4; return v; }
  f64() { this.#need(8); const v = this.#view.getFloat64(this.#pos, this.#le); this.#pos += 8; return v; }
  bigU64() { this.#need(8); const v = this.#view.getBigUint64(this.#pos, this.#le); this.#pos += 8; return v; }
  bigI64() { this.#need(8); const v = this.#view.getBigInt64(this.#pos, this.#le); this.#pos += 8; return v; }

  varUint() {
    let result = 0, scale = 1, byte;
    do { byte = this.u8(); result += (byte & 0x7f) * scale; scale *= 128; } while (byte & 0x80);
    return result;
  }

  varInt() { const u = this.varUint(); return u % 2 === 0 ? u / 2 : -(u + 1) / 2; }

  bytes() {
    const n = this.varUint();
    this.#need(n);
    const slice = this.#u8.subarray(this.#pos, this.#pos + n);
    this.#pos += n;
    return slice;
  }

  string() { const b = this.bytes(); return utf8Decode(b, 0, b.length); }
  get remaining() { return this.#u8.length - this.#pos; }
}

// --- schema layer ------------------------------------------------------------

const prim = (write, read) => ({ write, read });
export const t = {
  u8: prim((w, v) => w.u8(v), (r) => r.u8()),
  u16: prim((w, v) => w.u16(v), (r) => r.u16()),
  u32: prim((w, v) => w.u32(v), (r) => r.u32()),
  i32: prim((w, v) => w.i32(v), (r) => r.i32()),
  f32: prim((w, v) => w.f32(v), (r) => r.f32()),
  f64: prim((w, v) => w.f64(v), (r) => r.f64()),
  varUint: prim((w, v) => w.varUint(v), (r) => r.varUint()),
  varInt: prim((w, v) => w.varInt(v), (r) => r.varInt()),
  bigU64: prim((w, v) => w.bigU64(v), (r) => r.bigU64()),
  bigI64: prim((w, v) => w.bigI64(v), (r) => r.bigI64()),
  string: prim((w, v) => w.string(v), (r) => r.string()),
  bool: prim((w, v) => w.u8(v ? 1 : 0), (r) => r.u8() !== 0),
  array: (elem) => prim(
    (w, arr) => { w.varUint(arr.length); for (const x of arr) elem.write(w, x); },
    (r) => { const n = r.varUint(); const out = new Array(n); for (let i = 0; i < n; i++) out[i] = elem.read(r); return out; },
  ),
  struct: (fields) => prim(
    (w, obj) => { for (const [k, ty] of Object.entries(fields)) ty.write(w, obj[k]); },
    (r) => { const o = {}; for (const [k, ty] of Object.entries(fields)) o[k] = ty.read(r); return o; },
  ),
};

export function encode(schema, value, opts) {
  const w = new ByteWriter(opts);
  schema.write(w, value);
  return w.toUint8Array();
}

export function decode(schema, u8arr, opts) {
  return schema.read(new ByteReader(u8arr, opts));
}

// --- tests -------------------------------------------------------------------

test("primitive round-trips (little and big endian)", () => {
  for (const le of [true, false]) {
    const w = new ByteWriter({ littleEndian: le });
    w.u8(200).u16(50000).u32(4000000000).i32(-123456).f64(3.141592653589793);
    const r = new ByteReader(w.toUint8Array(), { littleEndian: le });
    assertEqual(r.u8(), 200);
    assertEqual(r.u16(), 50000);
    assertEqual(r.u32(), 4000000000);
    assertEqual(r.i32(), -123456);
    assertEqual(r.f64(), 3.141592653589793);
    assertEqual(r.remaining, 0);
  }
});

test("f32 keeps single precision", () => {
  const w = new ByteWriter().f32(0.1);
  const v = new ByteReader(w.toUint8Array()).f32();
  assertClose(v, 0.1, 1e-7);
});

test("LEB128 var-uint edge cases", () => {
  const cases = [0, 1, 127, 128, 300, 16383, 16384, 2 ** 21, 2 ** 32, Number.MAX_SAFE_INTEGER];
  const w = new ByteWriter();
  for (const n of cases) w.varUint(n);
  const r = new ByteReader(w.toUint8Array());
  for (const n of cases) assertEqual(r.varUint(), n);
  assertThrows(() => new ByteWriter().varUint(-1), "non-negative");
});

test("zig-zag signed var-int", () => {
  const cases = [0, -1, 1, -2, 2, 63, -64, 1000, -1000, -(2 ** 30), 2 ** 30];
  const w = new ByteWriter();
  for (const n of cases) w.varInt(n);
  const r = new ByteReader(w.toUint8Array());
  for (const n of cases) assertEqual(r.varInt(), n);
});

test("64-bit BigInt integers", () => {
  const w = new ByteWriter();
  w.bigU64(18446744073709551615n).bigI64(-9223372036854775808n).bigU64(0n);
  const r = new ByteReader(w.toUint8Array());
  assertEqual(r.bigU64(), 18446744073709551615n);
  assertEqual(r.bigI64(), -9223372036854775808n);
  assertEqual(r.bigU64(), 0n);
});

test("UTF-8 strings incl. multi-byte code points", () => {
  const samples = ["", "hello", "café", "naïve", "日本語", "emoji 😀🎉", "mix: aé日😀"];
  const w = new ByteWriter();
  for (const s of samples) w.string(s);
  const r = new ByteReader(w.toUint8Array());
  for (const s of samples) assertEqual(r.string(), s);
});

test("growable writer resizes past its initial capacity", () => {
  const w = new ByteWriter({ initial: 8 });
  for (let i = 0; i < 1000; i++) w.u32(i);
  assert(w.length === 4000);
  const r = new ByteReader(w.toUint8Array());
  for (let i = 0; i < 1000; i++) assertEqual(r.u32(), i);
});

test("schema codec round-trips a nested record", () => {
  const Point = t.struct({ x: t.f64, y: t.f64 });
  const Shape = t.struct({
    id: t.varUint,
    name: t.string,
    closed: t.bool,
    points: t.array(Point),
    checksum: t.bigU64,
  });
  const shape = {
    id: 4242,
    name: "polygon ✦",
    closed: true,
    points: [{ x: 0, y: 0 }, { x: 1.5, y: -2.25 }, { x: 3, y: 4 }],
    checksum: 12345678901234567890n,
  };
  const bytes = encode(Shape, shape);
  assert(deepEqual(decode(Shape, bytes), shape));
});

test("Base64 and Hex transport round-trips", () => {
  const Msg = t.struct({ seq: t.u32, body: t.string, tags: t.array(t.string) });
  const msg = { seq: 7, body: "ping ☕", tags: ["a", "bb", "ccc"] };
  const bytes = encode(Msg, msg);

  const b64 = bytes.toBase64();
  assert(deepEqual(Msg.read(ByteReader.fromBase64(b64)), msg));

  const hex = bytes.toHex();
  assert(/^[0-9a-f]*$/.test(hex));
  assert(deepEqual(Msg.read(ByteReader.fromHex(hex)), msg));
});

test("reader bounds-checks truncated input", () => {
  const bytes = encode(t.struct({ a: t.u32, b: t.u32 }), { a: 1, b: 2 });
  const truncated = bytes.subarray(0, 6); // one full u32 + 2 bytes
  const r = new ByteReader(truncated);
  assertEqual(r.u32(), 1);
  assertThrows(() => r.u32(), "unexpected end of buffer");
});

await run("bytebuffer codec");
