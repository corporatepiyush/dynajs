// bench: dyna:file sync vs file.async readFile/writeFile. Env: FB_MODE=sync|async|par|rounds, FB_N=calls, FB_SIZE=bytes (default 4096), FB_OP=read|write.
// Prints one JSON line: mode, n, ms, usPerOp, peakRss, rss growth per round for "rounds" (5 x FB_N async reads, plateau check).
import * as file from "dyna:file";
import * as sys from "dyna:sys";
import * as time from "dyna:time";
const env = (k, d) => { try { const v = sys.getEnv(k); return v === undefined || v === null || v === "" ? d : v; } catch { return d; } };
const mode = env("FB_MODE", "async"), n = +env("FB_N", "1000"), size = +env("FB_SIZE", "4096"), op = env("FB_OP", "read");
const p = new file.Path(file.Path.temp(), "dj_fbench_" + Date.now() + "_" + Math.random().toString(36).slice(2, 7));
const data = new Uint8Array(size).fill(0x61);
file.writeFile(p, data);
const now = () => Number(time.monotonicNano()) / 1e6;
const rss = () => sys.memoryUsage().peakRss;
const mode2 = mode === "rounds" ? "async" : mode;
async function run(count) {
    if (mode2 === "sync") { for (let i = 0; i < count; i++) op === "read" ? file.readFile(p) : file.writeFile(p, data); return; }
    if (mode2 === "par") {
        const W = 64;
        for (let i = 0; i < count; i += W) {
            const batch = [];
            for (let k = 0; k < W && i + k < count; k++) batch.push(op === "read" ? file.async.readFile(p) : file.async.writeFile(p, data));
            await Promise.all(batch);
        }
        return;
    }
    for (let i = 0; i < count; i++) await (op === "read" ? file.async.readFile(p) : file.async.writeFile(p, data));
}
const r0 = rss();
if (mode === "rounds") {
    const per = [];
    for (let r = 0; r < 5; r++) { const t = now(); await run(n); per.push({ ms: Math.round(now() - t), peakRss: rss() }); }
    print(JSON.stringify({ mode, op, n, size, rss0: r0, rounds: per }));
} else {
    const t = now();
    await run(n);
    const ms = now() - t;
    print(JSON.stringify({ mode, op, n, size, ms: Math.round(ms), usPerOp: +(ms * 1000 / n).toFixed(2), peakRss: rss(), rssGrowth: rss() - r0, bytesPerOp: +((rss() - r0) / n).toFixed(1) }));
}
file.remove(p);
