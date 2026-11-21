// The dyna-specific queries and metrics (codegraph_dyn.go).
import { I, S, F, NUL, optI, limit, byteCmp, likeMatch, inc, pushTo } from "./qcore.js";
import { dynAPI } from "./shapes.js";

const c3 = byteCmp;
const dynDesc = (...cols) => (a, b) => { for (const c of cols) { const d = c3(b[c].i, a[c].i); if (d) return d; } return 0; };
const sort = (rows, cmp) => rows.sort(cmp);
const dynFns = (g, p) => g.Symbols.filter((s) => s.Kind === "function" && g.modOK(s.ModuleID, p));
const prefix = (...ps) => (s) => ps.some((x) => s.startsWith(x));
const oneOf = (...ns) => (s) => ns.includes(s);
function apiSum(t, id, pred) {
  const m = t.bySym.get(id);
  if (!m) return 0;
  let n = 0;
  for (const [name, c] of m) if (pred(name)) n += c;
  return n;
}
const api = (g) => dynAPI(g.raw);

export function dynTainted(g) {
  return g.memo("dynTainted", () => {
    const t = new Uint8Array(g.Symbols.length + 1), out = g.edgeCSR(false), stack = [];
    for (const s of g.Symbols) if (s.Kind === "function" && s.NIO > 0) { t[s.ID] = 1; stack.push(s.ID); }
    while (stack.length) {
      const id = stack.pop();
      for (const j of out.of(id)) { const c = g.Edges[j].CalleeID; if (c >= 1 && c < t.length && !t[c]) { t[c] = 1; stack.push(c); } }
    }
    return t;
  });
}

function cloneGroups(g) {
  return g.memo("cloneGroups", () => {
    const m = new Map();
    for (const s of g.Symbols) {
      if (s.Kind !== "function" || s.Sloc < 20) continue;
      pushTo(m, [s.Sloc, s.Cyclomatic, s.NParams, s.NReturns, s.NBranches].join(","), s);
    }
    const out = [];
    for (const v of m.values()) if (v.length > 1) { v.sort((a, b) => a.ID - b.ID); out.push(v); }
    return out;
  });
}

function modEdges(g) {
  const me = new Map();
  for (const e of g.Edges) {
    const a = g.sym(e.CallerID), b = g.sym(e.CalleeID);
    if (!a || !b || a.ModuleID <= 0 || b.ModuleID <= 0 || a.ModuleID === b.ModuleID) continue;
    me.set(a.ModuleID * 4294967296 + b.ModuleID, [a.ModuleID, b.ModuleID]);
  }
  return [...me.values()];
}

export const D = {};
D.dynUntrustedReach = (g, p) => {
  const t = dynTainted(g), rows = [];
  for (const s of g.Symbols) {
    if (!t[s.ID] || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(s.NIO), I(s.NMemory), I(s.NAlloc), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]);
  }
  sort(rows, (a, b) => c3(b[2].i + b[3].i, a[2].i + a[3].i) || c3(b[1].i, a[1].i));
  return [["name", "io", "mem", "alloc", "cyclo", "fan_in", "at"], limit(rows, p)];
};

const isCreate = prefix("JS_New", "JS_Dup"), isFree = prefix("JS_FreeValue", "js_free_");
function refcountRows(g, p, keep) {
  const t = api(g), rows = [];
  for (const s of dynFns(g, p)) {
    const al = apiSum(t, s.ID, isCreate), fr = apiSum(t, s.ID, isFree);
    if (!keep(al, fr)) continue;
    rows.push([S(s.Name), I(al), I(fr), I(al - fr), I(s.NReturns), S(s.ReturnType), S(g.at(s))]);
  }
  return rows;
}
const RC_COLS = ["name", "alloc_dup", "free_n", "drift", "n_returns", "return_type", "at"];
D.dynRefcountLeak = (g, p) => { const r = refcountRows(g, p, (al, fr) => al > 0 || fr > 0); sort(r, dynDesc(3, 4)); return [RC_COLS, limit(r, p)]; };
D.dynOverFree = (g, p) => { const r = refcountRows(g, p, (al, fr) => fr > al); sort(r, (a, b) => c3(a[3].i, b[3].i)); return [RC_COLS, limit(r, p)]; };

D.dynDefineVsSet = (g, p) => {
  const t = api(g), tt = dynTainted(g), isSet = prefix("JS_SetProperty"), rows = [];
  for (const s of g.Symbols) {
    const n = apiSum(t, s.ID, isSet);
    if (n === 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(n), I(tt[s.ID] ? 1 : 0), S(g.at(s))]);
  }
  sort(rows, dynDesc(1, 2));
  return [["name", "set_calls", "on_frontier", "at"], limit(rows, p)];
};

D.dynThrowingPath = (g, p) => {
  const t = api(g), isThrow = prefix("JS_Throw", "JS_IsException"), rows = [];
  for (const s of dynFns(g, p)) {
    if (!s.ReturnType.startsWith("JSValue") || s.NReturns <= 1 || apiSum(t, s.ID, isThrow) !== 0) continue;
    rows.push([S(s.Name), I(s.NReturns), I(s.NCalls), S(s.ReturnType), S(g.at(s))]);
  }
  sort(rows, dynDesc(1, 2));
  return [["name", "n_returns", "n_calls", "return_type", "at"], limit(rows, p)];
};

D.dynLazyStatic = (g, p) => {
  const rows = [];
  for (const gl of g.Globals) {
    if (gl.IsConst !== 0 || !(gl.IsStatic === 0 || gl.HasInit === 0)) continue;
    if (gl.FileID < 1 || gl.FileID > g.Files.length || !g.modOK(gl.ModuleID, p)) continue;
    rows.push([S(gl.Name), S(gl.Type), I(gl.IsStatic), I(gl.IsConst), I(gl.HasInit), S(g.Files[gl.FileID - 1].Path + ":" + gl.Line)]);
  }
  sort(rows, (a, b) => c3(b[2].i, a[2].i) || c3(a[4].i, b[4].i) || c3(a[5].s, b[5].s));
  return [["name", "type", "is_static", "is_const", "has_init", "at"], limit(rows, p)];
};

D.dynModuleCycles = (g, p) => {
  const n = g.Modules.length + 1, adj = Array.from({ length: n }, () => []);
  const nOut = new Int32Array(n), nIn = new Int32Array(n);
  for (const [a, b] of modEdges(g)) { adj[a].push(b); nOut[a]++; nIn[b]++; }
  const reach = new Array(n);
  for (let m = 1; m < n; m++) {
    const seen = new Uint8Array(n), stack = adj[m].slice();
    while (stack.length) { const x = stack.pop(); if (seen[x]) continue; seen[x] = 1; for (const y of adj[x]) stack.push(y); }
    reach[m] = seen;
  }
  const rows = [];
  for (let a = 1; a < n; a++) {
    for (let b = a + 1; b < n; b++) {
      if (!reach[a][b] || !reach[b][a] || !likeMatch(g.modName(a), p.mod)) continue;
      rows.push([S(g.modName(a) + " <-> " + g.modName(b)), I(nOut[a]), I(nIn[b])]);
    }
  }
  sort(rows, (x, y) => c3(x[0].s, y[0].s));
  return [["cycle", "a_calls_out", "b_calls_in"], limit(rows, p)];
};

D.dynFeatureEnvy = (g, p) => {
  const out = g.edgeCSR(false), rows = [];
  for (const s of dynFns(g, p)) {
    if (s.ModuleID <= 0) continue;
    let own = 0;
    const cross = new Map();
    for (const j of out.of(s.ID)) {
      const e = g.Edges[j];
      if (e.SameModule !== 0) { own++; continue; }
      const c = g.sym(e.CalleeID);
      if (c && c.ModuleID > 0) inc(cross, c.ModuleID);
    }
    for (const [mod, n] of cross) {
      if (n >= 4 && n >= 2 * own + 2) rows.push([S(s.Name), S(g.modName(s.ModuleID)), S(g.modName(mod)), I(n), I(own), S(g.at(s))]);
    }
  }
  sort(rows, (a, b) => c3(b[3].i, a[3].i) || c3(a[0].s, b[0].s) || c3(a[2].s, b[2].s));
  return [["name", "own_module", "envied_module", "cross_edges", "own_edges", "at"], limit(rows, p)];
};

D.dynShotgun = (g, p) => {
  const inn = g.edgeCSR(true), rows = [];
  for (const s of g.Symbols) {
    if (!g.modOK(s.ModuleID, p)) continue;
    const seen = new Set();
    for (const j of inn.of(s.ID)) { const c = g.sym(g.Edges[j].CallerID); if (c) seen.add(c.FileID); }
    if (seen.size < 8) continue;
    rows.push([S(s.Name), I(seen.size), I(s.FanIn), S(g.modName(s.ModuleID)), S(g.at(s))]);
  }
  sort(rows, dynDesc(1, 2));
  return [["name", "file_breadth", "fan_in", "module", "at"], limit(rows, p)];
};

D.dynGodFunctions = (g, p) => {
  const rows = [];
  for (const s of dynFns(g, p)) {
    if (s.Sloc < 300 || s.Cyclomatic < 25) continue;
    rows.push([S(s.Name), I(s.Sloc), I(s.Cyclomatic), I(s.NSwitch), I(s.FanIn), I(s.NReturns), S(g.at(s))]);
  }
  sort(rows, (a, b) => c3(b[1].i * b[2].i, a[1].i * a[2].i));
  return [["name", "sloc", "cyclomatic", "n_switch", "fan_in", "n_returns", "at"], limit(rows, p)];
};

D.dynMiddleMen = (g, p) => {
  const rows = [];
  for (const s of dynFns(g, p)) {
    if (s.Sloc > 10 || s.FanOut !== 1 || s.Cyclomatic > 2 || s.FanIn < 2 || s.IsStatic !== 0) continue;
    rows.push([S(s.Name), I(s.Sloc), I(s.FanIn), I(s.FanOut), S(g.modName(s.ModuleID)), S(g.at(s))]);
  }
  sort(rows, dynDesc(2));
  return [["name", "sloc", "fan_in", "fan_out", "module", "at"], limit(rows, p)];
};

D.dynDeadExported = (g, p) => {
  const n = g.Symbols.length + 1, addr = new Uint8Array(n);
  for (const a of g.Addr) if (a.HasSym && a.SymbolID >= 1 && a.SymbolID < n) addr[a.SymbolID] = 1;
  const named = g.namedAsValue(), trans = new Int32Array(n), has = new Uint8Array(n);
  for (const r of g.Reach) if (r.SymbolID >= 1 && r.SymbolID < n) { trans[r.SymbolID] = r.NTransitive; has[r.SymbolID] = 1; }
  const rows = [];
  for (const s of dynFns(g, p)) {
    if (s.IsStatic !== 0 || s.FanIn !== 0 || addr[s.ID] || named(s)) continue;
    rows.push([S(s.Name), I(s.Sloc), I(s.NCalls), optI(trans[s.ID], has[s.ID]), S(g.at(s))]);
  }
  sort(rows, dynDesc(1));
  return [["name", "sloc", "n_calls", "n_transitive", "at"], limit(rows, p)];
};

D.dynHiddenCoupling = (g, p) => {
  const inn = g.edgeCSR(true), nS = g.Symbols.length + 1;
  const cOff = new Int32Array(nS + 1), cList = [], fwdN = new Int32Array(nS), mod = new Int32Array(nS);
  for (let id = 1; id < nS; id++) { const s = g.sym(id); mod[id] = s ? s.ModuleID : 0; }
  for (let id = 1; id < nS; id++) {
    cOff[id] = cList.length;
    let callers = [];
    for (const j of inn.of(id)) { const c = g.sym(g.Edges[j].CallerID); if (c && c.ModuleID > 0) callers.push(c.ID); }
    if (callers.length >= 2) {
      callers.sort((a, b) => a - b);
      callers = callers.filter((v, i) => i === 0 || v !== callers[i - 1]);
      if (callers.length >= 2) for (const c of callers) { cList.push(c); fwdN[c]++; }
    }
  }
  cOff[nS] = cList.length;
  const fOff = new Int32Array(nS + 1);
  for (let a = 1; a < nS; a++) fOff[a + 1] = fOff[a] + fwdN[a];
  const fill = fOff.slice(), fList = new Int32Array(cList.length);
  for (let id = 1; id < nS; id++) for (let k = cOff[id]; k < cOff[id + 1]; k++) fList[fill[cList[k]]++] = id;
  const cnt = new Int32Array(nS), touched = new Int32Array(nS), rows = [];
  for (let a = 1; a < nS; a++) {
    if (fOff[a] === fOff[a + 1]) continue;
    let nt = 0;
    const ma = mod[a];
    for (let k = fOff[a]; k < fOff[a + 1]; k++) {
      const c = fList[k];
      for (let q = cOff[c]; q < cOff[c + 1]; q++) {
        const b = cList[q];
        if (b <= a || mod[b] === ma) continue;
        if (cnt[b]++ === 0) touched[nt++] = b;
      }
    }
    for (let t = 0; t < nt; t++) {
      const b = touched[t], n = cnt[b];
      cnt[b] = 0;
      if (n < 4) continue;
      const sa = g.sym(a), sb = g.sym(b), m1 = g.modName(ma), m2 = g.modName(mod[b]);
      if (!likeMatch(m1, p.mod) && !likeMatch(m2, p.mod)) continue;
      rows.push([S(sa.Name), S(sb.Name), I(n), S(m1), S(m2)]);
    }
  }
  sort(rows, (a, b) => c3(b[2].i, a[2].i) || c3(a[0].s, b[0].s) || c3(a[1].s, b[1].s));
  return [["f1", "f2", "shared", "mod1", "mod2"], limit(rows, p)];
};

D.dynInstability = (g, p) => {
  const n = g.Modules.length + 1, ce = new Int32Array(n), ca = new Int32Array(n);
  for (const [a, b] of modEdges(g)) { ce[a]++; ca[b]++; }
  const rows = [];
  for (let m = 1; m < n; m++) {
    const name = g.modName(m);
    if (!likeMatch(name, p.mod)) continue;
    let inst = 0, dist = 0;
    const tot = ca[m] + ce[m];
    if (tot > 0) { const pc = (100.0 * ce[m]) / tot; inst = Math.round(pc); dist = Math.round(Math.abs(pc - 100.0)); }
    rows.push([S(name), I(ca[m]), I(ce[m]), I(inst), I(dist)]);
  }
  sort(rows, (a, b) => c3(a[4].i, b[4].i) || c3(a[3].i, b[3].i) || c3(a[0].s, b[0].s));
  return [["name", "afferent", "efferent", "instability_pct", "distance_pct"], limit(rows, p)];
};

D.dynMarkerClusters = (g, p) => {
  const by = new Map();
  for (const mk of g.Markers) {
    if (mk.FileID < 1 || mk.FileID > g.Files.length) continue;
    const mod = g.Files[mk.FileID - 1].ModuleID;
    if (!g.modOK(mod, p)) continue;
    let a = by.get(mod);
    if (!a) by.set(mod, (a = { all: 0, todo: 0, fixme: 0, hack: 0, files: new Set() }));
    a.all++;
    if (mk.Kind === "TODO") a.todo++; else if (mk.Kind === "FIXME") a.fixme++; else if (mk.Kind === "HACK") a.hack++;
    a.files.add(mk.FileID);
  }
  const rows = [];
  for (const [mod, a] of by) rows.push([S(g.modName(mod) || "(no module)"), I(a.all), I(a.todo), I(a.fixme), I(a.hack), I(a.files.size)]);
  sort(rows, (a, b) => c3(b[1].i, a[1].i) || c3(a[0].s, b[0].s));
  return [["module", "markers", "todo", "fixme", "hack", "files"], limit(rows, p)];
};

function pairRows(g, p, take, give, skipRet, withRet) {
  const t = api(g), rows = [];
  for (const s of dynFns(g, p)) {
    const tk = apiSum(t, s.ID, take);
    if (tk === 0) continue;
    const f = apiSum(t, s.ID, give);
    if (f >= tk || (skipRet !== "" && s.ReturnType.includes(skipRet))) continue;
    rows.push(withRet ? [S(s.Name), I(tk), I(f), I(s.NReturns), S(s.ReturnType), S(g.at(s))] : [S(s.Name), I(tk), I(f), I(s.NReturns), S(g.at(s))]);
  }
  return rows;
}
const gapThenReturns = (a, b) => c3(b[1].i - b[2].i, a[1].i - a[2].i) || c3(b[3].i, a[3].i);
D.dynCStringLeak = (g, p) => {
  const rows = pairRows(g, p, oneOf("JS_ToCString", "JS_ToCStringLen"), oneOf("JS_FreeCString"), "const char", true);
  sort(rows, gapThenReturns);
  return [["name", "to_cstring", "free_cstring", "n_returns", "return_type", "at"], limit(rows, p)];
};
D.dynAtomLeak = (g, p) => {
  const rows = pairRows(g, p, prefix("JS_NewAtom"), oneOf("JS_FreeAtom"), "JSAtom", true);
  sort(rows, gapThenReturns);
  return [["name", "new_atom", "free_atom", "n_returns", "return_type", "at"], limit(rows, p)];
};
D.dynPropEnumLeak = (g, p) => {
  const rows = pairRows(g, p, oneOf("JS_GetPropertyNames", "JS_GetOwnPropertyNames", "JS_GetPropertyEnum"), oneOf("JS_FreePropertyEnum"), "", false);
  sort(rows, dynDesc(1, 3));
  return [["name", "get_props", "free_props", "n_returns", "at"], limit(rows, p)];
};
function unguardedRows(g, p, call, guard) {
  const t = api(g), rows = [];
  for (const s of dynFns(g, p)) {
    const n = apiSum(t, s.ID, call);
    if (n === 0 || apiSum(t, s.ID, guard) !== 0) continue;
    rows.push([S(s.Name), I(n), I(s.NReturns), S(g.at(s))]);
  }
  sort(rows, dynDesc(1, 2));
  return rows;
}
D.dynUncheckedJSCall = (g, p) => [["name", "js_calls", "n_returns", "at"],
  limit(unguardedRows(g, p, oneOf("JS_Call", "JS_CallConstructor", "JS_Eval", "JS_EvalThis"), oneOf("JS_IsException", "JS_GetException")), p)];
D.dynOpaqueUnvalidated = (g, p) => [["name", "get_opaque_calls", "n_returns", "at"],
  limit(unguardedRows(g, p, oneOf("JS_GetOpaque"), prefix("JS_Throw")), p)];

D.dynDivergentClones = (g, p) => {
  const rows = [];
  for (const grp of cloneGroups(g)) {
    for (let x = 0; x < grp.length; x++) {
      const a = grp[x];
      if (!g.modOK(a.ModuleID, p)) continue;
      for (let y = x + 1; y < grp.length; y++) {
        const b = grp[y];
        if (a.FileID !== b.FileID) rows.push([S(a.Name), S(b.Name), I(a.Sloc), I(a.Cyclomatic), I(a.NParams), S(g.at(a)), S(g.at(b))]);
      }
    }
  }
  sort(rows, (a, b) => dynDesc(2, 3)(a, b) || c3(a[5].s, b[5].s) || c3(a[6].s, b[6].s));
  return [["f1", "f2", "sloc", "cyclo", "params", "at1", "at2"], limit(rows, p)];
};

D.dynDeepNesting = (g, p) => {
  const rows = [];
  for (const s of dynFns(g, p)) {
    if (s.MaxNesting < 5 && s.NBranches < 20) continue;
    rows.push([S(s.Name), I(s.MaxNesting), I(s.NBranches), I(s.Cyclomatic), I(s.Sloc), S(g.at(s))]);
  }
  sort(rows, dynDesc(1, 3));
  return [["name", "nesting", "branches", "cyclo", "sloc", "at"], limit(rows, p)];
};

D.dynStackPressure = (g, p) => {
  const rows = [];
  for (const s of dynFns(g, p)) {
    if (s.NLocals < 15 || s.NSizeof < 4) continue;
    rows.push([S(s.Name), I(s.NLocals), I(s.NSizeof), I(s.MaxNesting), I(s.Sloc), S(g.at(s))]);
  }
  sort(rows, dynDesc(1, 2));
  return [["name", "locals", "sizeofs", "nesting", "sloc", "at"], limit(rows, p)];
};

D.dynFrontierModules = (g, p) => {
  const t = dynTainted(g), by = new Map();
  for (const s of g.Symbols) {
    if (!t[s.ID] || s.ModuleID <= 0 || !g.modOK(s.ModuleID, p)) continue;
    let a = by.get(s.ModuleID);
    if (!a) by.set(s.ModuleID, (a = { fns: 0, io: 0, mem: 0, alloc: 0, fnptr: 0, unres: 0 }));
    a.fns++; a.io += s.NIO; a.mem += s.NMemory; a.alloc += s.NAlloc; a.fnptr += s.NFnptrCalls; a.unres += s.NUnresolvedCalls;
  }
  const rows = [];
  for (const [mod, a] of by) rows.push([S(g.modName(mod)), I(a.fns), I(a.io), I(a.mem), I(a.alloc), I(a.fnptr), I(a.unres)]);
  sort(rows, (a, b) => dynDesc(1, 3)(a, b) || c3(a[0].s, b[0].s));
  return [["module", "frontier_fns", "io", "mem", "alloc", "fnptr_calls", "unresolved"], limit(rows, p)];
};

D.dynNativeAPIDensity = (g, p) => {
  const t = api(g), by = new Map(), any = () => true;
  for (const s of dynFns(g, p)) {
    if (s.ModuleID <= 0) continue;
    let a = by.get(s.ModuleID);
    if (!a) by.set(s.ModuleID, (a = { fns: 0, exported: 0, fnptr: 0, js: 0 }));
    a.fns++;
    if (s.IsStatic === 0) a.exported++;
    a.fnptr += s.NFnptrCalls;
    a.js += apiSum(t, s.ID, any);
  }
  const rows = [];
  for (const [mod, a] of by) rows.push([S(g.modName(mod)), I(a.fns), I(a.exported), I(a.fnptr), I(a.js)]);
  sort(rows, (a, b) => dynDesc(4)(a, b) || c3(a[0].s, b[0].s));
  return [["module", "fns", "exported", "fnptr_calls", "js_api_calls"], limit(rows, p)];
};

D.dynRefactoringHotspots = (g, p) => {
  const by = new Map();
  for (const s of dynFns(g, p)) {
    let a = by.get(s.ModuleID);
    if (!a) by.set(s.ModuleID, (a = { fns: 0, god: 0, deep: 0, cyclo: 0 }));
    a.fns++;
    if (s.Sloc >= 200 && s.Cyclomatic >= 20) a.god++;
    if (s.MaxNesting >= 5) a.deep++;
    a.cyclo += s.Cyclomatic;
  }
  const rows = [];
  for (const [mod, a] of by) {
    rows.push([S(g.modName(mod) || "(root)"), I(a.fns), I(a.god), I(a.deep), I(a.cyclo), F(goRound((10 * a.cyclo) / a.fns) / 10)]);
  }
  sort(rows, (a, b) => dynDesc(2, 4)(a, b) || c3(a[0].s, b[0].s));
  return [["module", "fns", "god_fns", "deep_nesting", "total_cyclo", "avg_cyclo"], limit(rows, p)];
};

D.dynCloneDensity = (g, p) => {
  const by = new Map();
  for (const grp of cloneGroups(g)) {
    for (let x = 0; x < grp.length; x++) {
      const a = grp[x];
      if (!g.modOK(a.ModuleID, p)) continue;
      for (let y = x + 1; y < grp.length; y++) {
        if (a.FileID === grp[y].FileID) continue;
        let v = by.get(a.ModuleID);
        if (!v) by.set(a.ModuleID, (v = { pairs: 0, fns: new Set() }));
        v.pairs++;
        v.fns.add(a.ID);
      }
    }
  }
  const rows = [];
  for (const [mod, a] of by) rows.push([S(g.modName(mod) || "(root)"), I(a.pairs), I(a.fns.size)]);
  sort(rows, (a, b) => dynDesc(1)(a, b) || c3(a[0].s, b[0].s));
  return [["module", "clone_pairs", "cloned_fns"], limit(rows, p)];
};

export const goRound = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
export { NUL };
