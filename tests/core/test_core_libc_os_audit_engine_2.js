// flags: --std
// covers: c:src/modules/libc/libc-os.c
// Parser memory-safety regressions: E1-01 (const-prop byte offsets read from the
// wrong function def), E1-02 (a const-prop member folded from its first token
// only) and E1b-01/E1b-02 (class parser back-patching a saved bytecode offset
// after the bytecode DynBuf failed to grow).
//
// E1-01 and E1b-01 are memory-safety only -- there is no wrong VALUE to assert,
// so they are gated on the engine not dying: E1-01 on a child process parsing a
// large source (release SIGBUS / ASan heap-buffer-overflow before the fix), and
// E1b-01 (a heap-capped child) was removed with the CLI heap-cap flag. The row SKIPs when no child can be launched.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_libc_os_audit_engine_2");
import * as os from "os";
const TBASE = (std.getenv("TMPDIR") || "/tmp/").replace(/\/?$/, "/") + "dyna_audit_engine_2_" + os.getpid() + "_";

let n = 0, failures = 0, skips = 0;
function assert(c, m) {
    n++;
    if (!c) { failures++; print("FAIL: " + m); }
}
function eq(got, want, m) {
    assert(got === want, m + " (got " + JSON.stringify(got) +
           ", want " + JSON.stringify(want) + ")");
}
function eqJ(got, want, m) {
    eq(JSON.stringify(got), JSON.stringify(want), m);
}
function skip(m) { skips++; print("SKIP: " + m); }

function writeFile(path, text) {
    const f = std.open(path, "w");
    f.puts(text);
    f.close();
}

function removeFile(path) {
    try { os.remove(path); } catch (e) { /* nothing to clean up */ }
}

function childStatus(bin, args, outFd) {
    const opt = { blocking: false };
    if (outFd !== undefined) opt.stdout = outFd;
    const pid = os.exec([bin].concat(args), opt);
    return os.waitpid(pid)[1];
}

const binStat = (typeof os.stat === "function") ? os.stat("./dynajs") : null;

const binMode = binStat ? (binStat[0].mode & 0o111) : 0;

const canSpawn = (typeof os.exec === "function") && (typeof os.waitpid === "function")
                 && binMode !== 0;

__kitS.section("no executable ./dynajs in the working directory: the crash-gated rows need a child process", () => {
if (!canSpawn) {
    skip("no executable ./dynajs in the working directory: the crash-gated rows need a child process");
} else {
    const foldPath = TBASE + "fold.js";
    const foldOut = TBASE + "fold.out";
    const foldWant = [3, 3, "xy", "xy", 5, 5, 7, 7, 6, 6, 8, 8, 9, 9,
                      "s", "s", 1, 1, 0, 0];
    try {
        writeFile(foldPath,
            'const O = Object.freeze({a: 1 + 2, b: "x" + "y", c: true ? 5 : 6,' +
            ' d: null ?? 7, e: 2 * 3, f: 7 .valueOf() + 1, g: false || 9,' +
            ' h: "s" [0], i: 1, j: 1 ? 0 : 1});\n' +
            'print(JSON.stringify([O.a, O["a"], O.b, O["b"], O.c, O["c"], O.d, O["d"],' +
            ' O.e, O["e"], O.f, O["f"], O.g, O["g"], O.h, O["h"], O.i, O["i"],' +
            ' O.j, O["j"]]));\n');
        const outFd = os.open(foldOut, os.O_WRONLY | os.O_CREAT | os.O_TRUNC);
        const st = childStatus("./dynajs", ["--std", foldPath], outFd);
        let line = "";
        try { line = String(std.loadFile(foldOut)).trim(); } catch (e) { line = ""; }
        eq(st, 0, "the const-prop fold script runs clean");
        eqJ(JSON.parse(line), foldWant,
             "a global-script fold keeps every member's whole value, not its first token");
    } catch (e) {
        skip("could not stage the fold script: " + e);
    }
    removeFile(foldPath);
    removeFile(foldOut);

    let src = "var a=0;\n";
    for (let i = 0; i < 200000; i++) src += "a=a+1;\n";
    src += "const fz = Object.freeze;\nconst L = [1].map((v)=>({v, w: v}));\nprint(L.length);\n";
    const secPath = TBASE + "sec.js";
    try {
        writeFile(secPath, src);
        const nullFd = os.open("/dev/null", os.O_WRONLY);
        const st = childStatus("./dynajs", ["--std", secPath], nullFd);
        eq(st, 0, "parsing a large source with a folded literal inside an arrow does not kill the parser");
        if (st !== 0)
            print("     child status " + st + " (" +
                  (st > 128 ? "signal " + (st - 128) : "signal " + st) + ")");
    } catch (e) {
        skip("could not stage the large source: " + e);
    }
    removeFile(secPath);
}
});
__kitS.end();

if (failures)
    throw new Error("test_audit_engine_2: " + failures + " failures");
print("test_audit_engine_2: all " + n + " assertions passed" +
      (skips ? " (" + skips + " rows skipped)" : ""));
