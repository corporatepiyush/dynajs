// Query layer core: result values, sort keys, and the graph as the Go code names it.
import { likeMatch } from "./util.js";
import { trimSpace } from "./text.js";

export const I = (n) => ({ i: n });
export const S = (s) => ({ s });
export const F = (f) => ({ f });
export const NUL = null;
export const optS = (v, ok) => (ok ? { s: v } : null);
export const optI = (v, ok) => (ok ? { i: v } : null);
export const isStr = (v) => v !== null && v !== undefined && "s" in v;
export { likeMatch };

export const limit = (rows, p) => (p.lim >= 0 && rows.length > p.lim ? rows.slice(0, p.lim) : rows);
const c3 = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
export const cmpI = (c) => (a, b) => c3(a[c].i, b[c].i);
export const dI = (c) => (a, b) => c3(b[c].i, a[c].i);
export const cmpS = (c) => (a, b) => c3(a[c].s, b[c].s);
export const dS = (c) => (a, b) => c3(b[c].s, a[c].s);
export const dSum = (...cs) => { const f = (r) => { let t = 0; for (const c of cs) t += r[c].i; return t; }; return (a, b) => c3(f(b), f(a)); };
export const dWeighted = (w) => { const f = (r) => { let t = 0; for (const [k, c] of w) t += k * r[c].i; return t; }; return (a, b) => c3(f(b), f(a)); };
export const dLenStr = (c) => (a, b) => c3(b[c].s.length, a[c].s.length);
const lineOf = (at) => { const n = parseInt(at.slice(at.lastIndexOf(":") + 1), 10); return Number.isNaN(n) ? 0 : n; };
export const cmpLine = (c) => (a, b) => c3(lineOf(a[c].s), lineOf(b[c].s));
export const dF = (c) => (a, b) => c3(b[c].f, a[c].f);
export const cmpF = (c) => (a, b) => c3(a[c].f, b[c].f);
export const then = (...ks) => (a, b) => { for (const k of ks) { const v = k(a, b); if (v) return v; } return 0; };
export function orderBy(rows, ...keys) {
  if (rows.length < 2) return;
  const k = keys.length === 1 ? keys[0] : then(...keys);
  rows.sort(k);
}
export const byteCmp = c3;
export const pct = (a, b) => (b === 0 ? 0 : Math.trunc((a * 100) / b));
export const groupConcat = (v) => v.join(",");
export const distinct = (v) => [...new Set(v)];
export const I32 = (x) => x | 0;
export const asciiLower = (s) => s.replace(/[A-Z]+/g, (m) => m.toLowerCase());
export const asciiUpper = (s) => s.replace(/[a-z]+/g, (m) => m.toUpperCase());
export { trimSpace };
export function pathBase(p) {
  if (p === "") return ".";
  while (p.length > 1 && p.endsWith("/")) p = p.slice(0, -1);
  const i = p.lastIndexOf("/");
  return i >= 0 ? p.slice(i + 1) : p;
}
export const inc = (m, k, n = 1) => m.set(k, (m.get(k) || 0) + n);
export const pushTo = (m, k, v) => { let l = m.get(k); if (!l) m.set(k, (l = [])); l.push(v); return l; };
export const addUniq = (m, k, v) => { let l = m.get(k); if (!l) m.set(k, (l = [])); if (!l.includes(v)) l.push(v); };

const GO_FILE = (f) => ({ ID: f.id, Path: f.path, Dir: f.dir, Basename: f.basename, Ext: f.ext, Lang: f.lang, ModuleID: f.moduleID,
  Bytes: f.bytes, Lines: f.lines, Sloc: f.sloc, BlankLines: f.blankLines, CommentLines: f.commentLines, DocLines: f.docLines,
  MaxLineLen: f.maxLineLen, Sha1: f.sha1, Parsed: f.parsed, IsTest: f.isTest, IsGenerated: f.isGenerated, IsVendored: f.isVendored,
  NParseErrors: f.nParseErrors, NMissingNodes: f.nMissingNodes, NSymbols: f.nSymbols, NFunctions: f.nFunctions, NTypes: f.nTypes,
  NImports: f.nImports, TotalCyclo: f.totalCyclo, MaxCyclo: f.maxCyclo, TotalRisk: f.totalRisk });
const GO_MOD = (m) => ({ ID: m.id, Name: m.name, Kind: m.kind, NFiles: m.nFiles || 0, NSymbols: m.nSymbols || 0, NPublic: m.nPublic || 0,
  Sloc: m.sloc || 0, FanIn: m.fanIn || 0, FanOut: m.fanOut || 0, Instability: m.instability || 0 });
const ZERO_SYM = ["IsOverride", "IsAbstract", "IsAsync", "IsGenerator", "IsExported", "IsConst", "IsMutable", "IsDeprecated"];

export class QG {
  constructor(g, raw) {
    this.raw = raw;
    this.scanOrder = "file";
    this.Files = g.files.map(GO_FILE);
    this.Modules = g.modules.map(GO_MOD);
    this.Symbols = g.symbols;
    for (const s of g.symbols) for (const k of ZERO_SYM) if (s[k] === undefined) s[k] = 0;
    this.Params = g.params; this.Fields = g.fields; this.Locals = g.locals; this.Literals = g.literals; this.Markers = g.markers;
    this.Attrs = g.attrs; this.Imports = g.imports; this.Hazards = g.hazards; this.EnumMem = g.enums; this.Layout = g.layout;
    this.SSize = g.ssize; this.Decls = g.decls; this.Addr = g.addrs; this.Secrets = g.secrets; this.Allocs = g.allocs;
    this.Memops = g.memops; this.Macros = g.macros; this.Globals = g.globals; this.Cfgs = g.cfgs; this.Reach = g.reach;
    this.MkRules = g.mkRules; this.Locks = g.locks; this.EvOps = g.evops; this.APIUses = g.apiuses; this.Cycles = g.cycles;
    this.Edges = g.edges; this.Callsites = g.callsites; this.Unres = g.unres; this.Meta = g.meta; this.ParseMsgs = g.parseMsgs;
    this._c = new Map();
  }
  memo(k, f) { let v = this._c.get(k); if (v === undefined) { v = f(); this._c.set(k, v); } return v; }
  modName(id) { return id <= 0 || id > this.Modules.length ? "" : this.Modules[id - 1].Name; }
  modOK(id, p) { return likeMatch(this.modName(id), p.mod); }
  sym(id) { return id < 1 || id > this.Symbols.length ? null : this.Symbols[id - 1]; }
  fileOf2(id) { return id < 1 || id > this.Files.length ? null : this.Files[id - 1]; }
  fileOf(symID) { const s = this.sym(symID); return s ? this.fileOf2(s.FileID) : null; }
  at(s) { const f = this.fileOf(s.ID); return f ? f.Path + ":" + s.LineStart : ""; }
  notTest(s) { const f = this.fileOf(s.ID); return f !== null && f.IsTest === 0; }
  get byName() {
    return this.memo("byName", () => {
      const m = new Map();
      for (const s of this.Symbols) pushTo(m, s.Name, s.ID);
      return m;
    });
  }
  get byFile() {
    return this.memo("byFile", () => {
      const m = new Map();
      for (const s of this.Symbols) pushTo(m, s.FileID, s.ID);
      return m;
    });
  }
  get fileByBase() {
    return this.memo("fileByBase", () => {
      const m = new Map();
      for (const f of this.Files) { const b = pathBase(f.Path); if (!m.has(b)) m.set(b, f.ID); }
      return m;
    });
  }
  ids(name) { return this.byName.get(name) || []; }
  syms() {
    return this.memo("syms:" + this.scanOrder, () => {
      if (this.scanOrder === "name") return this.symsByName();
      if (this.scanOrder !== "file") return this.Symbols.slice();
      const out = this.Symbols.slice();
      out.sort((a, b) => {
        const fa = this.fileOf2(a.FileID), fb = this.fileOf2(b.FileID);
        if (!fa || !fb) return a.ID - b.ID;
        if (fa.Path !== fb.Path) return fa.Path < fb.Path ? -1 : 1;
        if (a.LineStart !== b.LineStart) return a.LineStart - b.LineStart;
        return a.ID - b.ID;
      });
      return out;
    });
  }
  symsByName() {
    return this.memo("symsByName", () => this.Symbols.slice().sort((a, b) => (a.Name !== b.Name ? (a.Name < b.Name ? -1 : 1) : a.ID - b.ID)));
  }
  symsName() { return this.symsByName(); }
  symsByID() { return this.Symbols.slice(); }
  perSym() { return new Int32Array(this.Symbols.length + 1); }
  perFile() { return new Int32Array(this.Files.length + 1); }
  perMod() { return new Int32Array(this.Modules.length + 1); }
  hz() {
    return this.memo("hz", () => {
      const ix = { byPat: new Map(), byCat: new Map(), shape: new Map(), dense: new Map(), ign: null, who: null };
      this.Hazards.forEach((h, i) => { pushTo(ix.byPat, h.Pattern, i); pushTo(ix.byCat, h.Category, i); });
      return ix;
    });
  }
  shapeCounts(pat) {
    const ix = this.hz();
    let m = ix.shape.get(pat);
    if (m) return m;
    m = new Map();
    for (const i of ix.byPat.get(pat) || []) { const h = this.Hazards[i]; if (h.Category === "shape") inc(m, h.SymbolID, h.N); }
    ix.shape.set(pat, m);
    return m;
  }
  shapeDense(pat) {
    const ix = this.hz();
    let d = ix.dense.get(pat);
    if (d) return d;
    d = new Int32Array(this.Symbols.length + 1);
    for (const [id, n] of this.shapeCounts(pat)) if (id >= 0 && id < d.length) d[id] = n;
    ix.dense.set(pat, d);
    return d;
  }
  discards() {
    const ix = this.hz();
    if (!ix.ign) {
      ix.ign = new Map(); ix.who = new Map();
      for (const i of ix.byCat.get("discard") || []) { const h = this.Hazards[i]; inc(ix.ign, h.Pattern, h.N); inc(ix.who, h.Pattern); }
    }
    return [ix.ign, ix.who];
  }
  hazRows(pats) {
    const ix = this.hz(), out = [];
    pats.forEach((pat, k) => { if (!pats.slice(0, k).includes(pat)) out.push(...(ix.byPat.get(pat) || [])); });
    return out.sort((a, b) => a - b);
  }
  hazCats(...cats) {
    const ix = this.hz();
    if (cats.length === 1) return ix.byCat.get(cats[0]) || [];
    const out = [];
    for (const c of cats) out.push(...(ix.byCat.get(c) || []));
    return out.sort((a, b) => a - b);
  }
  hazardsByPattern(pats) {
    const ix = this.hz(), out = [];
    for (const pat of [...pats].sort(byteCmp)) for (const i of ix.byPat.get(pat) || []) out.push(this.Hazards[i]);
    return out;
  }
  hazardsByCatN(cat) {
    const out = (this.hz().byCat.get(cat) || []).slice();
    return out.sort((a, b) => this.Hazards[b].N - this.Hazards[a].N);
  }
  hazardsBy(cat, pats) {
    const list = new Map(), total = new Map();
    for (const i of this.hazRows(pats)) {
      const h = this.Hazards[i];
      if (cat !== "" && h.Category !== cat) continue;
      addUniq(list, h.SymbolID, h.Pattern);
      inc(total, h.SymbolID, h.N);
    }
    return [list, total];
  }
  edgeCSR(byCallee) {
    return this.memo(byCallee ? "edgeIn" : "edgeOut", () => {
      const n = this.Symbols.length + 1;
      const end = (e) => { const id = byCallee ? e.CalleeID : e.CallerID; return id < 0 || id >= n ? -1 : id; };
      const start = new Int32Array(n + 1);
      for (const e of this.Edges) { const id = end(e); if (id >= 0) start[id + 1]++; }
      for (let i = 1; i <= n; i++) start[i] += start[i - 1];
      const idx = new Int32Array(start[n]), fill = start.slice(0, n);
      this.Edges.forEach((e, i) => { const id = end(e); if (id >= 0) idx[fill[id]++] = i; });
      return { start, idx, of(id) { return id < 0 || id + 1 >= start.length ? [] : idx.subarray(start[id], start[id + 1]); } };
    });
  }
  six() {
    return this.memo("six", () => {
      const n = this.Symbols.length + 1;
      const x = { nameN: new Int32Array(n), fnDefs: new Int32Array(n), rank: new Int32Array(n), declN: new Int32Array(n),
        declHdr: new Int32Array(n), addrAny: new Uint8Array(n), addrInFile: new Uint8Array(n), bodyN: new Int32Array(n),
        bodyInFile: new Int32Array(n), refs: new Map(), macro: new Array(n).fill(null) };
      for (const ids of this.byName.values()) {
        let fns = 0;
        for (const id of ids) if (this.Symbols[id - 1].Kind === "function") fns++;
        for (const id of ids) { x.nameN[id] = ids.length; x.fnDefs[id] = fns; }
      }
      this.syms().forEach((s, i) => { x.rank[s.ID] = i; });
      for (const d of this.Decls) {
        const f = this.fileOf2(d.FileID);
        const hdr = f && f.Ext === ".h" ? 1 : 0;
        for (const id of this.ids(d.Name)) { x.declN[id]++; x.declHdr[id] += hdr; }
      }
      for (const a of this.Addr) {
        const nm = a.Name;
        for (const id of this.ids(nm)) {
          const own = this.Symbols[id - 1].FileID === a.FileID;
          x.addrAny[id] = 1;
          if (own) x.addrInFile[id] = 1;
          if (a.HasSym) { x.bodyN[id]++; if (own) x.bodyInFile[id]++; }
        }
        if (!a.HasSym) pushTo(x.refs, nm, { file: a.FileID, kind: a.Kind });
      }
      for (const m of this.Macros) if (m.SymbolID >= 0 && m.SymbolID < n) x.macro[m.SymbolID] = m;
      return x;
    });
  }
  namedAsValue() {
    const x = this.six();
    return (s) => {
      if (!x.addrAny[s.ID]) return false;
      if (s.IsStatic === 0 || x.addrInFile[s.ID]) return true;
      const f = this.fileOf(s.ID);
      return f !== null && f.Ext.startsWith(".h");
    };
  }
  simpleQ(cols, keep, p, fill, ...keys) {
    const rows = [];
    for (const s of this.syms()) {
      if (!keep(s) || !this.modOK(s.ModuleID, p)) continue;
      rows.push(fill(s));
    }
    orderBy(rows, ...keys, cmpS(0));
    return [cols, limit(rows, p)];
  }
}

export function groupRows(n, nk, key) {
  const start = new Int32Array(nk + 1);
  for (let i = 0; i < n; i++) { const k = key(i); if (k >= 0 && k < nk) start[k + 1]++; }
  for (let k = 1; k <= nk; k++) start[k] += start[k - 1];
  const idx = new Int32Array(start[nk]), fill = start.slice(0, nk);
  for (let i = 0; i < n; i++) { const k = key(i); if (k >= 0 && k < nk) idx[fill[k]++] = i; }
  return [start, idx];
}
