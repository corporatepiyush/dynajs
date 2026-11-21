// flags: --std
// The child side of test_stdio_streams.js: argv[1] picks the mode.
import { stdin, stdout, stderr } from "dyna:stream";
const mode = scriptArgs[1];
if (mode === "upper") {
    std.out.puts("first\n");
    await stdin().pipeThrough(new TransformStream({ transform(c, ctl) { ctl.enqueue("[" + new TextDecoder().decode(c).toUpperCase() + "]"); } })).pipeTo(stdout());
    console.log("\nlast");
} else if (mode === "count") {
    let n = 0;
    for await (const c of stdin()) n += c.length;
    console.log("bytes " + n);
} else if (mode === "err") {
    const e = stderr().getWriter();
    await e.write("to-stderr");
    await e.close();
    await (e.write("x").then(() => console.log("late write ok"), (x) => console.log("late write: " + x.message)));
} else if (mode === "same") {
    console.log(String(stdin() === stdin()));
    const s = stdin();
    const rd = s.getReader();
    const r = rd.read();
    await rd.cancel();
    console.log("after cancel " + (await r).done + " fresh " + String(stdin() !== s) + " same " + String(stdin() === stdin()));
}
if (mode === "tick") {
    let ticks = 0;
    const iv = setInterval(() => { ticks++; }, 10);
    const n = (await stdin().getReader().read()).value.length;
    clearInterval(iv);
    console.log("read " + n + " ticks " + (ticks > 3 ? "many" : ticks));
}
function pattern(n, seed) {
    const b = new Uint8Array(n);
    let x = seed >>> 0;
    for (let i = 0; i < n; i++) { x = (Math.imul(x, 1103515245) + 12345) >>> 0; b[i] = x >>> 23; }
    return b;
}
const report = (o) => std.err.puts(JSON.stringify(o) + "\n");
if (mode === "epipe") {
    const w = stdout().getWriter();
    await w.write("first\n");
    await new Promise((r) => setTimeout(r, 400));
    let e1 = null, e2 = null, e3 = null;
    await w.write(new Uint8Array(70000)).catch((e) => { e1 = e; });
    await w.write("x").catch((e) => { e2 = e; });
    await stdout().getWriter().write("y").catch((e) => { e3 = e; });
    report({ m: e1 && e1.message, sticky: e1 !== null && e1 === e2, fresh: e3 && e3.message });
} else if (mode === "nb") {
    const { Spawn } = await import("dyna:sys");
    const os = await import("os");
    const before = new Set(os.readdir("/dev/fd")[0]);
    const p = new Spawn("/bin/sh", ["-c", "sleep 0.4; exec cat > \"$0\"", scriptArgs[2]], { stdin: "pipe" });
    const z = new ArrayBuffer(1);
    const wfd = os.readdir("/dev/fd")[0].filter((f) => !before.has(f)).map(Number).find((fd) => os.write(fd, z, 0, 0) === 0);
    const saved = os.dup(1);
    os.dup2(wfd, 1);
    const sizes = [200001, 1, 70000, 3, 400000, 65536, 17, 300000];
    const total = sizes.reduce((a, b) => a + b, 0), data = pattern(total, 7);
    const w = stdout().getWriter();
    let off = 0;
    for (const n of sizes) { await w.write(data.subarray(off, off + n)); off += n; }
    await w.close();
    os.dup2(saved, 1);
    os.close(saved);
    p.stdin.close();
    const r = await p.wait();
    const got = new Uint8Array(total + 16), fd = os.open(scriptArgs[2], os.O_RDONLY);
    let len = 0, k;
    while ((k = os.read(fd, got.buffer, len, got.length - len)) > 0) len += k;
    os.close(fd);
    let same = len === total;
    for (let i = 0; same && i < total; i++) same = got[i] === data[i];
    report({ code: r.code, total, got: len, same, wfd: wfd > 2 });
} else if (mode === "small") {
    const rd = stdin().getReader();
    const out = [];
    for (;;) {
        for (let k = 0; k < 4; k++) new Uint8Array(65536).fill(255);
        let r = await rd.read();
        if (r.done) break;
        const c = r.value;
        r = null;
        out.push([c.byteLength, c.buffer.byteLength, new Uint8Array(c.buffer, c.byteOffset + c.byteLength).every((v) => v === 0)]);
    }
    report(out);
} else if (mode === "byob") {
    const rd = stdin().getReader({ mode: "byob" });
    const r = await rd.read(new Uint8Array(new ArrayBuffer(32), 3, 10));
    report({ n: r.value.byteLength, off: r.value.byteOffset, s: new TextDecoder().decode(r.value) });
} else if (mode === "cancel") {
    const rd = stdin().getReader();
    const pend = rd.read();
    await new Promise((r) => setTimeout(r, 50));
    await rd.cancel();
    const v = await pend;
    report({ done: v.done, fd0: std.fdopen(0, "r") !== null });
}
