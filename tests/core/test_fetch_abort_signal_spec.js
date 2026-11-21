// covers: c:src/modules/libc/libc-fetch.c
// AbortSignal / AbortController against the WHATWG DOM spec ("Interface AbortSignal", "signal abort", "create a dependent abort
// signal", "add an event listener"). The WPT dom/abort directory is not in third_party/wpt, so this suite is the oracle; every
// case names the spec step it checks. Default reasons are DOMException
// instances named "AbortError" / "TimeoutError" (DOMException lives in libc-domex.c). Deviations kept (no EventTarget/Event
// globals in this runtime): the event is a plain { type, target, currentTarget } object and only "abort" listeners are stored. Dependent signals hold strong references to
// their sources' dependents list (the spec uses weak references); only GC timing differs.
import { suite } from "../lib/kit.js";

const t = suite("test_fetch_abort_signal_spec");

t.test("AbortSignal has no public constructor (spec: [Exposed] interface without a constructor)", (a) => {
    let e = null;
    try { new AbortSignal(); } catch (x) { e = x; }
    a.ok(e instanceof TypeError);
    a.eq(typeof AbortSignal.prototype._abort, "undefined");
});

t.test("add an event listener: duplicates (same callback, same capture) are ignored, null is ignored", (a) => {
    const c = new AbortController(), out = [];
    const f = () => out.push("f");
    c.signal.addEventListener("abort", f);
    c.signal.addEventListener("abort", f);
    c.signal.addEventListener("abort", f, { capture: true });
    c.signal.addEventListener("abort", null);
    c.abort();
    a.eq(out.join(","), "f,f");
});

t.test("once listeners are removed before they run; handleEvent objects are called with this = the object", (a) => {
    const c = new AbortController(), out = [];
    const o = { handleEvent(e) { out.push(this === o ? "obj:" + e.type : "bad"); } };
    c.signal.addEventListener("abort", () => out.push("once"), { once: true });
    c.signal.addEventListener("abort", o);
    c.abort();
    a.eq(out.join(","), "once,obj:abort");
});

t.test("onabort is an event handler that runs in listener order of first assignment", (a) => {
    const c = new AbortController(), out = [];
    c.signal.addEventListener("abort", () => out.push("1"));
    c.signal.onabort = () => out.push("on");
    c.signal.addEventListener("abort", () => out.push("2"));
    c.signal.onabort = () => out.push("on2");
    c.abort();
    a.eq(out.join(","), "1,on2,2");
    a.eq(Object.keys(c.signal).length, 0);
});

t.test("a listener removed during dispatch is not called; options.signal removes the listener", (a) => {
    const c = new AbortController(), out = [], rm = new AbortController();
    const g = () => out.push("g");
    c.signal.addEventListener("abort", () => { out.push("f"); c.signal.removeEventListener("abort", g); });
    c.signal.addEventListener("abort", g);
    c.signal.addEventListener("abort", () => out.push("h"), { signal: rm.signal });
    rm.abort();
    c.abort();
    a.eq(out.join(","), "f");
});

t.test("signal abort: default reason is an AbortError; abort fires once; a late listener is not called", async (a) => {
    const c = new AbortController(), out = [];
    c.signal.addEventListener("abort", () => out.push("x"));
    c.abort();
    c.abort("again");
    a.eq(c.signal.reason.name, "AbortError");
    a.ok(c.signal.reason instanceof Error);
    a.ok(c.signal.reason instanceof DOMException);
    a.eq(c.signal.reason.code, 20);
    c.signal.addEventListener("abort", () => out.push("late"));
    await null;
    a.eq(out.join(","), "x");
    let e = null;
    try { c.signal.throwIfAborted(); } catch (x) { e = x; }
    a.eq(e, c.signal.reason);
});

t.test("AbortSignal.any: dependents abort after the source's listeners, with the source's reason", (a) => {
    const c = new AbortController(), out = [];
    const d = AbortSignal.any([c.signal]);
    d.addEventListener("abort", () => out.push("dep:" + d.reason));
    c.signal.addEventListener("abort", () => out.push("src:" + d.aborted));
    c.abort("r");
    a.eq(out.join(","), "src:true,dep:r");
    a.eq(AbortSignal.any([AbortSignal.abort("pre")]).reason, "pre");
    let e = null;
    try { AbortSignal.any([{}]); } catch (x) { e = x; }
    a.ok(e instanceof TypeError);
});

t.test("AbortSignal.timeout aborts with a TimeoutError; negative delays are a TypeError", async (a) => {
    const s = AbortSignal.timeout(1);
    await new Promise((r) => s.addEventListener("abort", r));
    a.eq(s.reason.name, "TimeoutError");
    a.ok(s.reason instanceof DOMException);
    a.eq(s.reason.code, 23);
    let e = null;
    try { AbortSignal.timeout(-1); } catch (x) { e = x; }
    a.ok(e instanceof TypeError);
});

t.test("Request.signal is a new signal that follows init.signal (Fetch: Request constructor step 'signal')", (a) => {
    const c = new AbortController();
    const r = new Request("http://h/", { signal: c.signal });
    a.ok(r.signal !== c.signal);
    a.eq(r.signal.aborted, false);
    c.abort("why");
    a.eq(r.signal.reason, "why");
    a.ok(new Request("http://h/").signal instanceof AbortSignal);
});

await t.run();
