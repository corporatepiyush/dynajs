import { TOML } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqj(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b), msg +
        " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}

{
    for (let round = 0; round < 50; round++) {
        const doc = {};
        for (let i = 0; i < 40; i++)
            doc["key with spaces " + i] = { "sub.key": [i, "v".repeat(i * 16)], "ü": round };
        const text = TOML.stringify(doc);
        assert(text.indexOf('"key with spaces 39"') >= 0, "round " + round + ": quoted keys emitted");
        const back = TOML.parse(text);
        eqj(back, doc, "round " + round + ": stringify/parse round trip");
    }
    const mixed = TOML.stringify({ bare: 1, "needs quoting": 2 });
    assert(/^bare = 1/m.test(mixed), "bare keys stay bare");
    assert(/"needs quoting" = 2/.test(mixed), "others are quoted");
}

{
    let deep = {};
    let tail = deep;
    for (let i = 0; i < 50; i++) { tail.next = {}; tail = tail.next; }
    tail.v = "bottom";
    const deepText = TOML.stringify(deep);
    let walk = TOML.parse(deepText);
    for (let i = 0; i < 50; i++) walk = walk.next;
    eq(walk.v, "bottom", "50-deep nesting round trips");
    let abyss = {};
    tail = abyss;
    for (let i = 0; i < 200; i++) { tail.next = {}; tail = tail.next; }
    throwsMatch(() => TOML.stringify(abyss), /depth|nesting|unsupported/,
        "200-deep nesting is refused by name");

    const longKey = "k".repeat(4000);
    const longDoc = {};
    longDoc[longKey] = 1;
    const longText = TOML.stringify(longDoc);
    eq(TOML.parse(longText)[longKey], 1, "a 4000-char bare key round trips");
    const longQ = {};
    longQ["q ".repeat(2000)] = 2;
    eq(TOML.parse(TOML.stringify(longQ))["q ".repeat(2000)], 2,
       "a 4000-char quoted key round trips");

    {
        const nk = "a\0b";
        const nDoc = {};
        nDoc[nk] = 1;
        const nText = TOML.stringify(nDoc);
        assert(nText.indexOf('"a\\u0000b"') >= 0,
               "an embedded-NUL key is emitted escaped and WHOLE (got: " +
               JSON.stringify(nText) + ")");
        const nBack = TOML.parse(nText);
        eqj(Object.keys(nBack), [nk], "the embedded-NUL key round trips untruncated");
        eq(nBack[nk], 1, "and its value comes back");
    }
    {
        const cDoc = {};
        cDoc["a\0x"] = 1;
        cDoc["a\0y"] = 2;
        cDoc["a"] = 3;
        let cBack = null;
        try {
            cBack = TOML.parse(TOML.stringify(cDoc));
        } catch (e) {
            assert(false,
                "the collision document must round trip, not throw (got: " + e + ")");
        }
        if (cBack !== null) {
            eqj(Object.keys(cBack).sort(), ["a", "a\0x", "a\0y"],
                "NUL-truncated names would collide: all three keys survive");
            eq(cBack["a\0x"], 1, "first NUL key keeps its value");
            eq(cBack["a\0y"], 2, "second NUL key keeps its value");
            eq(cBack["a"], 3, "the plain sibling is untouched");
        }
        const eDoc = {};
        eDoc["\0"] = "nul";
        eDoc["z\0"] = "tail";
        eqj(TOML.parse(TOML.stringify(eDoc)), eDoc,
            "NUL-only and NUL-final keys round trip");
    }

    const dates = [
        "1979-05-27T07:32:00Z", "1979-05-27T00:32:00-07:00",
        "1979-05-27T07:32:00.999999", "1979-05-27", "07:32:00",
        "1979-05-27T07:32:00", "00:00:00.000001", "1979-05-27 07:32:00Z",
    ];
    for (const d of dates) {
        const back = TOML.parse("when = " + d + "\n");
        assert(typeof back.when === "string",
               "datetime form " + d + " lands as a string (documented cut)");
    }
    eqj(TOML.parse(TOML.stringify({ t: "1979-05-27T07:32:00Z" })),
        { t: "1979-05-27T07:32:00Z" }, "datetime STRING round trips as text");

    eq(TOML.parse('a = """\nfoo \\\n    bar"""\n').a, "foo bar",
       "multi-line basic string line-ending backslash trims the continuation");
    eq(TOML.parse("a = '''\nfoo\nbar'''\n").a, "foo\nbar",
       "multi-line literal keeps its breaks");

    throwsMatch(() => TOML.parse("a = 1\na = 2\n"), /duplicate/,
        "duplicate keys are refused");
    throwsMatch(() => TOML.parse("[t]\n[t]\n"), /already defined|conflict/,
        "redefined tables are refused");
    throwsMatch(() => TOML.parse("a = { x = 1, }\n"), /trailing comma|inline/,
        "inline tables refuse a trailing comma (TOML 1.0)");
    throwsMatch(() => TOML.parse("a = [\n"), /unterminated|expected a value|end|eof|newline/,
        "an unterminated array is refused");
}

{
    const lines = [];
    for (let i = 0; i < 2000; i++)
        lines.push('"k ' + i + '" = { inner = ' + i + ', s = "' + "z".repeat(i % 64) + '" }');
    const wide = lines.join("\n") + "\n";
    for (let round = 0; round < 20; round++) {
        const o = TOML.parse(wide);
        eq(o["k 1999"].inner, 1999, "round " + round + ": wide parse intact");
    }
}

print("test_toml_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)"));
if (fails) throw new Error(fails + " failures");
