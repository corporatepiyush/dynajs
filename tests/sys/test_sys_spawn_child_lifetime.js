// flags: --std
// covers: dyna:sys.Spawn
// F33: every Exec/Spawn child calls setsid() (it must stay killable as a group), so it left the parent's
// process group and outlived a parent that was killed or exited (timeout, bounded-run group kill, SIGTERM).
// The parent now tracks each live child group (src/core/dyn-childreg.c) and on its own exit signals them:
// SIGTERM, then SIGKILL after a 2 s grace, and reaps. Paths: normal exit (atexit), uncaught error exit
// (sync throw and the F31 async shape with a parked read), SIGTERM/SIGINT/SIGHUP while parked in
// Spawn.wait() or blocked inside Exec, and the --timeout-ms hard stop (exit 113). The parent must still
// die BY the signal it got (the hook re-raises with the old disposition). A child that ignores SIGTERM
// gets the SIGKILL follow-up. Each fixture child is `sleep 30` with a second `sleep 30` in its group, so
// "gone" means the whole group, checked with kill -0 -PGID (bounded poll, zombies are reaped by init). No `--`: dash (Debian /bin/sh)
// rejects `kill -0 -- -PGID` ("Illegal number: -") and busybox (Alpine) rejects any `--` ("invalid number").
// A signal the test's own process tree ignores (a non-interactive shell's `cmd &` ignores SIGINT; nohup
// ignores SIGHUP) is inherited as SIG_IGN; the hook then rightly stays off and the parent survives: that
// case is reported as a SKIP, never a pass. Any group left alive by a failing case is SIGKILLed here.
// Untrack on reap (d.ts: a group is tracked "until it reaps it"): once Spawn.wait() or Exec has reaped the
// leader, its slot is freed, so the exit path never signals that pgid again (it could name a recycled
// group) and 4096 sequential children never fill the table. Proven without 4096 spawns: the leader leaves
// a background `sleep 30` behind in its group and exits; after the reap the parent exits and that sleep
// must still be alive (a stale slot would have it SIGTERMed by the exit path). Mutants: drop the
// dyn_childreg_del in spawn_record_status (Spawn) or after Exec's waitpid -- each turns its case red.
import { suite } from "../lib/kit.js";
import { args, Exec, Spawn } from "dyna:sys";
import * as file from "dyna:file";
import { Path } from "dyna:file";

const t = suite("test_sys_spawn_child_lifetime");
const BIN = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];
const DIR = file.makeTempDir("childlife_");
const GROUP = "sleep 30 & echo up; exec sleep 30";

const FIXTURE = `
import { Spawn, Exec } from "dyna:sys";
import * as std from "std";
const mode = scriptArgs[1], pidFile = scriptArgs[2];
const LEFT = "sleep 30 </dev/null >/dev/null 2>&1 & echo $$";
if (mode === "reaped-exec") {
    std.out.puts(String(Exec("sh", ["-c", LEFT]).stdout).trim() + "\\n"); std.out.flush();
    std.exit(0);
}
if (mode === "reaped-spawn") {
    const q = new Spawn("sh", ["-c", LEFT], { stdin: "ignore" });
    const b = new Uint8Array(32), n = await q.stdout.read(b);
    await q.wait();
    std.out.puts(new TextDecoder().decode(b.subarray(0, n)).trim() + "\\n"); std.out.flush();
    std.exit(0);
}
const cmd = mode === "stubborn" ? "trap '' TERM; sleep 30 & echo up; exec sleep 30" : ${JSON.stringify(GROUP)};
if (mode === "exec" || mode === "hard") {
    Exec("sh", ["-c", "echo $$ > " + pidFile + "; " + cmd]);
} else {
    const p = new Spawn("sh", ["-c", cmd], { stdin: "ignore" });
    await p.stdout.read(new Uint8Array(16));
    std.out.puts(p.pid + "\\n"); std.out.flush();
    if (mode === "exit" || mode === "stubborn") std.exit(0);
    if (mode === "throw") throw new TypeError("fixture throws with a live child");
    if (mode === "async-throw") {
        (async () => { const b = new Uint8Array(64); for (;;) if (await p.stdout.read(b) <= 0) break; })();
        await (async () => { await null; undefined.code; })();
    }
    await p.wait();
}
`;
const FIX = DIR.join("fixture.js");
file.writeFile(FIX, FIXTURE);

const groupAlive = (pg) => Exec("sh", ["-c", "kill -0 -" + pg + " 2>/dev/null"]).code === 0;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function goneWithin(pg, ms) {
    const end = Date.now() + ms;
    for (;;) {
        if (!groupAlive(pg)) return true;
        if (Date.now() >= end) return false;
        await sleep(25);
    }
}
function reapLeftover(pg) { if (pg > 0 && groupAlive(pg)) Exec("sh", ["-c", "kill -9 -" + pg + " 2>/dev/null"]); }

async function readPid(parent, mode, pidFile) {
    const end = Date.now() + 5000;
    if (mode === "exec" || mode === "hard") {
        while (Date.now() < end) {
            const s = file.exists(new Path(pidFile)) ? String(file.readFile(new Path(pidFile))).trim() : "";
            if (/^\d+$/.test(s)) return Number(s);
            await sleep(20);
        }
        return 0;
    }
    const buf = new Uint8Array(64);
    let txt = "";
    while (Date.now() < end && !txt.includes("\n")) {
        const n = await parent.stdout.read(buf);
        if (n <= 0) break;
        txt += new TextDecoder().decode(buf.subarray(0, n));
    }
    return Number(txt.trim()) || 0;
}

async function run(a, mode, sig, flags) {
    const pidFile = String(DIR.join(mode + "-" + (sig || "none") + ".pid"));
    const parent = new Spawn(BIN, ["--std", ...(flags || []), String(FIX), mode, pidFile], { stdin: "ignore", timeoutMs: 20000 });
    const pg = await readPid(parent, mode, pidFile);
    try {
        a.must(pg > 0, mode + ": the fixture reported its child's pid");
        a.ok(mode === "exit" || mode === "throw" || mode === "async-throw" || mode === "stubborn" || groupAlive(pg), mode + ": the child group is alive before the parent goes");
        const t0 = Date.now();
        if (sig) { await sleep(100); parent.kill(sig); }
        let r = null;
        const waited = parent.wait().then((v) => (r = v));
        let timer;
        await Promise.race([waited, new Promise((res) => { timer = setTimeout(res, sig ? 1500 : 8000); })]);
        clearTimeout(timer);
        if (!r && sig) {
            a.eq(groupAlive(pg), true, mode + "/" + sig + ": the parent survived, so its child must too");
            parent.kill("SIGKILL");
            await waited;
            reapLeftover(pg);
            a.skip(sig + " is ignored in this process tree (inherited SIG_IGN): the hook stays off and nothing is orphaned");
        }
        a.must(r !== null, mode + ": the parent exited");
        return { r, pg, ms: Date.now() - t0 };
    } finally {
        if (!(await goneWithin(pg, 3000))) { a.ok(false, mode + "/" + (sig || "none") + ": child group " + pg + " outlived the parent"); reapLeftover(pg); }
        else a.ok(true);
        parent.close();
    }
}

t.test("normal exit (std.exit) takes the child group down", async (a) => {
    const { r } = await run(a, "exit");
    a.eq(r.code, 0, "exit code");
}, { timeoutMs: 15000 });

t.test("uncaught synchronous error exit takes the child group down", async (a) => {
    const { r } = await run(a, "throw");
    a.eq(r.code, 1, "exit code");
}, { timeoutMs: 15000 });

t.test("uncaught error in an async fn with a parked read exits 1, no abort (F31), child gone", async (a) => {
    const { r } = await run(a, "async-throw");
    a.eq(r.signal, null, "not killed by a signal (SIGABRT was F31)");
    a.eq(r.code, 1, "exit code");
}, { timeoutMs: 15000 });

for (const sig of ["SIGTERM", "SIGINT", "SIGHUP"]) {
    t.test(sig + " while parked in Spawn.wait(): parent dies by " + sig + ", child group gone", async (a) => {
        const { r } = await run(a, "wait", sig);
        a.eq(r.signal, sig, "the parent still dies by the signal it got");
    }, { timeoutMs: 15000 });
}

t.test("SIGTERM while blocked inside Exec: the Exec child group goes too", async (a) => {
    const { r } = await run(a, "exec", "SIGTERM");
    a.eq(r.signal, "SIGTERM", "death signal");
}, { timeoutMs: 15000 });

t.test("--timeout-ms hard stop (exit 113) while blocked inside Exec takes the child group down", async (a) => {
    const { r } = await run(a, "hard", null, ["--timeout-ms", "300"]);
    a.eq(r.code, 113, "hard-stop exit code");
}, { timeoutMs: 20000 });

t.test("a child group that ignores SIGTERM gets SIGKILL after the grace", async (a) => {
    const { r, ms } = await run(a, "stubborn");
    a.eq(r.code, 0, "exit code");
    a.ok(ms >= 1500, "the parent waited the grace before SIGKILL (" + ms + " ms)");
}, { timeoutMs: 15000 });

for (const mode of ["reaped-spawn", "reaped-exec"]) {
    t.test(mode + ": a reaped child's group is untracked, so the exit path leaves what remains of it alone", async (a) => {
        const parent = new Spawn(BIN, ["--std", String(FIX), mode, ""], { stdin: "ignore", timeoutMs: 20000 });
        const pg = await readPid(parent, "spawn", "");
        try {
            a.must(pg > 0, mode + ": the fixture reported the reaped leader's group");
            const r = await parent.wait();
            a.eq(r.code, 0, mode + ": exit code");
            await sleep(100);
            a.eq(groupAlive(pg), true, mode + ": the left-behind sleep in reaped group " + pg + " was not signalled at exit");
        } finally {
            reapLeftover(pg);
            parent.close();
        }
    }, { timeoutMs: 15000 });
}

t.after(() => file.removeAll(DIR));
await t.run();
