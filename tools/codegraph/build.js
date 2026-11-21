// Graph build: discovery, translation units, parse, and the derivation slice the
// ported modes read (parse diagnostics, function symbols, call sites, imports).
// Units run on os.Worker threads (CODEGRAPH_JOBS, default the CPU count); the
// claims and the merge run on the main thread in unit and file-id order.
import * as os from "os";
import * as std from "std";
import * as sys from "dyna:sys";
import { discover, fileStats, applyStats } from "./discover.js";
import { encode } from "dyna:bytes";
import { absPath, unitContext, preprocessUnit, claimUnits } from "./units.js";
import { projectDefines } from "./config.js";
import { cleanPath } from "./pp.js";
import { parseUnit, N } from "./parse.js";
import { DerivedFile, UnitState, fileOut } from "./derive.js";
import { newGraph, newExtractor, merge, resolveCalls, collapseHazards, materialize, postBuild, parseManifests, finishMeta } from "./graph.js";

const IMPORT_INDEXES = ["__init__.py", "index.ts", "index.tsx", "index.js", "index.mjs", "mod.rs", "lib.rs"];
const pathDir = (rel) => { const i = rel.lastIndexOf("/"); return i < 0 ? "." : rel.slice(0, i); };

// Phase 1 of a unit: preprocess and parse. The result stays on the thread that
// made it; only {root, loaded} or null / {panic} crosses to the claimer.
export function parseOne(ctx, id) {
  const u = preprocessUnit(ctx, id);
  if (u === null || u.panic) return u;
  const pp = u.pp;
  const { fa, errs } = parseUnit(pp.outToks, pp.outFile, new Map(pp.files.map((s) => [s.id, s.data])));
  pp.outToks = null;
  pp.outFile = null;
  u.fa = fa;
  u.errs = errs;
  return u;
}

// Phase 2: derive the files this unit claimed, each into a fileOut.
export function deriveClaimed(ctx, u, claimed) {
  const { pp, fa, errs } = u;
  const want = new Set(claimed);
  const us = new UnitState(fa);
  const derived = {};
  for (const [path, fid] of u.loaded) {
    if (!want.has(fid) || derived[fid]) continue;
    const src = pp.files.find((x) => x.path === path);
    const srcID = src ? src.id : 1;
    const raw = ctx.sources.get(path) ?? "";
    let n = 0;
    const msgs = [];
    const keep = (line, msg) => { if (msgs.length < 200) msgs.push({ fid, line, msg }); };
    if (srcID === 1) for (const e of pp.errs) { if (!e.msg.startsWith("#error")) n++; keep(e.line, e.msg); }
    for (const e of errs) if (e.file === srcID) { n++; keep(e.line, e.msg); }
    const df = new DerivedFile(us, pp, raw, srcID, ctx.files[fid - 1]);
    df.derive();
    const o = fileOut(df, ctx.absID, msgs, n, pp.cond.length);
    o.n = n; o.msgs = msgs;
    derived[fid] = o;
  }
  return derived;
}

const header = (u) => (u === null || u.panic ? u : { root: u.root, loaded: u.loaded });

const T = { pp: 0, parse: 0, derive: 0 };
function runSerial(ctx, ids) {
  const out = [];
  const claim = new Set();
  for (const id of ids) {
    let t0 = Date.now(); const t0x = t0;
    const u = preprocessUnit(ctx, id);
    T.pp += Date.now() - t0; t0 = Date.now();
    if (u !== null && !u.panic) {
      const pp = u.pp;
      const { fa, errs } = parseUnit(pp.outToks, pp.outFile, new Map(pp.files.map((s) => [s.id, s.data])));
      pp.outToks = null; pp.outFile = null; u.fa = fa; u.errs = errs;
    }
    T.parse += Date.now() - t0;
    const h = header(u);
    out.push(h);
    if (h === null || h.panic) continue;
    const mine = [];
    for (const [path, fid] of h.loaded) if (!claim.has(path)) { claim.add(path); mine.push(fid); }
    const t1 = Date.now();
    h.derived = deriveClaimed(ctx, u, mine);
    T.derive += Date.now() - t1;
    (T.per || (T.per = [])).push([Date.now() - t0x, ctx.files[id - 1].path]);
  }
  if (PROF && T.per) { T.per.sort((a, b) => b[0] - a[0]); for (const [ms, p] of T.per.slice(0, 12)) std.err.puts(`[prof]   unit ${ms}ms ${p}\n`); }
  if (PROF) std.err.puts(`[prof] serial pp ${T.pp}ms parse ${T.parse}ms derive ${T.derive}ms\n`);
  return out;
}

// A pool of workers. Units are parsed in any order; claims are made in unit
// order as soon as every earlier unit has reported what it loaded, and the
// holder of a unit then derives only what it claimed. At most `ahead` units
// are parsed but not yet claimed, which bounds the parked memory.
// Every file's bytes in one buffer, so a worker decodes its copy instead of reading the tree again.
function packSources(disc) {
  const parts = [], offs = [0];
  let n = 0;
  for (const f of disc.files) { const b = encode(disc.data.get(f.id), "iso-8859-1"); parts.push(b); n += b.length; offs.push(n); }
  const buf = new Uint8Array(n);
  let o = 0;
  for (const b of parts) { buf.set(b, o); o += b.length; }
  return { buf, offs };
}

function runParallel(abs, files, defines, ids, jobs, packed, onStats) {
  return new Promise((resolve, reject) => {
    const n = ids.length, heads = new Array(n), holder = new Array(n);
    const claim = new Set();
    let next = 0, turn = 0, derivedN = 0, statsN = 0;
    const ahead = 3 * jobs;
    const workers = [], idle = [];
    const url = import.meta.url.replace(/^file:\/\//, "").replace(/build\.js$/, "worker.js");
    const finish = () => {
      for (const x of workers) { x.postMessage({ done: true }); x.onmessage = null; }
      resolve(heads);
    };
    const feed = () => {
      while (idle.length && next < n && next - turn < ahead) {
        const w = idle.pop(), i = next++;
        holder[i] = w;
        w.postMessage({ parse: i, id: ids[i] });
      }
    };
    const advance = () => {
      for (; turn < n && heads[turn] !== undefined; turn++) {
        const h = heads[turn];
        if (h === null || h.panic) { derivedN++; continue; }
        const mine = [];
        for (const [path, fid] of h.loaded) if (!claim.has(path)) { claim.add(path); mine.push(fid); }
        holder[turn].postMessage({ derive: turn, claimed: mine });
      }
    };
    for (let k = 0; k < jobs; k++) {
      const w = new os.Worker(url);
      w.onmessage = (ev) => {
        const m = ev.data;
        if (m.error !== undefined) {
          for (const x of workers) { x.postMessage({ done: true }); x.onmessage = null; }
          reject(new Error("worker: " + m.error));
          return;
        }
        if (m.stats !== undefined) { onStats(m.stats); statsN++; if (derivedN === n && statsN === jobs) finish(); return; }
        if (m.parsed !== undefined) {
          heads[m.parsed] = m.head;
          idle.push(w);
          advance();
        } else {
          heads[m.derived].derived = m.files;
          derivedN++;
        }
        if (derivedN === n) { if (statsN === jobs) finish(); return; }
        feed();
      };
      w.onerror = (e) => reject(e);
      w.postMessage({ init: { abs, files, defines, buf: packed.buf, offs: packed.offs, statsOf: [k, jobs] } });
      workers.push(w);
      idle.push(w);
    }
    if (n === 0) return;
    feed();
  });
}

function resolveImportTargets(files, imports) {
  const byPath = new Map();
  for (const f of files) {
    if (!byPath.has(f.path)) byPath.set(f.path, f.id);
    const j = f.path.lastIndexOf(".");
    const stem = j > 0 ? f.path.slice(0, j) : f.path;
    if (!byPath.has(stem)) byPath.set(stem, f.id);
  }
  const look = (c) => {
    c = c.replace(/^\/+|\/+$/g, "");
    if (c === "") return 0;
    if (byPath.has(c)) return byPath.get(c);
    for (const ix of IMPORT_INDEXES) if (byPath.has(c + "/" + ix)) return byPath.get(c + "/" + ix);
    return 0;
  };
  let n = 0;
  for (const im of imports) {
    if (im.HasTargetID || im.Target === "") continue;
    const here = pathDir(files[im.FileID - 1].path);
    const t = im.Target;
    let hit = 0;
    if (t.startsWith(".")) {
      const nUp = t.length - t.replace(/^\.+/, "").length;
      const rest = t.includes("/") ? t.replace(/^[./]+/, "") : t.slice(nUp).replaceAll(".", "/");
      let base = here;
      for (let k = 0; k < Math.max(0, nUp - 1); k++) { base = pathDir(base); if (base === ".") break; }
      hit = base !== "" ? look(base + "/" + rest) : look(rest);
    } else {
      hit = look(t.replaceAll(".", "/"));
      if (hit === 0) hit = look(here + "/" + t);
    }
    if (hit !== 0 && hit !== im.FileID) { im.TargetID = hit; im.HasTargetID = true; n++; }
  }
  return n;
}

const PROF = std.getenv("CG_PROF") ? [] : null;
let tLast = 0;
const mark = (what) => { if (!PROF) return; const t = Date.now(); std.err.puts(`[prof] ${what} ${t - tLast}ms\n`); tLast = t; };

export async function buildGraph(root, argv, opts = { includeTests: true }) {
  tLast = Date.now();
  const abs = absPath(root);
  const disc = discover(abs, opts);
  mark("discover");
  const defines = projectDefines(argv);
  const files = disc.files.map((f) => ({ id: f.id, path: f.path, parsed: !!f.parsed, hasData: disc.data.get(f.id).length > 0, moduleID: f.moduleID, isTest: f.isTest, isGenerated: f.isGenerated }));
  const env = std.getenv("CODEGRAPH_JOBS");
  const jobs = Math.max(1, Math.min(disc.parse.length, env ? Number(env) || 1 : defaultJobs()));
  let results;
  let nStats = 0;
  const onStats = (rows) => { for (const [id, st] of rows) { applyStats(disc.files[id - 1], st); nStats++; } };
  if (jobs <= 1) {
    onStats(disc.files.map((f) => [f.id, fileStats(disc.data.get(f.id))]));
    const ctx = unitContext(abs, files, (f) => disc.data.get(f.id), defines);
    results = runSerial(ctx, disc.parse);
  } else {
    const packed = packSources(disc);
    mark("pack");
    results = await runParallel(abs, files, defines, disc.parse, jobs, packed, onStats);
    if (nStats !== disc.files.length) throw new Error(`file statistics: ${nStats} of ${disc.files.length}`);
  }
  mark("units (parse+derive)");
  const res = claimUnits(results);
  const perFile = new Map();
  for (const u of results) {
    if (!u || u.panic) continue;
    for (const fid of u.claimed) {
      const o = u.derived[fid];
      const f = disc.files[fid - 1];
      f.nParseErrors = o.nErrs; f.nMissingNodes = o.nMissing; f.docLines = o.docLines;
      if (!perFile.has(fid)) perFile.set(fid, o);
    }
  }
  const g = newGraph(abs, disc);
  g.root = root; g.perFile = perFile; g.failed = res.failed;
  const e = newExtractor(g);
  for (const fid of [...perFile.keys()].sort((a, b) => a - b)) merge(e, perFile.get(fid));
  mark("merge");
  const nres = resolveImportTargets(disc.files, g.imports);
  resolveCalls(e);
  mark("resolveCalls");
  collapseHazards(g);
  const nimp = g.imports.filter((im) => im.HasTargetID).length;
  g.meta.set("imports_resolved", `${nimp} of ${g.imports.length} import rows point at a file in this tree (${nres} resolved by path here)`);
  parseManifests(abs, g);
  materialize(g);
  mark("materialize");
  postBuild(g);
  mark("postBuild");
  finishMeta(g, abs, disc.parse.length, res.failed, disc.stats);
  return g;
}

function defaultJobs() {
  return Math.min(8, sys.cpuInfo().threads || 8);
}

