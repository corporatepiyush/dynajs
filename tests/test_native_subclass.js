/* test_native_subclass.js -- T-RANDOM-1: native-module classes are subclassable.
 *
 * `class R extends Random {}` used to produce objects whose prototype was
 * Random.prototype, because every native constructor ignored new_target and
 * wrapped with the class's own registered proto. The fix is central: the
 * wrap helpers (dyn_plain_wrap / dyn_res_wrap in src/dyna-nat.c) resolve the
 * final prototype from Get(new_target, "prototype") -- OrdinaryCreateFrom-
 * Constructor, the same rule the engine's builtins follow
 * (js_create_from_ctor) -- with the class's own proto as the non-object and
 * no-construction fallback, and a throwing "prototype" getter propagating.
 *
 * Battery covers, per class family: instanceof, prototype identity, method
 * resolution through the subclass, DynResource close()/closed through the
 * subclass, method-created results staying BASE instances (spec: these are
 * not under [[Construct]]), plain `new C()` unchanged, Ctor.call() still
 * refusing, Reflect.construct in both forms, the non-object-prototype
 * fallback, and the throwing-getter propagation.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_native_subclass.js
 * Prints "test_native_subclass: all N tests passed"; throws on failure.
 */

import { Random } from "dyna:random";
import { Decimal, Money } from "dyna:decimal";
import { Hasher } from "dyna:hash";
import { Hmac, AESGCM, ChaCha20Poly1305 } from "dyna:crypto";
import { PlainDate, Duration } from "dyna:time";
import { URL, URLSearchParams } from "dyna:url";
import { Deque } from "dyna:structures";
import { Path, File, FileReader, FileWriter } from "dyna:file";
import { KMeans } from "dyna:ml";
import { Bytes, Text } from "dyna:bytes";
import { Robots, Extractor, Fetcher, Crawl } from "dyna:scrape";
import { Selector, HTMLParse, HTMLText } from "dyna:html";
import { HTTPClient } from "dyna:net";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

function assertThrows(fn, msg) {
    n++;
    let threw = false;
    try { fn(); } catch (e) { threw = true; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
}

/* One shared scenario set over a ctor/class pair. */
function family(C, name, build) {
    const D = class extends C {};

    // plain construction unchanged
    const base = build(C);
    assert(base instanceof C, name + ": plain instanceof");
    assert(!(base instanceof D), name + ": plain NOT instanceof subclass");

    // subclass construction: the fix
    const sub = build(D);
    assert(sub instanceof D, name + ": subclass instanceof subclass");
    assert(sub instanceof C, name + ": subclass instanceof base");
    assert(Object.getPrototypeOf(sub) === D.prototype,
        name + ": subclass proto is D.prototype");
    assert(Object.getPrototypeOf(base) === C.prototype,
        name + ": plain proto is C.prototype");

    // static side untouched
    assert(Object.getPrototypeOf(D) === C,
        name + ": subclass static proto is the base ctor");

    // call without new still refused (unchanged)
    assertThrows(() => C.call(), name + ": C.call() without new throws");
    assertThrows(() => D.call(), name + ": D.call() without new throws");
    return D;
}

/* ---------- Random (plain class, deterministic through the subclass) ----- */
{
    const D = family(Random, "Random", (C) => new C(42));
    assert(new D(42).nextU53() === new Random(42).nextU53(),
        "Random: subclass instance follows the same seeded stream");
    assert(new Random(7).nextU53() !== new Random(42).nextU53(),
        "Random: streams differ per seed (sanity)");

    // Reflect.construct both forms
    n++;
    const r1 = Reflect.construct(Random, [42]);
    assert(r1 instanceof Random, "Random: Reflect.construct base form");
    const r2 = Reflect.construct(Random, [42], D);
    assert(r2 instanceof D && r2 instanceof Random,
        "Random: Reflect.construct with subclass new.target");

    // non-object prototype falls back to the class's own proto (spec)
    function F42() {}
    F42.prototype = 42;
    const r3 = Reflect.construct(Random, [42], F42);
    assert(r3 instanceof Random,
        "Random: non-object F.prototype falls back to class proto");

    // a throwing "prototype" getter propagates (and the native is disposed
    // by the wrapper -- a sanitizer run proves the free, here we prove the
    // exception and that the engine survives a loop of them). A Proxy is the
    // portable way to make Get(new_target, "prototype") throw: a function's
    // own "prototype" is non-configurable, so it cannot be redefined as a
    // getter.
    const Fboom = new Proxy(function () {}, {
        get(target, key) {
            if (key === "prototype") throw new RangeError("boom");
            return Reflect.get(target, key);
        },
    });
    assertThrows(() => Reflect.construct(Random, [42], Fboom),
        "Random: throwing prototype getter propagates");
    for (let i = 0; i < 50; i++) {
        try { Reflect.construct(Random, [i], Fboom); } catch (e) { /* ok */ }
    }

    // subclass of subclass
    class D2 extends D {}
    const r4 = new D2(42);
    assert(r4 instanceof D2 && r4 instanceof D && r4 instanceof Random,
        "Random: subclass of subclass full chain");
}

/* ---------- Decimal + Money (plain classes, real arithmetic) ------------- */
{
    const D = family(Decimal, "Decimal", (C) => new C("0.1"));
    const d = new D("0.1");
    const sum = d.add(1);
    assert(sum.toString() === "1.1", "Decimal: method works through subclass");
    assert(sum instanceof Decimal && !(sum instanceof D),
        "Decimal: arithmetic result stays a base instance");

    const M = family(Money, "Money", (C) => new C(199, "USD"));
    const m = new M(199, "USD");
    assert(m.format() === "$1.99", "Money: format through subclass");
    assert(m.add(new Money(1, "USD")) instanceof Money &&
        !(m.add(new Money(1, "USD")) instanceof M),
        "Money: add result stays a base instance");
    assertThrows(() => new Money(199, "us"), "Money: currency code still validated");
}

/* ---------- Hasher (dyna:hash) ------------------------------------------- */
{
    const D = family(Hasher, "Hasher", (C) => new C("sha256"));
    const h = new D("sha256");
    h.update("abc");
    assert(h.digestHex() === "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "Hasher: digest through subclass matches the FIPS vector");
}

/* ---------- Hmac (DynResource: close()/closed through the subclass) ------ */
{
    const D = family(Hmac, "Hmac", (C) => new C("sha256", "key"));
    const hh = new D("sha256", "key");
    hh.update("abc");
    const tag = hh.digestHex();
    assert(tag.length === 64, "Hmac: digest through subclass");
    assert(hh.closed === false, "Hmac: open after construction");
    hh.close();
    assert(hh.closed === true, "Hmac: close() through the subclass");
    hh.dispose(); // idempotent second release through the subclass handle
    assert(hh.closed === true, "Hmac: still closed after dispose");
    assert(hh instanceof D, "Hmac: instanceof survives close");
    assertThrows(() => hh.update("x"), "Hmac: use after close throws");
}

/* ---------- AESGCM / ChaCha20Poly1305 (TLS builds; shared ctor helper) --- */
if (typeof AESGCM === "function" && typeof ChaCha20Poly1305 === "function") {
    const key = new Uint8Array(32).fill(9);
    const nonce = new Uint8Array(12).fill(3);
    for (const [C, nm] of [[AESGCM, "AESGCM"], [ChaCha20Poly1305, "ChaCha20Poly1305"]]) {
        const D = family(C, nm, (K) => new K(key));
        const a = new D(key);
        const sealed = a.seal(nonce, "attack at dawn");
        const opened = a.open(nonce, sealed);
        assert(new TextDecoder().decode(opened) === "attack at dawn",
            nm + ": roundtrip through the subclass");
        a.close();
        assert(a.closed === true, nm + ": close() through the subclass");
    }
}

/* ---------- PlainDate / Duration (dyna:time) ----------------------------- */
{
    const PD = family(PlainDate, "PlainDate", (C) => new C(2024, 5, 6));
    const pd = new PD(2024, 5, 6);
    assert(pd.year === 2024 && pd.day === 6, "PlainDate: getters through subclass");

    const DU = family(Duration, "Duration", (C) => new C({ months: 1, days: 2 }));
    const du = new DU({ months: 1, days: 2 });
    assert(du.days === 2, "Duration: getters through subclass");
}

/* ---------- URL (had its own inline fix; must keep working) -------------- */
{
    const D = family(URL, "URL", (C) => new C("https://dynascript.dev/a?b=1"));
    const u = new D("https://dynascript.dev/a?b=1");
    assert(u.hostname === "dynascript.dev", "URL: getters through subclass");
    assert(u.searchParams.get("b") === "1", "URL: searchParams through subclass");
    assert(u.searchParams instanceof URLSearchParams,
        "URL: searchParams is a base URLSearchParams");
}

/* ---------- Deque (dyna:structures) -------------------------------------- */
{
    const D = family(Deque, "Deque", (C) => new C());
    const q = new D();
    q.pushBack(1);
    q.pushFront(0);
    assert(q.popFront() === 0 && q.popBack() === 1,
        "Deque: push/pop through the subclass");
}

/* ---------- Path (dyna:file) --------------------------------------------- */
{
    const D = family(Path, "Path", (C) => new C("/tmp", "dyna-test"));
    const p = new D("/tmp", "dyna-test");
    assert(String(p).indexOf("dyna-test") >= 0, "Path: toString through subclass");
    assert(p.join("x") instanceof Path, "Path: derived paths stay base instances");
}

/* ---------- KMeans (dyna:ml, DynResource) -------------------------------- */
{
    const D = family(KMeans, "KMeans", (C) => new C(2, 7));
    const km = new D(2, 7);
    km.fit([[0, 0], [0, 1], [9, 9], [9, 8], [0.5, 0]]);
    const labels = km.predict([[0, 0.1], [9.2, 9]]);
    assert(labels.length === 2 && labels[0] !== labels[1],
        "KMeans: predict through the subclass separates clusters");
    km.close();
    assert(km.closed === true, "KMeans: close() through the subclass");
}

/* ---------- Bytes / Text (dyna:bytes, custom-finalizer plain classes) ----- */
{
    const D = family(Bytes, "Bytes", (C) => new C("abc"));
    const b = new D("abc");
    assert(b.toUtf8() === "abc", "Bytes: toUtf8 through the subclass");
    assert(b.length === 3 && b.isAscii === true,
        "Bytes: cached-summary getters through the subclass");
    assert(b.slice(0, 1).toUtf8() === "a", "Bytes: slice through the subclass");
    assert(Bytes.alloc(4).length === 4, "Bytes: alloc still works");
    assert(Object.getPrototypeOf(Bytes.alloc(4)) === Bytes.prototype,
        "Bytes: static alloc stays a base instance");

    const T = family(Text, "Text", (C) => new C("\u0101bc"));
    const t = new T("\u0101bc");
    assert(t.value === "\u0101bc", "Text: value through the subclass");
    assert(t.isWide === true, "Text: isWide scan ran through the subclass");
    assertThrows(() => new Text(), "Text: missing argument still refused");
}

/* ---------- File + FileReader (dyna:file; reader()/writer() results) ----- */
{
    const probePath = Path.temp().join("dyna-native-subclass-probe.txt");
    const probe = new File(probePath);
    probe.writeText("subclass");

    const D = family(File, "File", (C) => new C(probePath));
    const f = new D(probePath);
    assert(f.readText() === "subclass", "File: readText through the subclass");
    assert(f.path instanceof Path, "File: .path is a base Path");
    assert(f.exists() === true, "File: exists through the subclass");

    /* reader()/writer() are METHOD-created results: not under [[Construct]],
     * so they stay BASE FileReader/FileWriter instances even when `f` is a
     * File subclass (spec -- only construction distributes the subclass). */
    const w = f.writer();
    assert(Object.getPrototypeOf(w) === FileWriter.prototype,
        "File: writer() result is a base FileWriter");
    w.close();
    assert(w.closed === true, "FileWriter: close() on the method result");
    const rd = f.reader();
    assert(Object.getPrototypeOf(rd) === FileReader.prototype,
        "File: reader() result is a base FileReader");
    rd.close();

    /* FileReader itself is subclassable (its ctor routes new_target from the
     * first sweep; pinned here so a regression of either half shows up). */
    const RD = family(FileReader, "FileReader",
        (C) => new C(probePath));
    const r2 = new RD(probePath);
    assert(r2 instanceof RD && r2 instanceof FileReader,
        "FileReader: subclass instance reads the same file");
    r2.close();
    assert(r2.closed === true, "FileReader: close() through the subclass");

    assertThrows(() => new File(42),
        "File: non-Path/string path still refused");
    probe.remove();
}

/* ---------- Robots (dyna:scrape, DynResource) ---------------------------- */
{
    const txt = "User-agent: *\nDisallow: /private/\nCrawl-delay: 5\n";
    const D = family(Robots, "Robots", (C) => new C(txt));
    const rb = new D(txt);
    assert(rb.allows("/public") === true,
        "Robots: allows() through the subclass");
    assert(rb.allows("/private/x") === false,
        "Robots: Disallow honored through the subclass");
    assert(rb.crawlDelay() === 5 && rb.ruleCount === 1,
        "Robots: delay/rules through the subclass");
    rb.close();
    assert(rb.closed === true, "Robots: close() through the subclass");
    assertThrows(() => rb.allows("/x"), "Robots: use after close throws");
}

/* ---------- Extractor (dyna:scrape, plain class, run() through subclass) - */
{
    const mk = (C) => new C({ title: { sel: new Selector("h1") } },
                            { text: HTMLText });
    const D = family(Extractor, "Extractor", mk);
    const ex = mk(D);
    const doc = HTMLParse("<html><body><h1>Hi</h1></body></html>");
    const out = ex.run(doc);
    assert(out.ok === true && out.value.title === "Hi" && out.missing.length === 0,
        "Extractor: run() through the subclass extracts");
    assertThrows(() => new Extractor({ title: "not an object" }),
        "Extractor: field spec still validated");
}

/* ---------- Fetcher + Crawl (dyna:scrape, DynResource; nt was dropped) ---- */
{
    const mk = (C) => new C({ agent: "dyna-subclass-test/1.0",
                              client: new HTTPClient() });
    const D = family(Fetcher, "Fetcher", mk);
    const fe = mk(D);
    assert(fe.closed === false, "Fetcher: open after construction");
    const st = fe.stats();
    assert(st.fetched === 0, "Fetcher: stats through the subclass");
    fe.close();
    assert(fe.closed === true, "Fetcher: close() through the subclass");
    assertThrows(() => fe.get("http://example.test/"),
        "Fetcher: use after close throws");
    assertThrows(() => new Fetcher({ agent: "x/1" }),
        "Fetcher: client still required");

    /* Crawl already routed new_target (first sweep); pinned so it stays. */
    const CR = class extends Crawl {};
    const cr = new CR({ agent: "dyna-subclass-test/1.0",
                        client: new HTTPClient() });
    assert(cr instanceof CR && cr instanceof Crawl,
        "Crawl: subclass instance");
    assert(Object.getPrototypeOf(cr) === CR.prototype,
        "Crawl: subclass proto");
    cr.close();
    assert(cr.closed === true, "Crawl: close() through the subclass");
}

globalThis.console && console.log("test_native_subclass: all " + n + " tests passed");
