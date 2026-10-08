import * as std from "std";
import * as os from "os";

const SCALE = parseFloat(scriptArgs[1] || "1");
const TMP = `${std.getenv("TMPDIR") || "/tmp"}/dj_stdio_bench.${Date.now() % 10000000}`;

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
const F_SHORT = TMP + ".short";
const F_MED   = TMP + ".med";
const F_LONG  = TMP + ".long";
const F_NONL  = TMP + ".nonl";

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
    fn();
    const t0 = performance.now();
    fn();
    const t = performance.now() - t0;
    print("#S " + name + " " + bytes + " " + t.toFixed(3) + " " +
          ((bytes / MB) / (t / 1000)).toFixed(1));
}

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
row("getline_nonewline", SZ, () => {
    const f = std.open(F_NONL, "r");
    while (f.getline() !== null) {}
    f.close();
});

row("readAsString_full", nMed, () => {
    const f = std.open(F_MED, "r"); f.readAsString(); f.close();
});
row("readAsString_capped", 1 * MB, () => {
    const f = std.open(F_MED, "r"); f.readAsString(1 * MB); f.close();
});
row("readAsString_tiny_cap", 1024, () => {
    const f = std.open(F_MED, "r"); f.readAsString(1024); f.close();
});

row("readAsString_pipe", 64 * 1024, () => {
    const [rfd, wfd] = os.pipe();
    const w = std.fdopen(wfd, "w");
    w.puts("z".repeat(64 * 1024));
    w.close();
    const r = std.fdopen(rfd, "r");
    r.readAsString();
    r.close();
});

row("getByte_1MB", 1 * MB, () => {
    const f = std.open(F_MED, "r");
    for (let i = 0; i < 1 * MB; i++) if (f.getByte() < 0) break;
    f.close();
});

row("loadFile", nMed, () => { std.loadFile(F_MED); });

for (const p of [F_SHORT, F_MED, F_LONG, F_NONL]) os.remove(p);
print("#S DONE");
