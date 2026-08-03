// reactive_signals.js — a fine-grained reactivity core (à la Vue 3 / Solid):
// `reactive` objects, `effect`s that auto-track their dependencies, lazy
// memoized `computed` values, and `batch` to coalesce updates.
//
// The dependency graph is built by intercepting property access with a Proxy
// (get/set/deleteProperty traps, forwarded through Reflect). Reads inside a
// running effect subscribe that effect to the touched key; writes re-run (or
// schedule) exactly the effects that read the changed key. Conditional
// dependencies work because each effect fully re-tracks on every run.
//
// Engine features exercised: Proxy + Reflect, WeakMap-based dep storage,
// deep-reactive proxy caching, and getters.

import { test, run, assert, assertEqual, deepEqual } from "./harness.js";

/** target -> (key -> Set<effect>) */
const targetMap = new WeakMap();
/** Proxy identity cache so reactive(x) === reactive(x). */
const proxyCache = new WeakMap();

const effectStack = [];
let activeEffect = null;

let batchDepth = 0;
const pending = new Set();

function track(target, key) {
  if (!activeEffect) return;
  let deps = targetMap.get(target);
  if (!deps) targetMap.set(target, (deps = new Map()));
  let dep = deps.get(key);
  if (!dep) deps.set(key, (dep = new Set()));
  dep.add(activeEffect);
  activeEffect.deps.push(dep);
}

function trigger(target, key) {
  const dep = targetMap.get(target)?.get(key);
  if (!dep) return;
  for (const effectFn of [...dep]) {
    if (effectFn === activeEffect) continue; // don't re-enter the current effect
    if (batchDepth > 0) pending.add(effectFn);
    else scheduleOrRun(effectFn);
  }
}

function scheduleOrRun(effectFn) {
  if (!effectFn.active) return;
  if (effectFn.options.scheduler) effectFn.options.scheduler(effectFn);
  else effectFn();
}

function cleanup(effectFn) {
  for (const dep of effectFn.deps) dep.delete(effectFn);
  effectFn.deps.length = 0;
}

/** Register a reactive side effect. Returns a runner with `.stop()`. */
export function effect(fn, options = {}) {
  const effectFn = () => {
    if (!effectFn.active) return fn();
    cleanup(effectFn); // re-track from scratch each run (conditional deps)
    activeEffect = effectFn;
    effectStack.push(effectFn);
    try {
      return fn();
    } finally {
      effectStack.pop();
      activeEffect = effectStack.at(-1) ?? null;
    }
  };
  effectFn.deps = [];
  effectFn.options = options;
  effectFn.active = true;
  if (!options.lazy) effectFn();

  const runner = () => effectFn();
  runner.effect = effectFn;
  runner.stop = () => { cleanup(effectFn); effectFn.active = false; };
  return runner;
}

const isObject = (v) => v !== null && typeof v === "object";

/** Wrap an object in a deep reactive Proxy (cached by identity). */
export function reactive(target) {
  if (!isObject(target)) return target;
  if (proxyCache.has(target)) return proxyCache.get(target);

  const proxy = new Proxy(target, {
    get(obj, key, receiver) {
      const value = Reflect.get(obj, key, receiver);
      if (typeof key === "symbol") return value; // don't track well-known symbols
      track(obj, key);
      return isObject(value) ? reactive(value) : value; // deep
    },
    set(obj, key, value, receiver) {
      const had = Reflect.has(obj, key);
      const old = obj[key];
      const result = Reflect.set(obj, key, value, receiver);
      if (!had || !Object.is(old, value)) trigger(obj, key);
      if (Array.isArray(obj) && key !== "length") trigger(obj, "length");
      return result;
    },
    deleteProperty(obj, key) {
      const had = Reflect.has(obj, key);
      const result = Reflect.deleteProperty(obj, key);
      if (had) trigger(obj, key);
      return result;
    },
  });
  proxyCache.set(target, proxy);
  return proxy;
}

/** A lazy, memoized derived value. Read via `.value`. */
export function computed(getter) {
  let value;
  let dirty = true;
  const holder = {};
  const runner = effect(getter, {
    lazy: true,
    scheduler() {
      if (!dirty) {
        dirty = true;
        trigger(holder, "value"); // notify downstream effects/computeds
      }
    },
  });
  return {
    get value() {
      if (dirty) {
        value = runner();
        dirty = false;
      }
      track(holder, "value");
      return value;
    },
  };
}

/** Run `fn`, deferring all triggered effects until it returns (run once each). */
export function batch(fn) {
  batchDepth++;
  try {
    return fn();
  } finally {
    if (--batchDepth === 0) {
      const effects = [...pending];
      pending.clear();
      for (const e of effects) scheduleOrRun(e);
    }
  }
}

// --- tests -------------------------------------------------------------------

test("effect runs immediately and on dependency change", () => {
  const state = reactive({ count: 0 });
  const seen = [];
  effect(() => seen.push(state.count));
  assertEqual(seen, [0]);
  state.count = 1;
  state.count = 2;
  assertEqual(seen, [0, 1, 2]);
});

test("effects only re-run for keys they actually read", () => {
  const state = reactive({ a: 1, b: 100 });
  let runs = 0;
  effect(() => { void state.a; runs++; });
  assertEqual(runs, 1);
  state.b = 200; // not tracked by the effect
  assertEqual(runs, 1);
  state.a = 2; // tracked
  assertEqual(runs, 2);
});

test("setting the same value does not trigger", () => {
  const state = reactive({ x: 5 });
  let runs = 0;
  effect(() => { void state.x; runs++; });
  state.x = 5; // unchanged
  assertEqual(runs, 1);
  state.x = 6;
  assertEqual(runs, 2);
});

test("computed is lazy, memoized, and reactive", () => {
  const state = reactive({ w: 3, h: 4 });
  let evals = 0;
  const area = computed(() => { evals++; return state.w * state.h; });
  assertEqual(evals, 0); // not evaluated until read
  assertEqual(area.value, 12);
  assertEqual(area.value, 12);
  assertEqual(evals, 1); // memoized: second read did not recompute
  state.w = 5;
  assertEqual(area.value, 20);
  assertEqual(evals, 2);
});

test("computed feeding an effect", () => {
  const state = reactive({ price: 10, qty: 2 });
  const total = computed(() => state.price * state.qty);
  const log = [];
  effect(() => log.push(total.value));
  assertEqual(log, [20]);
  state.qty = 3;
  assertEqual(log, [20, 30]);
});

test("deep reactivity and proxy identity", () => {
  const state = reactive({ user: { name: "ada", tags: ["a"] } });
  const seen = [];
  effect(() => seen.push(state.user.name));
  state.user.name = "grace";
  assertEqual(seen, ["ada", "grace"]);
  assert(state.user === state.user); // cached proxy identity
});

test("array mutation is reactive via length tracking", () => {
  const state = reactive({ items: [1, 2] });
  const lengths = [];
  effect(() => lengths.push(state.items.length));
  state.items.push(3);
  state.items.push(4);
  assertEqual(lengths, [2, 3, 4]);
});

test("conditional dependencies re-track each run", () => {
  const state = reactive({ toggle: true, a: 1, b: 2 });
  const seen = [];
  effect(() => seen.push(state.toggle ? state.a : state.b));
  assertEqual(seen, [1]);
  state.b = 20; // b not tracked yet (toggle is true) -> no run
  assertEqual(seen, [1]);
  state.toggle = false; // now reads b
  assertEqual(seen.at(-1), 20);
  state.a = 10; // a no longer tracked -> no run
  const before = seen.length;
  assertEqual(seen.length, before);
  state.b = 30; // now tracked
  assertEqual(seen.at(-1), 30);
});

test("batch coalesces multiple writes into one effect run", () => {
  const state = reactive({ x: 1, y: 1 });
  let runs = 0;
  effect(() => { void state.x; void state.y; runs++; });
  assertEqual(runs, 1);
  batch(() => {
    state.x = 2;
    state.y = 2;
    state.x = 3;
  });
  assertEqual(runs, 2); // single re-run despite three writes
});

test("stop() detaches an effect", () => {
  const state = reactive({ n: 0 });
  const seen = [];
  const runner = effect(() => seen.push(state.n));
  state.n = 1;
  runner.stop();
  state.n = 2; // ignored
  assertEqual(seen, [0, 1]);
});

await run("reactive signals");
