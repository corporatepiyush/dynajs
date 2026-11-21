// Structural metrics M1-M50.
import { I, S, F, NUL, optS, limit, orderBy, cmpI, dI, cmpS, dSum, dWeighted, pct, groupConcat, distinct, inc, pushTo, addUniq,
  likeMatch, groupRows, byteCmp } from "./qcore.js";
import { goRound } from "./qdyn.js";

const isFn = (s) => s.Kind === "function";
const b2i = (b) => (b ? 1 : 0);
const ret = (cols, rows, p, ...keys) => { orderBy(rows, ...keys); return [cols, limit(rows, p)]; };
function scan(g, p, pred, row, notTest = false, list = g.syms()) {
  const rows = [];
  for (const s of list) {
    if (!pred(s)) continue;
    if (notTest && !g.notTest(s)) continue;
    if (!g.modOK(s.ModuleID, p)) continue;
    rows.push(row(s));
  }
  return rows;
}
const modOpt = (g, s) => optS(g.modName(s.ModuleID), s.ModuleID !== 0);
const sortedIDs = (m) => [...m.keys()].sort((a, b) => a - b);

export const M = {};
M.mM1 = (g, p) => {
  const m = Array.from({ length: g.Modules.length + 1 }, () => ({ fns: 0, fp: 0, mac: 0, ext: 0, unr: 0, calls: 0 }));
  for (const s of g.Symbols) {
    if (!isFn(s) || s.ModuleID < 0 || s.ModuleID >= m.length) continue;
    const a = m[s.ModuleID];
    a.fns++; a.fp += s.NFnptrCalls; a.mac += s.NMacroCalls; a.ext += s.NExternalCalls; a.unr += s.NUnresolvedCalls; a.calls += s.NCalls;
  }
  const rows = [];
  for (const mod of g.Modules) {
    if (!likeMatch(mod.Name, p.mod) || mod.ID < 0 || mod.ID >= m.length || m[mod.ID].fns === 0) continue;
    const a = m[mod.ID];
    rows.push([S(mod.Name), I(a.fns), I(a.fp), I(a.mac), I(a.ext), I(a.unr), a.calls !== 0 ? I(pct(a.fp + a.unr, a.calls)) : NUL]);
  }
  return ret(["module", "fns", "fnptr", "via_macro", "external", "unresolved", "pct_unseen"], rows, p, dI(2), dI(5));
};

M.mM2 = (g, p) => ret(["name", "fan_in", "sites", "cyclo", "sloc", "stat", "inl", "module", "at"],
  scan(g, p, isFn, (s) => [S(s.Name), I(s.FanIn), I(s.NCallsites), I(s.Cyclomatic), I(s.Sloc), I(s.IsStatic), I(s.IsInline), modOpt(g, s),
    S(g.at(s))]), p, dI(1), dI(3), cmpS(0));

M.mM3 = (g, p) => ret(["name", "risk", "cyclo", "cog", "nest", "mem", "io", "int_", "rec", "at"],
  scan(g, p, isFn, (s) => [S(s.Name), I(s.RiskScore), I(s.Cyclomatic), I(s.Cognitive), I(s.MaxNesting), I(s.NMemory), I(s.NIO), I(s.NInteger),
    I(s.IsRecursive), S(g.at(s))]), p, dI(1), cmpS(0));

M.mM4 = (g, p) => {
  const n1 = g.Symbols.length + 1, ok = (id) => id >= 0 && id < n1;
  const mult = [];
  for (const c of g.Callsites) {
    const k = mult.length - 1;
    if (k >= 0 && mult[k].c === c.CallerID && mult[k].d === c.CalleeID) mult[k].n++;
    else mult.push({ c: c.CallerID, d: c.CalleeID, n: 1 });
  }
  const direct = new Float64Array(n1);
  for (const i of g.hazCats("alloc")) {
    const h = g.Hazards[i];
    if (h.Pattern === "free" || h.Pattern.toLowerCase().endsWith("free") || !ok(h.SymbolID)) continue;
    direct[h.SymbolID] = (direct[h.SymbolID] + h.N) | 0;
  }
  const total = direct.slice();
  let cur = direct;
  for (let r = 0; r < 3; r++) {
    const next = new Float64Array(n1);
    for (const k of mult) if (ok(k.c) && ok(k.d) && cur[k.d] !== 0) next[k.c] = (next[k.c] + Math.imul(k.n, cur[k.d])) | 0;
    for (let i = 0; i < n1; i++) total[i] = (total[i] + next[i]) | 0;
    cur = next;
  }
  const rows = scan(g, p, (s) => isFn(s) && total[s.ID] >= 8, (s) => [S(s.Name), I(total[s.ID]), I(direct[s.ID]), I(s.NLoops), modOpt(g, s),
    S(g.at(s))]);
  return ret(["name", "xalloc", "direct", "loops", "module", "at"], rows, p, dI(1));
};

M.mM5 = (g, p) => {
  const m = new Map(), inn = g.edgeCSR(true);
  for (let id = 1; id <= g.Symbols.length; id++) {
    const b = g.sym(id), parts = [];
    for (const j of inn.of(id)) {
      const e = g.Edges[j], a = g.sym(e.CallerID);
      if (e.SameModule !== 0 || !a) continue;
      const pt = parts.find((x) => x.mod === a.ModuleID);
      if (pt) { pt.edges++; pt.calls += e.NCalls; } else parts.push({ mod: a.ModuleID, edges: 1, calls: e.NCalls });
    }
    for (const pt of parts) {
      const k = pt.mod * 4294967296 + b.ModuleID;
      let v = m.get(k);
      if (!v) m.set(k, (v = { a: pt.mod, b: b.ModuleID, edges: 0, calls: 0, tgt: 0 }));
      v.edges += pt.edges; v.calls += pt.calls; v.tgt++;
    }
  }
  const rows = [];
  for (const v of [...m.values()].sort((x, y) => x.a - y.a || x.b - y.b)) {
    if (!likeMatch(g.modName(v.a), p.mod)) continue;
    rows.push([S(g.modName(v.a)), S(g.modName(v.b)), I(v.edges), I(v.calls), I(v.tgt)]);
  }
  return ret(["from_module", "to_module", "edges", "calls", "distinct_targets"], rows, p, dI(2));
};

M.mM6 = (g, p) => {
  const nf = g.Files.length + 1, ok = (id) => id >= 1 && id < nf;
  const incl = Array.from({ length: nf }, () => []);
  for (const im of g.Imports) if (im.HasTargetID && ok(im.FileID) && ok(im.TargetID)) incl[im.FileID].push(im.TargetID);
  const count = new Int32Array(nf), minD = new Int32Array(nf), seen = new Int32Array(nf);
  for (let src = 1; src < nf; src++) {
    if (incl[src].length === 0) continue;
    const reach = (t, d) => { seen[t] = src; count[t]++; if (minD[t] === 0 || d < minD[t]) minD[t] = d; };
    let cur = [];
    for (const t of incl[src]) if (seen[t] !== src) { reach(t, 1); cur.push(t); }
    for (let depth = 2; depth <= 4 && cur.length > 0; depth++) {
      const next = [];
      for (const h of cur) for (const t of incl[h]) if (t !== src && seen[t] !== src) { reach(t, depth); next.push(t); }
      cur = next;
    }
  }
  const rows = [];
  for (let h = 1; h < nf; h++) {
    if (count[h] === 0) continue;
    const f = g.fileOf2(h);
    if (!f || !g.modOK(f.ModuleID, p)) continue;
    rows.push([S(f.Path), I(count[h]), I(f.Sloc), I(f.NSymbols), I(minD[h])]);
  }
  return ret(["header", "rebuilt_files", "sloc", "syms", "min_depth"], rows, p, dI(1), dI(2));
};

M.mM7 = (g, p) => ret(["name", "depth", "loops", "calls", "divs", "libm", "brs", "sloc", "at"],
  scan(g, p, (s) => s.MaxLoopDepth > 1, (s) => [S(s.Name), I(s.MaxLoopDepth), I(s.NLoops), I(s.CallInLoop), I(s.DivInLoop), I(s.LibmInLoop),
    I(s.BranchInLoop), I(s.Sloc), S(g.at(s))]), p, dI(1), dI(3));

M.mM8 = (g, p) => ret(["name", "libm", "libm_in_loop", "loops", "depth", "sloc", "fan_in", "at"],
  scan(g, p, (s) => s.NLibm > 0 && s.NLoops > 0, (s) => [S(s.Name), I(s.NLibm), I(s.LibmInLoop), I(s.NLoops), I(s.MaxLoopDepth), I(s.Sloc),
    I(s.FanIn), S(g.at(s))]), p, dI(2), dI(1));

M.mM9 = (g, p) => ret(["name", "intrin", "hints", "restrict_", "builtins", "depth", "atomics", "at"],
  scan(g, p, (s) => !(s.NIntrinsic <= 0 && s.NLikely <= 0), (s) => [S(s.Name), I(s.NIntrinsic), I(s.NLikely), I(s.NRestrict), I(s.NBuiltin),
    I(s.MaxLoopDepth), I(s.NAtomic), S(g.at(s))]), p, dI(1), dI(2));

M.mM10 = (g, p) => {
  const packed = g.shapeDense("packed_size"), rows = [];
  for (const ss of g.SSize) {
    if (ss.Exact !== 1 || ss.TotalPad <= 0) continue;
    const s = g.sym(ss.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    const v = packed[ss.SymbolID] || 0, best = v > 0 ? v : ss.TotalSize;
    rows.push([S(s.Name), S(s.Kind), I(ss.TotalSize), I(ss.TotalPad), I(ss.TailPad), I(best), I(ss.TotalSize - best), I(ss.NLines64),
      I(Math.trunc((best + 63) / 64)), I(ss.MaxAlign), S(g.at(s))]);
  }
  return ret(["struct_", "kind", "sz", "pad", "tail", "packed_sz", "saves", "lines64", "lines64_packed", "algn", "at"], rows, p, dI(6), dI(3), cmpS(0));
};

M.mM11 = (g, p) => {
  const nFields = g.perSym();
  for (const l of g.Layout) if (l.SymbolID >= 0 && l.SymbolID < nFields.length) nFields[l.SymbolID]++;
  const rows = [];
  for (const ss of g.SSize) {
    if (ss.Exact !== 1 || ss.TotalSize < 65 || ss.TotalSize > 128) continue;
    const s = g.sym(ss.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(ss.TotalSize), I(ss.TotalPad), I(ss.NLines64), I(ss.MaxAlign), I(nFields[ss.SymbolID] || 0), S(g.at(s))]);
  }
  return ret(["struct_", "sz", "pad", "lines64", "algn", "fields", "at"], rows, p, cmpI(1));
};

M.mM12 = (g, p) => {
  const m = Array.from({ length: g.Symbols.length + 1 }, () => ({ n: 0, ptr: 0, fnp: 0, arr: 0 }));
  for (const l of g.Layout) {
    if (l.InUnion !== 0 || l.Depth !== 0) continue;
    const s = g.sym(l.SymbolID);
    if (!s || (s.Kind !== "struct" && s.Kind !== "union")) continue;
    const e = m[l.SymbolID];
    e.n++;
    if (l.PtrDepth > 0) e.ptr++;
    if (l.IsFnptr === 1) e.fnp++;
    if (l.ArrayLen > 0) e.arr++;
  }
  const rows = scan(g, p, (s) => { const e = m[s.ID]; return e.n !== 0 && !(e.n < 6 || (e.ptr * 100) / e.n < 50); }, (s) => {
    const e = m[s.ID];
    return [S(s.Name), I(e.n), I(e.ptr), I(e.fnp), I(e.arr), I(pct(e.ptr, e.n)), S(g.at(s))];
  });
  return ret(["struct_", "fields", "ptrs", "fnptr", "arrays", "pct_ptr", "at"], rows, p, dI(2));
};

function chainBytes(g, frame) {
  const n = g.Symbols.length + 1, out = g.edgeCSR(false);
  const total = new Float64Array(n), depth = new Int32Array(n), cyc = new Uint8Array(n), state = new Uint8Array(n);
  for (const s of g.syms()) {
    if (!isFn(s) || state[s.ID] === 2) continue;
    const stack = [{ id: s.ID, next: 0, best: 0, dep: 0 }];
    state[s.ID] = 1;
    while (stack.length) {
      const f = stack[stack.length - 1];
      const kids = out.of(f.id);
      if (f.next < kids.length) {
        const ed = g.Edges[kids[f.next]];
        f.next++;
        const c = ed.CalleeID;
        if (ed.IsSelf !== 0 || c < 0 || c >= n) continue;
        if (state[c] === 0) { state[c] = 1; stack.push({ id: c, next: 0, best: 0, dep: 0 }); }
        else if (state[c] === 1) cyc[f.id] = 1;
        else {
          if (total[c] > f.best) { f.best = total[c]; f.dep = depth[c]; }
          if (cyc[c]) cyc[f.id] = 1;
        }
        continue;
      }
      total[f.id] = frame[f.id] + f.best;
      depth[f.id] = f.dep + 1;
      state[f.id] = 2;
      stack.pop();
      if (stack.length) {
        const par = stack[stack.length - 1];
        if (total[f.id] > par.best) { par.best = total[f.id]; par.dep = depth[f.id]; }
        if (cyc[f.id]) cyc[par.id] = 1;
      }
    }
  }
  return [total, depth, cyc];
}

M.mM13 = (g, p) => {
  const frame = g.shapeDense("frame_bytes"), big = g.shapeDense("frame_max_local"), uns = g.shapeDense("frame_unsized");
  const [total, depth, cyc] = chainBytes(g, frame);
  return ret(["name", "frame_bytes", "biggest_local", "unsized", "locals", "chain_bytes", "chain_depth", "cyclic", "rec", "at"],
    scan(g, p, (s) => isFn(s) && s.NLocals > 0, (s) => [S(s.Name), I(frame[s.ID]), I(big[s.ID]), I(uns[s.ID]), I(s.NLocals), I(total[s.ID]),
      I(depth[s.ID]), I(cyc[s.ID]), I(s.IsRecursive), S(g.at(s))]), p, dI(5), dI(1), cmpS(0));
};

M.mM14 = (g, p) => ret(["name", "casts", "derefs", "shifts", "sizeofs", "mem", "io", "at"],
  scan(g, p, (s) => s.NCast > 0, (s) => [S(s.Name), I(s.NCast), I(s.NDeref), I(s.NShift), I(s.NSizeof), I(s.NMemory), I(s.NIO), S(g.at(s))]),
  p, dI(1));

function macroQ(g, p, cols, pred, fill, ...keys) {
  const bySym = g.six().macro, rows = [];
  for (const s of g.syms()) {
    const m = bySym[s.ID];
    if (!m || !pred(m) || !g.modOK(s.ModuleID, p)) continue;
    rows.push(fill(s, m));
  }
  return ret(cols, rows, p, ...keys);
}

M.mM15 = (g, p) => macroQ(g, p, ["name", "uses", "params", "body_len", "multiline", "at"], (m) => m.IsFunctionlike === 1,
  (s, m) => [S(s.Name), I(m.NUses), I(m.NParams), I(m.BodyLen), I(m.IsMultiline), S(g.at(s))], dI(1), dI(3));

M.mM16 = (g, p) => {
  const m = new Map();
  for (const cb of g.Cfgs) {
    if (cb.IsConfig !== 1) continue;
    let a = m.get(cb.Expr);
    if (!a) m.set(cb.Expr, (a = { sites: 0, files: new Set(), forms: [], inFile: [] }));
    a.sites++;
    if (!a.files.has(cb.FileID)) {
      a.files.add(cb.FileID);
      const f = g.fileOf2(cb.FileID);
      if (f && !a.inFile.includes(f.Basename)) a.inFile.push(f.Basename);
    }
    if (!a.forms.includes(cb.Directive)) a.forms.push(cb.Directive);
  }
  const rows = [];
  for (const [e, a] of m) {
    let ok = false;
    for (const fid of a.files) { const f = g.fileOf2(fid); if (f && g.modOK(f.ModuleID, p)) ok = true; }
    if (!ok) continue;
    rows.push([S(e), I(a.sites), I(a.files.size), S(groupConcat(a.forms)), S(groupConcat(a.inFile))]);
  }
  return ret(["expr", "sites", "files", "forms", "in_files"], rows, p, dI(1));
};

M.mM17 = (g, p) => {
  const m = new Map(), nameN = g.six().nameN;
  for (const s of g.syms()) {
    if (!isFn(s) || nameN[s.ID] < 2) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0) continue;
    const mod = g.modName(s.ModuleID);
    if (!likeMatch(mod, p.mod)) continue;
    if (s.ModuleID >= 1 && s.ModuleID <= g.Modules.length) { const k = g.Modules[s.ModuleID - 1].Kind; if (k === "test" || k === "tool") continue; }
    if (s.Name === "main" || s.Name === "LLVMFuzzerTestOneInput" || s.Name === "usage" || s.Name === "help") continue;
    let gp = m.get(s.Name);
    if (!gp) m.set(s.Name, (gp = { sloc: [], files: [], fileID: new Set(), defs: 0 }));
    gp.sloc.push(s.Sloc); gp.defs++; gp.fileID.add(f.ID);
    if (!gp.files.includes(f.Basename)) gp.files.push(f.Basename);
  }
  const rows = [];
  for (const [n, gp] of m) {
    let lo = 1 << 30, hi = 0;
    for (const v of gp.sloc) { if (v < lo) lo = v; if (v > hi) hi = v; }
    if (gp.defs <= 1 || hi < 3 * lo + 3) continue;
    if (gp.fileID.size >= 2 && gp.files.reduce((a, b) => (b < a ? b : a)) === gp.files.reduce((a, b) => (b > a ? b : a))) continue;
    rows.push([S(n), I(lo), I(hi), I(gp.defs), S(groupConcat(distinct(gp.files)))]);
  }
  return ret(["name", "small", "large", "defs", "files"], rows, p, dWeighted([[1, 2], [-1, 1]]));
};

M.mM18 = (g, p) => ret(["name", "fan_in", "sloc", "cyclo", "hidden", "at"],
  scan(g, p, (s) => isFn(s) && s.IsInline === 1 && s.IsStatic === 1 && s.FanIn >= 3 && s.Sloc >= 4, (s) => [S(s.Name), I(s.FanIn), I(s.Sloc),
    I(s.Cyclomatic), I(Math.imul(s.FanIn, s.Sloc)), S(g.at(s))]), p, dI(4), cmpS(0));

M.mM19 = (g, p) => ret(["name", "cyclo", "sloc", "cmts", "doc", "nest", "fan_in", "at"],
  scan(g, p, (s) => isFn(s) && s.Cyclomatic >= 20 && s.NCommentLines * 20 < s.Sloc, (s) => [S(s.Name), I(s.Cyclomatic), I(s.Sloc),
    I(s.NCommentLines), I(s.HasDoc), I(s.MaxNesting), I(s.FanIn), S(g.at(s))]), p, dI(1), cmpS(0));

M.mM20 = (g, p) => {
  const rows = [];
  for (const r of g.MkRules) if (r.NObjs + r.NSrcs >= 2) rows.push([S(r.Path), S(r.Rule), I(r.NObjs), I(r.NSrcs), I(r.UsesAr), I(r.Line)]);
  return ret(["path", "rule", "objs", "srcs", "archives", "at_line"], rows, p, dSum(2, 3));
};

M.mM21 = (g, p) => {
  const m = new Map();
  for (const f of g.Files) {
    let a = m.get(f.ModuleID);
    if (!a) m.set(f.ModuleID, (a = { files: 0, parsed: 0, errs: 0, unc: 0, sloc: 0, fns: 0, inc: 0 }));
    a.files++; a.parsed += f.Parsed; a.errs += f.NParseErrors; a.unc += f.NMissingNodes; a.sloc += f.Sloc; a.fns += f.NFunctions; a.inc += f.NImports;
  }
  const rows = [];
  for (const mod of g.Modules) {
    const a = m.get(mod.ID);
    if (!a || !likeMatch(mod.Name, p.mod)) continue;
    rows.push([S(mod.Name), I(a.files), I(a.parsed), I(a.errs), I(a.unc), I(a.sloc), I(a.fns), I(a.inc), I(0)]);
  }
  return ret(["module", "files", "parsed", "errors", "unclosed", "sloc", "fns", "includes", "ms"], rows, p, dI(5));
};

M.mM22 = (g, p) => ret(["name", "gotos", "labels", "cyclo", "sloc", "fan_in", "at"],
  scan(g, p, (s) => s.NGotos > 3, (s) => [S(s.Name), I(s.NGotos), I(s.NLabels), I(s.Cyclomatic), I(s.Sloc), I(s.FanIn), S(g.at(s))], true),
  p, dI(1), dI(3));

M.mM23 = (g, p) => ret(["name", "nesting", "cyclo", "loops", "sloc", "fan_in", "at"],
  scan(g, p, (s) => s.MaxNesting > 5 && isFn(s), (s) => [S(s.Name), I(s.MaxNesting), I(s.Cyclomatic), I(s.NLoops), I(s.Sloc), I(s.FanIn),
    S(g.at(s))], true), p, dI(1), dI(2), cmpS(0));

M.mM24 = (g, p) => ret(["name", "n_params", "sloc", "cyclo", "fan_in", "at"],
  scan(g, p, (s) => s.NParams > 6 && isFn(s), (s) => [S(s.Name), I(s.NParams), I(s.Sloc), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], true),
  p, dI(1), dI(4), cmpS(0));

M.mM25 = (g, p) => {
  const inn = g.edgeCSR(true), seen = new Int32Array(g.Modules.length + 1), rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || !g.notTest(s)) continue;
    const mods = [];
    for (const j of inn.of(s.ID)) {
      const e = g.Edges[j];
      if (e.IsSelf !== 0) continue;
      const cs = g.sym(e.CallerID);
      if (!cs) continue;
      const m = cs.ModuleID;
      if (m >= 0 && m < seen.length) { if (seen[m] !== s.ID) { seen[m] = s.ID; mods.push(m); } }
      else if (!mods.includes(m)) mods.push(m);
    }
    if (mods.length <= 5) continue;
    const names = [];
    for (const m of mods) { const n = g.modName(m); if (n !== "") names.push(n); }
    if (!g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(mods.length), I(s.FanIn), I(s.Cyclomatic), I(s.Sloc), S(groupConcat(distinct(names))), S(g.at(s))]);
  }
  return ret(["name", "n_caller_modules", "fan_in", "cyclo", "sloc", "modules", "at"], rows, p, dI(1), dI(2), cmpS(0));
};

M.mM26 = (g, p) => ret(["name", "magic_numbers", "float_literals", "cyclo", "sloc", "fan_in", "at"],
  scan(g, p, (s) => s.NMagic > 10 && isFn(s), (s) => [S(s.Name), I(s.NMagic), I(s.NFloatLit), I(s.Cyclomatic), I(s.Sloc), I(s.FanIn),
    S(g.at(s))], true), p, dI(1), dI(5), cmpS(0));

M.mM27 = (g, p) => {
  const rows = [];
  for (const r of g.Reach) {
    const s = g.sym(r.SymbolID);
    if (!s || !isFn(s) || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(r.NTransitiveOut), I(r.NTransitive), I(s.FanOut), I(s.Cyclomatic), modOpt(g, s), S(g.at(s))]);
  }
  return ret(["name", "reachable_fns", "transitive_callers", "fan_out", "cyclo", "module", "at"], rows, p, dI(1), cmpS(0));
};

M.mM28 = (g, p) => {
  const gs = new Map(), ss = new Map(), wf = new Map(), ws = new Map();
  for (const gl of g.Globals) {
    if (gl.IsConst !== 0) continue;
    const f = g.fileOf2(gl.FileID);
    if (!f || f.IsTest !== 0) continue;
    inc(gs, f.ModuleID); inc(ss, f.ModuleID, gl.IsStatic);
  }
  for (const s of g.syms()) {
    if (!isFn(s) || s.NGlobalWrite <= 0) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0) continue;
    inc(wf, f.ModuleID); inc(ws, f.ModuleID, s.NGlobalWrite);
  }
  const rows = [];
  for (const mod of g.Modules) {
    if (!likeMatch(mod.Name, p.mod)) continue;
    const a = gs.get(mod.ID) || 0, w = wf.get(mod.ID) || 0;
    if (a + w <= 0) continue;
    rows.push([S(mod.Name), I(a), I(ss.get(mod.ID) || 0), I(w), I(ws.get(mod.ID) || 0)]);
  }
  return ret(["module", "mutable_globals", "static_ones", "writer_fns", "write_sites"], rows, p, dI(1), dI(3));
};

M.mM29 = (g, p) => ret(["name", "returns_", "fail_shapes", "null_checks", "gotos", "err_pct", "cyclo", "fan_in", "at"],
  scan(g, p, (s) => isFn(s) && s.NReturns >= 4, (s) => {
    const shapes = s.RetNull + s.RetNeg + s.RetZero, den = s.NReturns + s.NBranches;
    return [S(s.Name), I(s.NReturns), I(shapes), I(s.NNullCheck), I(s.NGotos), den !== 0 ? I(pct(shapes + s.NNullCheck, den)) : NUL,
      I(s.Cyclomatic), I(s.FanIn), S(g.at(s))];
  }), p, dI(5), dI(7), cmpS(0));

M.mM30 = (g, p) => {
  const m = new Map();
  for (const a of g.Allocs) {
    const s = g.sym(a.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    let e = m.get(a.Fn);
    if (!e) m.set(a.Fn, (e = { sites: 0, with: 0, raw: 0, callers: new Set() }));
    e.sites++;
    if (a.SizeExpr.includes("sizeof")) e.with++; else e.raw++;
    e.callers.add(s.Name);
  }
  const rows = [];
  for (const [fn, e] of m) {
    const pv = e.sites !== 0 ? goRound(((e.with * 100) / e.sites) * 10) / 10 : 0;
    rows.push([S(fn), I(e.sites), I(e.with), I(e.raw), I(e.callers.size), F(pv)]);
  }
  return ret(["fn", "sites", "with_sizeof", "raw_bytes", "callers", "pct_sizeof"], rows, p, dI(1));
};

M.mM31 = (g, p) => ret(["name", "copy_sites", "mem_ops_total", "loop_depth", "allocs_in_loop", "fan_in", "module", "at"],
  scan(g, p, (s) => isFn(s) && s.NMemcpy >= 3, (s) => [S(s.Name), I(s.NMemcpy), I(s.NMemory), I(s.MaxLoopDepth), I(s.AllocInLoop), I(s.FanIn),
    modOpt(g, s), S(g.at(s))]), p, dI(1), dI(5), cmpS(0));

M.mM32 = (g, p) => macroQ(g, p, ["name", "uses", "body_len", "expanded_bytes", "multiline", "params", "at"],
  (m) => m.IsFunctionlike === 1 && m.NUses > 0, (s, m) => [S(s.Name), I(m.NUses), I(m.BodyLen), I(Math.imul(m.NUses, m.BodyLen)),
    I(m.IsMultiline), I(m.NParams), S(g.at(s))], dI(3));

M.mM33 = (g, p) => {
  const ptr = g.perSym(), fnp = g.perSym(), ok = (id) => id >= 0 && id < ptr.length;
  for (const l of g.Layout) {
    if (!ok(l.SymbolID)) continue;
    if (l.PtrDepth > 0 && l.InUnion === 0 && l.Depth === 0) ptr[l.SymbolID]++;
    if (l.IsFnptr === 1) fnp[l.SymbolID]++;
  }
  const pairs = new Set(), incl = g.perFile();
  for (const im of g.Imports) {
    if (!im.HasTargetID) continue;
    const k = im.TargetID * 4294967296 + im.FileID;
    if (!pairs.has(k)) { pairs.add(k); if (im.TargetID >= 0 && im.TargetID < incl.length) incl[im.TargetID]++; }
  }
  const rows = [];
  for (const ss of g.SSize) {
    if (ss.Exact !== 1) continue;
    const s = g.sym(ss.SymbolID);
    if (!s) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.Ext !== ".h" || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(ss.TotalSize), I(ss.TotalPad), I(ok(ss.SymbolID) ? ptr[ss.SymbolID] : 0), I(ok(ss.SymbolID) ? fnp[ss.SymbolID] : 0),
      I(incl[f.ID] || 0), I(ss.NLines64), S(g.at(s))]);
  }
  return ret(["struct_", "sz", "pad", "ptr_fields", "fnptr_fields", "includers", "lines64", "at"], rows, p, dI(5), dI(3), dI(1));
};

M.mM34 = (g, p) => {
  const types = ["TODO", "FIXME", "XXX", "HACK", "BUG", "UNSAFE", "DEPRECATED"], m = new Map();
  for (const mk of g.Markers) {
    let a = m.get(mk.FileID);
    if (!a) m.set(mk.FileID, (a = { n: 0, by: new Map() }));
    a.n++; inc(a.by, mk.Kind);
  }
  const rows = [];
  for (const f of g.Files) {
    const a = m.get(f.ID);
    if (!a || !g.modOK(f.ModuleID, p)) continue;
    rows.push([S(f.Path), S(g.modName(f.ModuleID) || "(none)"), I(a.n), ...types.map((t) => I(a.by.get(t) || 0)), I(f.Sloc), I(f.TotalRisk)]);
  }
  return ret(["path", "module", "n_markers", "todo", "fixme", "xxx", "hack", "bug", "unsafe", "deprecated", "sloc", "risk"], rows, p, dI(2), dI(10));
};

M.mM35 = (g, p) => {
  const m = new Map();
  for (const s of g.syms()) {
    if (!isFn(s) || s.Sloc < 10 || s.Cyclomatic < 3 || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    const sig = s.Sloc + "/" + s.Cyclomatic + "/" + s.NParams + "/" + s.NCalls;
    let gp = m.get(sig);
    if (!gp) m.set(sig, (gp = { k: [s.Sloc, s.Cyclomatic, s.NParams], copies: 0, dup: 0, mods: [] }));
    gp.copies++; gp.dup += s.Sloc;
    if (!gp.mods.includes(s.ModuleID)) gp.mods.push(s.ModuleID);
  }
  const rows = [];
  for (const sig of [...m.keys()].filter((k) => m.get(k).copies > 1).sort(byteCmp)) {
    const gp = m.get(sig);
    const names = gp.mods.map((mid) => g.modName(mid)).sort(byteCmp);
    rows.push([S(sig), I(gp.copies), I(gp.k[0]), I(gp.k[1]), I(gp.k[2]), I(gp.dup), S(groupConcat(distinct(names)))]);
  }
  return ret(["shape", "copies", "sloc", "cyclo", "params", "duplicated_sloc", "modules"], rows, p, dI(5), dI(1));
};

M.mM36 = (g, p) => {
  const [start, idx] = groupRows(g.Params.length, g.Symbols.length + 1, (i) => { const pr = g.Params[i]; return pr.IsRef === 1 && pr.Type !== "" ? pr.SymbolID : -1; });
  const rows = [];
  for (const s of g.syms()) {
    const ps = idx.subarray(start[s.ID], start[s.ID + 1]);
    if (ps.length === 0 || !isFn(s) || s.NRestrict !== 0 || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    let pairs = 0;
    for (const a of ps) for (const b of ps) if (a !== b && g.Params[a].Type === g.Params[b].Type) { pairs++; break; }
    if (pairs === 0) continue;
    rows.push([S(s.Name), I(pairs), I(s.NRestrict), I(s.NParams), I(s.Cyclomatic), I(s.MaxLoopDepth), I(s.FanIn), S(g.at(s))]);
  }
  return ret(["name", "aliasing_pairs", "restrict_hints", "params", "cyclo", "depth", "fan_in", "at"], rows, p, dI(1), dI(5), dI(6), cmpS(0));
};

M.mM37 = (g, p) => {
  const rows = [];
  for (const f of g.Files) {
    if (f.IsTest !== 0 || f.Sloc <= 0) continue;
    const mod = g.modName(f.ModuleID);
    if (!likeMatch(mod, p.mod)) continue;
    rows.push([S(f.Path), S(mod || "(none)"), I(f.Sloc), I(f.NFunctions), I(f.NTypes), I(f.TotalCyclo), I(f.MaxCyclo), I(f.TotalRisk), I(f.NImports),
      I(pct(f.TotalRisk, f.Sloc))]);
  }
  return ret(["path", "module", "sloc", "fns", "types", "cyclo", "worst", "risk", "includes", "risk_per_100_sloc"], rows, p, dI(7), dI(2));
};

M.mM38 = (g, p) => {
  const x = g.six(), rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.IsStatic !== 0 || s.IsInline !== 0) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.Ext !== ".c" || x.declHdr[s.ID] === 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(s.FanIn), I(s.FanOut), I(s.Cyclomatic), I(s.Sloc), I(s.HasDoc), I(s.NParams), I(x.declN[s.ID]), S(g.at(s))]);
  }
  return ret(["name", "fan_in", "fan_out", "cyclo", "sloc", "documented", "params", "declared_in", "at"], rows, p, dI(1), dI(4), cmpS(0));
};

M.mM39 = (g, p) => macroQ(g, p, ["name", "macro_params", "body_len", "uses", "multiline", "do_while", "at"], (m) => m.IsFunctionlike === 1,
  (s, m) => {
    const sp = (m.Body || "").replaceAll(" ", "");
    return [S(s.Name), I(m.NParams), I(m.BodyLen), I(m.NUses), I(m.IsMultiline), I(sp.includes("do{") && sp.includes("while(") ? 1 : 0), S(g.at(s))];
  }, cmpI(3), dI(2));

M.mM40 = (g, p) => {
  const m = Array.from({ length: g.Symbols.length + 1 }, () => ({ n: 0, uninit: 0, cst: 0, loop: 0 }));
  for (const l of g.Locals) {
    if (l.SymbolID < 0 || l.SymbolID >= m.length) continue;
    const a = m[l.SymbolID];
    a.n++;
    if (l.HasInit === 0) a.uninit++;
    a.cst += l.IsConst; a.loop += l.InLoop;
  }
  const rows = [];
  for (const s of g.syms()) {
    const a = m[s.ID];
    if (a.n === 0 || !isFn(s) || !g.notTest(s) || !g.modOK(s.ModuleID, p) || a.uninit < 3) continue;
    rows.push([S(s.Name), I(a.n), I(a.uninit), I(a.cst), I(a.loop), I(s.Cyclomatic), I(s.Sloc), S(g.at(s))]);
  }
  return ret(["name", "locals", "uninitialised", "const_locals", "in_loop", "cyclo", "sloc", "at"], rows, p, dI(2), dI(5), cmpS(0));
};

M.mM41 = (g, p) => {
  const m = new Map();
  for (const l of g.Locks) {
    const s = g.sym(l.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    let a = m.get(l.Name);
    if (!a) m.set(l.Name, (a = { fns: new Set(), fileSet: new Set(), sites: 0, cyc: 0, worst: 0, io: 0, alloc: 0, mods: [] }));
    a.sites++; a.fns.add(s.ID); a.cyc += s.Cyclomatic;
    if (s.Cyclomatic > a.worst) a.worst = s.Cyclomatic;
    a.io += s.NIO; a.alloc += s.NAlloc;
    const mn = g.modName(s.ModuleID);
    if (!a.mods.includes(mn)) a.mods.push(mn);
    const f = g.fileOf(s.ID);
    if (f) a.fileSet.add(f.ID);
  }
  const rows = [];
  for (const [n, a] of m) rows.push([S(n), I(a.fns.size), I(a.sites), I(a.fileSet.size), I(a.cyc), I(a.worst), I(a.io), I(a.alloc), S(groupConcat(distinct(a.mods)))]);
  return ret(["lock_name", "held_by_fns", "acquire_sites", "n_files", "cyclo_under_lock", "worst_cyclo", "io_under_lock", "allocs_under_lock", "modules"],
    rows, p, dI(2), dI(4));
};

M.mM42 = (g, p) => ret(["typedef_name", "underlying", "at"],
  scan(g, p, (s) => s.Kind === "typedef" && s.HasReturnType && (s.ReturnType.includes("*") || s.ReturnType.includes("]")),
    (s) => [S(s.Name), S(s.ReturnType), S(g.at(s))]), p, cmpS(0));

M.mM43 = (g, p) => {
  const incl = new Map();
  for (const im of g.Imports) if (im.HasTargetID) inc(incl, im.TargetID);
  const m = new Map();
  for (const s of g.syms()) {
    if (!isFn(s) || s.IsStatic !== 1 || s.IsInline !== 1) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.Ext !== ".h" || !g.modOK(s.ModuleID, p)) continue;
    let a = m.get(f.ID);
    if (!a) m.set(f.ID, (a = { n: 0, sloc: 0, fan: 0, worst: 0, cyc: 0 }));
    a.n++; a.sloc += s.Sloc; a.fan += s.FanIn;
    if (s.FanIn > a.worst) a.worst = s.FanIn;
    a.cyc += s.Cyclomatic;
  }
  const rows = [];
  for (const fid of sortedIDs(m)) {
    const a = m.get(fid), f = g.fileOf2(fid);
    rows.push([S(f.Path), I(a.n), I(a.sloc), I(a.fan), I(a.worst), I(a.cyc), I(incl.get(fid) || 0)]);
  }
  return ret(["path", "n_static_inline", "sloc", "total_fan_in", "worst_fan_in", "cyclo", "times_included"], rows, p, dI(3), dI(1));
};

M.mM44 = (g, p) => {
  const m = new Map();
  for (const s of g.syms()) {
    if (!isFn(s) || s.NReturns === 0) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0 || !g.modOK(s.ModuleID, p)) continue;
    let a = m.get(f.ModuleID);
    if (!a) m.set(f.ModuleID, (a = { fns: 0, nul: 0, neg: 0, zer: 0, mixed: 0 }));
    a.fns++;
    a.nul += b2i(s.RetNull > 0); a.neg += b2i(s.RetNeg > 0); a.zer += b2i(s.RetZero > 0);
    a.mixed += b2i((s.RetNull > 0) + (s.RetNeg > 0) + (s.RetZero > 0) >= 2);
  }
  const rows = [];
  for (const mid of sortedIDs(m)) {
    const a = m.get(mid);
    if (a.fns < 8) continue;
    rows.push([S(g.modName(mid)), I(a.fns), I(a.nul), I(a.neg), I(a.zer), I(a.mixed), I(pct(a.mixed, a.fns))]);
  }
  return ret(["module", "fns", "returns_null", "returns_neg", "returns_zero", "mixed", "pct_mixed"], rows, p, dI(6), dI(1), cmpS(0));
};

M.mM45 = (g, p) => {
  const hdrNames = new Set();
  for (const d of g.Decls) { const f = g.fileOf2(d.FileID); if (f && f.Ext === ".h") hdrNames.add(d.Name); }
  const srcN = new Map(), inhN = new Map(), seen = new Map();
  for (const d of g.Decls) {
    const f = g.fileOf2(d.FileID);
    if (!f || f.Ext !== ".c") continue;
    inc(srcN, d.FileID);
    if (hdrNames.has(d.Name)) {
      let s = seen.get(d.FileID);
      if (!s) seen.set(d.FileID, (s = new Set()));
      if (!s.has(d.Name)) { s.add(d.Name); inc(inhN, d.FileID); }
    }
  }
  const rows = [];
  for (const fid of sortedIDs(srcN)) {
    if (srcN.get(fid) < 3) continue;
    const f = g.fileOf2(fid), mod = g.modName(f.ModuleID);
    if (!likeMatch(mod, p.mod)) continue;
    rows.push([S(f.Path), S(mod || "(none)"), I(srcN.get(fid)), I(inhN.get(fid) || 0), I(f.Sloc)]);
  }
  return ret(["path", "module", "extern_decls", "also_in_a_header", "sloc"], rows, p, dI(2), dI(4));
};

M.mM46 = (g, p) => {
  const cols = ["name", "module", "epoll", "uring", "kqueue", "waits", "et", "oneshot", "write_ready", "err_flags", "rearms", "deregs", "eagain",
    "eintr", "calls", "at"];
  const bySym = new Map();
  for (const e of g.EvOps) addUniq(bySym, e.SymbolID, e.Fn);
  const rows = [];
  for (const s of g.syms()) {
    if (s.NEpoll + s.NUring + s.NKqueue <= 0) continue;
    const mod = g.modName(s.ModuleID);
    if (!likeMatch(mod, p.mod)) continue;
    const l = bySym.get(s.ID);
    rows.push([S(s.Name), S(mod || "(none)"), I(s.NEpoll), I(s.NUring), I(s.NKqueue), I(s.NEventWait), I(s.NEtReg), I(s.NOneshotReg),
      I(s.NWriteReady), I(s.NErrFlag), I(s.NRearm), I(s.NDereg), I(s.NEagain), I(s.NEintr), l && l.length ? S(groupConcat(l)) : NUL, S(g.at(s)),
      I(s.Cyclomatic)]);
  }
  orderBy(rows, dSum(2, 3, 4), dI(cols.length));
  return [cols, limit(rows.map((r) => r.slice(0, cols.length)), p)];
};

M.mM47 = (g, p) => ret(["name", "waits", "cyclo", "cog", "nest", "loop_depth", "calls_in_loop", "io_in_loop", "branches_in_loop", "sloc", "fan_in", "at"],
  scan(g, p, (s) => s.NEventWait > 0, (s) => [S(s.Name), I(s.NEventWait), I(s.Cyclomatic), I(s.Cognitive), I(s.MaxNesting), I(s.MaxLoopDepth),
    I(s.CallInLoop), I(s.IOInLoop), I(s.BranchInLoop), I(s.Sloc), I(s.FanIn), S(g.at(s))]), p, dI(2), dI(9));

M.mM48 = (g, p) => {
  const m = new Map();
  for (const s of g.syms()) {
    if (s.NEpoll + s.NUring + s.NKqueue <= 0) continue;
    const f = g.fileOf(s.ID);
    if (!f) continue;
    let a = m.get(f.ModuleID);
    if (!a) m.set(f.ModuleID, (a = new Array(13).fill(0)));
    a[0]++;
    a[1] += b2i(s.NEpoll > 0); a[2] += b2i(s.NUring > 0); a[3] += b2i(s.NKqueue > 0); a[4] += b2i(s.NEventWait > 0); a[5] += b2i(s.NEtReg > 0);
    a[6] += b2i(s.NErrFlag > 0); a[7] += b2i(s.NEintr > 0); a[8] += b2i(s.NNonblockSet > 0); a[9] += b2i(s.NEventWait > 0 && s.NEintr === 0);
    a[10] += b2i(s.NEtReg > 0 && s.NNonblockSet === 0); a[11] += b2i(s.NEventCreate > 0 && s.NEventDestroy === 0);
    a[12] += s.NEpoll + s.NUring + s.NKqueue;
  }
  const rows = [];
  for (const mid of sortedIDs(m)) {
    const mod = g.modName(mid);
    if (!likeMatch(mod, p.mod)) continue;
    rows.push([S(mod || "(none)"), ...m.get(mid).map(I)]);
  }
  return ret(["module", "fns", "epoll_fns", "uring_fns", "kqueue_fns", "wait_fns", "edge_triggered", "watches_errors", "handles_eintr",
    "sets_nonblocking", "eintr_blind", "et_unprepared", "leaks_instance", "api_calls"], rows, p, dI(13), dI(1));
};

const retNegNS = ["stdio.h", "sys/socket.h", "sys/mman.h", "sys/wait.h", "sys/uio.h", "unistd.h", "fcntl.h", "dirent.h", "stdio.h"];
M.mM49 = (g, p) => {
  const nf = g.Files.length + 1, per = new Array(nf);
  for (const a of g.APIUses) {
    const f = g.fileOf2(a.FileID);
    if (!f || !g.modOK(f.ModuleID, p)) continue;
    const l = per[a.FileID] || (per[a.FileID] = []);
    const e = l.find((x) => x.ns === a.NS);
    if (e) { e.sites++; e.fns.push(a.SymbolID); if (!e.calls.includes(a.Fn)) e.calls.push(a.Fn); }
    else l.push({ ns: a.NS, sites: 1, fns: [a.SymbolID], calls: [a.Fn] });
  }
  const rows = [];
  for (let fid = 0; fid < nf; fid++) {
    const l = per[fid];
    if (!l || !l.length) continue;
    l.sort((a, b) => byteCmp(a.ns, b.ns));
    for (const e of l) rows.push([S(g.Files[fid - 1].Path), S(e.ns), I(e.sites), I(new Set(e.fns).size), I(e.calls.length)]);
  }
  return ret(["file", "ns", "call_sites", "fns", "distinct_calls"], rows, p, dI(2));
};

M.mM50 = (g, p) => {
  const m = Array.from({ length: g.Files.length + 1 }, () => ({ hit: false, ur: 0, uc: 0, cu: 0, vu: 0, mu: 0, sites: 0, fns: [] }));
  for (const a of g.APIUses) {
    const f = g.fileOf2(a.FileID);
    if (!f || !g.modOK(f.ModuleID, p)) continue;
    const e = m[a.FileID];
    e.fns.push(a.SymbolID);
    const r = g.sym(a.SymbolID);
    if (!r) continue;
    e.hit = true; e.sites++;
    const ns = a.NS;
    if (retNegNS.includes(ns) && r.NRetNegCheck === 0) e.ur++;
    if (["strtol", "strtoll", "strtoul", "strtoull", "strtod"].includes(a.Fn) && !(r.NErrnoZero > 0 && r.NEndptr > 0)) e.uc++;
    if (ns === "ctype.h") { if (r.NUcharCast === 0) e.cu++; }
    else if (ns === "stdarg.h") { if (r.NVaEnd === 0) e.vu++; }
    else if (ns === "sys/mman.h") { if (r.NMapFailed === 0) e.mu++; }
  }
  const rows = [];
  m.forEach((e, fid) => {
    if (!e.hit) return;
    rows.push([S(g.Files[fid - 1].Path), I(new Set(e.fns).size), I(e.ur), I(e.uc), I(e.cu), I(e.vu), I(e.mu), I(e.sites)]);
  });
  return ret(["file", "fns", "unchecked_returns", "unchecked_conversions", "ctype_ub", "varargs_unclosed", "mmap_unchecked", "api_call_sites"],
    rows, p, dI(7));
};
