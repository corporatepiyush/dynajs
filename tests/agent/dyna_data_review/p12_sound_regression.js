// SOUND-module regression sweep: hash / random / uuid / structures unchanged by the patch
import { MD5Hex, SHA256Hex, Hasher } from "dyna:hash";
import { Random } from "dyna:random";
import { v4, v5, v7, validate, version, NAMESPACE_DNS } from "dyna:uuid";
import { Deque, Heap, BitSet, BloomFilter } from "dyna:structures";
import { eq as EQ, ok as OK, done as DONE } from "./harness.js";

// hash: NIST/python vectors
EQ(MD5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72", "MD5(abc)");
EQ(SHA256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256(abc)");
EQ(SHA256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256(empty)");
// streaming == one-shot
{
  const h = new Hasher("sha256");
  h.update("a"); h.update("b"); h.update("c");
  EQ(h.digestHex(), SHA256Hex("abc"), "streaming sha256 == one-shot");
}
// random: seeded determinism (xoshiro256** reference vectors from the suite gen)
{
  const r1 = new Random(42), r2 = new Random(42);
  let same = true;
  for (let i = 0; i < 100; i++) if (r1.nextU64() !== r2.nextU64()) same = false;
  OK(same, "same seed -> identical stream");
  const r3 = new Random(7);
  let stats = 0, n = 10000;
  for (let i = 0; i < n; i++) { const v = r3.nextFloat(); if (v < 0 || v >= 1) OK(false, "nextFloat out of range"); stats += v; }
  OK(Math.abs(stats / n - 0.5) < 0.02, "nextFloat() mean ~0.5 over 10k");
  const a = new Random(1).nextU53(), b = new Random(1).nextU53();
  EQ(a, b, "seeded u53 reproducible");
}
// uuid
{
  const u = v4();
  OK(validate(u) && version(u) === 4, "v4 shape");
  OK(v4() !== v4(), "v4 unique");
  EQ(v5(NAMESPACE_DNS, "example.com"), "cfbff0d1-9375-5685-968c-48ce8b15ae17", "v5(dns, example.com) python vector");
  const t1 = v7(), t2 = v7();
  OK(validate(t1) && version(t1) === 7, "v7 shape");
  OK(t1 <= t2, "v7 time-ordered");
}
// structures basics
{
  const d = new Deque();
  d.pushBack(1); d.pushFront(0); d.pushBack(2);
  EQ(d.popFront(), 0, "deque popFront");
  EQ(d.popBack(), 2, "deque popBack");
  EQ(d.popFront(), 1, "deque fifo");
  const h = new Heap();
  h.push(5); h.push(1); h.push(3);
  EQ(h.pop(), 1, "heap min order 1");
  EQ(h.pop(), 3, "heap min order 3");
  EQ(h.pop(), 5, "heap min order 5");
  const bs = new BitSet(100);
  bs.set(3); bs.set(99);
  EQ(bs.get(3), true, "bitset set/get 3");
  EQ(bs.get(2), false, "bitset unset 2");
  EQ(bs.get(99), true, "bitset high bit");
  const bf = new BloomFilter(1000, 3);
  bf.add("x");
  OK(bf.mayContain("x"), "bloom positive");
  OK(!bf.mayContain("never-added"), "bloom negative (probabilistic)");
}
DONE("p12_sound_regression");
