// flags: --std
// split-rename.js MODULE OLD NEW [OLD NEW ...] -- rename identifiers inside src/modules/MODULE/.
//
// A split module exports its formerly-static helpers, so a generic helper name (buf_reserve) can collide
// with another module's at link time. This renames whole identifiers, never a member access (`.x`/`->x`).
import * as std from "std";
import * as os from "os";

const [m, ...rest] = scriptArgs.slice(1);
if (!m || rest.length % 2) { print("usage: split-rename.js MODULE OLD NEW [OLD NEW ...]"); std.exit(2); }
const here = import.meta.url.replace(/^file:\/\//, "").replace(/\/[^/]*$/, "");
const d = here.replace(/\/tools$/, "") + "/src/modules/" + m;
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const [names, err] = os.readdir(d);
if (err) { print("split-rename: no directory " + d); std.exit(1); }
for (const f of names.filter((n) => n !== "." && n !== "..").sort()) {
    const p = d + "/" + f;
    const o = std.loadFile(p);
    if (o === null) continue;
    let t = o;
    for (let i = 0; i < rest.length; i += 2) t = t.replace(new RegExp("(?<![\\w.>])" + esc(rest[i]) + "(?!\\w)", "g"), () => rest[i + 1]);
    if (t !== o) { const fd = std.open(p, "w"); fd.puts(t); fd.close(); print("renamed in", p); }
}
