// covers: integration c:src/modules/libc/libc-stdout.c c:src/modules/libc/libc-event-loop.c c:src/modules/libc/libc-os.c -- one behaviour across the loop, top-level await and os.sleep
// flags: --std
// C2b: output written through C stdio (std.out / std.err puts, printf, write) sat in the FILE buffer, which is fully
// buffered when stdout is a pipe, while the event loop idled on a long timer, a top-level await or os.sleep: a reader of
// the pipe saw nothing until exit. The loop already drained the runtime's own print/console buffer before blocking but
// not the C stdio FILEs. dyn_stdio_idle() now drains both right before the loop polls (js_std_loop, js_std_await) and
// before os.sleep. Between blocks nothing changes: a TTY stays line-buffered and a pipe stays block-buffered while the
// script runs, so bursts still coalesce. Each case spawns this file as a child on a pipe, which writes and then blocks
// for 30 s; the parent must read the line within 5 s and then kills the child. Not covered here: a TTY stdout (needs a pty).
import * as std from "std";
import * as os from "os";
import { Spawn, args } from "dyna:sys";
import { suite } from "../lib/kit.js";

const SELF = scriptArgs[0];
const MODES = {
    "std.out.puts + timer": { out: "A-puts\n", run: () => { std.out.puts("A-puts\n"); setTimeout(() => {}, 30000); } },
    "std.printf partial line + timer": { out: "B-partial", run: () => { std.printf("B-partial"); setTimeout(() => {}, 30000); } },
    "std.out.write + top-level await": { out: "C-write\n", run: async () => { const b = new TextEncoder().encode("C-write\n"); std.out.write(b.buffer, 0, b.length); await new Promise((r) => setTimeout(r, 30000)); } },
    "std.out.puts + os.sleep": { out: "D-sleep\n", run: () => { std.out.puts("D-sleep\n"); os.sleep(30000); } },
    "print + console.log + std.out.puts interleaved": { out: "E1\nE2\nE3\n", run: () => { print("E1"); std.out.puts("E2\n"); console.log("E3"); setTimeout(() => {}, 30000); } },
    "std.err.puts on stderr + timer": { err: "F-err\n", run: () => { std.err.puts("F-err\n"); setTimeout(() => {}, 30000); } },
};

if (scriptArgs[1] === "--child") {
    await MODES[scriptArgs[2]].run();
} else {
    const t = suite("test_stdout_idle_flush");
    const BIN = args()[0];
    async function readUntil(pipe, want, ms) {
        const dec = new TextDecoder();
        let got = "";
        const deadline = Date.now() + ms;
        const buf = new Uint8Array(4096);
        while (got.length < want.length && Date.now() < deadline) {
            let timer;
            const n = await Promise.race([pipe.read(buf), new Promise((r) => { timer = setTimeout(() => r(-1), deadline - Date.now()); })]);
            clearTimeout(timer);
            if (n <= 0) break;
            got += dec.decode(buf.subarray(0, n));
        }
        return got;
    }
    for (const [mode, m] of Object.entries(MODES)) {
        t.test(mode, async (a) => {
            const p = new Spawn(BIN, ["--std", SELF, "--child", mode], { stdin: "ignore" });
            try {
                const got = await readUntil(m.err ? p.stderr : p.stdout, m.err || m.out, 5000);
                a.eq(got, m.err || m.out, "bytes on the pipe while the child is blocked");
                a.eq(p.exited, false, "child is still blocked, so the bytes did not come from exit");
            } finally {
                p.close();
            }
        }, { timeoutMs: 15000 });
    }
    await t.run();
}
