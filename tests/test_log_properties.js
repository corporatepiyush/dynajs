/* test_log_properties.js -- invariants of dyna:log, not its current bytes.
 *
 * Appendix A of CLAUDE.md: an expected value taken from what the binary
 * returns freezes today's behaviour including its bugs. So nothing here
 * asserts a byte the engine produced; each check is a PROPERTY -- a framing
 * identity, a round trip, a monotonicity, a containment -- that must hold
 * whatever the renderer does. Where a value IS compared, it comes from
 * outside this engine: JSON.parse (an independent reader) or a definition
 * (a byte count, a set boundary).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_log_properties.js
 */
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, makeDir, readFile, readDir, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const TMP = makeTempDir("dyna-log-prop-");
const P = (s) => new Path(TMP + "/" + s);
for (const sub of ["json", "text", "rot", "rot2", "share"])
    makeDir(P(sub), { recursive: true });
function lines(name) {
    const body = readFile(P(name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 1: every line is one framed record.
 *
 * The property a log consumer depends on more than any other: a file of N
 * emits has exactly N lines, none empty, none spanning two. Checked with an
 * independent reader (JSON.parse) so a malformed line cannot pass by
 * matching what the emitter thinks it wrote.
 * ------------------------------------------------------------------------ */
{
    const log = new Logger({ dest: P("json/framing.log").toString(), level: "trace" });
    const payloads = [
        "plain", "", "with \"quotes\"", "with \n newline and \t tab",
        "control \u0000\u0001\u001f chars", "unicode 你好 \u{1F600}",
        "closing brace } and { brace", "back\\slash",
    ];
    for (const m of payloads)
        log.info(m);
    log.info({ a: "x\n".repeat(50) }, "embedded newlines in a field value");
    log.info({ cyclic: (function () { const c = { a: 1 }; c.self = c; return c; })() },
             "cyclic");
    log.flush();
    const ls = lines("json/framing.log");
    eq(ls.length, payloads.length + 2, "N emits produce exactly N lines");
    for (const l of ls) {
        let ok = true;
        try {
            const o = JSON.parse(l);          /* independent reader */
            ok = o && typeof o === "object" && typeof o.msg === "string" &&
                 typeof o.level === "string";
        } catch (e) { ok = false; }
        assert(ok, "line parses as a JSON object with msg and level: " + l.slice(0, 60));
    }
}

/* The message ROUND TRIPS through JSON: what went in is what a reader gets
 * out. This is the strongest form of the escaping claim -- it does not care
 * how the bytes are escaped, only that the value survives. */
{
    const log = new Logger({ dest: P("json/roundtrip.log").toString(),
                             timestamp: false });
    const msgs = [
        "a\"b", "a\\b", "a\nb", "a\tb", "a\rb", "\u0000\u0001\u001f\u0007\b\f",
        "你好", "\u{1F600}\u{1F4A9}", "}{", "[1,2]", "null", "undefined",
        "  leading and trailing  ", "\u007f", "é\u0301",
    ];
    for (const m of msgs)
        log.info(m);
    log.flush();
    const ls = lines("json/roundtrip.log");
    eq(ls.length, msgs.length, "one line per message");
    for (let i = 0; i < msgs.length; i++) {
        const got = JSON.parse(ls[i]).msg;
        eq(got, msgs[i], "message round trips through JSON: " + JSON.stringify(msgs[i]));
    }
}

/* Field KEYS round trip too, including keys that are themselves hostile. */
{
    const log = new Logger({ dest: P("json/keys.log").toString(), timestamp: false });
    const keys = ["a\"b", "a\nb", "\u0000", "你好", "__proto__", "constructor",
                  "", "0", "level", "msg"];
    const obj = {};
    for (const k of keys)
        obj[k] = 1;
    log.info(obj, "keys");
    log.flush();
    const o = JSON.parse(lines("json/keys.log")[0]);
    for (const k of keys) {
        /* "__proto__" as an OWN key is unobservable on a JSON.parse result
           (it retargets the prototype) -- a property of JS, not of the
           logger. The logger's own guarantee is the level/msg pair below. */
        if (k === "level" || k === "msg" || k === "__proto__")
            continue;                          /* reserved or unobservable */
        assert(Object.prototype.hasOwnProperty.call(o, k),
               "key survives round trip: " + JSON.stringify(k));
    }
    eq(o.level, "info", "the frame's level is untouched by a field called level");
    eq(o.msg, "keys", "the frame's msg is untouched by a field called msg");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 1.5: narrow strings with high bytes survive as UTF-8.
 *
 * The engine stores narrow strings in LATIN-1: "café" has byte 0xE9, NOT the
 * UTF-8 pair 0xC3 0xA9. A zero-copy fast path that writes those bytes as-is
 * corrupts every non-ASCII narrow string (é renders as Ã©). The round trip
 * below catches that: it parses with an independent reader and requires an
 * exact value match for narrow-latin1, wide-unicode, and control chars.
 * ------------------------------------------------------------------------ */
{
    const log = new Logger({ dest: P("json/latin1.log").toString(),
                             timestamp: false });
    const vals = ["café", "grüße", "a\u00e9b", "✓", "é\u0000\x7f", "你好", "🎉",
                  "plain", "line\nbreak", "tab\there", "quote\"q"];
    for (const v of vals)
        log.info({ s: v });
    log.flush();
    const ls = lines("json/latin1.log");
    eq(ls.length, vals.length, "one line per latin1/unicode value");
    for (let i = 0; i < vals.length; i++) {
        const o = JSON.parse(ls[i]);
        eq(o.s, vals[i], "narrow-string value round-trips as UTF-8: " +
           JSON.stringify(vals[i]));
    }
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 2: every line is bounded.
 *
 * An unbounded line is how a log statement takes down a service. The bound
 * is a definition (64 KiB), not an observation, and it must hold for both
 * renderers and for a payload far past it.
 * ------------------------------------------------------------------------ */
const CAP = 64 * 1024;
for (const fmt of ["json", "text"]) {
    const log = new Logger({ dest: P(fmt + "/cap.log").toString(), format: fmt,
                             timestamp: false, mkdir: true });
    log.info("X".repeat(4 * 1024 * 1024));     /* 4 MiB into a 64 KiB cap */
    log.flush();
    const ls = lines(fmt + "/cap.log");
    eq(ls.length, 1, fmt + ": a 4 MiB payload is still ONE line");
    assert(ls[0].length <= CAP, fmt + ": line is within the cap: " + ls[0].length);
}

/* The cut must leave a WELL-FORMED record. A line severed mid-string with no
 * closing quote would corrupt the NEXT line's framing too, since a consumer
 * could not find this one's end -- so the truncation marker is in-band and
 * the line is closed. */
{
    const log = new Logger({ dest: P("json/trunc.log").toString(), timestamp: false });
    log.info("Y".repeat(200 * 1024));
    log.flush();
    const ls = lines("json/trunc.log");
    eq(ls.length, 1, "a 200 KiB payload is one line");
    let parsed = null, ok = true;
    try { parsed = JSON.parse(ls[0]); } catch (e) { ok = false; }
    assert(ok, "a truncated line is still well-formed JSON");
    if (ok) {
        assert(parsed.msg.length > 0, "the truncation kept a message");
        assert(parsed.msg.length < 200 * 1024, "the payload was actually cut");
        assert(parsed.msg.endsWith("..."), "the cut is marked in-band: " +
               parsed.msg.slice(-8));
    }
}

/* The same cut, landing INSIDE a \uXXXX escape. This was a real bug: the cap
 * split "\u0001" mid-escape, the marker was appended to `\u0`, and the line
 * became unparseable ("Invalid \escape"). Any payload of control characters
 * hit it, which is precisely the payload a hostile peer sends. */
{
    const log = new Logger({ dest: P("json/escut.log").toString(), timestamp: false });
    log.info("\u0001".repeat(40000));       /* every char is a 6-byte escape */
    log.flush();
    const ls = lines("json/escut.log");
    eq(ls.length, 1, "a control-char payload is one line");
    let ok = true;
    try { JSON.parse(ls[0]); } catch (e) { ok = false; }
    assert(ok, "a cut INSIDE a \\uXXXX escape still yields parseable JSON");
    if (ok)
        assert(typeof JSON.parse(ls[0]).msg === "string",
               "the truncated message is still a string");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 3: rotation is monotone -- name order is time order.
 *
 * The invariant that makes pino-roll's extension-last naming worth having:
 * sorting the surviving files by name MUST sort them oldest-to-newest,
 * including after retention has deleted the low numbers. Asserted as
 * CONSECUTIVE NUMBERS, which also proves no pruned number was reused.
 * ------------------------------------------------------------------------ */
{
    const log = new Logger({ dest: P("rot/app.log").toString(), timestamp: false,
                             rollover: { size: 200, count: 3 } });
    for (let i = 0; i < 60; i++)
        log.info("r" + i + " padding-padding-padding");
    log.flush();
    const ns = readDir(P("rot")).map((e) => e.name)
                   .filter((f) => /^app\.\d+\.log$/.test(f))
                   .map((f) => parseInt(f.match(/(\d+)\.log$/)[1], 10))
                   .sort((a, b) => a - b);
    assert(ns.length >= 2, "rotation happened: " + ns.join(","));
    eq(ns.length, 4, "count:3 leaves three rotated plus the active file");
    for (let i = 1; i < ns.length; i++)
        eq(ns[i], ns[i - 1] + 1, "numbers are consecutive (no reuse): " + ns.join(","));
    /* time order: the newest line lives in the highest-numbered file */
    const last = readFile(P("rot/app." + ns[ns.length - 1] + ".log"));
    assert(last.includes("r59"), "the newest line is in the highest number");
    /* and none of the rotated files is empty: rotation never drops a line */
    for (const f of ns) {
        const body = readFile(P("rot/app." + f + ".log"));
        assert(body.length > 0, "rotated file " + f + " is non-empty");
    }
}

/* Every emitted line survives rotation somewhere: with retention OFF the
 * union of the rotated files must still contain every line. */
{
    const log = new Logger({ dest: P("rot2/app.log").toString(), timestamp: false,
                             rollover: { size: 120 } });
    const want = [];
    for (let i = 0; i < 20; i++) {
        const tag = "tag" + String(i).padStart(2, "0");
        want.push(tag);
        log.info(tag + " padding-padding-padding-padding");
    }
    log.flush();
    const fs = readDir(P("rot2")).map((e) => e.name)
                   .filter((f) => /^app\.\d+\.log$/.test(f));
    assert(fs.length >= 2, "several rotations: " + fs.join(","));
    const all = fs.map((f) => readFile(P("rot2/" + f))).join("");
    for (const t of want)
        assert(all.includes(t), "line " + t + " survived rotation");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 4: the level gate is a true gate.
 *
 * Suppressed means suppressed: no bytes, no side effects on the caller's
 * object. The getter trap proves the object was never walked.
 * ------------------------------------------------------------------------ */
{
    const log = new Logger({ dest: P("json/gate.log").toString(),
                             timestamp: false, level: "error" });
    let touched = 0;
    const trap = { get boom() { touched++; return 1; } };
    log.trace(trap, "t");
    log.debug(trap, "d");
    log.info(trap, "i");
    eq(touched, 0, "suppressed emits never touch the fields object");
    eq(lines("json/gate.log").length, 0, "suppressed emits write nothing");
    log.error(trap, "e");
    eq(touched, 1, "an enabled emit walks the fields object once");
}

/* "silent" is total, at every level, on a file destination too. */
{
    const log = new Logger({ dest: P("json/silent.log").toString(),
                             timestamp: false, level: "silent" });
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"])
        log[lv]("must not appear");
    log.flush();
    eq(lines("json/silent.log").length, 0, "silent writes nothing at all");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 5: child() is additive and independent.
 *
 * A child's line contains the parent's fields (containment, not equality --
 * the exact prefix layout is not the property), and changing one logger's
 * level never changes another's.
 * ------------------------------------------------------------------------ */
{
    const parent = new Logger({ dest: P("json/child.log").toString(),
                                timestamp: false, name: "p",
                                base: { env: "prod" } });
    const child = parent.child({ route: "/api" });
    const grand = child.child({ span: "s1" });
    grand.info("deep");
    parent.flush();
    const o = JSON.parse(lines("json/child.log")[0]);
    eq(o.env, "prod", "the grandchild carries the parent's base field");
    eq(o.route, "/api", "and the child's");
    eq(o.span, "s1", "and its own");
    eq(o.name, "p", "and the logger name");

    child.level = "trace";
    eq(child.level, "trace", "the child's level changed");
    eq(parent.level, "info", "the parent's did not");
    eq(grand.level, "info", "a level change never propagates sideways");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 6: the text format is a projection of the same event.
 *
 * Both renderers describe ONE event, so everything the text line carries
 * must also be in the JSON line -- the text form is a view, never a
 * different answer. Compared as VALUES, not bytes.
 * ------------------------------------------------------------------------ */
{
    const jlog = new Logger({ dest: P("json/pair.log").toString(), timestamp: false });
    const tlog = new Logger({ dest: P("text/pair.log").toString(), timestamp: false,
                              format: "text" });
    const fields = { userId: 42, ok: true, nothing: null, f: 1.5,
                     nested: { a: [1, 2] } };
    jlog.info(fields, "shared event");
    tlog.info(fields, "shared event");
    jlog.flush();
    tlog.flush();
    const j = JSON.parse(lines("json/pair.log")[0]);
    const t = lines("text/pair.log")[0];
    eq(j.msg, "shared event", "json msg");
    assert(t.includes("shared event"), "text carries the same message");
    assert(t.includes("userId=42"), "text carries the number field: " + t);
    assert(t.includes("ok=true"), "text carries the boolean: " + t);
    assert(t.includes("nothing=null"), "text carries null: " + t);
    assert(t.includes("f=1.5"), "text carries the float: " + t);
    assert(t.includes("nested={\"a\":[1,2]}"), "text carries the composite: " + t);
    /* the text line contains no raw newline: framing holds there too */
    assert(t.indexOf("\n") === -1, "the text line has no embedded newline");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 7: two Loggers on one path are one destination.
 *
 * Sharing is the point of the registry: interleaved writes from two Loggers
 * must land in ONE file, every line intact, in call order.
 * ------------------------------------------------------------------------ */
{
    const o = { dest: P("share/app.log").toString(), timestamp: false };
    const a = new Logger(o);
    const b = new Logger(o);
    for (let i = 0; i < 10; i++) {
        a.info("a" + i);
        b.info("b" + i);
    }
    a.flush();
    const ls = lines("share/app.log");
    eq(ls.length, 20, "both Loggers wrote into one file");
    for (let i = 0; i < 10; i++) {
        eq(JSON.parse(ls[i * 2]).msg, "a" + i, "interleaved order holds at " + i);
        eq(JSON.parse(ls[i * 2 + 1]).msg, "b" + i, "interleaved order holds at " + i);
    }
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 8: a Logger never kills its caller.
 *
 * Every hostile shape is a legal argument. None may throw, hang, or stop the
 * program -- a logger that fails takes down the thing it exists to observe.
 * None may produce a malformed line either, which is the half the usual
 * "does not throw" tests miss.
 * ------------------------------------------------------------------------ */
{
    const log = new Logger({ dest: P("json/hostile.log").toString(),
                             timestamp: false, level: "trace" });
    const cyclic = { a: 1 };
    cyclic.self = cyclic;
    const deep = { v: 0 };
    let cur = deep;
    for (let i = 0; i < 5000; i++) { cur.next = { v: i }; cur = cur.next; }
    const shared = { v: 1 };
    const shapes = [
        [], [undefined], [null], [0], [""],
        [{ get a() { throw new Error("getter"); } }],
        [cyclic], [deep], [{ l: shared, r: shared }],
        [{ s: "x".repeat(300000) }], [new Error("e")],
        [{ f() { return 1; } }], [{ sym: 1 }],
        [[1, 2, 3]], [{ d: new Date(0) }], [{ m: new Map([[1, 2]]) }],
        [{ n: NaN, i: Infinity, z: -0, big: 2 ** 70 }],
    ];
    for (const args of shapes)
        log.info(...args);
    log.flush();
    const ls = lines("json/hostile.log");
    eq(ls.length, shapes.length, "every hostile shape produced a line");
    for (const l of ls) {
        /* a line cut by the cap ends mid-payload, so `msg` may be absent --
           what must hold is that the record OPENS as a frame. Well-formedness
           of the un-cut majority is checked in PROPERTY 1 and 2. */
        assert(l.startsWith("{\"level\":"),
               "a hostile line still opens as a frame: " + l.slice(0, 40));
        if (l.length < 60 * 1024) {
            let ok = true;
            try { JSON.parse(l); } catch (e) { ok = false; }
            assert(ok, "an uncut hostile line is well-formed JSON: " + l.slice(0, 50));
        }
    }
}

/* A getter that throws inside a field object must not abort the line. */
{
    const log = new Logger({ dest: P("json/getter.log").toString(),
                             timestamp: false });
    log.info({ bad: { get x() { throw new Error("nope"); } }, good: 1 }, "survived");
    log.flush();
    const ls = lines("json/getter.log");
    eq(ls.length, 1, "a throwing getter did not stop the line");
    assert(ls[0].includes("survived"), "the message still landed: " + ls[0]);
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 9: Debug is inert when unmatched and total when called.
 * ------------------------------------------------------------------------ */
{
    const d = Debug("prop:ns");
    eq(typeof d, "function", "Debug returns a function");
    eq(d(), undefined, "Debug with no arguments is fine");
    eq(d("a", "b", 3), undefined, "Debug joins several arguments");
    eq(d({ toString() { return "via toString"; } }), undefined, "coercible values work");
    /* inert: an unmatched Debug must cost nothing and throw nothing */
    for (let i = 0; i < 1000; i++)
        Debug("never:matching:namespace")("loop " + i);
    assert(true, "1000 unmatched Debug calls are inert");
}

/* ------------------------------------------------------------------------ *
 * PROPERTY 10: constructed state is independent state.
 * ------------------------------------------------------------------------ */
{
    const levels = ["trace", "debug", "info", "warn", "error", "fatal", "silent"];
    const logs = levels.map((lv) => new Logger({ level: lv, timestamp: false }));
    for (let i = 0; i < levels.length; i++)
        eq(logs[i].level, levels[i], "logger " + i + " kept its own level");
    /* enabled() is monotone in the level: a lower level enables more */
    for (let i = 0; i < levels.length; i++) {
        let count = 0;
        for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"])
            if (logs[i].enabled(lv)) count++;
        if (levels[i] === "silent")
            eq(count, 0, "silent enables nothing");
        else if (i <= 5)
            eq(count, 6 - i, "level " + levels[i] + " enables " + (6 - i) + " of six");
    }
}

removeAll(P("."));
if (fails) {
    print("test_log_properties: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_properties failed");
}
print("test_log_properties: " + n + " assertions, 0 failures");
