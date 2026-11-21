// Merge per-file outputs into the graph, resolve calls, aggregate, index.
import * as fs from "dyna:file";
import { BitSet } from "dyna:structures";
import { jtrunc } from "./text.js";
import { readBytesString } from "./pp.js";
import * as H from "./heur.js";
import data from "./data.json" with { type: "json" };

const LIBC_KNOWN = new Set(Object.keys(data.libcKnown).filter((k) => data.libcKnown[k]));
const b2i = (b) => (b ? 1 : 0);

const ROW_TABLES = ["params", "fields", "locals", "literals", "markers", "attrs", "imports", "hazards", "enums", "layout", "ssize",
  "decls", "addrs", "secrets", "allocs", "memops", "macros", "globals", "cfgs", "locks", "evops", "apiuses"];
const ID_TABLES = new Set(["literals", "markers", "attrs", "decls", "addrs", "secrets", "allocs", "memops", "locks", "evops", "apiuses"]);
const SYM_SHIFT = new Set(["params", "fields", "locals", "literals", "attrs", "hazards", "enums", "layout", "ssize", "secrets", "allocs",
  "memops", "macros", "locks", "evops", "apiuses"]);

export function newGraph(abs, disc) {
  const g = { abs, disc, files: disc.files, modules: disc.modules, symbols: [], edges: [], callsites: [], unres: [], reach: [],
    cycles: [], mkRules: [], meta: new Map(), parseMsgs: [] };
  for (const t of ROW_TABLES) g[t] = [];
  return g;
}

export function newExtractor(g) {
  return { g, fnByName: new Map(), pendSID: [], pendFID: [], pendMID: [], pendName: [], pendLine: [], macroSID: new Map(),
    declared: new Set(), incAdj: null, incReach: null, declFiles: null, pickMore: [], ambigN: 0, declPickN: 0, edgeN: 0, macroN: 0,
    externN: 0, declN: 0, unresN: 0 };
}

export function merge(e, o) {
  const g = e.g;
  g.parseMsgs.push(...o.perr);
  const base = g.symbols.length;
  for (let i = 0; i < o.symbols.length; i++) {
    const s = o.symbols[i];
    s.ID = i + 1 + base;
    g.symbols.push(s);
  }
  for (const t of ROW_TABLES) {
    const rows = o[t], dst = g[t];
    const shift = SYM_SHIFT.has(t), ids = ID_TABLES.has(t);
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i];
      if (ids) r.ID = dst.length + 1;
      if (shift) r.SymbolID += base;
      else if (t === "addrs" && r.HasSym) r.SymbolID += base;
      dst.push(r);
    }
  }
  for (let i = 0; i < o.pendSID.length; i++) {
    e.pendSID.push(o.pendSID[i] + base); e.pendFID.push(o.pendFID[i]); e.pendMID.push(o.pendMID[i]);
    e.pendName.push(o.pendName[i]); e.pendLine.push(o.pendLine[i]);
  }
  o.macros.forEach((m, i) => { const nm = o.macroNames[i]; if (!e.macroSID.has(nm)) e.macroSID.set(nm, m.SymbolID); });
  for (const d of o.decls) e.declared.add(d.Name);
  for (const f of o.funcs) {
    let c = e.fnByName.get(f.name);
    if (!c) e.fnByName.set(f.name, (c = { sid: [], fid: [], mid: [], st: [] }));
    c.sid.push(f.sid + base); c.fid.push(f.fid); c.mid.push(f.mid); c.st.push(f.static);
  }
}

function includes(e, from, to) {
  if (e.incAdj === null) {
    e.incAdj = new Map();
    for (const im of e.g.imports) {
      if (im.HasTargetID && im.TargetID !== im.FileID) {
        let a = e.incAdj.get(im.FileID);
        if (!a) e.incAdj.set(im.FileID, (a = []));
        a.push(im.TargetID);
      }
    }
    e.incReach = new Map();
  }
  let r = e.incReach.get(from);
  if (!r) {
    r = new Set();
    const st = [...(e.incAdj.get(from) || [])];
    while (st.length) {
      const f = st.pop();
      if (r.has(f)) continue;
      r.add(f);
      const nx = e.incAdj.get(f);
      if (nx) for (const x of nx) st.push(x);
    }
    e.incReach.set(from, r);
  }
  return r.has(to);
}

function pickByDecl(e, name, fid, files, stat) {
  if (e.declFiles === null) {
    e.declFiles = new Map();
    for (const d of e.g.decls) {
      let fs = e.declFiles.get(d.Name);
      if (!fs) e.declFiles.set(d.Name, (fs = []));
      if (fs.length === 0 || fs[fs.length - 1] !== d.FileID) fs.push(d.FileID);
    }
  }
  let pick = -1;
  for (const d of e.declFiles.get(name) || []) {
    if (d !== fid && !includes(e, fid, d)) continue;
    for (let x = 0; x < files.length; x++) {
      const f = files[x];
      if (stat[x] || (f !== d && !includes(e, f, d))) continue;
      if (pick >= 0 && pick !== x) return -1;
      pick = x;
    }
  }
  return pick;
}

function pickCallee(e, name, fid, mid) {
  const c = e.fnByName.get(name);
  const files = c.fid;
  for (let x = 0; x < files.length; x++) if (files[x] === fid) return x;
  let ext = -1, extN = 0, extMod = -1, extModN = 0, inc = -1;
  for (let x = 0; x < files.length; x++) {
    if (c.st[x]) {
      if (includes(e, fid, files[x])) { if (inc < 0) inc = x; else e.pickMore.push(x); }
      continue;
    }
    if (extN === 0) ext = x;
    extN++;
    if (c.mid[x] === mid) { if (extModN === 0) extMod = x; extModN++; }
  }
  if (inc >= 0) return inc;
  if (extN === 1) return ext;
  if (extModN === 1) return extMod;
  if (extN > 1) {
    const x = pickByDecl(e, name, fid, files, c.st);
    if (x >= 0) { e.declPickN++; return x; }
    e.ambigN++;
  }
  return -1;
}

function intrinsicName(w) {
  const lo = (c) => (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95;
  const splitAt = (from) => {
    for (let j = from; j + 1 < w.length; j++) {
      if (w.charCodeAt(j) !== 95) continue;
      let ok = true;
      for (let k = from; k < j; k++) if (!lo(w.charCodeAt(k))) { ok = false; break; }
      if (!ok) continue;
      for (let k = j + 1; k < w.length; k++) if (!lo(w.charCodeAt(k))) { ok = false; break; }
      if (ok) return true;
    }
    return false;
  };
  if (w.length < 3) return false;
  if (w[0] === "v") return splitAt(1);
  if (w.startsWith("_mm")) return splitAt(3);
  if (w.startsWith("sv")) return splitAt(2);
  return false;
}
const isExternal = (name) => LIBC_KNOWN.has(name) || data.builtinPrefixes.some((p) => name.startsWith(p)) || intrinsicName(name);
export function allocNameMatch(s) {
  const low = H.asciiLower(s);
  return ["alloc", "free", "strdup", "memdup"].some((suf) => low.length > suf.length && low.endsWith(suf));
}

export function resolveCalls(e) {
  const g = e.g;
  const edges = new Map(), callSites = new Set(), unres = new Map();
  const macroUses = new Map(), ext = new Map(), mac = new Map();
  const allocHz = [];
  const add = (m, k, v) => m.set(k, (m.get(k) || 0) + v);
  const addUnres = (caller, name, cnt, line) => {
    const nm = jtrunc(name, 160);
    const k = caller + "\0" + nm;
    const u = unres.get(k);
    if (u) u.N += cnt;
    else unres.set(k, { CallerID: caller, Name: nm, N: cnt, FirstLine: line });
    e.unresN += cnt;
  };
  for (let i = 0; i < e.pendName.length; i++) {
    const name = e.pendName[i], caller = e.pendSID[i], fid = e.pendFID[i], mid = e.pendMID[i], lines = e.pendLine[i];
    const cnt = lines.length, line = lines[0];
    let ti = -1;
    const cands = e.fnByName.get(name);
    if (cands) {
      e.pickMore.length = 0;
      ti = pickCallee(e, name, fid, mid);
      if (ti < 0) e.pickMore.length = 0;
    }
    for (let pick = -1; ti >= 0 && pick < e.pickMore.length; pick++) {
      const tx = pick >= 0 ? e.pickMore[pick] : ti;
      const target = cands.sid[tx], tf = cands.fid[tx], tm = cands.mid[tx];
      for (const site of lines) {
        const k = caller * 4294967296 + target;
        const ed = edges.get(k);
        if (!ed) edges.set(k, { CallerID: caller, CalleeID: target, NCalls: 1, SameFile: b2i(tf === fid), SameModule: b2i(tm === mid), IsSelf: b2i(caller === target) });
        else ed.NCalls++;
        const ck = caller + "," + target + "," + site;
        if (!callSites.has(ck)) { callSites.add(ck); g.callsites.push({ CallerID: caller, CalleeID: target, Line: site }); }
      }
    }
    if (ti >= 0) {
      e.edgeN += cnt;
      if (allocNameMatch(name)) allocHz.push({ SymbolID: caller, Pattern: jtrunc(name, 80), Category: "alloc", N: cnt, FirstLine: line });
      continue;
    }
    const msid = e.macroSID.get(name);
    if (msid !== undefined) { add(mac, caller, cnt); add(macroUses, msid, cnt); e.macroN += cnt; continue; }
    if (cands) { addUnres(caller, name, cnt, line); continue; }
    if (isExternal(name)) { add(ext, caller, cnt); e.externN += cnt; continue; }
    if (e.declared.has(name)) { add(ext, caller, cnt); e.externN += cnt; e.declN += cnt; continue; }
    addUnres(caller, name, cnt, line);
  }
  for (const h of allocHz) g.hazards.push(h);
  for (const ed of edges.values()) g.edges.push(ed);
  for (const u of unres.values()) g.unres.push(u);
  for (const [sid, n] of ext) g.symbols[sid - 1].NExternalCalls += n;
  for (const [sid, n] of mac) g.symbols[sid - 1].NMacroCalls += n;
  for (const m of g.macros) { const n = macroUses.get(m.SymbolID); if (n !== undefined) m.NUses = n; }
  const total = e.edgeN + e.macroN + e.externN + e.unresN;
  const pct = total > 0 ? Math.trunc((100 * (total - e.unresN)) / total) : 0;
  g.meta.set("calls_resolved", `${e.edgeN} in-tree (${e.declPickN} callers matched to one of several definitions by a shared prototype) / ` +
    `${e.macroN} via macro / ${e.externN} external (${e.declN} of them declared-here-defined-elsewhere) / ${e.unresN} unresolved ` +
    `(${e.ambigN} names defined more than once with external linkage) -- ${pct}% of ${Math.max(total, 1)} call sites accounted for`);
}

export function collapseHazards(g) {
  const idx = new Map(), out = [];
  for (const h of g.hazards) {
    const k = h.SymbolID + "\0" + h.Pattern;
    const j = idx.get(k);
    if (j !== undefined) { out[j].N += h.N; continue; }
    idx.set(k, out.length);
    out.push(h);
  }
  g.hazards = out;
}

const CAT_COL = { memory: "NMemory", alloc: "NAlloc", io: "NIO", stdio: "NStdio", exec: "NExec", libm: "NLibm", integer: "NInteger",
  concurrency: "NConcurrency", reentrancy: "NReentrancy" };
const I32 = (x) => x | 0;

export function materialize(g) {
  const n = g.symbols.length + 1;
  const byCat = new Map();
  for (const h of g.hazards) {
    let m = byCat.get(h.SymbolID);
    if (!m) byCat.set(h.SymbolID, (m = {}));
    m[h.Category] = (m[h.Category] || 0) + h.N;
  }
  const fanIn = new Int32Array(n), fanOut = new Int32Array(n), nsites = new Int32Array(n), selfCall = new Uint8Array(n);
  for (const ed of g.edges) {
    if (ed.IsSelf === 0) { fanOut[ed.CallerID]++; fanIn[ed.CalleeID]++; } else selfCall[ed.CallerID] = 1;
  }
  for (const c of g.callsites) nsites[c.CalleeID]++;
  const unresN = new Int32Array(n);
  for (const u of g.unres) unresN[u.CallerID] += u.N;
  const freeN = new Int32Array(n), allocN = new Int32Array(n);
  for (const h of g.hazards) {
    if (h.Category !== "alloc") continue;
    if (h.Pattern === "free" || H.asciiLower(h.Pattern).endsWith("free")) freeN[h.SymbolID] += h.N;
    else allocN[h.SymbolID] += h.N;
  }
  for (const s of g.symbols) {
    s.FanOut = fanOut[s.ID]; s.FanIn = fanIn[s.ID]; s.NCallsites = nsites[s.ID];
    if (selfCall[s.ID]) s.IsRecursive = 1;
    s.NUnresolvedCalls = unresN[s.ID];
    const m = byCat.get(s.ID);
    if (m) for (const c in CAT_COL) s[CAT_COL[c]] = m[c] || 0;
    s.NFree = freeN[s.ID];
    s.NAlloc = allocN[s.ID];
  }
  for (const s of g.symbols) {
    let r = s.Cyclomatic * 2 + s.Cognitive + s.MaxNesting * 5 + s.NMemory * 10 + s.NIO * 8 + s.NExec * 15 + s.NInteger + s.NAlloc * 2 + s.NConcurrency * 3;
    if (s.IsRecursive === 1) r += 25;
    if (s.NAlloc > 0 && s.NFree === 0) r += 10;
    s.RiskScore = I32(r);
  }
  const fagg = new Map();
  for (const s of g.symbols) {
    let a = fagg.get(s.FileID);
    if (!a) fagg.set(s.FileID, (a = { n: 0, fn: 0, ty: 0, cyclo: 0, maxc: 0, risk: 0 }));
    a.n++;
    switch (s.Kind) {
      case "function": case "method": case "constructor": case "closure": a.fn++; break;
      case "class": case "struct": case "interface": case "trait": case "enum": case "union": case "record": case "protocol": case "type": case "impl": a.ty++; break;
    }
    a.cyclo += s.Cyclomatic;
    if (s.Cyclomatic > a.maxc) a.maxc = s.Cyclomatic;
    a.risk += s.RiskScore;
  }
  const impN = new Map();
  for (const im of g.imports) impN.set(im.FileID, (impN.get(im.FileID) || 0) + 1);
  for (const f of g.files) {
    const a = fagg.get(f.id);
    if (a) { f.nSymbols = a.n; f.nFunctions = a.fn; f.nTypes = a.ty; f.totalCyclo = I32(a.cyclo); f.maxCyclo = a.maxc; f.totalRisk = I32(a.risk); }
    f.nImports = impN.get(f.id) || 0;
  }
  const magg = new Map();
  const ma = (id) => { let m = magg.get(id); if (!m) magg.set(id, (m = { n: 0, p: 0, files: 0, sloc: 0 })); return m; };
  for (const s of g.symbols) { if (s.ModuleID === 0) continue; const m = ma(s.ModuleID); m.n++; m.p += s.IsPublic; }
  for (const f of g.files) { if (f.moduleID === 0) continue; const m = ma(f.moduleID); m.files++; m.sloc += f.sloc; }
  const mo = new Map(), mi = new Map();
  const put = (mp, a, b) => { let s = mp.get(a); if (!s) mp.set(a, (s = new Set())); s.add(b); };
  for (const ed of g.edges) {
    if (ed.CallerID < 1 || ed.CalleeID < 1) continue;
    const a = g.symbols[ed.CallerID - 1], b = g.symbols[ed.CalleeID - 1];
    if (a.ModuleID === 0 || b.ModuleID === 0 || a.ModuleID === b.ModuleID) continue;
    put(mo, a.ModuleID, b.ModuleID);
    put(mi, b.ModuleID, a.ModuleID);
  }
  for (const m of g.modules) {
    const a = magg.get(m.id);
    if (a) { m.nSymbols = a.n; m.nPublic = a.p; m.nFiles = a.files; m.sloc = a.sloc; }
    m.fanOut = mo.get(m.id)?.size ?? 0;
    m.fanIn = mi.get(m.id)?.size ?? 0;
    m.instability = m.fanIn + m.fanOut !== 0 ? m.fanOut / (m.fanIn + m.fanOut) : 0;
  }
}

export function postBuild(g) {
  const fnNames = new Set();
  for (const s of g.symbols) if (s.Kind === "function") fnNames.add(s.Name);
  let pruned = 0;
  const out = [];
  for (const a of g.addrs) {
    if (!a.HasSym && !fnNames.has(a.Name)) { pruned++; continue; }
    out.push(a);
  }
  g.addrs = out;
  g.meta.set("addr_taken_pruned", pruned + " non-function file-scope references dropped");
  transitiveFan(g);
  includeCycles(g);
}

// Tarjan SCC over the call graph, then ancestor/descendant sets per
// component as bitsets in reverse topological order.
export function transitiveFan(g) {
  const n = g.symbols.length + 1;
  const ea = [], eb = [];
  const has = new Uint8Array(n);
  for (const ed of g.edges) {
    if (ed.CallerID === ed.CalleeID) continue;
    ea.push(ed.CallerID); eb.push(ed.CalleeID);
    has[ed.CallerID] = 1; has[ed.CalleeID] = 1;
  }
  if (ea.length === 0) return;
  const nodes = [];
  for (let v = 1; v < n; v++) if (has[v]) nodes.push(v);
  const start = new Int32Array(n + 1);
  for (const a of ea) start[a + 1]++;
  for (let v = 1; v < n; v++) start[v + 1] += start[v];
  const fill = start.slice(0, n), adj = new Int32Array(ea.length);
  for (let i = 0; i < ea.length; i++) adj[fill[ea[i]]++] = eb[i];
  const index = new Int32Array(n).fill(-1), low = new Int32Array(n), onstk = new Uint8Array(n), compOf = new Int32Array(n).fill(-1);
  const stk = [], comps = [];
  let counter = 0;
  for (const root of nodes) {
    if (index[root] >= 0) continue;
    index[root] = low[root] = counter++;
    stk.push(root); onstk[root] = 1;
    const wv = [root], ww = [0];
    while (wv.length) {
      const t = wv.length - 1, v = wv[t];
      let descended = false;
      while (ww[t] < start[v + 1] - start[v]) {
        const w = adj[start[v] + ww[t]];
        ww[t]++;
        if (index[w] < 0) {
          index[w] = low[w] = counter++;
          stk.push(w); onstk[w] = 1;
          wv.push(w); ww.push(0);
          descended = true;
          break;
        }
        if (onstk[w] && index[w] < low[v]) low[v] = index[w];
      }
      if (descended) continue;
      wv.pop(); ww.pop();
      if (wv.length) { const u = wv[wv.length - 1]; if (low[v] < low[u]) low[u] = low[v]; }
      if (low[v] === index[v]) {
        const comp = [];
        for (;;) { const w = stk.pop(); onstk[w] = 0; comp.push(w); if (w === v) break; }
        comps.push(comp);
      }
    }
  }
  comps.forEach((c, ci) => { for (const v of c) compOf[v] = ci; });
  const slot = new Int32Array(n);
  nodes.forEach((v, i) => { slot[v] = i; });
  const nComp = comps.length, words = (nodes.length + 31) >> 5;
  const predE = Array.from({ length: nComp }, () => []), succE = Array.from({ length: nComp }, () => []);
  for (let i = 0; i < ea.length; i++) {
    const pc = compOf[ea[i]], cc = compOf[eb[i]];
    if (pc === cc) continue;
    predE[cc].push(pc); succE[pc].push(cc);
  }
  const nIn = new Int32Array(nComp), nOut = new Int32Array(nComp);
  const grow = (order, edgesOf, outN) => {
    const sets = new Array(nComp);
    for (const ci of order) {
      const acc = new BitSet(nodes.length);
      for (const p of edgesOf[ci]) {
        acc.or(sets[p]);
        for (const v of comps[p]) acc.set(slot[v]);
      }
      sets[ci] = acc;
      outN[ci] = acc.count;
    }
  };
  const down = [], up = [];
  for (let ci = nComp - 1; ci >= 0; ci--) down.push(ci);
  for (let ci = 0; ci < nComp; ci++) up.push(ci);
  grow(down, predE, nIn);
  grow(up, succE, nOut);
  comps.forEach((comp, ci) => {
    const extra = comp.length - 1;
    if (nIn[ci] + nOut[ci] + extra === 0) return;
    for (const v of comp) g.reach.push({ SymbolID: v, NTransitive: nIn[ci] + extra, NTransitiveOut: nOut[ci] + extra });
  });
}

class PySet {
  constructor() { this.table = new Int32Array(8); this.used = 0; }
  static cap(n) { let t = 8; while (n > t / 2) t *= 4; return t; }
  add(v) {
    const want = PySet.cap(this.used + 1);
    if (want > this.table.length) {
      const old = this.table;
      this.table = new Int32Array(want);
      this.place(v);
      for (const x of old) if (x !== 0) this.place(x);
      this.used++;
      return;
    }
    this.place(v);
    this.used++;
  }
  place(v) {
    const mask = this.table.length - 1;
    let i = v & mask;
    while (this.table[i] !== 0) i = (i + 1) & mask;
    this.table[i] = v;
  }
  order() { const out = []; for (const v of this.table) if (v !== 0) out.push(v); return out; }
}

export function includeCycles(g) {
  const adj = new Map(), roots = [];
  for (const im of g.imports) {
    if (!im.HasTargetID || im.IsRelative !== 1 || im.Kind !== "include") continue;
    if (g.files[im.FileID - 1].isTest === 1) continue;
    let s = adj.get(im.FileID);
    if (!s) { adj.set(im.FileID, (s = new PySet())); roots.push(im.FileID); }
    s.add(im.TargetID);
  }
  roots.sort((a, b) => a - b);
  const paths = new Map(g.files.map((f) => [f.id, f.path]));
  const seen = new Set(), rows = [];
  for (const root of roots) {
    const stack = [[root, [root]]];
    while (stack.length) {
      const [node, path] = stack.pop();
      const s = adj.get(node);
      if (s) {
        for (const nxt of s.order()) {
          if (nxt === root && path.length >= 2) {
            const key = [...path].sort((a, b) => a - b).join(",");
            if (seen.has(key)) continue;
            seen.add(key);
            const members = path.map((f) => paths.get(f) ?? "?");
            rows.push({ APath: members[0], BPath: members[members.length - 1], Length: path.length, Members: members.join(" -> ") });
          } else if (!path.includes(nxt) && path.length < 8) stack.push([nxt, [...path, nxt]]);
        }
      }
      if (rows.length > 5000) break;
    }
  }
  g.cycles = rows;
}

// Makefile rules at the root and its first 64 directories.
export function parseManifests(root, g) {
  const bases = [root];
  let names = [];
  try { names = fs.readDir(new fs.Path(root)).map((x) => (typeof x === "string" ? x : x.name)).sort(); } catch (err) {}
  if (names.length > 64) names = names.slice(0, 64);
  for (const d of names) {
    const p = root + "/" + d;
    try { if (fs.stat(new fs.Path(p)).isDir) bases.push(p); } catch (err) {}
  }
  let seen = 0;
  for (const base of bases) {
    for (const nm of ["Makefile", "makefile", "GNUmakefile"]) {
      const p = base + "/" + nm;
      let st;
      try { st = fs.stat(new fs.Path(p)); } catch (err) { continue; }
      if (!st.isFile) continue;
      seen++;
      const rel = p.slice(root.length + 1);
      let text;
      try { text = readBytesString(p); } catch (err) { break; }
      makeRules(g, rel, text);
      break;
    }
  }
  g.meta.set("makefiles_read", String(seen));
}

const isMakeName = (c) => /[a-zA-Z0-9_./$()%-]/.test(c);
const isWordB = (c) => c !== undefined && /[A-Za-z0-9_]/.test(c);
function matchMakeRule(ln) {
  let k = 0;
  while (k < ln.length && isMakeName(ln[k])) k++;
  if (k === 0) return "";
  let m = k;
  while (m < ln.length && (ln[m] === " " || ln[m] === "\t")) m++;
  if (m >= ln.length || ln[m] !== ":") return "";
  if (m + 1 >= ln.length || ln[m + 1] === "=") return "";
  return ln.slice(0, k);
}
function matchMakeVar(ln) {
  if (ln === "" || !H.isIdentStart(ln.charCodeAt(0))) return ["", ""];
  const k = H.identEnd(ln, 0);
  let m = k;
  while (m < ln.length && (ln[m] === " " || ln[m] === "\t")) m++;
  if (m < ln.length && (ln[m] === ":" || ln[m] === "+" || ln[m] === "?")) m++;
  if (m >= ln.length || ln[m] !== "=") return ["", ""];
  m++;
  while (m < ln.length && (ln[m] === " " || ln[m] === "\t")) m++;
  return [ln.slice(0, k), ln.slice(m)];
}
function countMakeExt(s, ext) {
  const inClass = ext === "c" ? (c) => isWordB(c) || "/.-".includes(c) : (c) => isWordB(c) || "/$(){}.-".includes(c);
  let n = 0, i = 0;
  while (i < s.length) {
    if (!inClass(s[i])) { i++; continue; }
    let greedy = i;
    while (greedy < s.length && inClass(s[greedy])) greedy++;
    let found = -1;
    for (let j = greedy - 1; j > i; j--) {
      if (s[j] !== "." || j + 1 >= s.length || s[j + 1] !== ext) continue;
      if (j + 2 < s.length && isWordB(s[j + 2])) continue;
      found = j;
      break;
    }
    if (found < 0) { i = greedy; continue; }
    n++;
    i = found + 2;
  }
  return n;
}
function makeRules(g, rel, text) {
  let cont = "", start = 0;
  const lines = text.split("\n");
  if (lines.length && lines[lines.length - 1] === "") lines.pop();
  lines.forEach((ln0, i) => {
    const lnNo = i + 1;
    let ln = ln0.replace(/\r+$/, "");
    if (cont !== "") { ln = cont + " " + ln.replace(/^[ \t]+/, ""); cont = ""; }
    if (ln.endsWith("\\")) { cont = ln.slice(0, -1); if (start === 0) start = lnNo; return; }
    const r = matchMakeRule(ln);
    if (r !== "") {
      const pre = ln.slice(ln.indexOf(":") + 1);
      g.mkRules.push({ Path: rel, Rule: r, Line: start || lnNo, NObjs: countMakeExt(pre, "o"), NSrcs: countMakeExt(pre, "c"), UsesAr: b2i(pre.includes(".a")) });
      start = 0;
      return;
    }
    const [v, val] = matchMakeVar(ln);
    if (v !== "") {
      const objs = countMakeExt(val, "o"), srcs = countMakeExt(val, "c");
      if (objs > 0 || srcs > 0) g.mkRules.push({ Path: rel, Rule: v + " =", Line: start || lnNo, NObjs: objs, NSrcs: srcs, UsesAr: b2i(val.includes(".a")) });
    }
    start = 0;
  });
}

const GRAMMAR_NOTE = "no compiler frontend and no third-party package: C23 (ISO/IEC 9899:2024) is analysed by a real preprocessor (hide-set macro " +
  "expansion, conditionals, include graph) and a recursive-descent parser producing a pointer-free flat AST. Race-surface and guard counters keep " +
  "their text heuristics, fed exact AST body spans.";
export const TARGET = "C23 (ISO/IEC 9899:2024) + GNU extensions tolerated (real preprocessor)";

export function finishMeta(g, abs, nParsed, nFailed, stats) {
  const m = g.meta;
  m.set("schema_version", "2"); m.set("lang", "c"); m.set("target", TARGET); m.set("root", abs); m.set("parse_mode", "c23-ast");
  m.set("parser", "C23 lexer + hide-set preprocessor + recursive-descent parser (stdlib only)");
  m.set("files_parsed", String(nParsed)); m.set("files_failed", String(nFailed)); m.set("grammar_note", GRAMMAR_NOTE);
  m.set("layout_model", "LP64: pointer 8/8, long 8, int 4; sizes reported only where exact=1");
  const st = stats || {};
  m.set("files_skipped", `big=${st.skippedBig || 0} special=${st.skippedSpecial || 0} escaping_symlink=${st.skippedEscape || 0} denied=${st.skippedDenied || 0} walk_errors=${st.walkErrors || 0}`);
}
