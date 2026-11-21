import * as std from "std";
import * as os from "os";

const ROOT = scriptArgs[0].replace(/tools\/dts\.js$/, "") || "./";
const ORDER = "tools/dts-order.tsv";
const PRELUDE = "src/types/prelude.d.ts";
const GLOBALS = "src/types/globals.d.ts";
const SPECIAL = {
    "std": "src/modules/libc/std.d.ts",
    "os": "src/modules/libc/os.d.ts",
    "dyna:hash": "src/modules/crypto/hash.d.ts",
    "*/src/pool.js": "src/pool.d.ts",
};
const MARK = "//@dts-block ";

function fileFor(name) {
    if (name.startsWith("@prelude")) return PRELUDE;
    if (name.startsWith("@globals")) return GLOBALS;
    if (SPECIAL[name]) return SPECIAL[name];
    const m = /^dyna:([a-z0-9]+)$/.exec(name);
    if (!m) throw new Error("dts: no file mapping for module " + JSON.stringify(name));
    return "src/modules/" + m[1] + "/" + m[1] + ".d.ts";
}

function refLine(file) {
    return "/// <reference path=\"" + "../".repeat(file.split("/").length - 1) + PRELUDE + "\" />";
}

function read(p) {
    const s = std.loadFile(ROOT + p);
    if (s === null) throw new Error("dts: cannot read " + p);
    return s;
}

function write(p, s) {
    const parts = p.split("/");
    let d = ROOT.replace(/\/$/, "") || ".";
    for (let i = 0; i < parts.length - 1; i++) { d += "/" + parts[i]; os.mkdir(d, 0o755); }
    const f = std.open(ROOT + p, "w");
    if (!f) throw new Error("dts: cannot write " + p);
    f.puts(s);
    f.close();
}

function parseBig(text) {
    const lines = text.split("\n");
    const isComment = (l) => /^\s*(\/\*|\*|\/\/)/.test(l);
    const mods = [];
    for (let i = 0; i < lines.length; i++) {
        const m = /^declare module "([^"]+)" \{/.exec(lines[i]);
        if (!m) continue;
        let s = i;
        while (s > 0 && isComment(lines[s - 1])) s--;
        let e = i;
        while (e < lines.length && lines[e] !== "}") e++;
        if (e >= lines.length) throw new Error("dts: unterminated module " + m[1]);
        mods.push({ name: m[1], start: s, end: e });
        i = e;
    }
    if (!mods.length) throw new Error("dts: no modules found");
    const segs = [];
    let g = 0;
    const gap = (a, b, owner) => {
        if (a > b) return;
        const body = lines.slice(a, b + 1);
        if (body.every((l) => l === "") && owner) { owner.end = b; return; }
        segs.push({ name: "@globals-" + (++g), start: a, end: b });
    };
    if (mods[0].start > 0) segs.push({ name: "@prelude", start: 0, end: mods[0].start - 1 });
    for (let k = 0; k < mods.length; k++) {
        const seg = { name: mods[k].name, start: mods[k].start, end: mods[k].end };
        segs.push(seg);
        gap(mods[k].end + 1, (k + 1 < mods.length ? mods[k + 1].start : lines.length) - 1, seg);
    }
    return segs.map((s) => ({ name: s.name, text: lines.slice(s.start, s.end + 1).join("\n") }));
}

function split() {
    const segs = parseBig(read("dynajs.d.ts"));
    const files = new Map();
    for (const s of segs) {
        const f = fileFor(s.name);
        if (!files.has(f)) files.set(f, []);
        files.get(f).push(s);
    }
    for (const [f, ss] of files) {
        const head = f.startsWith("src/types/") ? "" : refLine(f) + "\n";
        write(f, head + ss.map((s) => MARK + s.name + "\n" + s.text).join("\n"));
    }
    write(ORDER, segs.map((s) => s.name + "\t" + fileFor(s.name)).join("\n") + "\n");
    print("dts split: " + segs.length + " segments into " + files.size + " files");
}

function loadBlocks(f) {
    const lines = read(f).split("\n");
    const blocks = new Map();
    let cur = null;
    for (let i = 0; i < lines.length; i++) {
        if (i === 0 && /^\/\/\/ <reference path=/.test(lines[0])) continue;
        if (lines[i].startsWith(MARK)) {
            const n = lines[i].slice(MARK.length);
            if (blocks.has(n)) throw new Error("dts: " + f + ":" + (i + 1) + " duplicate block " + n);
            cur = { name: n, lines: [] };
            blocks.set(n, cur);
            continue;
        }
        if (!cur) throw new Error("dts: " + f + ":" + (i + 1) + " text before the first " + MARK.trim() + " marker");
        cur.lines.push(lines[i]);
    }
    return blocks;
}

function bundle() {
    const order = read(ORDER).split("\n").filter(Boolean).map((l) => l.split("\t"));
    const cache = new Map(), used = new Set(), parts = [];
    for (const [name, f] of order) {
        if (!cache.has(f)) cache.set(f, loadBlocks(f));
        const b = cache.get(f).get(name);
        if (!b) throw new Error("dts: " + f + " has no block " + name + " (listed in " + ORDER + ")");
        used.add(f + "\t" + name);
        parts.push(b.lines.join("\n"));
    }
    for (const [f, bs] of cache) for (const n of bs.keys())
        if (!used.has(f + "\t" + n)) throw new Error("dts: " + f + " block " + n + " is not listed in " + ORDER);
    return parts.join("\n");
}

const mode = scriptArgs[1] || "check";
try {
    if (mode === "split") split();
    else if (mode === "bundle") { const s = bundle(); write("dynajs.d.ts", s); print("dts bundle: dynajs.d.ts " + s.split("\n").length + " lines"); }
    else if (mode === "check") {
        const want = bundle(), have = read("dynajs.d.ts");
        if (want !== have) {
            const a = want.split("\n"), b = have.split("\n");
            let i = 0;
            while (i < a.length && a[i] === b[i]) i++;
            print("dts check: FAIL dynajs.d.ts differs from the per-module .d.ts files at line " + (i + 1) +
                "\n  modules: " + JSON.stringify(a[i]) + "\n  big:     " + JSON.stringify(b[i]) +
                "\n  fix: edit the module's .d.ts, then ./dynajs --std tools/dts.js bundle");
            std.exit(1);
        }
        print("dts check: ok (" + have.split("\n").length + " lines)");
    } else throw new Error("usage: ./dynajs --std tools/dts.js split | bundle | check");
} catch (e) { print(String(e && e.message || e)); std.exit(1); }
