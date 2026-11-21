// flags: --std
// Does a patch silently revert lines that main gained after the patch's author took their copy?
//
// A track works on a snapshot of main. When it later diffs its tree against a NEWER main (or
// hand-rebases a hunk), every line main gained since the snapshot shows up as a removal, and
// applying the patch quietly undoes another track's merged work (a patch once reverted the
// eng_bg_reap fix this way). The removals a patch may make are exactly the author's own:
//
//   own(f, L)  = max(0, base(f, L) - work(f, L))     lines the author deleted from their snapshot
//   net(f, L)  = removed(f, L) - added(f, L)         lines the patch deletes from main
//   net > own  => REVERTS: main has the line, the author never deleted it
//
// counted as multisets per file, so repeated lines ("fi", "}") and moved lines are exact.
//
//   patch-reverts.js stamp PATCH DIR          name the patch's snapshot: write (or replace) a first
//                                             line "# snapshot: <abs DIR>@<DIR's HEAD sha>"
//                                             (patch -p1 and git apply skip it)
//   patch-reverts.js PATCH                    a stamped patch is checked against the tree it names
//   patch-reverts.js PATCH --git DIR          base = DIR's HEAD (the snapshot commit), work = DIR
//   patch-reverts.js PATCH --base B --work W  the snapshot and the author's tree as directories
//   patch-reverts.js PATCH --list             no snapshot: list every removal not re-added, for a
//                                             human to confirm as the author's own
//
// A stamped patch refuses (exit 2) a --git DIR whose path or HEAD is not the one it names: a check
// against the wrong tree reports every line main gained since THAT tree's snapshot (a patch cut on
// .scratch/t14 once "reverted" 1409 lines when checked against the track's root). An unstamped
// patch needs --git, --base/--work or --list; it is never checked against a guessed tree.
//   patch-reverts.js selftest                 planted stale-base fixtures
//
// Exit 1 when the patch reverts a line (or, without a snapshot, removes any).
import * as std from "std";
import { Exec } from "dyna:sys";

function parsePatch(text) {
    const files = new Map();
    let cur = null, minus = null;
    const get = (f) => {
        if (!files.has(f)) files.set(f, { removed: new Map(), added: new Map(), order: [] });
        return files.get(f);
    };
    const bump = (m, k) => m.set(k, (m.get(k) || 0) + 1);
    for (const line of text.split("\n")) {
        if (line.startsWith("diff ")) { cur = null; minus = null; continue; }
        if (line.startsWith("--- ")) { minus = line.slice(4).replace(/\t.*$/, ""); continue; }
        if (line.startsWith("+++ ")) {
            let f = line.slice(4).replace(/\t.*$/, "");
            if (f === "/dev/null") f = minus;
            cur = get(f.replace(/^[ab]\//, ""));
            continue;
        }
        if (!cur || line.startsWith("@@") || line.startsWith("\\")) continue;
        if (line[0] === "-") { bump(cur.removed, line.slice(1)); cur.order.push(line.slice(1)); }
        else if (line[0] === "+") bump(cur.added, line.slice(1));
    }
    return files;
}

function counts(text) {
    const m = new Map();
    if (text === null || text === undefined) return m;
    const lines = text.split("\n");
    if (lines.length && lines[lines.length - 1] === "") lines.pop();
    for (const l of lines) m.set(l, (m.get(l) || 0) + 1);
    return m;
}

export function reverts(patchText, base, work) {
    const out = [];
    for (const [f, p] of parsePatch(patchText)) {
        const b = base ? counts(base(f)) : null, w = base ? counts(work(f)) : null;
        const seen = new Set();
        for (const l of p.order) {
            if (seen.has(l)) continue;
            seen.add(l);
            const net = p.removed.get(l) - (p.added.get(l) || 0);
            if (net <= 0) continue;
            const own = b ? Math.max(0, (b.get(l) || 0) - (w.get(l) || 0)) : 0;
            if (net > own) out.push({ file: f, line: l, n: net - own });
        }
    }
    return out;
}

function readFile(p) {
    try {
        return std.loadFile(p);
    } catch (e) {
        return null;
    }
}

function gitShow(dir, f) {
    const r = Exec("git", ["show", "HEAD:" + f], { cwd: dir, maxBuffer: 1 << 30 });
    return r.code === 0 ? String(r.stdout) : null;
}

function report(found, snapshot) {
    for (const r of found)
        print(`${r.file}: ${snapshot ? "REVERTS" : "REMOVES"}: ${r.line.slice(0, 140)}${r.n > 1 ? `  (x${r.n})` : ""}`);
    if (!found.length) print(snapshot ? "patch-reverts: every removal is the author's own" : "patch-reverts: the patch removes nothing it does not re-add");
    else if (snapshot) print(`patch-reverts: ${found.length} line(s) main has that the author never deleted: the patch was cut against a newer main than its snapshot; rebase the hunks`);
    else print(`patch-reverts: ${found.length} removed line(s) not re-added: confirm each is the author's own change`);
    return found.length ? 1 : 0;
}

const STAMP = /^# snapshot: (.+)@([0-9a-f]{40})$/;

export function snapshotOf(patchText) {
    const first = patchText.slice(0, patchText.indexOf("\n") + 1 || patchText.length).replace(/\n$/, "");
    const m = STAMP.exec(first);
    return m ? { dir: m[1], sha: m[2] } : null;
}

export function stampText(patchText, dir, sha) {
    const body = snapshotOf(patchText) ? patchText.slice(patchText.indexOf("\n") + 1) : patchText;
    return `# snapshot: ${dir}@${sha}\n` + body;
}

export function resolveSnapshot(patchText, gitDir, realDir, headOf) {
    const snap = snapshotOf(patchText);
    if (!snap) return gitDir === null ? { error: "the patch names no snapshot: stamp it (patch-reverts.js stamp PATCH DIR) or pass --git DIR / --list" } : { dir: gitDir };
    const dir = gitDir === null ? snap.dir : gitDir;
    if (gitDir !== null && realDir(gitDir) !== realDir(snap.dir))
        return { error: `--git ${gitDir} is not the patch's snapshot ${snap.dir}` };
    const head = headOf(dir);
    if (head !== snap.sha)
        return { error: `${dir} is at ${head || "no commit"}, the patch was cut on ${snap.sha}: that tree moved or is the wrong one` };
    return { dir };
}

function selftest() {
    let bad = 0;
    const expect = (name, got, want) => {
        const g = got.map((r) => `${r.file}:${r.line}:${r.n}`).sort().join("|");
        const w = want.slice().sort().join("|");
        if (g !== w) { print(`selftest: ${name}: got [${g}] want [${w}]`); bad++; }
    };
    const files = (o) => (f) => (f in o ? o[f] : null);
    const base = { "build.sh": "a\nfi\nb\nfi\nc\n", "x.c": "keep\nmine\n" };
    const work = { "build.sh": "a\nfi\nb\nB2\nfi\nc\n", "x.c": "keep\n" };
    const stale =
        "diff --git a/build.sh b/build.sh\n--- a/build.sh\n+++ b/build.sh\n@@ -1,8 +1,7 @@\n a\n fi\n b\n" +
        "-eng_bg_reap\n-fi\n+B2\n+fi\n c\n-fi\n" +
        "diff --git a/x.c b/x.c\n--- a/x.c\n+++ b/x.c\n@@ -1,2 +1,1 @@\n keep\n-mine\n";
    expect("stale base: lines main gained since the snapshot are caught, own deletions pass",
        reverts(stale, files(base), files(work)), ["build.sh:eng_bg_reap:1", "build.sh:fi:1"]);
    const clean =
        "--- a/build.sh\n+++ b/build.sh\n@@ -3,2 +3,3 @@\n b\n+B2\n fi\n" +
        "--- a/x.c\n+++ b/x.c\n@@ -1,2 +1,1 @@\n keep\n-mine\n";
    expect("a patch cut against its own snapshot reverts nothing", reverts(clean, files(base), files(work)), []);
    const moved = "--- a/build.sh\n+++ b/build.sh\n@@ -1,3 +1,3 @@\n-a\n fi\n+a\n b\n";
    expect("a moved line (removed and re-added) is not a removal", reverts(moved, files(base), files(work)), []);
    const created = "--- /dev/null\n+++ b/tests/new.sh\n@@ -0,0 +1 @@\n+echo\n" +
        "--- a/gone.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-old\n";
    expect("a deleted file the author also deleted is own; creating a file removes nothing",
        reverts(created, files({ "gone.txt": "old\n" }), files({})), []);
    expect("a deleted file the author never had is a revert",
        reverts(created, files({}), files({})), ["gone.txt:old:1"]);
    expect("without a snapshot every net removal is listed",
        reverts(stale, null, null), ["build.sh:eng_bg_reap:1", "build.sh:fi:1", "x.c:mine:1"]);
    const sha = "a".repeat(40), other = "b".repeat(40);
    const heads = { "/w/t14": sha, "/w/root": other };
    const real = (d) => d.replace(/\/+$/, "");
    const head = (d) => heads[real(d)] || null;
    const stamped = stampText(stale, "/w/t14", sha);
    const check = (name, got, want) => {
        const g = got.error ? "error" : got.dir;
        if (g !== want) { print(`selftest: ${name}: got ${JSON.stringify(got)} want ${want}`); bad++; }
    };
    check("a stamped patch is checked against the tree it names", resolveSnapshot(stamped, null, real, head), "/w/t14");
    check("--git naming the same tree passes", resolveSnapshot(stamped, "/w/t14/", real, head), "/w/t14/");
    check("--git naming another tree is refused (the 1409-line false alarm)", resolveSnapshot(stamped, "/w/root", real, head), "error");
    heads["/w/copy"] = sha;
    check("another tree at the same commit is still not the snapshot (its work files differ)", resolveSnapshot(stamped, "/w/copy", real, head), "error");
    heads["/w/t14"] = other;
    check("a snapshot tree whose HEAD moved is refused", resolveSnapshot(stamped, null, real, head), "error");
    check("an unstamped patch with no tree is refused, never guessed", resolveSnapshot(stale, null, real, head), "error");
    check("an unstamped patch with --git uses that tree", resolveSnapshot(stale, "/w/root", real, head), "/w/root");
    if (snapshotOf(stampText(stamped, "/w/x", other)).dir !== "/w/x" || stampText(stamped, "/w/x", other).split("# snapshot:").length !== 2) {
        print("selftest: restamping must replace the snapshot line, not add a second");
        bad++;
    }
    expect("a stamp line is not part of the diff", reverts(stamped, null, null), ["build.sh:eng_bg_reap:1", "build.sh:fi:1", "x.c:mine:1"]);
    print(bad ? `patch-reverts selftest: ${bad} problem(s)` : "patch-reverts selftest: stale-base reverts caught, own removals and moves pass");
    return bad ? 1 : 0;
}

const USAGE = "usage: dynajs --std tools/patch-reverts.js PATCH [--git DIR | --base DIR --work DIR | --list] | stamp PATCH DIR | selftest";

function realDir(d) {
    const r = Exec("/bin/sh", ["-c", 'cd "$1" 2>/dev/null && pwd -P', "real", d]);
    return r.code === 0 ? String(r.stdout).trim() : d;
}

function headOf(d) {
    const r = Exec("git", ["rev-parse", "HEAD"], { cwd: d });
    return r.code === 0 ? String(r.stdout).trim() : null;
}

function main(args) {
    if (args[0] === "selftest") return selftest();
    if (args[0] === "stamp") {
        const [, patch, dir] = args;
        const text = patch && dir ? readFile(patch) : null;
        const sha = dir ? headOf(dir) : null;
        if (text === null || !sha) { print(text === null ? USAGE : `patch-reverts: ${dir} has no git HEAD to name`); return 2; }
        const f = std.open(patch, "w");
        f.puts(stampText(text, realDir(dir), sha));
        f.close();
        print(`patch-reverts: ${patch} names snapshot ${realDir(dir)}@${sha}`);
        return 0;
    }
    const patch = args[0];
    let gitDir = null, baseDir = null, workDir = null, list = false;
    for (let i = 1; i < args.length; i++) {
        if (args[i] === "--git" && i + 1 < args.length) gitDir = args[++i];
        else if (args[i] === "--base" && i + 1 < args.length) baseDir = args[++i];
        else if (args[i] === "--work" && i + 1 < args.length) workDir = args[++i];
        else if (args[i] === "--list") list = true;
        else { print(USAGE); return 2; }
    }
    if (patch === undefined || !baseDir !== !workDir || (baseDir && gitDir)) { print(USAGE); return 2; }
    const text = readFile(patch);
    if (text === null) { print(`patch-reverts: cannot read ${patch}`); return 2; }
    if (list) return report(reverts(text, null, null), false);
    if (baseDir) return report(reverts(text, (f) => readFile(baseDir + "/" + f), (f) => readFile(workDir + "/" + f)), true);
    const r = resolveSnapshot(text, gitDir, realDir, headOf);
    if (r.error) { print(`patch-reverts: REFUSED: ${r.error}`); return 2; }
    const d = r.dir;
    print(`patch-reverts: snapshot ${d}`);
    return report(reverts(text, (f) => gitShow(d, f), (f) => readFile(d + "/" + f)), true);
}

if (typeof scriptArgs !== "undefined" && /patch-reverts\.js$/.test(scriptArgs[0] || ""))
    std.exit(main(scriptArgs.slice(1)));
