// flags: --std
// tools/workdir.js -- create and check an agent work dir (.agent-work/<name>/), a full copy of this tree that
// is its OWN git repository, so git there can never act on the main repo (F20, F21).
//
//   ./dynajs --std tools/workdir.js create <name>   rsync the tree (no .agent-work, .obj*, .git) into
//                                                  .agent-work/<name>, git init, commit everything as
//                                                  "baseline", copy ./dynajs to <name>/dynajs.base
//   ./dynajs --std tools/workdir.js check <name>    verify an existing work dir (exit 1 on any problem)
//   ./dynajs --std tools/workdir.js selftest        create + check + break + check again in a temp dir
//
// Why: an rsync copy has no .git, so `git -C .agent-work/x diff` or `git add -N` there walked up to the main
// repo and changed ITS index (F20); test_module_surface and every suite that spawns ./dynajs need the binary at
// the work dir root (F21). check verifies: own .git whose top level is the work dir, a "baseline" commit, an
// executable ./dynajs at the root that runs, and dynajs.base for A/B. The old fmt target of build.sh (the other half of F21)
// no longer exists: build.sh only builds (CLAUDE.md rule 3), so PRETTIER is not needed in a work dir.
import * as std from "std";
import * as sys from "dyna:sys";

const ROOT = sys.cwd();
const say = (s) => print("workdir: " + s);

function run(cmd, args, opts) {
    const r = sys.Exec(cmd, args, { ...(opts || {}), maxBuffer: 64 << 20 });
    return { code: r.code, out: String(r.stdout).trim(), err: String(r.stderr).trim() };
}
function must(cmd, args, opts) {
    const r = run(cmd, args, opts);
    if (r.code !== 0) throw new Error(cmd + " " + args.join(" ") + " -> " + r.code + ": " + (r.err || r.out).slice(0, 400));
    return r.out;
}
function exists(p) {
    return run("/bin/test", ["-e", p]).code === 0;
}
function validName(name) {
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name || "")) throw new Error("work dir name must match [A-Za-z0-9][A-Za-z0-9._-]* (got " + JSON.stringify(name) + ")");
}

export function create(src, dir) {
    if (exists(dir)) throw new Error(dir + " already exists; pick a fresh name (work dirs are never reused)");
    const t0 = Date.now();
    must("mkdir", ["-p", dir]);
    must("rsync", ["-a", "--exclude", ".agent-work", "--exclude", ".obj*", "--exclude", ".git", src + "/", dir + "/"]);
    const env = { GIT_AUTHOR_NAME: "agent", GIT_AUTHOR_EMAIL: "agent@localhost", GIT_COMMITTER_NAME: "agent", GIT_COMMITTER_EMAIL: "agent@localhost" };
    must("git", ["init", "-q"], { cwd: dir });
    must("git", ["add", "-A"], { cwd: dir });
    must("git", ["commit", "-q", "--no-verify", "-m", "baseline"], { cwd: dir, env, extendEnv: true });
    if (exists(dir + "/dynajs")) must("cp", ["-p", dir + "/dynajs", dir + "/dynajs.base"]);
    say("created " + dir + " in " + ((Date.now() - t0) / 1000).toFixed(1) + " s");
}

export function check(dir) {
    const bad = [];
    const real = must("/bin/sh", ["-c", 'cd "$1" && pwd -P', "_", dir]);
    const git = run("git", ["rev-parse", "--show-toplevel"], { cwd: dir });
    if (!exists(dir + "/.git")) bad.push("no .git: git in this dir acts on the enclosing repo (F20); recreate it with `tools/workdir.js create`");
    else if (git.code !== 0 || git.out !== real) bad.push("git top level is " + (git.out || "?") + ", not the work dir " + real);
    else if (run("git", ["log", "--format=%s", "--reverse"], { cwd: dir }).out.split("\n")[0] !== "baseline") bad.push("no \"baseline\" commit to diff against");
    if (!exists(dir + "/dynajs")) bad.push("no ./dynajs at the root (suites that spawn ./dynajs and test_module_surface need it, F21)");
    else if (run(dir + "/dynajs", ["-e", "0"], { cwd: dir }).code !== 0) bad.push("./dynajs at the root does not run");
    if (!exists(dir + "/dynajs.base")) bad.push("no dynajs.base for A/B timing");
    for (const b of bad) print("FAIL: " + dir + ": " + b);
    if (!bad.length) say(dir + ": ok (own git, baseline commit, ./dynajs runs, dynajs.base)");
    return bad.length;
}

function selftest() {
    const tmp = must("mktemp", ["-d", ROOT + "/.agent-work/wdself.XXXXXX"]);
    const src = tmp + "/src", dir = tmp + "/wd";
    let fail = 0;
    try {
        must("mkdir", ["-p", src + "/tests", src + "/.agent-work/junk", src + "/.obj/x"]);
        must("cp", ["-p", sys.args()[0], src + "/dynajs"]);
        must("/bin/sh", ["-c", 'echo a > "$1/tests/a.js"', "_", src]);
        create(src, dir);
        if (check(dir) !== 0) { print("FAIL: selftest: a fresh work dir does not check"); fail++; }
        if (exists(dir + "/.agent-work") || exists(dir + "/.obj")) { print("FAIL: selftest: .agent-work or .obj* was copied"); fail++; }
        must("rm", ["-rf", dir + "/.git"]);
        if (check(dir) === 0) { print("FAIL: selftest: a work dir without .git passed the check"); fail++; }
        must("rm", ["-f", dir + "/dynajs"]);
        if (check(dir) < 2) { print("FAIL: selftest: a missing ./dynajs was not reported"); fail++; }
        try { create(src, dir); print("FAIL: selftest: create reused an existing dir"); fail++; } catch (e) { void e; }
    } finally {
        must("rm", ["-rf", tmp]);
    }
    print(fail ? "workdir selftest: " + fail + " failure(s)" : "workdir selftest: ok (create, check, missing .git, missing ./dynajs, no reuse)");
    return fail;
}

const [cmd, name] = scriptArgs.slice(1);
let rc = 0;
try {
    if (cmd === "create") { validName(name); create(ROOT, ROOT + "/.agent-work/" + name); rc = check(ROOT + "/.agent-work/" + name) ? 1 : 0; }
    else if (cmd === "check") { validName(name); rc = check(ROOT + "/.agent-work/" + name) ? 1 : 0; }
    else if (cmd === "selftest") rc = selftest() ? 1 : 0;
    else { print("usage: ./dynajs --std tools/workdir.js create|check <name> | selftest"); rc = 2; }
} catch (e) {
    print("FAIL: workdir: " + e.message);
    rc = 1;
}
std.exit(rc);
