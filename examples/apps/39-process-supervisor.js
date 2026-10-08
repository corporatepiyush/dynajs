// 39 · Process supervisor — runs child programs, streams their output, restarts crashes with backoff.
//
// WHAT IT SHOWS
//   - dyna:sys Spawn: asynchronous children with no shell in between (arguments are never re-parsed)
//   - dyna:stream lines(): a child's stdout as an async stream of lines
//   - restart policy: exponential backoff, a restart budget, and "give up" as an explicit state
//   - timeouts and clean shutdown that leave no orphaned processes
//
// RUN      dynajs examples/apps/39-process-supervisor.js
//          The Supervisor class is the reusable part; the demo supervises small shell scripts.

import { Spawn, Which } from "dyna:sys";
import { lines } from "dyna:stream";

class Supervisor {
    constructor({ maxRestarts = 3, backoffMs = 20, log = () => {} } = {}) {
        this.maxRestarts = maxRestarts; this.backoffMs = backoffMs; this.log = log;
        this.services = new Map();
    }

    // Start a service and keep it running. Resolves when it has stopped for
    // good: exited cleanly, exhausted its restarts, or was stopped by us.
    run(name, command, args = [], opts = {}) {
        const state = { name, restarts: 0, status: "starting", output: [], child: null, stopping: false };
        this.services.set(name, state);
        state.finished = this.#loop(state, command, args, opts);
        return state.finished;
    }

    async #loop(state, command, args, opts) {
        for (;;) {
            // argv is passed as an array: a name such as "a; rm -rf /" is one
            // argument to the program, never a second command.
            const child = new Spawn(command, args, { timeoutMs: opts.timeoutMs, env: opts.env });
            state.child = child;
            state.status = "running";
            this.log(`${state.name}: started pid ${child.pid}`);

            // Drain both pipes while the child runs. If nobody reads, the
            // child eventually blocks on a full pipe: that is back-pressure.
            const pump = async (pipe, stream) => {
                for await (const line of lines(pipe)) {
                    state.output.push({ stream, line });
                    if (state.output.length > 1000) state.output.shift();     // bounded history
                }
            };
            const pumps = Promise.all([pump(child.stdout, "out"), pump(child.stderr, "err")]).catch(() => {});
            const result = await child.wait();
            // The child is gone, but a grandchild it forked may still hold the
            // pipes open, so "wait for end of output" could wait forever. Give
            // trailing output a moment, then close(): that SIGKILLs whatever is
            // left of the process group and ends the pending reads.
            await Promise.race([pumps, sleep(200)]);
            child.close();
            await pumps;

            if (state.stopping) { state.status = "stopped"; return state; }
            if (result.timedOut) this.log(`${state.name}: killed after timeout`);
            if (result.code === 0) { state.status = "exited"; return state; }

            if (state.restarts >= this.maxRestarts) {
                state.status = "failed";
                this.log(`${state.name}: giving up after ${state.restarts} restarts`);
                return state;
            }
            // 20ms, 40ms, 80ms ...: a crash loop must not become a fork bomb.
            const delay = this.backoffMs * 2 ** state.restarts++;
            this.log(`${state.name}: exit ${result.code ?? result.signal}, restart ${state.restarts} in ${delay}ms`);
            state.status = "backoff";
            await sleep(delay);
            if (state.stopping) { state.status = "stopped"; return state; }
        }
    }

    async stop(name) {
        const state = this.services.get(name);
        if (!state || state.status === "exited" || state.status === "failed") return;
        state.stopping = true;
        state.child?.kill("SIGTERM");               // signals the whole process group
        await state.finished;
    }

    async stopAll() { await Promise.all([...this.services.keys()].map((n) => this.stop(n))); }
    status() { return Object.fromEntries([...this.services].map(([n, s]) => [n, s.status])); }
}

// ---- demo / self-test ------------------------------------------------------
const sh = Which("sh");
if (!sh) throw new Error("this demo needs a POSIX shell on PATH");
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const events = [];
const sup = new Supervisor({ maxRestarts: 2, backoffMs: 10, log: (m) => events.push(m) });

// 1. A job that prints to both streams and exits cleanly: no restart.
const job = await sup.run("report", sh, ["-c", "echo building; echo warning: slow disk 1>&2; echo done"]);
check(job.status === "exited" && job.restarts === 0, "a clean exit is not restarted");
check(job.output.filter((o) => o.stream === "out").map((o) => o.line).join() === "building,done", "stdout is captured line by line");
check(job.output.some((o) => o.stream === "err" && o.line.includes("slow disk")), "stderr is captured separately");

// 2. A service that always crashes: restarted with backoff, then abandoned.
const started = Date.now();
const crasher = await sup.run("crasher", sh, ["-c", "echo boot; exit 3"]);
check(crasher.status === "failed" && crasher.restarts === 2, "a crash loop is restarted up to the budget, then given up");
check(crasher.output.length === 3, "it ran three times in total");
check(Date.now() - started >= 30, "restarts wait for the backoff (10ms + 20ms)");

// 3. A hang: the timeout kills it; with no restarts left it is marked failed.
const strict = new Supervisor({ maxRestarts: 0, log: (m) => events.push(m) });
const hung = await strict.run("hung", sh, ["-c", "sleep 30"], { timeoutMs: 100 });
check(hung.status === "failed" && events.some((e) => e.includes("killed after timeout")), "a hung child is killed by its timeout");

// 4. A long-running service stopped on request.
const server = sup.run("server", sh, ["-c", "echo listening; sleep 30"]);
while (!sup.services.get("server").output.length) await sleep(5);     // wait until it is up
check(sup.status().server === "running", "the service reports running");
await sup.stopAll();
check((await server).status === "stopped", "stop() ends it without counting a crash");

// 5. Arguments are data. This would be catastrophic through a shell.
const echo = await sup.run("args", Which("echo") ?? "echo", ["hello; rm -rf /tmp/never && echo pwned"]);
check(echo.output[0].line === "hello; rm -rf /tmp/never && echo pwned", "shell metacharacters arrive as literal text");

console.log(events.join("\n"));
console.log("self-test passed:", JSON.stringify(sup.status()));
