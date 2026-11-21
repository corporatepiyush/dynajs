// covers: c:src/dyna-cli.c
// The command line's own contract: --version and -v print the version and exit
// 0; --help lists every option and exits 0 (an unknown option prints the same
// text but exits 1); and the README's "Command line" block is the --help text
// byte for byte from the usage line on, so a flag added to the CLI (as
// --timeout-ms, --memory-limit and --native-memory-limit once were, documented
// in the README but missing from --help) cannot drift from its documentation.
import { args, cwd, Exec } from "dyna:sys";
import { Path, readFile } from "dyna:file";
import { suite } from "../lib/kit.js";

const BIN = args()[0];
const run = (...args) => { const r = Exec(BIN, args, { timeoutMs: 10000 }); return { code: r.code, out: String(r.stdout), err: String(r.stderr) }; };
const t = suite("cli_flags");

for (const flag of ["--version", "-v"]) {
    t.test(`${flag} prints the version and exits 0`, (a) => {
        const r = run(flag);
        a.eq(r.code, 0, "exit code");
        a.ok(/^DynaJS version \d+\.\d+\.\d+\n$/.test(r.out), `one version line (got ${JSON.stringify(r.out)})`);
    });
}

t.test("--help exits 0 and an unknown option exits 1 with the same text", (a) => {
    const h = run("--help"), u = run("--no-such-flag");
    a.eq(h.code, 0, "--help exit code");
    a.eq(u.code, 1, "unknown option exit code");
    a.ok(u.err.includes("unknown option '--no-such-flag'"), "names the unknown option");
    a.eq(u.out, h.out, "same option list");
});

t.test("--help lists every flag the CLI accepts", (a) => {
    const h = run("--help").out;
    for (const f of ["--eval", "--module", "--std", "--no-unhandled-rejection", "--no-prototypes", "--timeout-ms", "--native-memory-limit", "--io-threads", "--version"])
        a.ok(h.includes(f), `--help lists ${f}`);
});

t.test("the README Command line block is the --help text", (a) => {
    const readme = readFile(new Path(cwd() + "/README.md"));
    const at = readme.indexOf("## Command line");
    const open = readme.indexOf("```text\n", at) + 8;
    const block = readme.slice(open, readme.indexOf("```", open));
    const help = run("--help").out;
    a.eq(block, help.slice(help.indexOf("usage:")), "README block equals --help from the usage line on");
});

await t.run();
