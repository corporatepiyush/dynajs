// --save-ast / --load-ast: the built graph as one file, so a later run skips parsing.
// The port's own format ("CGJS" + version + JSON of every table); the oracle's CGAS
// files are a memory image of Go structs and are refused with the oracle's bad-magic text.
import * as std from "std";
import * as fs from "dyna:file";
import { encode, decode } from "dyna:bytes";

const MAGIC = "CGJS", VERSION = 1;
const TABLES = ["symbols", "edges", "callsites", "unres", "reach", "cycles", "mkRules", "parseMsgs", "params", "fields", "locals", "literals",
  "markers", "attrs", "imports", "hazards", "enums", "layout", "ssize", "decls", "addrs", "secrets", "allocs", "memops", "macros", "globals",
  "cfgs", "locks", "evops", "apiuses"];

export function saveAST(g, path) {
  const o = { abs: g.abs, root: g.root, failed: g.failed, files: g.files, modules: g.modules, nParsed: g.disc.parse.length, stats: g.disc.stats,
    meta: [...g.meta], perFile: [...g.perFile].map(([fid, f]) => [fid, f.n]) };
  for (const t of TABLES) o[t] = g[t];
  const bytes = encode(MAGIC + " " + VERSION + "\n" + JSON.stringify(o), "iso-8859-1");
  const f = std.open(path, "w");
  if (!f) throw new Error(`open ${path}: cannot create`);
  f.writeBytes(bytes);
  f.close();
  return bytes.length;
}

export function loadAST(path) {
  let st;
  try { st = fs.stat(new fs.Path(path)); } catch (e) { throw new Error(`open ${path}: no such file or directory`); }
  if (!st.isFile) throw new Error(`${path}: not a regular file`);
  const text = decode(fs.readBytes(new fs.Path(path)), "iso-8859-1");
  if (text.length < 32) throw new Error(`${path}: too small (${text.length} bytes) to be an AST state file`);
  if (!text.startsWith(MAGIC + " ")) throw new Error(`${path}: bad magic ${JSON.stringify(text.slice(0, 4))}, want "${MAGIC}" -- not an AST state file`);
  const nl = text.indexOf("\n");
  const v = Number(text.slice(5, nl));
  if (v !== VERSION) throw new Error(`${path}: unsupported state format version ${v} -- re-save with this build to produce v${VERSION}`);
  const o = JSON.parse(text.slice(nl + 1));
  const g = { abs: o.abs, root: o.root, failed: o.failed, files: o.files, modules: o.modules, meta: new Map(o.meta), fromAST: true,
    disc: { files: o.files, modules: o.modules, stats: o.stats, parse: new Array(o.nParsed), data: new Map() },
    perFile: new Map(o.perFile.map(([fid, n]) => [fid, { n, msgs: [] }])) };
  for (const t of TABLES) g[t] = o[t];
  return g;
}
