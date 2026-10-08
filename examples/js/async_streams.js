import { test, run, assert, assertEqual, assertClose } from "./harness.js";

async function* mapAsync(src, fn) {
  let i = 0;
  for await (const x of src) yield await fn(x, i++);
}

async function* filterAsync(src, pred) {
  let i = 0;
  for await (const x of src) if (await pred(x, i++)) yield x;
}

async function* takeAsync(src, n) {
  if (n <= 0) return;
  let count = 0;
  for await (const x of src) {
    yield x;
    if (++count >= n) return;
  }
}

async function* dropAsync(src, n) {
  let count = 0;
  for await (const x of src) {
    if (count++ < n) continue;
    yield x;
  }
}

async function* flatMapAsync(src, fn) {
  for await (const x of src) {
    for await (const y of await fn(x)) yield y;
  }
}

async function* scanAsync(src, fn, seed) {
  let acc = seed;
  yield acc;
  for await (const x of src) {
    acc = await fn(acc, x);
    yield acc;
  }
}

async function* batchAsync(src, size) {
  let buf = [];
  for await (const x of src) {
    buf.push(x);
    if (buf.length >= size) { yield buf; buf = []; }
  }
  if (buf.length) yield buf;
}

async function* mapConcurrent(src, limit, fn) {
  const iterator = src[Symbol.asyncIterator]();
  const inFlight = [];
  let exhausted = false;

  const topUp = async () => {
    while (inFlight.length < limit && !exhausted) {
      const { value, done } = await iterator.next();
      if (done) { exhausted = true; break; }
      inFlight.push(Promise.resolve(fn(value)));
    }
  };

  await topUp();
  while (inFlight.length > 0) {
    const result = await inFlight.shift();
    yield result;
    await topUp();
  }
}

export class Stream {
  #src;

  constructor(src) { this.#src = src; }

  static of(...items) {
    return new Stream((async function* () { for (const x of items) yield x; })());
  }

  static from(asyncIterable) { return new Stream(asyncIterable); }

  static range(start, end) {
    return new Stream((async function* () {
      for (let i = start; i < end; i++) { await Promise.resolve(); yield i; }
    })());
  }

  static naturals() {
    return new Stream((async function* () {
      let i = 1;
      for (;;) { await Promise.resolve(); yield i++; }
    })());
  }

  map(fn) { return new Stream(mapAsync(this.#src, fn)); }
  filter(pred) { return new Stream(filterAsync(this.#src, pred)); }
  take(n) { return new Stream(takeAsync(this.#src, n)); }
  drop(n) { return new Stream(dropAsync(this.#src, n)); }
  flatMap(fn) { return new Stream(flatMapAsync(this.#src, fn)); }
  scan(fn, seed) { return new Stream(scanAsync(this.#src, fn, seed)); }
  batch(size) { return new Stream(batchAsync(this.#src, size)); }
  mapConcurrent(limit, fn) { return new Stream(mapConcurrent(this.#src, limit, fn)); }

  [Symbol.asyncIterator]() { return this.#src[Symbol.asyncIterator](); }

  toArray() { return Array.fromAsync(this.#src); }

  async reduce(fn, init) {
    let acc = init;
    for await (const x of this.#src) acc = await fn(acc, x);
    return acc;
  }

  async forEach(fn) {
    for await (const x of this.#src) await fn(x);
  }
}

test("map/filter/take pipeline over an async source", async () => {
  const out = await Stream.range(0, 100)
    .filter((n) => n % 2 === 0)
    .map((n) => n * n)
    .take(5)
    .toArray();
  assertEqual(out, [0, 4, 16, 36, 64]);
});

test("take short-circuits an infinite async generator", async () => {
  const out = await Stream.naturals().map((n) => n * 10).take(4).toArray();
  assertEqual(out, [10, 20, 30, 40]);
});

test("early return runs the source's cleanup (finally)", async () => {
  let cleanedUp = false;
  async function* source() {
    try {
      let i = 0;
      for (;;) { await Promise.resolve(); yield i++; }
    } finally {
      cleanedUp = true;
    }
  }
  const out = await Stream.from(source()).take(3).toArray();
  assertEqual(out, [0, 1, 2]);
  assert(cleanedUp, "source finally block should have run");
});

test("flatMap expands each element", async () => {
  const out = await Stream.of(1, 2, 3)
    .flatMap(async (n) => (async function* () { for (let i = 0; i < n; i++) yield n; })())
    .toArray();
  assertEqual(out, [1, 2, 2, 3, 3, 3]);
});

test("scan yields running aggregates", async () => {
  const sums = await Stream.range(1, 5).scan((a, b) => a + b, 0).toArray();
  assertEqual(sums, [0, 1, 3, 6, 10]);
});

test("batch groups into fixed-size chunks", async () => {
  const chunks = await Stream.range(0, 7).batch(3).toArray();
  assertEqual(chunks, [[0, 1, 2], [3, 4, 5], [6]]);
});

test("reduce and forEach terminals", async () => {
  const total = await Stream.range(1, 6).reduce((a, b) => a + b, 0);
  assertEqual(total, 15);
  const seen = [];
  await Stream.of("a", "b", "c").forEach((x) => seen.push(x));
  assertEqual(seen, ["a", "b", "c"]);
});

test("mapConcurrent preserves order and respects the limit", async () => {
  let active = 0;
  let peak = 0;
  const mapper = async (n) => {
    active++;
    peak = Math.max(peak, active);
    for (let i = 0; i < (n % 3) + 6; i++) await Promise.resolve();
    active--;
    return n * n;
  };
  const out = await Stream.range(0, 12).mapConcurrent(3, mapper).toArray();
  assertEqual(out, Array.from({ length: 12 }, (_, i) => i * i));
  assert(peak <= 3, `peak concurrency ${peak} exceeded limit 3`);
  assert(peak >= 2, `expected real concurrency, got peak ${peak}`);
});

test("Array.fromAsync consumes an array of promises directly", async () => {
  const values = await Array.fromAsync([Promise.resolve(1), Promise.resolve(2), Promise.resolve(3)]);
  assertEqual(values, [1, 2, 3]);
  const doubled = await Array.fromAsync([1, 2, 3].map((x) => Promise.resolve(x)), (x) => x * 2);
  assertEqual(doubled, [2, 4, 6]);
});

test("errors propagate through the pipeline", async () => {
  let threw = false;
  try {
    await Stream.range(0, 10)
      .map((n) => { if (n === 3) throw new Error("boom at 3"); return n; })
      .toArray();
  } catch (e) {
    threw = e.message === "boom at 3";
  }
  assert(threw, "expected the pipeline to surface the error");
});

await run("async streams");
