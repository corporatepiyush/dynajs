// Black-box contract test for dyna:config, generated from dynajs.d.ts lines 673-716. Engine sources not consulted; every expectation cites the contract.
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

/* Table runners: rows are [args..., expected] (expected may be a predicate) or [args..., ErrorClass]. */
function labelOf(name, args) {
    return name + "(" + args.map(a => typeof a === "string" ? JSON.stringify(a.length > 40 ? a.slice(0, 40) + "…" : a) : String(a)).join(", ") + ")";
}
function J(v) { return typeof v === "bigint" ? v + "n" : JSON.stringify(v); }
function assertCases(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const want = row[row.length - 1];
        n++;
        let got, threw = null;
        try { got = fn(...args); } catch (e) { threw = e; }
        const label = labelOf(name, args);
        if (threw) throw new Error("case threw " + threw + ": " + label);
        if (typeof want === "function") { if (!want(got)) throw new Error("case predicate failed: " + label + " — got |" + J(got) + "|"); }
        else if (J(got) !== J(want)) throw new Error("case failed: " + label + " — got |" + J(got) + "| expected |" + J(want) + "|");
    }
}
function assertCasesThrow(fn, cases, name) {
    for (const row of cases) {
        const args = row.slice(0, row.length - 1);
        const Err = row[row.length - 1];
        n++;
        let threw = false, err = null;
        try { fn(...args); } catch (e) { threw = true; err = e; }
        const label = labelOf(name, args);
        if (!threw) throw new Error("expected throw: " + label);
        if (Err && !(err instanceof Err)) throw new Error("wrong error class " + (err && err.constructor ? err.constructor.name : String(err)) + ": " + label);
    }
}

/* ---------------- TOML.parse ---------------- */
{
    // d.ts: "Parses a full TOML 1.0 document; key collisions and leading zeros are refused."
    assertCases((text) => TOML.parse(text), [
        ['title = "DynaJS"', { title: "DynaJS" }],
        ["port = 8080", { port: 8080 }],
        ["pi = 3.14", { pi: 3.14 }],
        ["f = 1_000.5", { f: 1000.5 }],                  // TOML 1.0: underscores allowed between digits of the fractional part too
        ["g = 1.5e1_0", { g: 1.5e10 }],                  // underscores in the exponent (TOML 1.0)
        ["a = +0x10", { a: 16 }],                        // signed base-prefixed int (TOML 1.0: +/- before 0x)
        ["b = -0b101", { b: -5 }],                       // signed binary int (TOML 1.0)
        ["neg = -5", { neg: -5 }],
        ["b = true", { b: true }],
        ["b = false", { b: false }],
        ["a = [1, 2, 3]", { a: [1, 2, 3] }],
        ['s = "a\\tb"', { s: "a\tb" }],                 // basic string escapes (d.ts: "string escapes")
        ["[server]\nport = 8080", { server: { port: 8080 } }],
        ["[a.b]\nx = 1", { a: { b: { x: 1 } } }],       // dotted section headers
        ['[[items]]\nname = "a"\n[[items]]\nname = "b"', { items: [{ name: "a" }, { name: "b" }] }],   // arrays of tables
        ["a = inf", { a: Infinity }],                    // floats with inf (dynajs.d.ts)
        ["a = -inf", { a: -Infinity }],
        ["a = nan", (v) => Number.isNaN(v.a)],
        // d.ts: "Date-time values parse as their raw RFC 3339 STRINGS"
        ["d = 1979-05-27T07:32:00Z", { d: "1979-05-27T07:32:00Z" }],
        ["d = 1979-05-27", { d: "1979-05-27" }],
        ["t = 07:32:00", { t: "07:32:00" }],
    ], "TOML.parse");
    assertCasesThrow((text) => TOML.parse(text), [
        ["a = 1\na = 2", null],      // a key collision throws (d.ts)
        ["x = 01", null],            // leading zeros are refused (d.ts)
        ["x = ", SyntaxError],
        ['s = """\x01bad"""', null], // TOML 1.0: multiline strings forbid C0 controls except tab/newline
        ["s = '''\x01bad'''", null], // same for multiline literal strings
        ["x = +010", null],          // signed leading zero still refused (TOML 1.0)
    ], "TOML.parse");
}

/* ---------------- TOML.stringify ---------------- */
{
    // d.ts: "Serializes a plain object root; NaN/Infinity render as nan/inf/-inf."
    const round = { title: "t", ok: true, server: { port: 8080, host: "localhost" } };
    assertCases((v) => TOML.parse(TOML.stringify(v)), [
        // property row: parse(stringify(x)) deep-identical
        [round, round],
        [{ a: NaN }, (v) => Number.isNaN(v.a)],          // nan round trips
        [{ a: Infinity }, { a: Infinity }],
        [{ a: -Infinity }, { a: -Infinity }],
        [{ items: [1, 2, 3] }, { items: [1, 2, 3] }],
    ], "TOML.stringify round trip");
    assertCases((v) => TOML.stringify(v), [
        [{ port: 8080 }, (s) => s.indexOf("port = 8080") !== -1],        // k = v spacing (dynajs.d.ts)
        [{ a: NaN }, (s) => /nan/.test(s)],                              // NaN -> nan (d.ts)
        [{ a: -Infinity }, (s) => /-inf/.test(s)],                       // -Infinity -> -inf (d.ts)
        [{ a: { b: 1 } }, (s) => s.indexOf("b = 1") !== -1 && s.indexOf("{") !== -1],   // nested tables emit inline {k = v} (dynajs.d.ts)
    ], "TOML.stringify");
    assertCasesThrow((v) => TOML.stringify(v), [
        [{ a: null }, TypeError],      // null is refused (dynajs.d.ts: TOML has no null literal)
        [{ a: undefined }, TypeError],  // undefined is refused
        [42, null],          // a non-object root throws (dynajs.d.ts)
        ["x", null],
    ], "TOML.stringify");
}

/* ---------------- INI.parse ---------------- */
{
    // d.ts: "Reads [section] headers, key=value pairs, key[]=v lists, and bare keys as true."
    assertCases((text) => INI.parse(text), [
        ["[server]\nhost = localhost", { server: { host: "localhost" } }],
        ["[server]\nports[] = 80\nports[] = 443", { server: { ports: ["80", "443"] } }],   // lists of strings (INI has no numeric type)
        ["flag", { flag: true }],                        // a bare key is true (d.ts)
        ["# c\n; d\nk = v", { k: "v" }],                 // '#' and ';' comments (dynajs.d.ts)
        ["[a.b]\nx = 1", { a: { b: { x: "1" } } }],      // sections nest via dots (dynajs.d.ts)
        ["k = 5", { k: "5" }],                           // values are strings (d.ts: INI has no numeric type)
    ], "INI.parse");
    // dynajs.d.ts: "a file containing __proto__ = x produces an own property"
    assertCases((text) => { const r = INI.parse(text); return Object.getOwnPropertyDescriptor(r.s, "__proto__") !== undefined; }, [
        ["[s]\n__proto__ = x", true],
    ], "INI.parse __proto__");
}

/* ---------------- INI.stringify ---------------- */
{
    // d.ts: "top-level scalars first, then one [section] block per object value"; numbers and
    // booleans "emit bare and parse back as strings"; "Deeper nesting, arrays and null are refused."
    assertCases((r) => INI.parse(INI.stringify(r)), [
        [{ top: "x", sec: { k: "v", n: "7" } }, { top: "x", sec: { k: "v", n: "7" } }],   // property row: value-identical
        [{ k: 'a "b" c' }, { k: 'a "b" c' }],            // quoting round trips through the escapes
        [{ k: "a;b" }, { k: "a;b" }],
    ], "INI round trip");
    assertCases((r) => INI.parse(INI.stringify(r)).a, [
        [{ a: 5 }, "5"],        // numbers emit bare and parse back as STRINGS (d.ts)
        [{ a: true }, "true"],  // booleans likewise (d.ts)
    ], "INI.stringify typed scalars");
    assertCases((r) => { const s = INI.stringify(r); return s.indexOf("top") !== -1 && s.indexOf("top") < s.indexOf("[sec"); }, [
        [{ top: "x", sec: { k: "v" } }, true],   // scalars first, then [section] blocks (d.ts)
    ], "INI.stringify order");
    assertCasesThrow((r) => INI.stringify(r), [
        [{ a: { b: { c: 1 } } }, null],  // deeper nesting refused (d.ts)
        [{ a: [1, 2] }, null],           // arrays refused (d.ts)
        [{ a: null }, null],             // null refused (d.ts)
    ], "INI.stringify");
}

/* ---------------- Env.parse / Env.stringify / Env.load ---------------- */
{
    // d.ts: "Parses KEY=value records; lines without `=` are skipped."
    assertCases((text) => Env.parse(text), [
        ['PORT=8080', { PORT: "8080" }],                          // values are strings (dotenv)
        ['export DB_URL="postgres://x"', { DB_URL: "postgres://x" }],   // optional export prefix + double quotes
        ["A=1", { A: "1" }],
        ['A="x\\ny"', { A: "x\ny" }],     // \n expands inside double quotes (dynajs.d.ts)
        ["B=x\\ny", { B: "x\\ny" }],      // bare values are literal
        ["C='x\\ny'", { C: "x\\ny" }],    // single-quoted values are literal
        ["justtext\nA=1", { A: "1" }],    // a line without '=' is skipped, not an error (d.ts)
        ["# comment\nA=1", { A: "1" }],   // '#' comments (dynajs.d.ts)
    ], "Env.parse");
    // d.ts: stringify "every value must be a string"; quoting so parse(stringify(r)) is
    // value-identical.
    assertCases((r) => Env.parse(Env.stringify(r)), [
        [{ A: "hello world", B: "with # hash", C: "" }, { A: "hello world", B: "with # hash", C: "" }],
        [{ A: "x\ny" }, { A: "x\ny" }],   // quoted escapes round trip
    ], "Env round trip");
    assertCases((r) => Env.stringify(r).indexOf('"') !== -1, [
        [{ A: "" }, true],               // a value that cannot sit bare is double-quoted (d.ts)
        [{ A: "with # hash" }, true],
    ], "Env.stringify quoting");
    assertCasesThrow((r) => Env.stringify(r), [
        [{ A: 1 }, null],                // numbers throw (d.ts: "every value must be a string")
        [{ A: null }, null],
    ], "Env.stringify");
    // d.ts/dynajs.d.ts: the option bag is strict — booleans only, an unknown key throws a TypeError
    // naming the key and the valid set, "checked before the file is opened"; a missing file throws.
    assertCasesThrow((...a) => Env.load(...a), [
        ["/definitely-missing-dynajs-env", { overide: true }, TypeError],   // unknown key, pre-open check
        ["/definitely-missing-dynajs-env", { assign: "yes" }, TypeError],   // non-boolean value
        ["/definitely-missing-dynajs-env", null],                            // a missing file throws
    ], "Env.load");
}

/* ---------------- FrontMatter.split ---------------- */
{
    // d.ts: "Splits at a first-line fence; `data`/`lang` are null when absent." Data stays TEXT;
    // an unclosed fence is not front matter (dynajs.d.ts).
    assertCases((text) => { const r = FrontMatter.split(text); return [r.data === null, r.lang === null, r.body]; }, [
        ["plain text", [true, true, "plain text"]],                    // no fence: whole input is the body
        ["---\nnever closed", [true, true, "---\nnever closed"]],      // unclosed fence (d.ts)
        ["text first\n---\nnot a fence\n---\n", [true, true, "text first\n---\nnot a fence\n---\n"]],   // the fence must be the FIRST line
    ], "FrontMatter none");
    assertCases((text) => { const r = FrontMatter.split(text); return [typeof r.data === "string" && r.data.indexOf("title:") === 0, typeof r.lang === "string", r.body.indexOf("body text") !== -1]; }, [
        ["---\ntitle: DynaJS\n---\n\nbody text", [true, true, true]],   // YAML fence
    ], "FrontMatter yaml");
    assertCases((text) => { const r = FrontMatter.split(text); return [typeof r.data === "string", typeof r.lang === "string"]; }, [
        ["+++\nk = 1\n+++\nbody", [true, true]],    // +++ marks TOML (d.ts)
        [';;;\n{"k":1}\n;;;\nbody', [true, true]],  // ;;; marks JSON (d.ts)
    ], "FrontMatter fences");
    assertCases((text) => { const r = FrontMatter.split(text); return typeof r.data === "string" && r.data.indexOf('{"k":1}') === 0; }, [
        [';;;\n{"k":1}\n;;;\nbody', true],   // data stays text — split does not parse (d.ts)
    ], "FrontMatter data is text");
}

print("bb_config: all tests passed (" + n + " assertions)");
