// flags: --std
// min-includes.js -- give every file of a split library only the headers whose names it uses.
//
// usage: ./dynajs --std tools/min-includes.js DIR UMBRELLA_INCLUDE [--parts PARTS_DIR --prefix INCLUDE_PREFIX] [--dry]
//   DIR               e.g. src/core/ds (holds ds-*.h parts, ds-*.c pieces and one *-internal.h)
//   UMBRELLA_INCLUDE  the line the internal header uses today, e.g. '#include "../dyn-ds.h"'
//   --parts/--prefix  the parts live elsewhere (a module using a core library): read them from PARTS_DIR and
//                     name them as INCLUDE_PREFIX + part, e.g. --parts src/core/ds --prefix core/ds/
//
// The internal header's umbrella include is replaced by the parts its own text uses; each .c gets the
// parts it uses beyond those, after its internal-header include. A header edit then rebuilds only the
// pieces that name something in it. Names are read from the parts: macros, typedef names, struct tags,
// function names. The compiler is the check: build every piece after running this.
import * as std from "std";
import * as os from "os";

const argv = scriptArgs.slice(1);
const die = (m) => { std.err.puts("min-includes: " + m + "\n"); std.exit(1); };
if (argv.length < 2) die("usage: DIR UMBRELLA_INCLUDE [--parts DIR --prefix P] [--dry]");
const [d, umb] = argv;
const dry = argv.includes("--dry");
const opt = (k) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : null);
const pdir = opt("--parts") ?? d;
const pfx = opt("--prefix") ?? "";
const ls = (dir) => { const [names, err] = os.readdir(dir); if (err) die("cannot read " + dir); return names.filter((n) => n !== "." && n !== "..").sort(); };
const read = (p) => { const t = std.loadFile(p); if (t === null) die("cannot read " + p); return t; };
const all = (re, t) => [...t.matchAll(re)].map((m) => m[1]);

const parts = ls(pdir).filter((f) => f.endsWith(".h") && !f.endsWith("-internal.h"));
const internals = ls(d).filter((f) => f.endsWith("-internal.h"));
if (internals.length !== 1) die("need exactly one *-internal.h in " + d);
const internal = internals[0];

const owner = new Map(), deps = new Map();
for (const p of parts) {
    const t = read(pdir + "/" + p);
    deps.set(p, new Set(all(/#include "([^"/]+\.h)"/g, t)));
    const names = new Set([
        ...all(/#\s*define\s+(\w+)/g, t),
        ...all(/\}\s*(\w+)\s*;/g, t),
        ...all(/typedef\s+[^;{]*?\b(\w+)\s*;/g, t),
        ...all(/\(\s*\*\s*(\w+)\s*\)\s*\(/g, t),
        ...all(/\bstruct\s+(\w+)/g, t),
        ...all(/^[^#\s][^;{]*?\b(\w+)\s*\(/gm, t),
    ]);
    names.delete(p.replace(/[^A-Za-z0-9]/g, "_").toUpperCase());
    for (const n of names) if (!owner.has(n)) owner.set(n, p);
}
function closure(hs) {
    const out = new Set(hs), stack = [...hs];
    while (stack.length) for (const x of deps.get(stack.pop()) ?? []) if (!out.has(x)) { out.add(x); stack.push(x); }
    return out;
}
const used = (text) => new Set(all(/\b([A-Za-z_]\w*)\b/g, text).filter((i) => owner.has(i)).map((i) => owner.get(i)));
const order = (hs) => parts.filter((p) => hs.has(p));

const ip = d + "/" + internal;
const it = read(ip);
if (!it.includes(umb)) die(`${internal} does not contain ${umb}`);
const inner = used(it.replace(umb, ""));
const base = closure(inner);
const changes = new Map([[ip, it.replace(umb, order(inner).map((h) => `#include "${pfx}${h}"`).join("\n"))]]);
for (const f of ls(d).filter((f) => f.endsWith(".c"))) {
    const fp = d + "/" + f;
    let t = read(fp);
    const own = f.slice(0, -2) + ".h";
    const need = used(t);
    if (parts.includes(own) && pdir === d) need.add(own);
    const extra = order(new Set([...need].filter((h) => !base.has(h))));
    const line = `#include "${internal}"\n`;
    if (!t.includes(line)) die(`${f} does not include ${internal}`);
    if (extra.length) t = t.replace(line, line + extra.map((h) => `#include "${pfx}${h}"\n`).join(""));
    changes.set(fp, t);
    print(f.padEnd(34) + " " + extra.join(" "));
}
print(internal.padEnd(34) + " " + order(inner).join(" "));
if (!dry) for (const [p, t] of changes) { const fd = std.open(p, "w"); fd.puts(t); fd.close(); }
