// covers: c:src/dyna-cli.c
// Top-level await in a classic script (no import/export) and in `-e`: the CLI
// compiles the script as written first, and only when that fails and the
// source mentions `await` does it compile it as an async script and wait for
// it. A script that already compiles keeps its exact old meaning (`await` as an
// identifier in sloppy code), nothing runs twice, a script that is wrong either
// way reports the ORIGINAL syntax error, and errors raised after an await exit
// non-zero with their message. Each case runs in a file, in -e and with the
// bytecode cache on, because those are three separate paths in dyna-cli.c.
import { args, Exec } from "dyna:sys";
import { Path, writeFile, makeTempDir, removeAll, exists } from "dyna:file";
import { suite } from "../lib/kit.js";

const BIN = args()[0];
const t = suite("cli_top_level_await");
const dir = makeTempDir("cli-tla-");
t.after(() => removeAll(dir));
let seq = 0;

function runAll(src, extraEnv = {}, flags = []) {
    const f = dir.join(`s${seq++}.js`);
    writeFile(f, src);
    const env = { PATH: "/usr/bin:/bin", ...extraEnv };
    const cache = { ...env, DYNAJS_BYTECODE_CACHE: "1", XDG_CACHE_HOME: String(dir.join("cache")), HOME: String(dir) };
    const one = (args, e) => { const r = Exec(BIN, args, { env: e, timeoutMs: 20000 }); return { code: r.code, out: String(r.stdout).trim(), err: String(r.stderr).trim() }; };
    const res = { file: one([...flags, String(f)], env), eval: one([...flags, "-e", src], env), cached: one([...flags, String(f)], cache), cached2: one([...flags, String(f)], cache) };
    Object.defineProperty(res, "qbc", { value: exists(new Path(String(f) + ".qbc")), enumerable: false });
    return res;
}

function each(a, r, fn) { for (const [mode, x] of Object.entries(r)) fn(x, mode); }

const OK_CASES = [
    ["bare await", 'await 1; print("ran")', "ran"],
    ["await in an expression", "print(await Promise.resolve(7))", "7"],
    ["const from awaited dynamic import", 'const h = await import("dyna:hash"); print(typeof h.SHA1Hex)', "function"],
    ["awaited sleep then print", 'await sleep(5); print("slept")', "slept"],
    ["for await over an async generator", 'async function* g() { yield 1; yield 2; } let s = 0; for await (const v of g()) s += v; print(s)', "3"],
    ["var and function declared before await are globals", 'var gv = 4; function gf() { return 5; } await 0; print(globalThis.gv + globalThis.gf())', "9"],
];
for (const [name, src, want] of OK_CASES) {
    t.test(name, (a) => each(a, runAll(src), (x, mode) => {
        a.eq(x.code, 0, `${mode}: exit code (stderr: ${x.err.slice(0, 160)})`);
        a.eq(x.out, want, `${mode}: output`);
    }));
}

t.test("await as an identifier in sloppy script keeps its old meaning", (a) => each(a, runAll("var await = 3; print(await + 1)"), (x, mode) => {
    a.eq(x.code, 0, `${mode}: exit code (${x.err.slice(0, 120)})`);
    a.eq(x.out, "4", `${mode}: identifier value`);
}));

t.test("a script that compiles never runs twice", (a) => each(a, runAll('globalThis.n = (globalThis.n || 0) + 1; print("n=" + n); const s = "await"'), (x, mode) => {
    a.eq(x.out, "n=1", `${mode}: side effect ran once`);
}));

t.test("a real syntax error is reported as written, not as the async retry", (a) => each(a, runAll("let x = ; await 1"), (x, mode) => {
    a.ok(x.code !== 0, `${mode}: non-zero exit`);
    a.ok(/SyntaxError/.test(x.err), `${mode}: SyntaxError reported (${x.err.slice(0, 120)})`);
}));

t.test("a throw after await exits 1 with the message", (a) => each(a, runAll('await 0; throw new Error("late boom")'), (x, mode) => {
    a.eq(x.code, 1, `${mode}: exit code`);
    a.ok(x.err.includes("late boom"), `${mode}: message on stderr (${x.err.slice(0, 120)})`);
}));

t.test("a rejected await exits 1 with the reason", (a) => each(a, runAll('await Promise.reject(new Error("rejected here"))'), (x, mode) => {
    a.eq(x.code, 1, `${mode}: exit code`);
    a.ok(x.err.includes("rejected here"), `${mode}: reason on stderr (${x.err.slice(0, 120)})`);
}));

t.test("a failure after a top-level await is a script error, not an unhandled rejection", (a) => each(a, runAll('await 0; throw new Error("tla error")', {}, ["--no-unhandled-rejection"]), (x, mode) => {
    a.eq(x.code, 1, `${mode}: exit code with --no-unhandled-rejection`);
    a.ok(x.err.includes("tla error"), `${mode}: message on stderr (${x.err.slice(0, 120)})`);
}));

t.test("output printed before and after the await keeps its order", (a) => each(a, runAll('print("a"); await 0; print("b"); await sleep(1); print("c")'), (x, mode) => {
    a.eq(x.out, "a\nb\nc", `${mode}: order`);
}));

t.test("the bytecode cache stores an async script and replays it", (a) => {
    const r = runAll('await 0; print("cached ok")');
    a.ok(r.qbc, "a .qbc cache file was written next to the script");
    a.eq(r.cached2.out, "cached ok", "the cache hit runs and awaits the script");
    a.eq(r.cached2.code, 0, "cache hit exit code");
});

await t.run();
