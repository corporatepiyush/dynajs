// codegraph (C) -- dynajs port. Entry point and CLI.
import * as std from "std";
import * as fs from "dyna:file";
import { encode } from "dyna:bytes";
import { buildGraph } from "./codegraph/build.js";
import { dumpText } from "./codegraph/dump.js";
import { report, selfCheck } from "./codegraph/report.js";
import { render } from "./codegraph/render.js";
import { jtrunc } from "./codegraph/text.js";
import { goQuote } from "./codegraph/util.js";
import { csv, json } from "./codegraph/output.js";
import { SHAPE_RUNNERS } from "./codegraph/shapes.js";
import { QG, isStr } from "./codegraph/qcore.js";
import { RUNNERS } from "./codegraph/queries.js";
import { saveAST as writeAST, loadAST as readAST } from "./codegraph/astio.js";
import catalog from "./codegraph/catalog.json" with { type: "json" };

const TARGET = "C23 (ISO/IEC 9899:2024) + GNU extensions tolerated (real preprocessor)";
const u8 = (s) => (/[^\x00-\x7f]/.test(s) ? String.fromCharCode(...encode(s, "utf-8")) : s);
const QUERIES = [...catalog.queries, ...catalog.dynQueries, ...catalog.dynShapeQueries].map(([name, title, notes, fn]) => ({ name, title: u8(title), notes: u8(notes), fn }));
const METRICS = [...catalog.metricsCat, ...catalog.dynMetrics].map(([name, title, notes, fn]) => ({ name, title: u8(title), notes: u8(notes), fn }));
const VALUE_FLAGS = new Set(["module", "limit", "csv", "json", "save", "dump", "save-ast", "load-ast"]);
const BOOL_FLAGS = new Set(["list", "metrics", "report", "quiet", "no-tests", "include-generated", "include-vendored", "version",
  "deps", "force", "install-deps", "schema", "help", "h", "summary", "parse-errors", "selfcheck"]);

const outBuf = [];
const flushOut = () => { if (outBuf.length) { std.out.writeBytes(encode(outBuf.join(""), "iso-8859-1")); outBuf.length = 0; } std.out.flush(); };
const errOut = (s) => { flushOut(); std.err.writeBytes(encode(s, "iso-8859-1")); std.err.flush(); };
const exit = (rc) => { flushOut(); std.exit(rc); };

function usage() {
  outBuf.push(`usage: codegraph_c [options] [root] [which ...]

Parse a C tree into an in-memory graph and query it in one shot.
Target: ${TARGET}

  root            directory to analyse (default: .)
  which           queries to run, by number or by name (default: all of them)

what to print
  --list          list the ${QUERIES.length} defect queries and exit
  --metrics       switch to the ${METRICS.length} structural metrics (combine with --list,
                  --summary, --json, --csv or a selector)
  --summary       one line per query: rows found and the top row. Start here.
  --report        overview: size, call graph, how much of it to trust, the
                  biggest modules and the heaviest functions
  --parse-errors  what the parser could not read, and where
  --selfcheck     test the graph against invariants its own numbers must obey
                  (loop depth vs loop count, fan-in vs the edge table, member
                  offsets vs struct size ...). Exits 1 if any is violated: that
                  is a bug in this tool, not in the code it read.
  --schema        print the tables and columns of the graph and exit

narrowing
  --module PAT    only rows whose module matches PAT (% is a wildcard)
  --limit N       at most N rows per query (default: all)
  --no-tests      leave test files out of the graph
  --include-generated, --include-vendored
                  keep files that are skipped by default

machine output
  --json Q        one query as a JSON array (Q is a number or a name)
  --csv Q         one query as CSV
  --dump PATH     write every table as text
  --save PATH     same as --dump; refuses to overwrite without --force
  --save-ast PATH write the parsed state; --load-ast PATH reads it back and
                  skips parsing. It takes no root: --load-ast s.ast dead-code

other
  --quiet         no progress on stderr
  --force         allow --save / --save-ast to overwrite
  --deps, --install-deps, --version, -h

Progress and warnings go to stderr; results go to stdout.

examples
  codegraph_c src --summary
  codegraph_c src --report
  codegraph_c src dead-code untrusted-frontier --limit 20
  codegraph_c src --metrics risk-ranked --limit 25 --module 'src%'
  codegraph_c src --json lock-order-inversion
`);
}

function isNegNumber(a) {
  if (a.length < 2 || a[0] !== "-") return false;
  let digits = 0, dot = false;
  for (const c of a.slice(1)) {
    if (c >= "0" && c <= "9") digits++;
    else if (c === "." && !dot) dot = true;
    else return false;
  }
  if (!dot) return digits >= 1;
  const last = a[a.length - 1];
  return last >= "0" && last <= "9";
}

function schemaNative() {
  let b = "";
  for (const t of catalog.graphShape) {
    b += "table " + t.name + " -- " + t.note + "\n";
    let line = "  ";
    t.cols.forEach(([name, kind, opt], i) => {
      const piece = name + (opt ? "?" : "") + " " + kind;
      if (i > 0 && line.length + 1 + piece.length > 78) { b += line + "\n"; line = "  "; }
      if (line.length > 2) line += " ";
      line += piece;
    });
    if (line.length > 2) b += line + "\n";
    b += "\n";
  }
  return b + "no query layer: there is no expression language here. Every\nquestion is a loop over a slice, a lookup of a column, and a reduce.\n";
}

function pickQuery(cat, sel, metrics) {
  const which = metrics ? "metric" : "query", other = metrics ? QUERIES : METRICS;
  if (/^[+-]?\d+$/.test(sel) && Math.abs(Number(sel)) < 2 ** 63) {
    const n = Number(sel);
    if (n >= 1 && n <= cat.length) return n;
    errOut(`codegraph_c: no ${which} ${n} (there are ${cat.length}; see --list)\n`);
    exit(2);
  }
  const ix = cat.findIndex((q) => q.name === sel);
  if (ix >= 0) return ix + 1;
  const near = cat.filter((q) => q.name.includes(sel)).map((q) => q.name);
  if (near.length === 1) return cat.findIndex((q) => q.name === near[0]) + 1;
  let msg = `codegraph_c: no ${which} named ${goQuote(sel)} (see --list)\n`;
  if (near.length) msg += `  did you mean: ${near.join(", ")}\n`;
  for (const q of other) {
    if (q.name === sel) msg += metrics ? `  ${goQuote(sel)} is a defect query: drop --metrics\n` : `  ${goQuote(sel)} is a metric: add --metrics\n`;
  }
  errOut(msg);
  exit(2);
}

function warnCoverage(g) {
  let parsed = 0, bad = 0, errs = 0;
  for (const f of g.files) {
    if (!f.parsed) continue;
    parsed++;
    const n = f.nParseErrors || 0;
    if (n > 0) { bad++; errs += n; }
  }
  if (parsed > 0 && bad * 100 > parsed) {
    errOut(`codegraph_c: ${bad} of ${parsed} files have parse diagnostics (${errs} in all); functions near them may be missing or merged. --parse-errors lists them.\n`);
  }
}

const byteCmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

function msgKind(m) {
  let i = m.indexOf(" near '");
  if (i >= 0) { const j = m.lastIndexOf("'"); if (j > i + 6) return m.slice(0, i + 6) + "..." + m.slice(j + 1); }
  i = m.indexOf(" token '");
  if (i >= 0) { const j = m.lastIndexOf("'"); if (j > i + 7) return m.slice(0, i + 7) + "..." + m.slice(j + 1); }
  return m.startsWith("#error") ? "#error reached" : m;
}

function printParseErrors(out, g, lim, fromAST) {
  out.push("\n" + "=".repeat(78) + "\nPARSE ERRORS\n" + "-".repeat(78) + "\n");
  if (fromAST) {
    let tot = 0;
    for (const f of g.files) tot += f.nParseErrors || 0;
    if (tot === 0) { out.push(" none: every file parsed without a diagnostic\n"); return; }
    out.push(` ${tot} diagnostics were counted when this state was saved; the messages\n are not stored in it. Re-run against the source tree to list them.\n`);
    return;
  }
  let total = 0;
  const all = [];
  for (const fid of [...g.perFile.keys()].sort((a, b) => a - b)) { total += g.perFile.get(fid).n; all.push(...g.perFile.get(fid).msgs); }
  const nErrDir = all.filter((m) => m.msg.startsWith("#error")).length;
  if (total === 0 && nErrDir === 0) { out.push(" none: every file parsed without a diagnostic\n"); return; }
  const kinds = new Map();
  for (const m of all) { const k = msgKind(m.msg); kinds.set(k, (kinds.get(k) || 0) + 1); }
  const kr = [...kinds].map(([k, v]) => [v, k]).sort((a, b) => b[0] - a[0] || byteCmp(a[1], b[1]));
  out.push(` ${total} diagnostics. One unreadable construct is counted once per token it\n derails, so the FIRST message in a file is the cause and the rest are\n usually its echo.\n`);
  if (nErrDir > 0) out.push(` Plus ${nErrDir} \`#error\` directive(s) reached. Those are not parse failures: the\n code refused a configuration this run could not determine (a generated\n config.h, a compiler it does not recognise), and what was read on the\n way there is intact. They are not in the count above.\n`);
  out.push("\n");
  render(out, kr, ["n", "kind"]);
  out.push("\n");
  const P = (fid) => g.files[fid - 1].path;
  const sorted = all.map((m, i) => [m, i]).sort(([x, i], [y, j]) =>
    (x.fid !== y.fid ? byteCmp(P(x.fid), P(y.fid)) : 0) || x.line - y.line || i - j).map(([m]) => m);
  const rows = [];
  let lf = 0, ll = -1;
  for (const m of sorted) {
    if (m.fid === lf && m.line === ll) continue;
    lf = m.fid; ll = m.line;
    rows.push([P(m.fid) + ":" + m.line, jtrunc(m.msg, 90)]);
  }
  if (lim >= 0 && rows.length > lim) rows.length = lim;
  render(out, rows, ["at", "first diagnostic on this line"]);
}

function runQuery(q, g, qg, p) {
  if (SHAPE_RUNNERS[q.name]) return SHAPE_RUNNERS[q.name](g, p);
  const r = RUNNERS[q.fn];
  if (!r) return [["NOT_PORTED"], []];
  return r(qg, p);
}

function splitNotes(s) {
  if (s === "") return [];
  const out = s.split("\n");
  while (out.length && out[out.length - 1] === "") out.pop();
  return out;
}

function summaryTop(cols, r) {
  let name = "", at = "";
  for (let i = 0; i < cols.length; i++) {
    const c = cols[i];
    if (i >= r.length || !isStr(r[i])) continue;
    if (c === "at" || c === "first_at") { if (at === "") at = r[i].s; }
    else if (name === "") name = r[i].s;
  }
  if (at !== "" && name !== "") return jtrunc(name, 44) + "  " + at;
  return jtrunc(name + at, 70);
}

function printSummary(out, g, qg, cat, sel, p, metrics) {
  if (sel.length === 0) for (let i = 1; i <= cat.length; i++) sel.push(i);
  const all = { mod: p.mod, lim: -1 };
  const rows = [];
  let hit = 0;
  for (const k of sel) {
    const q = cat[k - 1];
    const [cols, rs] = runQuery(q, g, qg, all);
    let top = "";
    if (rs.length > 0) { hit++; top = summaryTop(cols, rs[0]); }
    rows.push([k, q.name, rs.length, top]);
  }
  out.push(`\n${"=".repeat(78)}\nSUMMARY OF ${metrics ? "METRICS" : "QUERIES"}  module=${p.mod}\n${"-".repeat(78)}\n`);
  out.push(` ${hit} of ${sel.length} returned rows. \`rows\` is how many the query found, not how\n many are defects: read a query's MISLEADS note before acting on it.\n\n`);
  render(out, rows, ["#", "name", "rows", "first row"]);
}

function writeDump(path, g) {
  const f = std.open(path, "w");
  if (!f) throw new Error("open failed");
  f.writeBytes(encode(dumpText(g), "iso-8859-1"));
  f.close();
}

function saveGraph(g, path, force) {
  if (!path) return;
  let st = null;
  try { st = fs.lstat(new fs.Path(path)); } catch (e) {}
  if (st) {
    if (st.isSymlink) {
      if (!force) {
        let real = path;
        try { real = fs.realPath(new fs.Path(path)).toString(); } catch (e) {}
        errOut(`refusing to overwrite ${path} (pass --force) -- it is a symlink to ${real}\n`);
        return;
      }
      try { fs.remove(new fs.Path(path)); } catch (e) {}
    } else if (!force) { errOut(`refusing to overwrite ${path} (pass --force)\n`); return; }
  }
  try { writeDump(path, g); } catch (e) { errOut(`could not write ${path}: ${e.message}\n`); exit(2); }
  outBuf.push(`(graph also written to ${path})\n`);
}

const notPorted = (what) => { errOut(`codegraph (dynajs port): mode not ported yet: ${what}\n`); exit(2); };

async function main(args) {
  if (args.some((a) => a === "-h" || a === "--help")) { usage(); return 0; }
  const positional = [], flags = new Map(), bool = new Set();
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (isNegNumber(a) || !a.startsWith("-")) { positional.push(a); continue; }
    let name = a.replace(/^-+/, ""), val = "";
    const j = name.indexOf("=");
    if (j >= 0) { val = name.slice(j + 1); name = name.slice(0, j); }
    else if (i + 1 < args.length && (!args[i + 1].startsWith("-") || isNegNumber(args[i + 1])) && VALUE_FLAGS.has(name)) val = args[++i];
    if (!VALUE_FLAGS.has(name) && !BOOL_FLAGS.has(name)) {
      usage();
      errOut(`codegraph_c: error: unrecognized arguments: ${a}\n`);
      return 2;
    }
    if (VALUE_FLAGS.has(name)) flags.set(name, val); else bool.add(name);
  }
  let lim = -1;
  if (flags.has("limit")) {
    const v = flags.get("limit");
    if (!/^[+-]?\d+$/.test(v)) { usage(); errOut(`codegraph_c: error: argument --limit: invalid int value: '${v}'\n`); return 2; }
    lim = Number(v);
  }
  const modPat = flags.has("module") ? flags.get("module") : "%";
  const saveAST = flags.get("save-ast") || "", loadAST = flags.get("load-ast") || "";
  if (saveAST && loadAST) { errOut("--save-ast and --load-ast cannot be used together\n"); return 2; }
  if (bool.has("version")) { outBuf.push(`codegraph_c  target=${TARGET}  schema=v2\n`); return 0; }
  if (bool.has("deps") || bool.has("install-deps")) { outBuf.push("dependencies for codegraph-c:\n  (none -- pure standard library)\n"); return 0; }
  if (bool.has("schema")) { outBuf.push(schemaNative()); return 0; }
  const metrics = bool.has("metrics");
  const cat = metrics ? METRICS : QUERIES;
  if (bool.has("list")) {
    const w = Math.max(...cat.map((q) => q.name.length));
    cat.forEach((q, i) => outBuf.push(`${String(i + 1).padStart(3)}. ${q.name.padEnd(w)} ${q.title}\n`));
    outBuf.push(metrics ? `\n${cat.length} metrics. Without --metrics, --list shows the ${QUERIES.length} defect queries.\n`
      : `\n${cat.length} defect queries. --metrics --list shows the ${METRICS.length} structural metrics.\n`);
    return 0;
  }
  const csvN = flags.has("csv") ? pickQuery(cat, flags.get("csv"), metrics) : -1;
  const jsonN = flags.has("json") ? pickQuery(cat, flags.get("json"), metrics) : -1;
  let dir = ".";
  if (loadAST) positional.unshift(dir); else if (positional.length) dir = positional[0];
  const sel = positional.slice(1).map((p) => pickQuery(cat, p, metrics));
  if (!loadAST) {
    let ok = false;
    try { ok = fs.stat(new fs.Path(dir)).isDir; } catch (e) {}
    if (!ok) { errOut(`not a directory: ${dir}\n`); return 2; }
  }
  const machine = csvN >= 0 || jsonN >= 0;
  const quiet = machine || bool.has("quiet");
  const savePath = flags.get("save") || "", force = bool.has("force");
  const t0 = Date.now();
  let g;
  if (loadAST) {
    try { g = readAST(loadAST); } catch (e) { errOut(`load-ast: ${e.message}\n`); return 2; }
  } else g = await buildGraph(dir, args, { includeTests: !bool.has("no-tests"), includeGenerated: bool.has("include-generated"),
    includeVendored: bool.has("include-vendored") });
  const took = (Date.now() - t0) / 1000;
  if (saveAST) {
    let exists = false;
    try { fs.lstat(new fs.Path(saveAST)); exists = true; } catch (e) {}
    if (exists && !force) { errOut(`refusing to overwrite ${saveAST} (pass --force)\n`); return 2; }
    const ts = Date.now();
    let nb;
    try { nb = writeAST(g, saveAST); } catch (e) { errOut(`save-ast: ${e.message}\n`); return 2; }
    if (!quiet) errOut(`ast state written to ${saveAST}: ${nb} bytes in ${((Date.now() - ts) / 1000).toFixed(1)}s\n`);
  }
  if (flags.has("dump")) {
    const path = flags.get("dump");
    try { writeDump(path, g); } catch (e) { errOut(`could not write ${path}: ${e.message}\n`); return 2; }
  }
  const qg = new QG(g, g);
  const p = { mod: modPat, lim };
  if (machine) {
    warnCoverage(g);
    const q = cat[(csvN >= 0 ? csvN : jsonN) - 1];
    const [cols, rows] = runQuery(q, g, qg, p);
    if (csvN >= 0) csv(outBuf, cols, rows); else json(outBuf, cols, rows);
    return 0;
  }
  if (!quiet) errOut(`codegraph-c: ${loadAST ? g.files.length : g.disc.parse.length} files ${loadAST ? "loaded from AST" : "parsed into memory"} in ${took.toFixed(1)}s module=${modPat} limit=${lim >= 0 ? lim : "all"}\n`);
  warnCoverage(g);
  let alone = false;
  if (bool.has("report")) { report(outBuf, g); alone = true; }
  if (bool.has("parse-errors")) { printParseErrors(outBuf, g, lim, !!loadAST); alone = true; }
  if (bool.has("selfcheck")) {
    if (selfCheck(outBuf, g) > 0) { saveGraph(g, savePath, force); return 1; }
    alone = true;
  }
  if (bool.has("summary")) printSummary(outBuf, g, qg, cat, sel.slice(), p, metrics);
  if (bool.has("summary") || (alone && sel.length === 0)) { saveGraph(g, savePath, force); return 0; }
  const run = sel.length ? sel : cat.map((_, i) => i + 1);
  for (const k of run) {
    const q = cat[k - 1];
    outBuf.push(`\n${"=".repeat(78)}\nQ${k}. ${q.name} -- ${q.title}\n${"-".repeat(78)}\n`);
    for (const line of splitNotes(q.notes)) outBuf.push(` ${line}\n`);
    outBuf.push("\n");
    const [cols, rows] = runQuery(q, g, qg, p);
    render(outBuf, rows, cols);
  }
  saveGraph(g, savePath, force);
  return 0;
}

main(scriptArgs.slice(1)).then(exit, (e) => { errOut(String(e) + "\n" + (e.stack || "")); exit(3); });
