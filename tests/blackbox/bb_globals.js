// Parametric black-box contract test, generated from dynajs.d.ts lines 6904-7341. Engine sources not consulted.
// Coverage: WHATWG/engine globals -- AbortController/AbortSignal, Headers, FormData, Request, Response,
// WebSocket/URL/URLSearchParams/fetch identity notes, TextEncoder/TextDecoder (incl. decode {stream:true}
// semantics), performance, console, print, scriptArgs, timers, queueMicrotask, atob/btoa (latin-1 WARNING),
// Iterator + IteratorObject helpers + zip/zipKeyed, Float16Array, SuppressedError, DisposableStack/
// AsyncDisposableStack, InternalError, Lens, sleep, structuredClone.
//
// Table convention: every row is [label, () => actual, expectation] where expectation is
//   - an exact value        -> assertEq (NaN-safe),
//   - an array              -> deep equality via JSON,
//   - a predicate function  -> structural/property fact (the row comment says which fact and why the doc
//                              does not pin an exact output),
//   - {throws: patternOrNull, type: ErrCtorOrNull} -> must throw.
// One loop per table; failure messages name the row label.

// Static imports: a file with no static import/export is parsed as a SCRIPT by
// this engine, where general top-level await is a SyntaxError; a module
// (static import present) supports it. These four namespaces replace the
// `await import(...)` calls further down (test-generation slip, see FINDINGS).
import * as urlMod from "dyna:url";
import * as httpMod from "dyna:http";
import * as serMod from "dyna:serialize";
import * as bytesMod from "dyna:bytes";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
const U8 = (bytes) => new Uint8Array(bytes);

function runRows(rows) {
  for (const [label, get, exp] of rows) {
    if (exp !== null && typeof exp === "object" && "throws" in exp) { assertThrows(() => get(), label, exp.type, exp.throws); continue; }
    const actual = get();
    if (typeof exp === "function") { assert(exp(actual), label + " [structural predicate]"); continue; }
    // arrays AND plain-object expectations deep-compare via JSON (assertEq is
    // primitive-only; a bare object expectation would never pass it).
    if (Array.isArray(exp) || (exp !== null && typeof exp === "object")) { assertDeepEq(actual, exp, label); continue; }
    assertEq(actual, exp, label);
  }
}

async function runRowsAsync(rows) {
  for (const [label, get, exp] of rows) {
    const actual = await get();
    if (typeof exp === "function") { assert(exp(actual), label + " [structural predicate]"); continue; }
    if (Array.isArray(exp) || (exp !== null && typeof exp === "object")) { assertDeepEq(actual, exp, label); continue; }
    assertEq(actual, exp, label);
  }
}

// ---------------------------------------------------------------------------
// Identity rows -- dynajs.d.ts documents these as THE same binding, not a copy.
// ---------------------------------------------------------------------------
{
  runRows([
    // doc line 7026-7030: `globalThis.URL === (await import("dyna:url")).URL`, not a copy
    ["identity: global URL === dyna:url URL (doc: THE same class, not a copy)", () => globalThis.URL === urlMod.URL, true],
    // doc line 7031-7032
    ["identity: global URLSearchParams === dyna:url URLSearchParams", () => globalThis.URLSearchParams === urlMod.URLSearchParams, true],
    // doc line 7034-7038: same function object, identity runtime-verified
    ["identity: global fetch === dyna:http fetch (doc: THE same function object)", () => globalThis.fetch === httpMod.fetch, true],
    // doc line 6973-6974: thin alias over dyna:http WsClient (same constructor)
    ["identity: global WebSocket === dyna:http WsClient (doc: same constructor, no network touched)", () => globalThis.WebSocket === httpMod.WsClient, true],
    // doc line 7337-7340
    ["identity: global structuredClone === dyna:serialize structuredClone", () => globalThis.structuredClone === serMod.structuredClone, true],
  ]);
}

// ---------------------------------------------------------------------------
// atob / btoa -- WHATWG latin-1 STRING codecs (doc lines 7097-7104: exact rows
// "btoa('é') === '6Q=='", chars above U+00FF throw; contrast with UTF-8
// String.prototype.encodeBase64 whose encoding of 'é' is "w6k=").
// Base64 alphabet values for the ASCII rows are the universal RFC 4648 ones.
// ---------------------------------------------------------------------------
runRows([
  ["btoa('') === ''", () => btoa(""), ""],
  ["btoa('f') === 'Zg==' (latin-1 raw byte 0x66)", () => btoa("f"), "Zg=="],
  ["btoa('fo') === 'Zm8='", () => btoa("fo"), "Zm8="],
  ["btoa('foo') === 'Zm9v'", () => btoa("foo"), "Zm9v"],
  ["DOC EXACT: btoa('é') === '6Q==' (one raw byte 0xE9)", () => btoa("é"), "6Q=="],
  ["DOC EXACT: atob('6Q==') === 'é'", () => atob("6Q=="), "é"],
  ["atob('Zm9v') === 'foo'", () => atob("Zm9v"), "foo"],
  ["atob('') === ''", () => atob(""), ""],
  ["btoa throws for a char above U+00FF (U+0100)", () => btoa("\u0100"), { throws: null }],
  ["btoa throws for a lone surrogate (> U+00FF)", () => btoa("\uD800"), { throws: null }],
  ["atob throws on invalid base64 input (WHATWG forgiving-base64)", () => atob("!!!"), { throws: null }],
  ["DOC contrast: String.encodeBase64 is UTF-8, so 'é'.encodeBase64() === 'w6k='", () => "é".encodeBase64(), "w6k="],
  ["DOC contrast: btoa('é') !== 'é'.encodeBase64() (latin-1 vs UTF-8 codecs)", () => btoa("é") !== "é".encodeBase64(), true],
  ["decodeBase64('w6k=') === 'é' (UTF-8 round trip)", () => "w6k=".decodeBase64(), "é"],
]);

// ---------------------------------------------------------------------------
// TextEncoder / TextDecoder (doc lines 7040-7064).
// ---------------------------------------------------------------------------
{
  runRows([
    ["TextEncoder.encoding === 'utf-8'", () => new TextEncoder().encoding, "utf-8"],
    // doc: byte-identical to dyna:bytes' fromUtf8; h/é(C3 A9)/l/l/o pinned from UTF-8
    ["TextEncoder.encode('héllo') byte-identical to bytes.fromUtf8 (bytes 104,195,169,108,108,111)", () => {
      const enc = Array.from(new TextEncoder().encode("héllo"));
      const mod = Array.from(bytesMod.fromUtf8("héllo"));
      return enc.join(",") === "104,195,169,108,108,111" && JSON.stringify(enc) === JSON.stringify(mod);
    }, true],
    ["encode() with no input encodes the empty string", () => new TextEncoder().encode().length, 0],
    // WHATWG Encoding: a lone surrogate encodes as U+FFFD (EF BF BD), not raw WTF-8
    ["encode() lone surrogate becomes U+FFFD (239,191,189)", () => Array.from(new TextEncoder().encode("\uD800")).join(","), "239,191,189"],
    ["encodeInto() lone surrogate becomes U+FFFD, read counts one unit", () => {
      const dest = new Uint8Array(8);
      const r = new TextEncoder().encodeInto("\uDC00X", dest);
      return r.read === 2 && r.written === 4 && Array.from(dest.subarray(0, 4)).join(",") === "239,191,189,88";
    }, true],
    ["encodeInto ASCII: read 3 written 3, dest filled", () => {
      const dest = new Uint8Array(8);
      const r = new TextEncoder().encodeInto("abc", dest);
      return r.read === 3 && r.written === 3 && Array.from(dest.subarray(0, 3)).join(",") === "97,98,99";
    }, true],
    ["TextDecoder defaults: encoding 'utf-8', fatal false, ignoreBOM false", () => {
      const d = new TextDecoder();
      return d.encoding === "utf-8" && d.fatal === false && d.ignoreBOM === false;
    }, true],
    ["constructor opts surface on .fatal/.ignoreBOM", () => {
      const f = new TextDecoder("utf-8", { fatal: true });
      const b = new TextDecoder("utf-8", { ignoreBOM: true });
      return f.fatal === true && b.ignoreBOM === true;
    }, true],
    ["decode basic bytes", () => new TextDecoder().decode(U8([104, 105])), "hi"],
    ["decode accepts an ArrayBuffer", () => new TextDecoder().decode(U8([65, 66]).buffer), "AB"],
    ["stream carry: incomplete trailing byte held, no U+FFFD yet", () => new TextDecoder().decode(U8([0xC3]), { stream: true }), ""],
    ["stream carry completes: C3 A9 split across calls yields 'é'", () => {
      const d = new TextDecoder();
      const first = d.decode(U8([0xC3]), { stream: true });
      return first + "|" + d.decode(U8([0xA9]), { stream: true });
    }, "|é"],
    ["final (non-stream) call: incomplete tail becomes U+FFFD and decoder resets", () => {
      const d = new TextDecoder();
      const flushed = d.decode(U8([0xC3]));
      return flushed + "|" + d.decode(U8([65]));
    }, "\uFFFD|A"],
    ["fatal decoder throws on an incomplete tail", () => new TextDecoder("utf-8", { fatal: true }).decode(U8([0xC3])), { throws: null }],
    ["fatal decoder throws on an invalid byte", () => new TextDecoder("utf-8", { fatal: true }).decode(U8([0xFF])), { throws: null }],
    ["non-fatal decoder maps an invalid byte to U+FFFD", () => new TextDecoder().decode(U8([0xFF])), "\uFFFD"],
    ["BOM stripped once (plain call)", () => new TextDecoder().decode(U8([0xEF, 0xBB, 0xBF, 65])), "A"],
    ["DOC: BOM stripped once even when its bytes complete in a later chunk", () => {
      const d = new TextDecoder();
      const first = d.decode(U8([0xEF, 0xBB]), { stream: true });
      return first + "|" + d.decode(U8([0xBF, 65]), { stream: true });
    }, "|A"],
    // doc sentence "Unknown option keys throw TypeError" lives in decode()'s own comment (doc-literal reading).
    ["unknown option key to decode() throws TypeError", () => new TextDecoder().decode(U8([65]), { bogus: true }), { throws: null, type: TypeError }],
  ]);
}

// ---------------------------------------------------------------------------
// Iterator constructor + helpers (doc lines 7106-7180: ES2025 helper surface
// plus engine extras; zip modes; string inputs refused).
// ---------------------------------------------------------------------------
runRows([
  ["Iterator.from wraps an iterable in the helper-equipped iterator", () => Iterator.from([1, 2]).map((x) => x + 1).toArray(), [2, 3]],
  ["Iterator.from over a string yields code points", () => Iterator.from("ab").toArray(), ["a", "b"]],
  ["Iterator.concat concatenates (ES2025)", () => Iterator.concat([1], [2, 3], [4]).toArray(), [1, 2, 3, 4]],
  ["Iterator.zip default mode 'shortest'", () => Iterator.zip([[1, 2, 3], ["a", "b"]]).toArray(), [[1, "a"], [2, "b"]]],
  ["Iterator.zip mode 'shortest' explicit", () => Iterator.zip([[1, 2], ["a", "b", "c"]], { mode: "shortest" }).toArray(), [[1, "a"], [2, "b"]]],
  ["Iterator.zip mode 'longest' pads with undefined (structural: element checked without JSON)", () => {
    const r = Iterator.zip([[1, 2], ["a"]], { mode: "longest" }).toArray();
    return r.length === 2 && r[0][0] === 1 && r[0][1] === "a" && r[1][0] === 2 && r[1].length === 2 && r[1][1] === undefined;
  }, true],
  ["Iterator.zip mode 'strict' throws on length mismatch", () => Iterator.zip([[1, 2], ["a"]], { mode: "strict" }).toArray(), { throws: null }],
  ["DOC: Iterator.zip refuses string inputs", () => Iterator.zip(["ab", [1, 2]]).toArray(), { throws: null }],
  ["Iterator.zipKeyed zips object values by key", () => Iterator.zipKeyed({ a: [1, 2], b: ["x", "y"] }).toArray(), [{ a: 1, b: "x" }, { a: 2, b: "y" }]],
  ["Iterator.zipKeyed 'strict' throws on mismatch", () => Iterator.zipKeyed({ a: [1, 2], b: ["x"] }, { mode: "strict" }).toArray(), { throws: null }],
]);

// ---------------------------------------------------------------------------
// IteratorObject helper surface -- one row per documented helper family.
// Chainability/laziness shape per doc lines 7124-7180.
// ---------------------------------------------------------------------------
runRows([
  ["map/take chain (doc's own shape)", () => [1, 2, 3, 4].values().map((x) => x * 2).take(2).toArray(), [2, 4]],
  ["filter", () => Iterator.from([1, 2, 3, 4]).filter((x) => x % 2 === 0).toArray(), [2, 4]],
  ["filter+map compose", () => Iterator.from([1, 2, 3, 4]).filter((x) => x % 2 === 0).map((x) => x * 10).toArray(), [20, 40]],
  ["drop", () => Iterator.from([1, 2, 3, 4]).drop(1).toArray(), [2, 3, 4]],
  ["takeWhile", () => Iterator.from([1, 2, 3, 1]).takeWhile((x) => x < 3).toArray(), [1, 2]],
  ["dropWhile", () => Iterator.from([1, 2, 3, 1]).dropWhile((x) => x < 3).toArray(), [3, 1]],
  ["flatMap", () => Iterator.from([1, 2]).flatMap((x) => [x, x * 10]).toArray(), [1, 10, 2, 20]],
  ["scan(fn, seed)", () => Iterator.from([1, 2, 3]).scan((a, v) => a + v, 0).toArray(), [0, 1, 3, 6]],
  ["intersperse", () => Iterator.from([1, 2]).intersperse(0).toArray(), [1, 0, 2]],
  ["compact", () => Iterator.from([1, null, 2, undefined, 3]).compact().toArray(), [1, 2, 3]],
  ["dropRepeats", () => Iterator.from([1, 1, 2, 2, 1]).dropRepeats().toArray(), [1, 2, 1]],
  ["dropRepeatsWith", () => Iterator.from([1, 1.1, 2]).dropRepeatsWith((a, b) => Math.floor(a) === Math.floor(b)).toArray(), [1, 2]],
  ["dropRepeatsBy", () => Iterator.from([1, 1.1, 2]).dropRepeatsBy((x) => Math.floor(x)).toArray(), [1, 2]],
  ["aperture", () => Iterator.from([1, 2, 3]).aperture(2).toArray(), [[1, 2], [2, 3]]],
  ["splitEvery", () => Iterator.from([1, 2, 3]).splitEvery(2).toArray(), [[1, 2], [3]]],
  // DOC-TENSION resolved: d.ts listed zipWith(other, fn), but API.md:15980
  // ("zipWith(fn, other)"), the engine's own tests (test_array_ext.js
  // "zipWith(fn, other)", test_iterator_lazy.js) and the implementation all
  // pin zipWith(fn, other); d.ts corrected to match.
  ["zipWith", () => Iterator.from([1, 2]).zipWith((a, b) => a + b, [10, 20]).toArray(), [11, 22]],
  ["pluck", () => Iterator.from([{ k: 1 }, { k: 2 }]).pluck("k").toArray(), [1, 2]],
  ["tee(2) yields two full iterators", () => {
    const [t1, t2] = Iterator.from([1, 2, 3]).tee(2);
    return JSON.stringify(t1.toArray()) + JSON.stringify(t2.toArray());
  }, "[1,2,3][1,2,3]"],
  ["unique", () => Iterator.from([1, 1, 2]).unique().toArray(), [1, 2]],
  ["uniq (parity alias of unique)", () => Iterator.from([1, 1, 2]).uniq().toArray(), [1, 2]],
  ["uniqBy", () => Iterator.from([1, 2, 3]).uniqBy((x) => Math.floor(x / 2)).toArray(), [1, 2]],
  ["reduce with seed", () => Iterator.from([1, 2, 3]).reduce((a, v) => a + v, 0), 6],
  ["reduce without seed", () => Iterator.from([1, 2, 3]).reduce((a, v) => a + v), 6],
  ["some / every", () => [Iterator.from([1, 2]).some((x) => x > 1), Iterator.from([1, 2]).every((x) => x > 0)], [true, true]],
  ["find / findIndex", () => { const it = Iterator.from([1, 2, 3]); const f = it.find((x) => x > 1); const fi = Iterator.from([1, 2, 3]).findIndex((x) => x > 1); return [f, fi]; }, [2, 1]],
  ["includes", () => Iterator.from([1, 2]).includes(2), true],
  ["count() all / count(pred)", () => [Iterator.from([1, 2, 3]).count(), Iterator.from([1, 2, 3]).count((x) => x > 1)], [3, 2]],
  ["first / head", () => [Iterator.from([1, 2]).first(), Iterator.from([1, 2]).head()], [1, 1]],
  ["last", () => Iterator.from([1, 2]).last(), 2],
  ["nth / nth beyond end", () => [Iterator.from([1, 2, 3]).nth(1), Iterator.from([1, 2, 3]).nth(9)], [2, undefined]],
  ["init (all but last)", () => Iterator.from([1, 2, 3]).init().toArray(), [1, 2]],
  ["tail (all but first)", () => Iterator.from([1, 2, 3]).tail().toArray(), [2, 3]],
  ["sum / product", () => [Iterator.from([1, 2, 3]).sum(), Iterator.from([2, 3]).product()], [6, 6]],
  ["average / mean (parity pair)", () => [Iterator.from([2, 4]).average(), Iterator.from([2, 4]).mean()], [3, 3]],
  ["min / max", () => [Iterator.from([3, 1, 2]).min(), Iterator.from([3, 1, 2]).max()], [1, 3]],
  ["none / any / all", () => [Iterator.from([1, 2]).none((x) => x > 5), Iterator.from([1, 2]).any((x) => x > 1), Iterator.from([1, 2]).all((x) => x > 0)], [true, true, true]],
  ["countBy", () => Iterator.from([1, 2, 3]).countBy((x) => (x % 2 ? "odd" : "even")), { odd: 2, even: 1 }],
  ["indexBy", () => Iterator.from([1, 2]).indexBy(String), { "1": 1, "2": 2 }],
  ["groupBy", () => Iterator.from([1, 2, 3]).groupBy((x) => (x % 2 ? "odd" : "even")), { odd: [1, 3], even: [2] }],
  // DOC-TENSION resolved: d.ts listed reduceBy(key, fn, seed), but API.md:16034
  // ("reduceBy(valueFn, acc, keyFn)"), the engine's own tests
  // (test_iterator_lazy.js:77) and the implementation pin (fn, seed, key);
  // d.ts corrected to match.
  ["reduceBy", () => Iterator.from([{ t: "a", v: 1 }, { t: "b", v: 2 }, { t: "a", v: 3 }]).reduceBy((acc, o) => acc + o.v, 0, (o) => o.t), { a: 4, b: 2 }],
  ["forEach", () => { const seen = []; Iterator.from([1, 2]).forEach((v) => seen.push(v)); return seen; }, [1, 2]],
  ["[1,2].values() carries the helpers (doc's route onto Iterator.prototype)", () => [1, 2].values().map((x) => x * 2).toArray(), [2, 4]],
]);

// ---------------------------------------------------------------------------
// Float16Array (doc lines 7182-7227): standard %TypedArray%, binary16.
// Exact values pinned are exactly representable in binary16.
// ---------------------------------------------------------------------------
{
  const F16 = (a) => new Float16Array(a);
  const AF = (f) => Array.from(f);
  runRows([
    ["Float16Array.BYTES_PER_ELEMENT === 2", () => Float16Array.BYTES_PER_ELEMENT, 2],
    ["new Float16Array(3): length 3, byteLength 6", () => { const f = new Float16Array(3); return f.length === 3 && f.byteLength === 6; }, true],
    ["exact binary16 values 0.5/1/2/-2 round-trip", () => AF(F16([0.5, 1, 2, -2])), [0.5, 1, 2, -2]],
    ["65504 is the max finite binary16", () => F16([65504])[0], 65504],
    ["overflow rounds to Infinity", () => F16([70000])[0], Infinity],
    ["negative overflow rounds to -Infinity", () => F16([-70000])[0], -Infinity],
    ["set(array, offset)", () => { const f = F16([1, 2, 3]); f.set([9], 1); return AF(f); }, [1, 9, 3]],
    ["subarray view", () => AF(F16([1, 2, 3]).subarray(1)), [2, 3]],
    ["subarray shares the underlying buffer (typed-array view semantics)", () => { const f = F16([1, 2, 3]); return f.subarray(1).buffer === f.buffer; }, true],
    ["sort defaults to numeric ascending (typed-array semantics)", () => { const f = F16([3, 1, 2]); f.sort(); return AF(f); }, [1, 2, 3]],
    ["join", () => F16([1, 2]).join("+"), "1+2"],
    ["map returns a new Float16Array", () => AF(F16([1, 2]).map((v) => v * 2)), [2, 4]],
    ["at(-1) and at(out of range)", () => [F16([1, 2]).at(-1), F16([1, 2]).at(5)], [2, undefined]],
    ["includes / indexOf", () => [F16([1, 2]).includes(2), F16([1, 2]).indexOf(2)], [true, 1]],
    ["Float16Array.of", () => { const f = Float16Array.of(1, 2); return f.length === 2 && f[0] === 1 && f[1] === 2; }, true],
    ["Float16Array.from with mapFn", () => AF(Float16Array.from([1, 2], (v) => v * 2)), [2, 4]],
  ]);
}

// ---------------------------------------------------------------------------
// SuppressedError / InternalError (doc lines 7229-7282).
// ---------------------------------------------------------------------------
runRows([
  // DOC-TENSION resolved: d.ts listed new SuppressedError(message, error,
  // suppressed), but TC39 explicit resource management (and the engine's own
  // test_disposable.js:46) pin (error, suppressed, message); d.ts corrected.
  ["SuppressedError carries .error and .suppressed", () => { const e = new SuppressedError("E", "S", "m"); return [e.error, e.suppressed]; }, ["E", "S"]],
  ["SuppressedError is an Error with the given message", () => { const e = new SuppressedError("E", "S", "m"); return e instanceof Error && e instanceof SuppressedError && e.message === "m"; }, true],
  ["SuppressedError fields optional", () => { const e = new SuppressedError(); return e.error === undefined && e.suppressed === undefined; }, true],
  ["InternalError constructible with name 'InternalError'", () => { const e = new InternalError("boom"); return [e.name, e.message]; }, ["InternalError", "boom"]],
  ["InternalError instanceof Error and InternalError", () => { const e = new InternalError("x"); return e instanceof Error && e instanceof InternalError; }, true],
]);

// ---------------------------------------------------------------------------
// DisposableStack (doc lines 7239-7256): LIFO disposal, use/adopt/defer/move.
// ---------------------------------------------------------------------------
runRows([
  // DOC-TENSION resolved: d.ts's use() comment ("its dispose()/close() (when
  // present)") reads as duck-typing, but TC39 and the engine (see
  // test_disposable.js: use({}) throws, use({[Symbol.dispose](){}}) works,
  // null/undefined pass through) require [Symbol.dispose]; d.ts corrected.
  ["LIFO dispose order: use/adopt/defer run in reverse registration order", () => {
    const order = [];
    const d = new DisposableStack();
    d.use({ [Symbol.dispose]: () => order.push("use") });
    d.adopt("A", (v) => order.push("adopt:" + v));
    d.defer(() => order.push("defer"));
    d.dispose();
    return order;
  }, ["defer", "adopt:A", "use"]],
  ["use returns the registered value", () => { const v = { [Symbol.dispose]() {} }; return new DisposableStack().use(v) === v; }, true],
  ["adopt returns the adopted value", () => new DisposableStack().adopt(7, () => {}), 7],
  // DOC-TENSION resolved: d.ts typed defer() as returning `this`, but TC39
  // explicit resource management (and the engine's own test_disposable.js,
  // assert(stack.defer(...), undefined)) pin undefined; d.ts corrected.
  ["defer returns undefined (TC39; was mis-typped `this` in d.ts)", () => new DisposableStack().defer(() => {}), undefined],
  ["disposed flag flips on dispose()", () => { const d = new DisposableStack(); const before = d.disposed; d.dispose(); return before === false && d.disposed === true; }, true],
  ["Symbol.dispose triggers disposal", () => { const d = new DisposableStack(); d[Symbol.dispose](); return d.disposed; }, true],
  ["move(): source becomes disposed, the moved stack carries the callbacks", () => {
    const order = [];
    const d = new DisposableStack();
    d.defer(() => order.push("cb"));
    const m = d.move();
    const srcDisposed = d.disposed === true && m.disposed === false;
    m.dispose();
    return srcDisposed && m.disposed === true && order.join(",") === "cb";
  }, true],
]);

// ---------------------------------------------------------------------------
// AsyncDisposableStack (doc lines 7258-7272): await disposeAsync().
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["LIFO async dispose order (defer callbacks run in reverse)", async () => {
    const order = [];
    const s = new AsyncDisposableStack();
    s.defer(async () => { await Promise.resolve(); order.push("d1"); });
    s.adopt("A", (v) => { order.push("adopt:" + v); });
    s.defer(() => { order.push("d2"); });
    await s.disposeAsync();
    return order;
  }, ["d2", "adopt:A", "d1"]],
  ["disposed flag after disposeAsync", async () => { const s = new AsyncDisposableStack(); await s.disposeAsync(); return s.disposed; }, true],
  ["Symbol.asyncDispose returns a promise and disposes", async () => { const s = new AsyncDisposableStack(); const p = s[Symbol.asyncDispose](); await p; return p instanceof Promise && s.disposed; }, true],
  ["move(): source disposed, moved stack runs the callbacks", async () => {
    const order = [];
    const s = new AsyncDisposableStack();
    s.defer(() => order.push("cb"));
    const m = s.move();
    const srcDisposed = s.disposed === true && m.disposed === false;
    await m.disposeAsync();
    return srcDisposed && order.join(",") === "cb";
  }, true],
]);

// ---------------------------------------------------------------------------
// Lens (doc lines 7284-7321): immutable optics; view/set/over never touch the
// source ("All updates are IMMUTABLE").
// ---------------------------------------------------------------------------
runRows([
  ["Lens.prop view", () => Lens.prop("a").view({ a: 1 }), 1],
  ["Lens.prop view absent -> undefined (doc: undefined when absent)", () => Lens.prop("a").view({}), undefined],
  ["Lens.prop set -> copy with focus set; source untouched", () => {
    const s = { a: 1, b: 2 };
    const out = Lens.prop("a").set(9, s);
    return out !== s && out.a === 9 && out.b === 2 && s.a === 1;
  }, true],
  ["Lens.prop over transforms at focus, immutably", () => {
    const s = { a: 1 };
    const out = Lens.prop("a").over((x) => x * 10, s);
    return out.a === 10 && s.a === 1 && out !== s;
  }, true],
  ["Lens.index view", () => Lens.index(1).view([1, 2, 3]), 2],
  ["Lens.index set preserves the array shape, source untouched", () => {
    const a = [1, 2, 3];
    const out = Lens.index(1).set(9, a);
    return JSON.stringify(out) === "[1,9,3]" && a[1] === 2;
  }, true],
  ["Lens.index over", () => Lens.index(0).over((x) => x + 1, [1, 2]), (r) => JSON.stringify(r) === "[2,2]"],
  ["Lens.path dotted view", () => Lens.path("a.b.c").view({ a: { b: { c: 7 } } }), 7],
  ["Lens.path array-form view", () => Lens.path(["a", "b"]).view({ a: { b: 5 } }), 5],
  ["Lens.path view through a missing segment -> undefined", () => Lens.path("x.y").view({ a: 1 }), undefined],
  ["Lens.path set builds a nested copy; inner objects not shared with source", () => {
    const s = { a: { b: 1 } };
    const out = Lens.path("a.b").set(2, s);
    return out !== s && out.a !== s.a && out.a.b === 2 && s.a.b === 1;
  }, true],
  ["custom lens via new: view+set+over", () => {
    const l = new Lens((s) => s.n, (v, s) => ({ ...s, n: v }));
    const s = { n: 3 };
    return l.view(s) === 3 && l.set(4, s).n === 4 && s.n === 3 && l.over((x) => x + 1, s).n === 4;
  }, true],
  ["custom lens WITHOUT new works the same (doc: works with or without new)", () => {
    const l = Lens((s) => s.n, (v, s) => ({ ...s, n: v }));
    return l.view({ n: 3 }) === 3 && l.set(4, { n: 3 }).n === 4;
  }, true],
  ["static Lens.view / Lens.set / Lens.over", () => {
    const l = Lens.prop("a");
    const s = { a: 1 };
    const v = Lens.view(l, s);
    const st = Lens.set(l, 2, s);
    const ov = Lens.over(l, (x) => x + 100, s);
    return v === 1 && st.a === 2 && ov.a === 101 && s.a === 1;
  }, true],
]);

// ---------------------------------------------------------------------------
// structuredClone (doc lines 7334-7340): type coverage table.
// ---------------------------------------------------------------------------
runRows([
  ["number", () => structuredClone(42), 42],
  ["string", () => structuredClone("s"), "s"],
  ["boolean", () => structuredClone(true), true],
  ["null", () => structuredClone(null), null],
  ["undefined", () => structuredClone(undefined), undefined],
  ["BigInt", () => structuredClone(123n), 123n],
  ["Date: fresh object, same time", () => { const d = new Date(1000); const c = structuredClone(d); return c !== d && c instanceof Date && c.getTime() === 1000; }, true],
  ["Map: deep copy, not the same reference", () => { const m = new Map([["a", 1]]); const c = structuredClone(m); return c !== m && c instanceof Map && c.get("a") === 1; }, true],
  ["Set: deep copy", () => { const s = new Set([1, 2]); const c = structuredClone(s); return c !== s && c instanceof Set && c.has(1) && c.has(2) && c.size === 2; }, true],
  ["RegExp: same source/flags, not the same reference", () => { const r = /ab+c/gi; const c = structuredClone(r); return c !== r && c.source === "ab+c" && c.flags === "gi"; }, true],
  ["typed array: fresh buffer, same bytes (doc: fresh buffer, same bytes)", () => {
    const a = U8([1, 2, 3]);
    const c = structuredClone(a);
    return c !== a && c.buffer !== a.buffer && JSON.stringify(Array.from(c)) === "[1,2,3]";
  }, true],
  ["nested objects are deep-copied", () => { const o = { inner: { x: 1 } }; const c = structuredClone(o); return c !== o && c.inner !== o.inner && c.inner.x === 1; }, true],
  ["cycles point into the copy (doc: the copy's cycle points into the copy)", () => { const o = { v: 1 }; o.self = o; const c = structuredClone(o); return c !== o && c.self === c && c.v === 1; }, true],
  ["functions cannot be cloned and throw (doc)", () => structuredClone(() => 1), { throws: null }],
]);

// ---------------------------------------------------------------------------
// sleep / timers / queueMicrotask / performance / console / print / scriptArgs
// (doc lines 7066-7132 and 7328-7332). Timing rows use generous slack; the
// NaN/negative/absent -> 0 contract is asserted as FAST RESOLVE, never a long
// sleep (doc: "NaN/negative/absent count as 0").
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["sleep() (absent) counts as 0 -> resolves fast", async () => { const t0 = performance.now(); await sleep(); return performance.now() - t0; }, (ms) => ms < 100],
  ["sleep(-5) counts as 0 -> resolves fast", async () => { const t0 = performance.now(); await sleep(-5); return performance.now() - t0; }, (ms) => ms < 100],
  ["sleep(NaN) counts as 0 -> resolves fast", async () => { const t0 = performance.now(); await sleep(NaN); return performance.now() - t0; }, (ms) => ms < 100],
  ["sleep(5) waits at least ~5ms", async () => { const t0 = performance.now(); await sleep(5); return performance.now() - t0; }, (ms) => ms >= 4 && ms < 200],
  ["setTimeout returns a numeric id (doc: return a numeric id)", () => { const id = setTimeout(() => {}, 50); clearTimeout(id); return typeof id; }, "number"],
  ["clearTimeout cancels: the callback never runs", async () => { let ran = false; const id = setTimeout(() => { ran = true; }, 5); clearTimeout(id); await sleep(20); return ran; }, false],
  ["queueMicrotask runs before the setTimeout(0) macrotask (doc: before the next macrotask)", async () => { const order = []; setTimeout(() => order.push("timeout"), 0); queueMicrotask(() => order.push("micro")); await sleep(10); return order; }, ["micro", "timeout"]],
  ["setInterval ticks repeatedly until clearInterval", async () => { let ticks = 0; const id = setInterval(() => { ticks++; }, 5); await sleep(24); clearInterval(id); const atClear = ticks; await sleep(15); return atClear >= 2 && ticks === atClear; }, true],
  ["performance.now is monotonic across two calls", () => { const a = performance.now(); const b = performance.now(); return typeof a === "number" && typeof b === "number" && b >= a; }, true],
  ["AbortSignal.timeout(delayMs) aborts after the delay", async () => { const s = AbortSignal.timeout(5); const early = s.aborted; await sleep(25); return early === false && s.aborted === true; }, true],
  ["console exposes all 7 documented methods (log info debug trace warn error assert)", () => ["log", "info", "debug", "trace", "warn", "error", "assert"].every((k) => typeof console[k] === "function"), true],
  ["print is a function (doc: prints values to stdout)", () => typeof print, "function"],
  ["scriptArgs is an array of strings (doc: the engine's argument vector)", () => Array.isArray(scriptArgs) && scriptArgs.every((a) => typeof a === "string"), true],
]);

// ---------------------------------------------------------------------------
// AbortController / AbortSignal (doc lines 6910-6935).
// ---------------------------------------------------------------------------
runRows([
  ["fresh controller: signal.aborted === false", () => new AbortController().signal.aborted, false],
  ["abort() sets signal.aborted", () => { const ac = new AbortController(); ac.abort(); return ac.signal.aborted; }, true],
  ["abort(reason) exposes signal.reason", () => { const ac = new AbortController(); ac.abort("boom"); return ac.signal.reason; }, "boom"],
  ["abort event fires once with ev.type 'abort' and ev.target === signal (repeat aborts do not re-fire)", () => {
    const ac = new AbortController();
    let calls = 0; let ev = null;
    ac.signal.addEventListener("abort", (e) => { calls++; ev = e; });
    ac.abort(); ac.abort();
    return calls === 1 && ev.type === "abort" && ev.target === ac.signal;
  }, true],
  ["removeEventListener prevents the abort callback", () => {
    const ac = new AbortController();
    const fn = () => { calls++; };
    let calls = 0;
    ac.signal.addEventListener("abort", fn);
    ac.signal.removeEventListener("abort", fn);
    ac.abort();
    return calls;
  }, 0],
  ["onabort handler is invoked", () => { let called = false; const ac = new AbortController(); ac.signal.onabort = () => { called = true; }; ac.abort(); return called; }, true],
  ["throwIfAborted no-ops on a live signal", () => { new AbortController().signal.throwIfAborted(); return "ok"; }, "ok"],
  ["throwIfAborted throws on an aborted signal", () => { const ac = new AbortController(); ac.abort(); ac.signal.throwIfAborted(); }, { throws: null }],
  ["AbortSignal.abort(reason) is pre-aborted with the reason", () => { const s = AbortSignal.abort("r"); return s.aborted === true && s.reason === "r"; }, true],
]);

// ---------------------------------------------------------------------------
// Headers (doc lines 6937-6953). dynajs.d.ts declares the surface; the
// case-insensitivity/lowercasing and duplicate-combining-with-", " semantics
// are the WHATWG fetch Headers contract the globals parity pack implements
// (cited per WHATWG Fetch spec, "Headers" section).
// ---------------------------------------------------------------------------
runRows([
  ["Record init; get is case-insensitive", () => new Headers({ "X-Test": "1" }).get("x-test"), "1"],
  ["get lowercases the queried name", () => new Headers({ "x-test": "1" }).get("X-TEST"), "1"],
  ["has true", () => new Headers({ a: "1" }).has("a"), true],
  ["has false when absent", () => new Headers({ a: "1" }).has("b"), false],
  ["get absent -> null", () => new Headers().get("a"), null],
  ["array init; entries iterate name/value pairs", () => Array.from(new Headers([["a", "1"], ["b", "2"]]).entries()), [["a", "1"], ["b", "2"]]],
  ["keys() iteration", () => Array.from(new Headers([["a", "1"], ["b", "2"]]).keys()), ["a", "b"]],
  ["values() iteration", () => Array.from(new Headers([["a", "1"], ["b", "2"]]).values()), ["1", "2"]],
  ["Symbol.iterator matches entries", () => Array.from(new Headers([["a", "1"]])[Symbol.iterator]()), [["a", "1"]]],
  ["WHATWG: duplicate append combines values with ', '", () => { const h = new Headers(); h.append("x", "1"); h.append("x", "2"); return h.get("x"); }, "1, 2"],
  ["set replaces all previous values", () => { const h = new Headers(); h.append("x", "1"); h.append("x", "2"); h.set("x", "3"); return h.get("x"); }, "3"],
  ["delete removes the field", () => { const h = new Headers({ a: "1" }); h.delete("a"); return h.get("a"); }, null],
  ["forEach walks (value, key, parent)", () => { const seen = []; new Headers([["a", "1"], ["b", "2"]]).forEach((v, k, p) => seen.push(k + ":" + v + ":" + (p instanceof Headers))); return seen; }, ["a:1:true", "b:2:true"]],
  ["Headers init from a Headers copies", () => { const h = new Headers({ a: "1" }); return new Headers(h).get("a"); }, "1"],
]);

// ---------------------------------------------------------------------------
// FormData (doc lines 6955-6971).
// ---------------------------------------------------------------------------
runRows([
  ["append + get", () => { const f = new FormData(); f.append("k", "v"); return f.get("k"); }, "v"],
  ["has true / false", () => { const f = new FormData(); f.append("k", "v"); return [f.has("k"), f.has("j")]; }, [true, false]],
  ["append twice -> getAll returns both in order", () => { const f = new FormData(); f.append("k", "v1"); f.append("k", "v2"); return f.getAll("k"); }, ["v1", "v2"]],
  ["set replaces previous entries", () => { const f = new FormData(); f.append("k", "v1"); f.append("k", "v2"); f.set("k", "v3"); return f.getAll("k"); }, ["v3"]],
  ["delete removes", () => { const f = new FormData(); f.append("k", "v"); f.delete("k"); return [f.get("k"), f.has("k")]; }, [null, false]],
  ["get absent -> null", () => new FormData().get("k"), null],
  ["Uint8Array value round-trips byte-identically", () => {
    const f = new FormData();
    f.append("b", U8([1, 2, 3]));
    const g = f.get("b");
    return g instanceof Uint8Array && Array.from(g).join(",") === "1,2,3";
  }, true],
  ["keys/values/entries iteration", () => {
    const f = new FormData();
    f.append("a", "1"); f.append("b", "2");
    return [Array.from(f.keys()), Array.from(f.values()), Array.from(f.entries())];
  }, [["a", "b"], ["1", "2"], [["a", "1"], ["b", "2"]]]],
  ["forEach walks (value, key, parent)", () => { const seen = []; const f = new FormData(); f.append("a", "1"); f.forEach((v, k, p) => seen.push(k + ":" + v + ":" + (p === f))); return seen; }, ["a:1:true"]],
]);

// ---------------------------------------------------------------------------
// Request / Response on CONSTRUCTED objects only -- no network anywhere.
// (doc lines 6976-7024; statusText default "" and 2xx ok are WHATWG Response
// semantics cited per the WHATWG Fetch spec.)
// ---------------------------------------------------------------------------
await runRowsAsync([
  ["Request default method GET", () => new Request("https://example.com/x").method, "GET"],
  ["Request url preserved", () => new Request("https://example.com/x").url, "https://example.com/x"],
  ["Request init headers", () => new Request("https://example.com/", { headers: { "x-a": "1" } }).headers.get("x-a"), "1"],
  ["Request.signal is null when init.signal is absent", () => new Request("https://example.com/").signal, null],
  ["Request.signal is the passed signal (same object)", () => { const ac = new AbortController(); return new Request("https://example.com/", { signal: ac.signal }).signal === ac.signal; }, true],
  ["Request.text() reads the constructed POST body", () => new Request("https://example.com/", { method: "POST", body: "payload" }).text(), "payload"],
  ["Request.json() parses the constructed body", () => new Request("https://example.com/", { method: "POST", body: '{"b":2}' }).json(), { b: 2 }],
  ["Response defaults: status 200, ok true", () => { const r = new Response("x"); return [r.status, r.ok]; }, [200, true]],
  ["Response 201 is ok (WHATWG: 2xx)", () => new Response("x", { status: 201 }).ok, true],
  ["Response 404 is not ok", () => new Response("x", { status: 404 }).ok, false],
  // Project convention pins the default "OK" (API.md:110 "statusText — default \"OK\""),
  // diverging from WHATWG's "" on purpose; the d.ts now documents it.
  ["Response statusText defaults to 'OK' (API.md, not WHATWG '')", () => new Response("x").statusText, "OK"],
  ["Response statusText from init", () => new Response("x", { status: 201, statusText: "Created" }).statusText, "Created"],
  ["Response headers from init", () => new Response("x", { headers: { "x-h": "v" } }).headers.get("x-h"), "v"],
  ["Response url from init", () => new Response("x", { url: "https://y/" }).url, "https://y/"],
  ["bodyUsed flips false -> true after text()", async () => { const r = new Response("abc"); const before = r.bodyUsed; await r.text(); return before === false && r.bodyUsed === true; }, true],
  ["Response.text() round-trips the body", () => new Response("abc").text(), "abc"],
  ["Response.json() round-trips JSON", () => new Response('{"a":[1,2]}').json(), { a: [1, 2] }],
  ["Response.bytes() returns the body bytes", () => new Response("abc").bytes().then((b) => Array.from(b)), [97, 98, 99]],
  ["Response.arrayBuffer() byteLength", () => new Response("ab").arrayBuffer().then((b) => b.byteLength), 2],
  ["clone(): both bodies readable with the same text", async () => { const r = new Response("abc"); const c = r.clone(); return (await c.text()) === "abc" && (await r.text()) === "abc"; }, true],
]);

print("bb_globals: all tests passed (" + n + " assertions)");
