// flags: --std
// From 16 live os.setReadHandler/os.setWriteHandler fds the loop keeps them in
// one kqueue/epoll set and puts only that set's fd into poll(), so a wake-up no
// longer costs O(registered fds); it drops back to plain poll() below 8. What
// poll() gave for free and the set must reproduce:
//   - level-triggered readiness, read and write, EOF/hang-up reaches the read
//     handler, one callback per handler per wake (read preferred);
//   - a handler whose fd was closed is dropped, so it cannot keep the loop
//     alive: os.close drops it at once, any other close within a second;
//   - fds the set refuses (regular files under epoll) still work;
//   - moving across the thresholds in both directions loses nothing.
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }
const sleep = (ms) => new Promise((r) => os.setTimeout(r, ms));
const until = async (pred, ms) => { const t0 = Date.now(); while (!pred() && Date.now() - t0 < ms) await sleep(2); return pred(); };

function pipes(k) { const a = []; for (let i = 0; i < k; i++) { const p = os.pipe(); a.push({ r: p[0], w: p[1] }); } return a; }
function closeAll(ps) { for (const p of ps) { os.setReadHandler(p.r, null); os.setWriteHandler(p.w, null); os.close(p.r); os.close(p.w); } }
const one = new Uint8Array(1);
const drain = (fd) => { const b = new Uint8Array(64); return os.read(fd, b.buffer, 0, 64); };

// 1. readiness through the set: exactly the written pipes fire, every round
async function readRounds(count, label) {
    const ps = pipes(count);
    const fired = new Array(count).fill(0);
    ps.forEach((p, i) => os.setReadHandler(p.r, () => { fired[i]++; drain(p.r); }));
    let good = true;
    for (let round = 0; round < 6; round++) {
        const pick = [];
        for (let i = round % 3; i < count; i += 3 + round) pick.push(i);
        fired.fill(0);
        for (const i of pick) os.write(ps[i].w, one.buffer, 0, 1);
        await until(() => pick.every((i) => fired[i] > 0), 2000);
        await sleep(5);
        const extra = fired.findIndex((f, i) => f && !pick.includes(i));
        if (!pick.every((i) => fired[i] === 1) || extra >= 0) { good = false; print("  round " + round + " fired " + fired.join("")); break; }
    }
    ok(good, label + ": exactly the written pipes fire, once each");
    const watched = os.loopStats().watched;
    ok(count >= 16 ? watched === count : watched === 0, label + ": " + (count >= 16 ? "every handler is in the set" : "the set is not used"), watched);
    closeAll(ps);
}
await readRounds(4, "4 handlers (poll path)");
await readRounds(40, "40 handlers (watch set)");
await readRounds(300, "300 handlers (watch set, more ready than one batch)");

// 2. level-triggered: data left unread fires again
{
    const ps = pipes(20);
    let hits = 0;
    os.setReadHandler(ps[7].r, () => { if (++hits === 3) drain(ps[7].r); });
    for (const p of ps) if (p !== ps[7]) os.setReadHandler(p.r, () => {});
    os.write(ps[7].w, one.buffer, 0, 1);
    await until(() => hits >= 3, 2000);
    await sleep(20);
    ok(hits === 3, "unread data fires again until it is read", hits);
    closeAll(ps);
}

// 3. write handlers and hang-up
{
    const ps = pipes(20);
    const w = new Array(20).fill(0);
    ps.forEach((p, i) => os.setWriteHandler(p.w, () => { w[i]++; os.setWriteHandler(p.w, null); }));
    ok(await until(() => w.every((x) => x === 1), 2000), "every writable pipe's write handler fires once", w.join(""));
    let eof = -1;
    ps.forEach((p) => os.setReadHandler(p.r, () => {}));
    os.setReadHandler(ps[3].r, () => { eof = drain(ps[3].r); os.setReadHandler(ps[3].r, null); });
    os.close(ps[3].w);
    ps[3].w = os.pipe()[1];
    ok(await until(() => eof === 0, 2000), "closing the write end reaches the read handler as EOF", eof);
    closeAll(ps);
}

// 4. read preferred, one callback per handler per wake, with both armed
{
    const ps = pipes(20);
    for (const p of ps) os.setReadHandler(p.r, () => {});
    const order = [];
    os.setReadHandler(ps[0].r, () => { order.push("r"); drain(ps[0].r); });
    os.write(ps[0].w, one.buffer, 0, 1);
    os.setWriteHandler(ps[0].w, () => { order.push("w"); os.setWriteHandler(ps[0].w, null); });
    await until(() => order.length >= 2, 2000);
    ok(order.length === 2 && order.includes("r") && order.includes("w"), "a read handler and a write handler on one pipe both run", order.join(""));
    closeAll(ps);
}

// 4b. a handler cleared while its fd stays open is out of the set: data that
//     arrives afterwards calls nothing (a stale registration would hand the
//     loop a freed handler)
{
    const ps = pipes(24);
    let stale = 0, live = 0;
    ps.forEach((p, i) => os.setReadHandler(p.r, i < 12 ? () => { stale++; } : () => { live++; drain(p.r); }));
    await sleep(5);
    for (let i = 0; i < 12; i++) os.setReadHandler(ps[i].r, null);
    for (let i = 0; i < 24; i++) os.write(ps[i].w, one.buffer, 0, 1);
    await until(() => live >= 12, 2000);
    await sleep(20);
    ok(stale === 0 && live === 12, "cleared handlers stay silent, live ones fire", stale + "/" + live);
    closeAll(ps);
}

// 5. a regular file among them (epoll refuses it; poll() calls it readable)
{
    const path = (std.getenv("TMPDIR") || "/tmp") + "/dj_evloop_watch_" + os.getpid();
    const f = std.open(path, "w"); f.puts("x"); f.close();
    const fd = os.open(path, os.O_RDONLY);
    const ps = pipes(20);
    for (const p of ps) os.setReadHandler(p.r, () => {});
    let hit = 0;
    os.setReadHandler(fd, () => { hit++; os.setReadHandler(fd, null); });
    ok(await until(() => hit === 1, 2000), "a regular-file fd in the set still fires");
    os.close(fd); os.remove(path);
    closeAll(ps);
}

// 5b. one fd readable AND writable (a regular file opened read-write): like
//     poll(), each wake runs one callback for it and the read handler wins;
//     the write handler runs once the read handler is gone
{
    const path = (std.getenv("TMPDIR") || "/tmp") + "/dj_evloop_watch_rw_" + os.getpid();
    const f = std.open(path, "w"); f.puts("abc"); f.close();
    const fd = os.open(path, os.O_RDWR);
    const ps = pipes(20);
    for (const p of ps) os.setReadHandler(p.r, () => {});
    const order = [];
    os.setReadHandler(fd, () => { order.push("r"); if (order.length === 5) os.setReadHandler(fd, null); });
    os.setWriteHandler(fd, () => { order.push("w"); os.setWriteHandler(fd, null); });
    await until(() => order.length >= 6, 2000);
    ok(order.join("") === "rrrrrw", "read wins while both are ready, then write runs", order.join(""));
    os.close(fd); os.remove(path);
    closeAll(ps);
}

// 6. thresholds both ways: grow 4 -> 40, shrink to 2, grow again
{
    const ps = pipes(40);
    const fired = new Array(40).fill(0);
    const arm = (i) => os.setReadHandler(ps[i].r, () => { fired[i]++; drain(ps[i].r); });
    let good = true, prevWatched = false;
    for (const live of [4, 40, 12, 2, 30]) {
        for (let i = 0; i < 40; i++) if (i < live) arm(i); else os.setReadHandler(ps[i].r, null);
        fired.fill(0);
        for (let i = 0; i < live; i++) os.write(ps[i].w, one.buffer, 0, 1);
        if (!await until(() => fired.slice(0, live).every((x) => x === 1), 2000)) { good = false; print("  at " + live + ": " + fired.join("")); }
        const want = live >= 16 || (live >= 8 && prevWatched) ? live : 0;
        const got = os.loopStats().watched;
        if (got !== want) { good = false; print("  at " + live + ": watched " + got + ", want " + want); }
        prevWatched = got > 0;
    }
    ok(good, "nothing is lost while crossing the thresholds");
    closeAll(ps);
}

// 7. the loop stays idle: 64 quiet handlers, a 300 ms timer
{
    const ps = pipes(64);
    for (const p of ps) os.setReadHandler(p.r, () => {});
    await sleep(10);
    const w0 = os.loopStats().waits;
    await sleep(300);
    const waits = os.loopStats().waits - w0;
    ok(waits <= 3, "64 quiet handlers do not wake the loop", waits);
    closeAll(ps);
}

// 8. a closed fd cannot keep the loop alive. The child sets 20 handlers, then
//    closes one fd behind the loop's back (std FILE close, not os.close) and
//    clears the rest; it must exit by itself.
function child(src) {
    const path = (std.getenv("TMPDIR") || "/tmp") + "/dj_evloop_watch_child_" + os.getpid() + ".js";
    const f = std.open(path, "w"); f.puts(src); f.close();
    const t0 = Date.now();
    const pid = os.exec([std.getenv("DJ_BIN") || "./dynajs", "--std", path], { blocking: false });
    let st = null;
    while (Date.now() - t0 < 8000) {
        const r = os.waitpid(pid, os.WNOHANG);
        if (r[0] === pid) { st = r[1]; break; }
        os.sleep(10);
    }
    if (st === null) { os.kill(pid, 9); os.waitpid(pid); }
    os.remove(path);
    return { st, ms: Date.now() - t0 };
}
const childHead = `import * as os from "os"; import * as std from "std";
const ps = []; for (let i = 0; i < 20; i++) ps.push(os.pipe());
for (const p of ps) os.setReadHandler(p[0], () => {});
`;
{
    // ten closed behind the loop's back keep it above the leave threshold, so
    // only the set's own sweep can find them
    const r = child(childHead + `os.setTimeout(() => { for (let i = 0; i < 10; i++) std.fdopen(ps[i][0], "r").close(); for (let i = 10; i < 20; i++) os.setReadHandler(ps[i][0], null); }, 20);\n`);
    ok(r.st === 0 && r.ms < 4000, "handlers whose fds were closed elsewhere are reaped (child exited)", JSON.stringify(r));
}
{
    const r = child(childHead + `os.setTimeout(() => { for (let i = 0; i < 20; i++) os.close(ps[i][0]); }, 20);\n`);
    ok(r.st === 0 && r.ms < 1500, "os.close drops its handler at once (child exited)", JSON.stringify(r));
}

print("test_evloop_watch: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
