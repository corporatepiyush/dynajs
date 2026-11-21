// covers: integration c:src/modules/stream/stream-pipe.c c:src/modules/stream/stream-lines.c c:src/modules/stream/stream-module.c c:src/modules/stream/stream-push.c c:src/modules/libc/libc-wst-misc.c c:src/modules/libc/libc-wst-adapt.c -- the --timeout-ms interrupt stays uncatchable through dyna:stream and the TextDecoderStream / transfer error paths
// flags: --std
// The interrupt that --timeout-ms raises is uncatchable. The dyna:stream pipe engine (a duck source piped to a
// duck sink) and lines()/ndjson() used to take it with JS_GetException and turn
// it into an ordinary rejection or a TypeError, so user code saw it ("LEAK" lines). Each interrupt case runs in its
// own child process. The in-process cases pin the catchable behaviour of the same paths: a ToString that throws in
// lines({encoding}) now propagates (it used to be replaced by the "only utf-8" TypeError), a byte count whose
// valueOf throws still maps to the documented TypeError, and an error whose name getter throws still crosses to a
// worker as a plain Error (pack_value keeps the defaults when the name read fails).
import { args, Exec } from "dyna:sys";
import * as os from "os";
import { toReadableStream, toWritableStream, lines, fromBytes } from "dyna:stream";
import { suite } from "../lib/kit.js";

const BIN = args()[0];
const DEADLINE_MS = 200;
const LIMIT_MS = 3000;
const t = suite("stream_interrupt");

const pre = "function spin(){ for(;;){} }\nconst leak = (e) => console.log(\"LEAK\", e);\nsetTimeout(() => console.log(\"LEAK timer\"), 100);\nconst tick = () => new Promise((r) => setTimeout(r, 0));\n";
const ds = (body) => `import("dyna:stream").then(async ({ toReadableStream, toWritableStream, lines, ndjson, fromBytes }) => { ${body} });`;
const src1 = "toReadableStream({ read(b) { b[0] = 65; return Promise.resolve(1); }, close() {} })";
const spinThen = "Object.defineProperty(Object.prototype, \"then\", { configurable: true, get() { spin(); } });";

const CASES = [
    ["fast pipe: sink write resolves a count whose valueOf spins", ds(`const rs = ${src1}; await tick(); rs.pipeTo(toWritableStream({ write() { return Promise.resolve({ valueOf() { spin(); } }); }, close() {} })).catch(leak);`)],
    ["fast pipe: source read resolves a count whose valueOf spins", ds(`const rs = toReadableStream({ read() { return Promise.resolve({ valueOf() { spin(); } }); }, close() {} }); await tick(); rs.pipeTo(toWritableStream({ write(v) { return Promise.resolve(v.length); }, close() {} })).catch(leak);`)],
    ["fast pipe: sink write spins", ds(`const rs = ${src1}; await tick(); rs.pipeTo(toWritableStream({ write() { spin(); }, close() {} })).catch(leak);`)],
    ["lines: ReadableStream pull spins", ds(`(async () => { try { for await (const l of lines(new ReadableStream({ pull() { spin(); } }))); } catch (e) { leak(e); } })();`)],
    ["lines batch: ReadableStream pull spins", ds(`(async () => { try { for await (const l of lines(new ReadableStream({ pull() { spin(); } }), { batch: true })); } catch (e) { leak(e); } })();`)],
    ["ndjson: ReadableStream pull spins", ds(`(async () => { try { for await (const l of ndjson(new ReadableStream({ pull() { spin(); } }))); } catch (e) { leak(e); } })();`)],
    ["lines: encoding toString spins", ds(`try { lines(fromBytes("a\\n"), { encoding: { toString() { spin(); } } }); } catch (e) { leak(e); } console.log("LEAK after lines");`)],
    ["lines: resolving a line runs a then getter that spins", ds(`const it = lines(new ReadableStream({ start(c) { c.enqueue("a\\nb"); } })); ${spinThen} it.next().then(leak, leak);`)],
    ["lines: done result runs a then getter that spins", ds(`const it = lines(new ReadableStream({ start(c) { c.close(); } })); ${spinThen} it.next().then(leak, leak);`)],
    ["TextDecoderStream: enqueue runs a then getter that spins", `const ts = new TextDecoderStream(); ts.readable.getReader().read().catch(leak); ${spinThen} ts.writable.getWriter().write(new Uint8Array([65])).catch(leak);`],
];

for (const [name, body] of CASES) {
    t.test(name, (a) => {
        const t0 = Date.now();
        const r = Exec(BIN, ["--std", "--timeout-ms", String(DEADLINE_MS), "--eval", pre + body], { timeoutMs: 10000 });
        const ms = Date.now() - t0, out = String(r.stdout), err = String(r.stderr);
        a.ok(r.code !== 0, `exits non-zero (code ${r.code})`);
        a.ok(err.includes("interrupted"), `reports interrupted (stderr ${JSON.stringify(err.slice(0, 160))})`);
        a.ok(!out.includes("LEAK"), `no user code ran after the interrupt (stdout ${JSON.stringify(out.slice(0, 160))})`);
        a.ok(ms < LIMIT_MS, `exits within ${LIMIT_MS} ms (took ${ms} ms)`);
    });
}

const tick = () => new Promise((r) => setTimeout(r, 0));

t.test("lines: an encoding whose toString throws propagates that error", (a) => {
    let got = "none";
    try { lines(fromBytes("a\n"), { encoding: { toString() { throw new RangeError("enc"); } } }); } catch (e) { got = `${e.name}:${e.message}`; }
    a.eq(got, "RangeError:enc");
    let bad = "none";
    try { lines(fromBytes("a\n"), { encoding: "latin1" }); } catch (e) { bad = e.name; }
    a.eq(bad, "TypeError", "an unsupported encoding is still a TypeError");
});

t.test("fast pipe: a byte count whose valueOf throws is the documented TypeError", async (a) => {
    for (const side of ["write", "read"]) {
        const bad = Promise.resolve({ valueOf() { throw new RangeError("vo"); } });
        const rs = toReadableStream({ read(b) { if (side === "read") return bad; b[0] = 65; return Promise.resolve(1); }, close() {} });
        await tick();
        const ws = toWritableStream({ write(v) { return side === "write" ? bad : Promise.resolve(v.length); }, close() {} });
        let got = "resolved";
        await rs.pipeTo(ws).catch((e) => { got = `${e.name}:${e.message}`; });
        a.eq(got, side === "write" ? "TypeError:ByteSink.write must resolve to a non-negative byte count" : "TypeError:ByteSource.read must resolve to a non-negative byte count", side);
    }
});

t.test("transfer: an error whose name getter throws crosses as a plain Error", async (a) => {
    const w = new os.Worker("../core/web_streams_transfer_child.js");
    const reply = new Promise((r) => { w.onmessage = (e) => r(e.data); });
    const err = new RangeError("broken source");
    Object.defineProperty(err, "name", { get() { throw new Error("no name"); } });
    const rs = new ReadableStream({ pull(c) { c.error(err); } });
    w.postMessage({ op: "readErr", rs }, [rs]);
    a.deep(await reply, { op: "readErr", name: "Error", message: "" });
    w.postMessage({ op: "bye" });
    w.onmessage = null;
}, { timeoutMs: 10000 });

await t.run();
