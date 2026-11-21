// flags: --std
// covers: dyna:stream.ndjson
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_stream_ndjson");
import { fromBytes, fromFile, toFile, lines, ndjson } from "dyna:stream";
import {
    Path, writeFile, readFile, readBytes, makeDir, removeAll,
} from "dyna:file";
import * as std from "std";
import * as mod_compress from "dyna:compress";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + " (got " + got + ", want " + want + ")");
}
async function assertRejects(p, msg, code) {
    n++;
    try {
        await p;
    } catch (e) {
        if (code !== undefined && e.code !== code)
            throw new Error("assertion failed: " + msg + ": wrong code " + e.code);
        return;
    }
    throw new Error("assertion failed: " + msg + " (promise resolved, expected rejection)");
}
function assertThrows(fn, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        return;
    }
    throw new Error("assertion failed: " + msg + " (did not throw)");
}

const TMP = std.getenv("TMPDIR") || "/tmp";
const dir = new Path(TMP, "dynastream-test-" + (Date.now() % 10000000));
makeDir(dir, { recursive: true });
let seq = 0;
const tmpPath = (name) => new Path(dir, name + "." + (++seq) + ".tmp");
const enc = new TextEncoder();

await __kitS.section("ndjson value count", async () => {
    const out = [];
    for await (const v of ndjson(fromBytes('{"a":1}\n{"b":[2]}\n\n{"c":"three"}\n')))
        out.push(v);
    assertEq(out.length, 3, "ndjson value count");
    assertEq(out[0].a, 1, "ndjson first value");
    assertEq(JSON.stringify(out[1]), JSON.stringify({ b: [2] }), "ndjson nested");
    assertEq(out[2].c, "three", "ndjson last value (blank line skipped)");
});

await __kitS.section("{\"ok\":1} not json {\"fine\":2}", async () => {
    const it = ndjson(fromBytes('{"ok":1}\nnot json\n{"fine":2}\n'));
    const first = await it.next();
    assertEq(first.value.ok, 1, "ndjson first line parses");
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assert(/line 2/.test(msg), "ndjson error includes line number: " + msg);
    assert(/not json|unexpected token/.test(msg), "ndjson error includes the reason");
});

await __kitS.section("ndjson file count", async () => {
    const f = tmpPath("events.ndjson");
    const w = toFile(f).getWriter();
    for (let i = 0; i < 2000; i++)
        await w.write(enc.encode(JSON.stringify({ i, sq: i * i }) + "\n"));
    await w.close();
    let count = 0, sum = 0;
    for await (const v of ndjson(fromFile(f))) { count++; sum += v.sq; }
    assertEq(count, 2000, "ndjson file count");
    assertEq(sum, 1999 * 2000 * 3999 / 6, "ndjson file sum of squares");
});

await __kitS.section("ndjson residual last line", async () => {
    const out = [];
    for await (const v of ndjson(fromBytes('{"x":true}\n{"y":false}'))) out.push(v);
    assertEq(out.length, 2, "ndjson residual last line");
    assertEq(out[1].y, false, "ndjson residual value");
});

await __kitS.section("ndjson recoverable error", async () => {
    const it = ndjson(fromBytes('bad\n{"good":7}\n'));
    let msg = "";
    try { await it.next(); } catch (e) { msg = e.message; }
    assert(/line 1/.test(msg), "ndjson recoverable error");
    const good = await it.next();
    assertEq(good.value.good, 7, "ndjson continues after error");
});
__kitS.end();

removeAll(dir);
print("test_stream: all tests passed (" + n + " assertions)");
