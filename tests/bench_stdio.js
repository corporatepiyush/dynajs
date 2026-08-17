/* bench_stdio.js -- file I/O paths in src/dyna-libc.c.
 *
 * RUN THIS BEFORE AND AFTER ANY CHANGE TO the std.File methods.
 *
 * `getline` and `readAsString` are byte-at-a-time `fgetc` loops. fgetc takes
 * the stdio lock and does a buffer check per byte, so the cost is dominated by
 * call overhead rather than by the read. The shapes that expose it are:
 *
 *   - MANY SHORT LINES vs FEW LONG LINES. A chunked rewrite that scans with
 *     memchr wins hugely on long lines and only modestly on 2-byte lines, so
 *     both must be rows or the improvement is overstated.
 *   - readAsString WITH and WITHOUT a max_size argument. The size-capped path
 *     cannot simply slurp the file, and a rewrite that only handles the
 *     uncapped case would look free while regressing the other.
 *   - A NON-REGULAR file (a pipe), where fstat gives no useful size. Any
 *     pre-sizing must degrade gracefully rather than mis-size the buffer.
 *
 * Output lines beginning with `#S` are machine-readable:
 *   #S <name> <bytes> <ms> <MB/s>
 *
 * Usage: dynajs --std tests/bench_stdio.js [scale]
 */
import * as std from "std";
import * as os from "os";

const SCALE = parseFloat(scriptArgs[1] || "1");
const TMP = "/tmp/_dyna_stdio_bench";

/* ---- fixtures: same total bytes, very different line structure ---------- */
function build(path, lineLen, totalBytes) {
    const line = "x".repeat(lineLen - 1) + "\n";
    const chunk = line.repeat(Math.max(1, (65536 / lineLen) | 0));
    const f = std.open(path, "w");
    let written = 0;
    while (written < totalBytes) { f.puts(chunk); written += chunk.length; }
    f.close();
    return written;
}

const MB = 1024 * 1024;
const F_SHORT = TMP + ".short";   /* 2-byte lines: worst case for memchr */
const F_MED   = TMP + ".med";     /* 80-byte lines: ordinary text */
const F_LONG  = TMP + ".long";    /* 4096-byte lines: best case */
const F_NONL  = TMP + ".nonl";    /* no newline at all: whole file is one line */

const SZ = 4 * MB;
const nShort = build(F_SHORT, 2, SZ);
const nMed   = build(F_MED, 80, SZ);
const nLong  = build(F_LONG, 4096, SZ);
{
    const f = std.open(F_NONL, "w");
    const c = "y".repeat(65536);
    for (let i = 0; i < SZ / 65536; i++) f.puts(c);
    f.close();
}

function row(name, bytes, fn) {
    fn();                                   /* warm: also primes the page cache */
    const t0 = performance.now();
    fn();
    const t = performance.now() - t0;
    print("#S " + name + " " + bytes + " " + t.toFixed(3) + " " +
          ((bytes / MB) / (t / 1000)).toFixed(1));
}

/* ---- getline ------------------------------------------------------------ */
for (const [nm, path, bytes] of [["short", F_SHORT, nShort],
                                 ["med", F_MED, nMed],
                                 ["long", F_LONG, nLong]]) {
    row("getline_" + nm, bytes, () => {
        const f = std.open(path, "r");
        let n = 0;
        while (f.getline() !== null) n++;
        f.close();
        return n;
    });
}
/* A file with no newline: getline must consume the whole thing as one line. */
row("getline_nonewline", SZ, () => {
    const f = std.open(F_NONL, "r");
    while (f.getline() !== null) {}
    f.close();
});

/* ---- readAsString ------------------------------------------------------- */
row("readAsString_full", nMed, () => {
    const f = std.open(F_MED, "r"); f.readAsString(); f.close();
});
row("readAsString_capped", 1 * MB, () => {
    const f = std.open(F_MED, "r"); f.readAsString(1 * MB); f.close();
});
row("readAsString_tiny_cap", 1024, () => {
    const f = std.open(F_MED, "r"); f.readAsString(1024); f.close();
});

/* Non-regular file: a pipe has no useful st_size. Any pre-sizing must not
   mis-handle it. Kept small so the pipe buffer cannot deadlock. */
row("readAsString_pipe", 64 * 1024, () => {
    const [rfd, wfd] = os.pipe();
    const w = std.fdopen(wfd, "w");
    w.puts("z".repeat(64 * 1024));
    w.close();
    const r = std.fdopen(rfd, "r");
    r.readAsString();
    r.close();
});

/* ---- byte-at-a-time control: must NOT get slower ------------------------ */
row("getByte_1MB", 1 * MB, () => {
    const f = std.open(F_MED, "r");
    for (let i = 0; i < 1 * MB; i++) if (f.getByte() < 0) break;
    f.close();
});

/* ---- whole-file helper for comparison ----------------------------------- */
row("loadFile", nMed, () => { std.loadFile(F_MED); });

for (const p of [F_SHORT, F_MED, F_LONG, F_NONL]) os.remove(p);
print("#S DONE");
