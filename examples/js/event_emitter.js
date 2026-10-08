import * as std from "std";
import { test, run, assert, assertEqual, assertThrows, deepEqual } from "./harness.js";

const WILDCARD = "*";

export class EventEmitter {
  #listeners = new Map();
  #registry = new FinalizationRegistry((heldEntry) => {
    heldEntry.set?.delete(heldEntry.listener);
  });

  #bucket(event) {
    let set = this.#listeners.get(event);
    if (!set) this.#listeners.set(event, (set = new Set()));
    return set;
  }

  on(event, fn, { once = false } = {}) {
    if (typeof fn !== "function") throw new TypeError("handler must be a function");
    const entry = { once, weak: false, fn, orig: fn };
    this.#bucket(event).add(entry);
    return () => this.off(event, fn);
  }

  once(event, fn) {
    return this.on(event, fn, { once: true });
  }

  onWeak(event, target, method) {
    if (typeof target?.[method] !== "function") {
      throw new TypeError(`target has no method ${method}`);
    }
    const set = this.#bucket(event);
    const entry = { once: false, weak: true, ref: new WeakRef(target), method, orig: undefined };
    set.add(entry);
    this.#registry.register(target, { set, listener: entry }, entry);
    return () => this.off(event, target);
  }

  off(event, orig) {
    const set = this.#listeners.get(event);
    if (!set) return this;
    for (const entry of set) {
      const matches = entry.weak ? entry.ref.deref() === orig : entry.orig === orig;
      if (matches) {
        if (entry.weak) this.#registry.unregister(entry);
        set.delete(entry);
      }
    }
    if (set.size === 0) this.#listeners.delete(event);
    return this;
  }

  emit(event, ...args) {
    let invoked = 0;
    invoked += this.#fire(this.#listeners.get(event), event, args, false);
    if (event !== WILDCARD) {
      invoked += this.#fire(this.#listeners.get(WILDCARD), event, args, true);
    }
    return invoked;
  }

  #fire(set, event, args, wildcard) {
    if (!set) return 0;
    let invoked = 0;
    for (const entry of [...set]) {
      if (entry.weak) {
        const target = entry.ref.deref();
        if (target === undefined) {
          set.delete(entry);
          continue;
        }
        wildcard ? target[entry.method](event, ...args) : target[entry.method](...args);
      } else {
        wildcard ? entry.fn(event, ...args) : entry.fn(...args);
        if (entry.once) set.delete(entry);
      }
      invoked++;
    }
    if (set.size === 0) this.#listeners.delete(event);
    return invoked;
  }

  listenerCount(event) {
    return this.#listeners.get(event)?.size ?? 0;
  }

  eventNames() {
    return [...this.#listeners.keys()];
  }
}

test("basic on/emit with multiple args", () => {
  const bus = new EventEmitter();
  const seen = [];
  bus.on("data", (a, b) => seen.push([a, b]));
  assertEqual(bus.emit("data", 1, 2), 1);
  assertEqual(bus.emit("data", 3, 4), 1);
  assertEqual(seen, [[1, 2], [3, 4]]);
  assertEqual(bus.emit("other"), 0);
});

test("once fires exactly once then auto-removes", () => {
  const bus = new EventEmitter();
  let count = 0;
  bus.once("boot", () => count++);
  bus.emit("boot");
  bus.emit("boot");
  assertEqual(count, 1);
  assertEqual(bus.listenerCount("boot"), 0);
});

test("off and unsubscribe handle both work", () => {
  const bus = new EventEmitter();
  const fn = () => {};
  const unsub = bus.on("x", fn);
  assertEqual(bus.listenerCount("x"), 1);
  unsub();
  assertEqual(bus.listenerCount("x"), 0);

  bus.on("y", fn);
  bus.off("y", fn);
  assertEqual(bus.listenerCount("y"), 0);
});

test("wildcard listeners receive the event name", () => {
  const bus = new EventEmitter();
  const log = [];
  bus.on(WILDCARD, (evt, payload) => log.push(`${evt}:${payload}`));
  bus.on("click", () => {});
  const n = bus.emit("click", 5);
  assertEqual(n, 2);
  bus.emit("hover", 9);
  assertEqual(log, ["click:5", "hover:9"]);
});

test("handler order is insertion order (Set semantics)", () => {
  const bus = new EventEmitter();
  const order = [];
  bus.on("e", () => order.push("a"));
  bus.on("e", () => order.push("b"));
  bus.on("e", () => order.push("c"));
  bus.emit("e");
  assertEqual(order, ["a", "b", "c"]);
});

test("input validation", () => {
  const bus = new EventEmitter();
  assertThrows(() => bus.on("e", 123), "must be a function");
  assertThrows(() => bus.onWeak("e", {}, "nope"), "no method");
});

test("weak listener is invoked while its target is alive", () => {
  const bus = new EventEmitter();
  const received = [];
  let subscriber = { onTick(n) { received.push(n); } };
  bus.onWeak("tick", subscriber, "onTick");
  bus.emit("tick", 1);
  bus.emit("tick", 2);
  assertEqual(received, [1, 2]);
  assertEqual(bus.listenerCount("tick"), 1);
  assert(subscriber !== null);
});

test("weak listener is pruned after its target is collected", () => {
  const bus = new EventEmitter();
  let calls = 0;
  let subscriber = { onGone() { calls++; } };
  bus.onWeak("ping", subscriber, "onGone");
  bus.emit("ping");
  assertEqual(calls, 1);

  subscriber = null;
  std.gc();

  const invoked = bus.emit("ping");
  assertEqual(invoked, 0);
  assertEqual(calls, 1);
  assertEqual(bus.listenerCount("ping"), 0);
});

await run("event emitter");
