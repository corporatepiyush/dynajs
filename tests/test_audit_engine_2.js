// flags: --std
// Parser memory-safety regressions: E1-01 (const-prop byte offsets read from the
// wrong function def), E1-02 (a const-prop member folded from its first token
// only) and E1b-01/E1b-02 (class parser back-patching a saved bytecode offset
// after the bytecode DynBuf failed to grow).
//
// E1-01 and E1b-01 are memory-safety only -- there is no wrong VALUE to assert,
// so they are gated on the engine not dying: E1-01 on a child process parsing a
// large source (release SIGBUS / ASan heap-buffer-overflow before the fix), and
// E1b-01 on a child process run under --memory-limit so the DynBuf growth is
// refused mid-parse. Both rows SKIP when no child can be launched.
import * as os from "os";

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

{
    const O = Object.freeze({ a: 1 + 2, b: "x" + "y", c: true ? 5 : 6,
                              d: null ?? 7, e: 2 * 3, f: 7 .valueOf() + 1,
                              g: false || 9, h: "s" [0], i: 1, j: 1 ? 0 : 1 });
    const want = [3, "xy", 5, 7, 6, 8, 9, "s", 1, 0];
    eqJ([O.a, O.b, O.c, O.d, O.e, O.f, O.g, O.h, O.i, O.j], want,
         "folded members keep the whole value");
    eqJ([O["a"], O["b"], O["c"], O["d"], O["e"], O["f"], O["g"], O["h"], O["i"], O["j"]], want,
         "folded members read the same through a computed key");

    const N = Object.freeze({ e: 1 .toString(), k: 1 in {}, l: "s".length,
                              m: true.x, n: null?.x, o: 5 / 2, p: "q" [0] });
    eqJ([N.e, N.k, N.l, N.m, N.n, N.o, N.p], ["1", false, 1, undefined, undefined, 2.5, "q"],
         "members the folder cannot fold keep their runtime value");

    const C = Object.freeze({ r: [1, 2].map(v => v * 3) });
    eqJ(C.r, [3, 6], "a call result is never folded");

    const D = Object.freeze({ t: (function () { return 5; })() });
    eq(D.t, 5, "an IIFE member is never folded");

    function reader() { return O.a + O.f; }
    eq(reader(), 11, "a folded member still reads right inside a function");
    eq((() => { switch (3) { case O.a: return "case3"; default: return "default"; } })(),
       "case3", "a folded member still switches right");
}

{
    const L = Object.freeze([1].map((v) => ({ v, w: v * 2 })));
    eqJ(L, [{ v: 1, w: 2 }], "object literal inside an arrow inside a const initializer");
    eq(L.length, 1, "the mapped array keeps its length");
    assert(Object.isFrozen(L), "the const-propagated array is frozen");

    const M = Object.freeze([1, 2].map(v => ({ v, get g() { return v; } })));
    eqJ(M.map(e => e.g), [1, 2], "a getter in the folded literal stays live");
    eqJ(M.map(e => e.v), [1, 2], "the folded literal still has its own fields");

    var a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8, a8 = 9, a9 = 10;
    var b0 = [1], b1 = [2], b2 = [3];
    const fz = Object.freeze;
    const P = fz([1].map((v) => ({ v, w: v })));
    eqJ(P, [{ v: 1, w: 1 }], "the E1 lane probe shape at its smallest crashing size");
    eq(a0 + a9 + b2[0], 14, "the lane probe prelude still evaluates");

    const Q = fz({ r: [1].map(v => ({ v })) });
    eqJ(Q, { r: [{ v: 1 }] }, "an object literal holding a mapped object literal");

    function mk() { return fz({ s: [1, 2].map(v => ({ v, t: v + 1 })) }); }
    eqJ(mk(), { s: [{ v: 1, t: 2 }, { v: 2, t: 3 }] },
         "a folded literal built inside a function body");

    const deep = fz([1].map(v => [2].map(w => ({ v, w }))[0]));
    eqJ(deep, [{ v: 1, w: 2 }], "two nested arrows inside a const initializer");

    try {
        const S = fz([1].map(v => ({ v })));
        S.push({ v: 9 });
        assert(false, "a frozen const-propagated array rejects push");
    } catch (e) {
        assert(e instanceof TypeError, "a frozen const-propagated array throws TypeError");
    }
}

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

if (!canSpawn) {
    skip("no executable ./dynajs in the working directory: the crash-gated rows need a child process");
} else {
    const foldPath = "/tmp/dyna_audit_engine_2_fold.js";
    const foldOut = "/tmp/dyna_audit_engine_2_fold.out";
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
    const secPath = "/tmp/dyna_audit_engine_2_sec.js";
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

    let csrc = "";
    for (let i = 0; i < 1500000; i++) csrc += "0;";
    csrc += "\n(class{});\n(class{});\n(class{});\n(class{});\n(class{});\n" +
            "(class{});\n(class{});\n(class{});\n(class{});\n(class{});\n" +
            "(class{});\n(class{});\n(class{});\n(class{});\n(class{});\n" +
            "(class{});\n(class{});\n(class{});\n(class{});\n(class{});\n";
    const classPath = "/tmp/dyna_audit_engine_2_class.js";
    const errPath = "/tmp/dyna_audit_engine_2_class.err";
    try {
        writeFile(classPath, csrc);
        const errFd = os.open(errPath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC);
        const pid = os.exec(["./dynajs", "--std", "--memory-limit", "16777216", classPath],
                            { blocking: false, stderr: errFd });
        const st = os.waitpid(pid)[1];
        let err = "";
        try { err = String(std.loadFile(errPath)); } catch (e) { err = ""; }
        assert(err.indexOf("AddressSanitizer") < 0,
               "the class parser back-patch stays inside the bytecode buffer under --memory-limit");
        if (err.indexOf("AddressSanitizer") >= 0)
            print("     child reported: " + err.split("\n").slice(0, 3).join(" | "));
        assert(st === 0 || st === 256, "the memory-limited class parse fails cleanly (got " + st + ")");
    } catch (e) {
        skip("could not stage the memory-limited class probe: " + e);
    }
    removeFile(classPath);
    removeFile(errPath);
}

if (failures)
    throw new Error("test_audit_engine_2: " + failures + " failures");
print("test_audit_engine_2: all " + n + " assertions passed" +
      (skips ? " (" + skips + " rows skipped)" : ""));