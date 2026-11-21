// flags: --std
// covers: dyna:sys.Spawn
// SpawnPipe is async-iterable (dogfood G9: `for await (x of p.stdout)` threw
// "not iterable"). Oracle: Exec of the same argv; chunks must be COPIES (the
// iterator reuses one read buffer, so a view would be overwritten by the next
// read and the concatenation would come out wrong), and `break` must close the
// pipe so an endless writer dies of SIGPIPE instead of blocking forever. close() must also drop
// bytes the reactor already buffered: it once only closed the fd, so read() after a break served
// the leftover buffer (got 16, want 0 on the docker gcc leg, timing-dependent); the last block
// buffers 990 unread bytes on purpose and must still see EOF.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_spawn_pipe_iter");
import { Exec, Spawn } from "dyna:sys";
import { lines } from "dyna:stream";
import * as std from "std";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const dec = (chunks) => { let len = 0; for (const c of chunks) len += c.length; const all = new Uint8Array(len); let o = 0; for (const c of chunks) { all.set(c, o); o += c.length; } return new TextDecoder().decode(all); };
const GEN = ["-c", "i=0; while [ $i -lt 30000 ]; do echo line-$i-abcdefghij; i=$((i+1)); done"];

await __kitS.section("oracle output spans many 64 KiB chunks", async () => {
(async () => {
    const want = String(Exec("/bin/sh", GEN, { maxBuffer: 64 << 20 }).stdout);
    ok(want.length > 600000, "oracle output spans many 64 KiB chunks (" + want.length + " bytes)");
    {
        const p = new Spawn("/bin/sh", GEN, { timeoutMs: 20000 });
        const chunks = [];
        for await (const c of p.stdout) chunks.push(c);
        const r = await p.wait();
        eq(r.code, 0, "the child exits cleanly");
        ok(chunks.length > 1, "several chunks arrived (" + chunks.length + ")");
        ok(chunks.every((c) => c instanceof Uint8Array && c.length > 0 && c.length <= 65536), "each chunk is a non-empty Uint8Array of at most 64 KiB");
        ok(new Set(chunks.map((c) => c.buffer)).size === chunks.length, "each chunk owns its own buffer (a copy, not a view)");
        eq(dec(chunks), want, "the chunks concatenate to exactly Exec's stdout");
    }
    {
        const p = new Spawn("/bin/sh", ["-c", "echo out; echo err-text 1>&2"], { timeoutMs: 10000 });
        const errs = [];
        for await (const c of p.stderr) errs.push(c);
        eq(dec(errs), "err-text\n", "stderr iterates too");
        const outs = [];
        for await (const c of p.stdout) outs.push(c);
        eq(dec(outs), "out\n", "stdout still readable after stderr was drained");
        await p.wait();
    }
    {
        const p = new Spawn("/usr/bin/true", [], { timeoutMs: 10000 });
        let k = 0;
        for await (const c of p.stdout) k += c.length;
        eq(k, 0, "an empty output yields no chunk");
        await p.wait();
    }
    {
        const p = new Spawn("/usr/bin/yes", ["endless"], { timeoutMs: 15000 });
        let got = 0;
        for await (const c of p.stdout) { got += c.length; if (got > 200000) break; }
        ok(got > 200000, "read from an endless writer");
        const after = await p.stdout.read(new Uint8Array(16));
        eq(after, 0, "break closed the pipe: a later read() is EOF");
        const r = await p.wait();
        eq(r.timedOut, false, "the endless writer ended (SIGPIPE) instead of blocking until the timeout");
        eq(r.signal, "SIGPIPE", "it died of SIGPIPE once the pipe closed");
    }
    {
        const p = new Spawn("/bin/sh", GEN, { timeoutMs: 20000 });
        const ls = [];
        for await (const l of lines(p.stdout)) ls.push(l);
        await p.wait();
        eq(ls.join("\n") + "\n", want, "dyna:stream lines() consumes a SpawnPipe (the documented route)");
    }
    {
        const p = new Spawn("/bin/sh", ["-c", "echo a"], { timeoutMs: 10000 });
        const it = p.stdout[Symbol.asyncIterator]();
        eq(it[Symbol.asyncIterator](), it, "the iterator is itself async-iterable");
        const r1 = await it.next();
        eq(dec([r1.value]), "a\n", "next() resolves the first chunk");
        const r2 = await it.next();
        eq(r2.done, true, "then done at EOF");
        await p.wait();
    }
    {
        const p = new Spawn("/bin/sh", ["-c", "head -c 1000 /dev/zero | tr '\\0' a"], { timeoutMs: 10000 });
        eq(await p.stdout.read(new Uint8Array(10)), 10, "a first read takes 10 of the 1000 bytes");
        await p.wait();
        await new Promise((r) => setTimeout(r, 50));
        p.stdout.close();
        eq(await p.stdout.read(new Uint8Array(64)), 0, "close() drops the bytes already buffered: a later read() is EOF");
    }
    print("test_sys_spawn_pipe_iter: " + n + " checks, " + bad + " failures");
    if (bad) throw new Error(bad + " failures");
})().catch((e) => { print("FAIL: " + e + "\n" + e.stack); std.exit(1); });
});
__kitS.end();
