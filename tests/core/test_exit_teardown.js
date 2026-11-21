// covers: c:src/mm/js_malloc.inc.c
// Exit teardown (JS_SetExitTeardown, set by dyna-cli on its exit paths only):
// when the main runtime is the last one alive and the decrement pass proves no
// object is referenced from C, the final collection restores the candidates'
// child counts, runs only the finalizers of native (module) classes, in the
// same order as the precise collection, and frees nothing; the process exits
// right after. Release runs keep the leak guard (an externally referenced
// object still takes the precise path and its assert). DUMP_LEAKS, ASan and
// DYNA_LEAK_DUMP always tear down precisely. DYNA_TEARDOWN_TRACE=1 makes a
// runtime with the exit flag print "teardown: exit" or "teardown: precise";
// a DUMP_LEAKS or ASan build prints "teardown: precise (build)" instead, since
// it must free every allocation to count leaks, so the leaks and san legs see
// the precise path on every run. Only those builds can print the "(build)"
// marker (it is chosen at compile time), so the fast runs accept it in place
// of "teardown: exit" and a release engine whose fast path gave up still reads
// plain "precise" and fails. The byte-for-byte flush comparison runs either way.
// This suite pins the user-visible contract on all three CLI exit paths
// (normal end, uncaught throw, --timeout-ms stop): 40 buffered FileWriters
// (native finalizers that flush on teardown), held in cycles and closures
// beside a 20000-object heap, all append to one file at exit, and the bytes
// match the precise teardown (DYNA_LEAK_DUMP=1) byte for byte; SAXParsers that
// hold event templates (a native finalizer that releases GC-managed shapes)
// are alive at exit; the exit code is 0 / 1 / nonzero with no signal; and the
// trace proves the fast path actually ran (a CLI without the flag, or a fast
// path that gave up, reads "precise" or nothing).
// Mutants (red): the CLI does not set the flag; the exit pass skips the count
// restore (shape refcount assert, SIGABRT); the exit pass skips native
// finalizers; the exit pass walks the candidates in reverse.
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { Exec, args } from "dyna:sys";
import { makeTempDir, removeAll, writeFile, readFile, exists } from "dyna:file";

const BIN = args()[0];
const root = makeTempDir("dyna-exit-teardown-");
let failures = 0;
function check(c, msg) {
    if (!c) {
        failures++;
        print("  FAIL " + msg);
    }
}

const CHILD = `
import { File } from "dyna:file";
import { args } from "dyna:sys";
import { SAXParser } from "dyna:xml";
const a = args(), k = a.findIndex((x) => x.endsWith("child.js"));
const f = new File(a[k + 1]);
globalThis.keep = [];
for (let i = 0; i < 40; i++) {
    const w = f.writer({ append: true });
    w.write("w" + i + ";");
    const node = { w, i };
    node.self = node;
    keep.push(node);
    if (i % 3 === 0) keep.push(() => w);
}
for (let i = 0; i < 4; i++) {
    const p = new SAXParser(i % 2 ? { onOpen() {}, onText() {}, onClose() {} } : undefined);
    p.write('<a x="1"><b>t</b><c/></a>');
    if (!(i % 2)) for (let e; (e = p.next()) !== null;) keep.push(e);
    keep.push(p);
}
globalThis.big = Array.from({ length: 20000 }, (_, i) => ({ i, s: "k" + i }));
if (a[k + 2] === "spin") for (;;) {}
if (a[k + 2] === "throw") throw new Error("boom");
`;

try {
    const script = root.join("child.js");
    writeFile(script, CHILD);
    const run = (name, precise, mode) => {
        const out = root.join(name + ".txt");
        const env = precise ? { DYNA_LEAK_DUMP: "1", DYNA_TEARDOWN_TRACE: "1" } : { DYNA_TEARDOWN_TRACE: "1" };
        const flags = mode === "spin" ? ["--timeout-ms", "300"] : [];
        const r = Exec(BIN, [...flags, String(script), String(out), mode], { timeoutMs: 20000, encoding: "utf8", env });
        check(!r.timedOut, name + ": child finished");
        check(r.signal === null, name + ": no signal (got " + r.signal + ")\n    " + String(r.stderr).slice(-300));
        check(mode === "end" ? r.code === 0 : r.code !== 0, name + ": exit code " + r.code);
        const forced = String(r.stderr).includes("teardown: precise (build)");
        const want = precise || forced ? "teardown: precise" : "teardown: exit";
        check(String(r.stderr).includes(want), name + ": trace says '" + want + "'\n    " + String(r.stderr).slice(-300));
        return exists(out) ? readFile(out) : "";
    };
    for (const mode of ["end", "throw", "spin"]) {
        const fast = run("fast-" + mode, false, mode);
        const precise = run("precise-" + mode, true, mode);
        const tokens = fast.split(";").filter((t) => t);
        check(tokens.length === 40 && new Set(tokens).size === 40, mode + ": every writer flushed exactly once at exit (got " + tokens.length + ")");
        check(fast === precise, mode + ": exit teardown runs finalizers in the precise order\n    fast:    " + fast.slice(0, 120) + "\n    precise: " + precise.slice(0, 120));
    }
} finally {
    removeAll(root);
}
if (failures) throw new Error("test_exit_teardown: " + failures + " failures");
print("ok test_exit_teardown");
