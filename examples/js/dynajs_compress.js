// flags: --std
import { gzip, gunzip } from "dyna:compress";
import { test, run, assert, assertEqual, assertThrows, rng, randInt } from "./harness.js";

test("gzip returns a Uint8Array with the gzip magic header", () => {
  const packed = gzip("hello world");
  assert(packed instanceof Uint8Array, "gzip output is a Uint8Array");
  assert(packed[0] === 0x1f && packed[1] === 0x8b, "starts with gzip magic 1f 8b");
});

test("string round-trips through gzip -> gunzip (many sizes, incl. empty)", () => {
  const unit = "The quick brown fox — café 你好 😀 ";
  for (const n of [0, 1, 2, 7, 64, 1000, 70000]) {
    let s = "";
    while (s.length < n) s += unit;
    s = s.slice(0, n);
    assertEqual(gunzip(gzip(s), { asString: true }), s, `len ${s.length}`);
  }
});

test("binary (Uint8Array) round-trips byte-for-byte", () => {
  for (const n of [0, 1, 255, 256, 65535, 65536, 200000]) {
    const u = new Uint8Array(n);
    for (let i = 0; i < n; i++) u[i] = (i * 2654435761) & 0xff;
    assertEqual(gunzip(gzip(u)), u, `binary len ${n}`);
  }
});

test("accepts an ArrayBuffer and a subarray view (nonzero byteOffset)", () => {
  const u = new Uint8Array([10, 20, 30, 40, 50]);
  assertEqual(gunzip(gzip(u.buffer)), u, "ArrayBuffer input");

  const base = new Uint8Array([9, 9, 1, 2, 3, 4, 9, 9]);
  const view = base.subarray(2, 6);
  assertEqual(gunzip(gzip(view)), new Uint8Array([1, 2, 3, 4]), "subarray input");
});

test("string input and its raw UTF-8 bytes compress identically", () => {
  const s = "grüße 🌍";
  const bytes = gunzip(gzip(s));
  assertEqual(gunzip(gzip(bytes)), bytes, "byte path matches string path");
  assertEqual(gunzip(gzip(s), { asString: true }), s, "string decodes back");
});

test("level 0 (stored) and level 9 both round-trip", () => {
  const s = "hello world ".repeat(400);
  assertEqual(gunzip(gzip(s, 0), { asString: true }), s, "level 0");
  assertEqual(gunzip(gzip(s, 9), { asString: true }), s, "level 9");
});

test("property-based: random binary of random length round-trips", () => {
  const next = rng(0xC0FFEE);
  for (let t = 0; t < 300; t++) {
    const len = randInt(next, 0, 4096);
    const u = new Uint8Array(len);
    for (let i = 0; i < len; i++) u[i] = randInt(next, 0, 255);
    assertEqual(gunzip(gzip(u)), u, `t=${t} len=${len}`);
  }
});

test("malformed gunzip input throws a clean Error (no crash, no leak)", () => {
  assertThrows(() => gunzip(new Uint8Array([])));
  assertThrows(() => gunzip(new Uint8Array([1, 2, 3])));
  assertThrows(() => gunzip(new Uint8Array(30)));
  const good = new Uint8Array(gzip("round trip me"));
  good[good.length - 1] ^= 0xff;
  assertThrows(() => gunzip(good));
});

test("wrong-type input is rejected with a TypeError", () => {
  assertThrows(() => gzip(123));
  assertThrows(() => gzip(null));
  assertThrows(() => gzip({}));
});

await run("dyna:compress (native gzip / gunzip)");
