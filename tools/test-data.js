// tools/test-data.js [check|verify|fetch|paths] -- the git-ignored test data listed in tests/test-data.tsv
// (path<TAB>sha256 or -<TAB>source). source is url:URL (downloaded, sha-checked), gen:CMD (stdout becomes the
// file) or run:CMD (the command writes the file). check: every file exists. verify: and matches its sha.
// fetch: makes every missing or mismatched file from its source. paths: prints the paths.
// repro [PATH...]: for the gen: rows run through tests/ref/py.sh (default: every one whose generator
// reads DYNA_REF_ULP_JITTER), regenerates at -k, 0 and +k ulps of jitter (k = $DYNA_REF_JITTER_K, 1)
// and fails when the bytes differ: the rounding in the generator must absorb another machine's libm.
// A gen: row that fails through tests/ref/py.sh is followed by the reference packages its
// generator imports that the reference python lacks (tests/ref/requirements.txt pins them).
import * as std from "std";
import * as os from "os";

const MANIFEST = "tests/test-data.tsv";
const sh = (cmd) => os.exec(["sh", "-c", cmd]);
const q = (s) => "'" + s.replace(/'/g, "'\\''") + "'";
const exists = (p) => os.stat(p)[1] === 0;
const sha = (p) => {
    const tmp = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/test-data-sha." + os.getpid();
    sh("{ shasum -a 256 " + q(p) + " 2>/dev/null || sha256sum " + q(p) + "; } > " + q(tmp));
    const out = std.loadFile(tmp) || ""; os.remove(tmp);
    return out.split(/\s+/)[0];
};
const rows = () => {
    const t = std.loadFile(MANIFEST);
    if (t === null) throw new Error("no " + MANIFEST);
    return t.split("\n").slice(1).filter(l => l.trim() !== "" && !l.startsWith("#")).map(l => {
        const [path, hash, src] = l.split("\t");
        return { path, hash, src: src || "" };
    });
};
const REF_PY = () => std.getenv("DYNA_REF_PYTHON") || ".venv-ref/bin/python";
const PKG_MODULE = { "scikit-learn": "sklearn" };
function missingRefPackages(cmd) {
    const gen = (/tests\/ref\/\S+\.py/.exec(cmd) || [])[0];
    const req = std.loadFile("tests/ref/requirements.txt") || "";
    const src = gen ? std.loadFile(gen) || "" : "";
    const out = [];
    for (const line of req.split("\n")) {
        const m = /^([A-Za-z0-9_.-]+)==(\S+)/.exec(line);
        if (!m) continue;
        const mod = PKG_MODULE[m[1]] || m[1];
        if (!new RegExp("^\\s*(import|from)\\s+" + mod + "\\b", "m").test(src)) continue;
        if (sh(q(REF_PY()) + " -c " + q("import " + mod) + " >/dev/null 2>&1") !== 0) out.push(m[1] + "==" + m[2]);
    }
    return out;
}
const make = (r) => {
    const dir = r.path.replace(/\/[^/]*$/, "");
    if (dir !== r.path) sh("mkdir -p " + q(dir));
    const tmp = r.path + ".part." + os.getpid();
    if (r.src.startsWith("url:")) {
        const url = r.src.slice(4);
        if (sh("curl -fsSL --retry 2 -o " + q(tmp) + " " + q(url)) !== 0) { sh("rm -f " + q(tmp)); return "download failed (" + url + ")"; }
        const got = sha(tmp);
        if (r.hash !== "-" && got !== r.hash) { sh("rm -f " + q(tmp)); return "upstream sha " + got + " != manifest " + r.hash + " (upstream moved: pin it or update the row)"; }
        os.rename(tmp, r.path);
        print("test-data: fetched " + r.path);
    } else if (r.src.startsWith("gen:")) {
        if (sh("(" + r.src.slice(4) + ") > " + q(tmp)) !== 0) {
            sh("rm -f " + q(tmp));
            const miss = r.src.includes("tests/ref/py.sh") ? missingRefPackages(r.src) : [];
            return "generator failed (" + r.src.slice(4) + ")" + (miss.length ? "; the reference python " + REF_PY() + " lacks " + miss.join(", ") +
                ": install them with `" + REF_PY() + " -m pip install -r tests/ref/requirements.txt` (or point DYNA_REF_PYTHON at a python that has them)" : "");
        }
        os.rename(tmp, r.path);
        print("test-data: generated " + r.path);
    } else if (r.src.startsWith("run:")) {
        if (sh(r.src.slice(4)) !== 0) return "generator failed (" + r.src.slice(4) + ")";
        print("test-data: generated " + r.path);
    } else {
        return "source '" + r.src + "' has no generator; a fresh clone cannot make it";
    }
    if (exists(r.path) && r.hash !== "-" && sha(r.path) !== r.hash)
        print("test-data: WARN " + r.path + " regenerated with a new sha " + sha(r.path) + ": review, then update the manifest");
    return null;
};

const mode = scriptArgs[1] || "check";
if (!["check", "verify", "fetch", "paths", "repro"].includes(mode)) { print("test-data: unknown mode " + mode + " (check|verify|fetch|paths|repro)"); std.exit(2); }
const all = rows();
if (mode === "paths") { for (const r of all) print(r.path); std.exit(0); }
if (mode === "repro") {
    const want = scriptArgs.slice(2);
    const k = Number(std.getenv("DYNA_REF_JITTER_K") || 1);
    const pick = all.filter((r) => r.src.startsWith("gen:") && r.src.includes("tests/ref/py.sh") && (want.length ? want.includes(r.path) :
        (std.loadFile((/tests\/ref\/\S+\.py/.exec(r.src) || [""])[0]) || "").includes("DYNA_REF_ULP_JITTER")));
    let bad = 0;
    for (const r of pick) {
        const t0 = Date.now(), outs = [];
        for (const j of [-k, 0, k]) {
            const tmp = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/test-data-repro." + os.getpid() + "." + outs.length;
            if (sh("(DYNA_REF_ULP_JITTER=" + j + " " + r.src.slice(4) + ") > " + q(tmp) + " 2>/dev/null") !== 0) {
                const miss = missingRefPackages(r.src);
                print("test-data repro: FAIL " + r.path + ": generator failed" + (miss.length ? "; " + REF_PY() + " lacks " + miss.join(", ") : ""));
                bad = 1; outs.length = 0; break;
            }
            outs.push(sha(tmp)); os.remove(tmp);
        }
        if (outs.length !== 3) continue;
        const same = outs[0] === outs[1] && outs[1] === outs[2];
        const pinned = r.hash === "-" || outs[1] === r.hash;
        print(`test-data repro: ${same && pinned ? "ok  " : "FAIL"} ${r.path} at -${k}/0/+${k} ulps: ${same ? "same bytes" : "bytes differ"}${pinned ? "" : ", sha " + outs[1] + " != manifest " + r.hash} (${((Date.now() - t0) / 1000).toFixed(1)}s)`);
        if (!same || !pinned) bad = 1;
    }
    if (!pick.length) { print("test-data repro: no row matched"); bad = 1; }
    std.exit(bad);
}
let bad = 0;
const done = new Set();
for (const r of all) {
    const stale = !exists(r.path) || (r.hash !== "-" && sha(r.path) !== r.hash);
    if (mode === "fetch" && stale) {
        if (r.src.startsWith("run:") && done.has(r.src) && exists(r.path)) continue;
        const t0 = Date.now();
        const err = make(r);
        const secs = ((Date.now() - t0) / 1000).toFixed(1) + "s";
        if (r.src.startsWith("run:")) done.add(r.src);
        if (err) { print("test-data: FAIL " + r.path + " (" + secs + "): " + err); bad = 1; }
        else print("test-data: made " + r.path + " in " + secs + " (" + r.src.slice(0, r.src.indexOf(":")) + ")");
        continue;
    }
    if (!exists(r.path)) { print("test-data: FAIL missing " + r.path + " (source " + r.src + "): run ./dynajs --std tools/test-data.js fetch"); bad = 1; }
    else if (mode === "verify" && r.hash !== "-" && sha(r.path) !== r.hash) { print("test-data: FAIL " + r.path + " sha " + sha(r.path) + " != manifest " + r.hash); bad = 1; }
}
if (!bad) print("test-data " + mode + ": ok (" + all.length + " files)");
std.exit(bad);
