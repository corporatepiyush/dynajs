// flags: --std
// The child side of test_stdio_coalesce.js: argv[1] picks the mode.
import { stdout, stderr } from "dyna:stream";
const mode = scriptArgs[1];
const report = (o) => std.err.puts(JSON.stringify(o) + "\n");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export function pattern(n, seed) {
    const b = new Uint8Array(n);
    let x = seed >>> 0;
    for (let i = 0; i < n; i++) { x = (Math.imul(x, 1103515245) + 12345) >>> 0; b[i] = x >>> 23; }
    return b;
}
export const SIZES = [1, 63, 16383, 16384, 5, 70000, 2, 65535, 65536, 7, 300000, 1, ...Array.from({ length: 120 }, (_, i) => 700 + (i * 37) % 911), 16385, 3, 9000, 9000, 9000, 9000, 9000, 9000, 9000, 9000, 1];
if (mode === "mixed") {
    const total = SIZES.reduce((a, b) => a + b, 0), data = pattern(total, 99);
    const w = stdout().getWriter();
    let off = 0, k = 0;
    for (const n of SIZES) {
        const p = w.write(data.subarray(off, off + n));
        if (k++ % 3 !== 2) await p;
        off += n;
    }
    await w.close();
} else if (mode === "console") {
    const w = stdout().getWriter(), w2 = stdout().getWriter();
    await w.write("a");
    console.log("b");
    await w.write("c");
    std.out.puts("d");
    await w.write("e");
    print("f");
    await w.write("g");
    std.printf("%s", "h");
    await w2.write("i");
    os.write(1, new TextEncoder().encode("j").buffer, 0, 1);
    await w.write("k");
    std.out.write(new TextEncoder().encode("l").buffer, 0, 1);
    await w.write(new Uint8Array(20000).fill(109));
    console.log("n");
    await w.write("o");
} else if (mode === "exit") {
    await stdout().getWriter().write("exit-bytes\n");
    std.exit(Number(scriptArgs[2] || 0));
} else if (mode === "throw") {
    await stdout().getWriter().write("before-throw\n");
    throw new Error("boom");
} else if (mode === "reject") {
    await stdout().getWriter().write("before-reject\n");
    Promise.reject(new Error("nobody-catches"));
} else if (mode === "end") {
    stdout().getWriter().write("tail\n");
} else if (mode === "epipe") {
    const w = stdout().getWriter();
    await w.write("first\n");
    await sleep(400);
    let r1 = "pending", r2 = "pending", r3 = "pending";
    await w.write("x").then(() => { r1 = "ok"; }, (e) => { r1 = e.message; });
    await sleep(20);
    await w.write("y").then(() => { r2 = "ok"; }, (e) => { r2 = e.message; });
    await stdout().getWriter().write("z").then(() => { r3 = "ok"; }, (e) => { r3 = e.message; });
    report({ r1, r2, r3 });
} else if (mode === "epipe-close") {
    const w = stdout().getWriter();
    await w.write("first\n");
    await sleep(400);
    await w.write("x");
    let c = "ok";
    await w.close().catch((e) => { c = e.message; });
    report({ c });
} else if (mode === "epipe-exit") {
    const w = stdout().getWriter();
    await w.write("first\n");
    await sleep(400);
    await w.write("x");
} else if (mode === "order") {
    const o = stdout().getWriter(), e = stderr().getWriter();
    await o.write("o1");
    await e.write("e1");
    console.log("o2");
    console.error("e2");
    await o.write("o3");
    await o.write("o4");
    await e.write("e3");
    std.err.puts("e4");
    await o.write("o5");
    await e.write("e5");
    std.out.puts("o6");
    await e.write("e6");
} else if (mode === "nbsmall") {
    const { Spawn } = await import("dyna:sys");
    const before = new Set(os.readdir("/dev/fd")[0]);
    const p = new Spawn("/bin/sh", ["-c", "sleep 0.4; exec cat > \"$0\"", scriptArgs[2]], { stdin: "pipe" });
    const z = new ArrayBuffer(1);
    const wfd = os.readdir("/dev/fd")[0].filter((x) => !before.has(x)).map(Number).find((fd) => os.write(fd, z, 0, 0) === 0);
    const saved = os.dup(1);
    os.dup2(wfd, 1);
    const sizes = Array.from({ length: 400 }, (_, i) => 1 + (i * 7919) % 1500);
    const total = sizes.reduce((a, b) => a + b, 0), data = pattern(total, 5);
    const w = stdout().getWriter();
    const t0 = Date.now();
    let off = 0;
    for (const n of sizes) { await w.write(data.subarray(off, off + n)); off += n; }
    await w.close();
    const waited = Date.now() - t0;
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
    report({ code: r.code, total, got: len, same, wfd: wfd > 2, waited: waited >= 300 });
} else if (mode === "file") {
    const path = scriptArgs[2], size = () => os.stat(path)[0].size;
    const w = stdout().getWriter();
    const s = [];
    await w.write("abc");
    s.push(size());
    await sleep(0);
    s.push(size());
    await w.write("de");
    s.push(size());
    await w.close();
    s.push(size());
    report(s);
}
