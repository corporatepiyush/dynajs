// Unit worker: parses the units the parent hands over, reports what each one
// loaded, and derives a unit's claimed files when the parent says which.
// Any exception is posted back as {error} so the parent fails instead of waiting.
import * as os from "os";
import { decode } from "dyna:bytes";
import { fileStats } from "./discover.js";
import { unitContext } from "./units.js";
import { parseOne, deriveClaimed } from "./build.js";

const port = os.Worker.parent;
const parked = new Map();
let ctx = null;
port.onmessage = (ev) => {
  const m = ev.data;
  try {
    if (m.init) {
      const { abs, files, defines, buf, offs, statsOf } = m.init;
      const text = files.map((f, i) => (offs[i + 1] > offs[i] ? decode(buf.subarray(offs[i], offs[i + 1]), "iso-8859-1") : ""));
      ctx = unitContext(abs, files, (f) => text[f.id - 1], defines);
      const [k, jobs] = statsOf, rows = [];
      for (let i = k; i < files.length; i += jobs) rows.push([files[i].id, fileStats(text[i])]);
      port.postMessage({ stats: rows });
    } else if (m.parse !== undefined) {
      const u = parseOne(ctx, m.id);
      if (u !== null && !u.panic) parked.set(m.parse, u);
      port.postMessage({ parsed: m.parse, head: u === null || u.panic ? u : { root: u.root, loaded: u.loaded } });
    } else if (m.derive !== undefined) {
      const u = parked.get(m.derive);
      parked.delete(m.derive);
      port.postMessage({ derived: m.derive, files: deriveClaimed(ctx, u, m.claimed) });
    } else if (m.done) port.onmessage = null;
  } catch (e) {
    port.postMessage({ error: String(e) + "\n" + (e.stack || "") });
    port.onmessage = null;
  }
};
