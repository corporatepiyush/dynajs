// Defect queries Q1-Q55.
import { I, S, NUL, optS, limit, orderBy, cmpI, dI, cmpS, dSum, dWeighted, dLenStr, cmpLine, pct, groupConcat, distinct,
  asciiLower, trimSpace, inc, addUniq } from "./qcore.js";
import { jtrunc } from "./text.js";

const convFns = ["atoi", "atol", "atoll", "atof", "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof", "strtoimax", "strtoumax"];
const strcpyFns = ["sprintf", "strcpy", "strcat", "gets", "vsprintf", "scanf", "sscanf", "fscanf", "vfscanf", "vsscanf"];
const printfFns = ["printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "syslog"];
const sigUnsafe = ["malloc", "calloc", "realloc", "free", "printf", "fprintf", "sprintf", "snprintf", "syslog", "fopen", "fclose", "fread",
  "fwrite", "strdup"];
export const riskyApis = ["system", "popen", "execve", "execl", "execlp", "execvp", "execv", "fork", "posix_spawn", "posix_spawnp", "vfork",
  "wordexp", "dlopen", "dlmopen", "mktemp", "tmpnam", "tempnam", "access"];
export const libcAllocNames = new Set(["malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign", "memalign", "valloc",
  "reallocarray", "strdup", "strndup"]);

function symRows(g, p, pred, row, notTest = true) {
  const rows = [];
  for (const s of g.syms()) {
    if (!pred(s)) continue;
    if (notTest && !g.notTest(s)) continue;
    if (!g.modOK(s.ModuleID, p)) continue;
    rows.push(row(s));
  }
  return rows;
}
const ret = (cols, rows, p, ...keys) => { orderBy(rows, ...keys); return [cols, limit(rows, p)]; };
const isFn = (s) => s.Kind === "function";

export const Q = {};
Q.qQ1 = (g, p) => ret(["name", "io", "mem", "int_", "alloc", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => !(s.NIO <= 0 || (s.NMemory <= 0 && s.NInteger <= 0)),
    (s) => [S(s.Name), I(s.NIO), I(s.NMemory), I(s.NInteger), I(s.NAlloc), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], false),
  p, dSum(2, 3), dI(1));

Q.qQ2 = (g, p) => {
  const frame = g.shapeDense("frame_bytes"), uns = g.shapeDense("frame_unsized");
  return ret(["name", "frame_bytes", "levels_per_mb", "unsized", "cyclo", "nest", "fan_in", "sloc", "at"],
    symRows(g, p, (s) => s.IsRecursive === 1 && isFn(s), (s) => {
      const per = frame[s.ID] + 48;
      return [S(s.Name), I(frame[s.ID]), I(Math.trunc((1 << 20) / per)), I(uns[s.ID]), I(s.Cyclomatic), I(s.MaxNesting), I(s.FanIn), I(s.Sloc), S(g.at(s))];
    }, false), p, dI(1), dI(4), cmpS(0));
};

Q.qQ3 = (g, p) => ret(["name", "allocs", "frees", "fan_in", "returns_", "gotos", "labels", "at"],
  symRows(g, p, (s) => s.NAlloc > 0 && s.NFree === 0,
    (s) => [S(s.Name), I(s.NAlloc), I(s.NFree), I(s.FanIn), I(s.NReturns), I(s.NGotos), I(s.NLabels), S(g.at(s))], false),
  p, dI(1), dI(3));

Q.qQ4 = (g, p) => {
  const byFile = new Map(), order = [];
  for (const hi of g.hazardsByCatN("alloc")) {
    const h = g.Hazards[hi], sid = h.SymbolID;
    if (sid < 1 || sid > g.Symbols.length) continue;
    const fid = g.Symbols[sid - 1].FileID;
    let a = byFile.get(fid);
    if (!a) { byFile.set(fid, (a = { libc: 0, project: 0, pats: [] })); order.push(fid); }
    if (libcAllocNames.has(h.Pattern)) a.libc += h.N; else a.project += h.N;
    if (!a.pats.includes(h.Pattern)) a.pats.push(h.Pattern);
  }
  const rows = [];
  for (const fid of order) {
    const a = byFile.get(fid);
    if (a.libc <= 0 || a.project <= 0 || fid > g.Files.length) continue;
    const f = g.Files[fid - 1];
    if (!g.modOK(f.ModuleID, p)) continue;
    rows.push([S(f.Path), I(a.libc), I(a.project), I(a.pats.length), S(groupConcat(a.pats))]);
  }
  return ret(["path", "libc", "project", "distinct_fns", "via"], rows, p, dSum(1, 2));
};

Q.qQ5 = (g, p) => ret(["name", "allocs", "depth", "frees", "calls", "fan_in", "at"],
  symRows(g, p, (s) => s.AllocInLoop > 0,
    (s) => [S(s.Name), I(s.AllocInLoop), I(s.MaxLoopDepth), I(s.NFree), I(s.CallInLoop), I(s.FanIn), S(g.at(s))], false),
  p, dI(1), dI(2));

Q.qQ6 = (g, p) => ret(["name", "allocs", "frees", "loops", "brs", "sloc", "fan_in", "at"],
  symRows(g, p, (s) => isFn(s) && s.NAlloc > 0 && s.NFree > 0 && s.NLoops > 0 && s.Sloc <= 120,
    (s) => [S(s.Name), I(s.NAlloc), I(s.NFree), I(s.NLoops), I(s.NBranches), I(s.Sloc), I(s.FanIn), S(g.at(s))], false),
  p, dWeighted([[3, 3], [1, 4], [2, 1]]), cmpI(5), cmpS(0));

Q.qQ7 = (g, p) => {
  const modConc = g.perMod();
  for (const s of g.Symbols) if (s.NConcurrency > 0 && s.ModuleID >= 0 && s.ModuleID < modConc.length) modConc[s.ModuleID]++;
  const rows = [];
  for (const gl of g.Globals) {
    if (gl.IsConst !== 0 || gl.IsAtomic !== 0 || gl.IsVolatile !== 0) continue;
    if (gl.FileID < 1 || gl.FileID > g.Files.length) continue;
    const f = g.Files[gl.FileID - 1];
    if (f.IsTest !== 0 || !g.modOK(gl.ModuleID, p)) continue;
    rows.push([S(gl.Name), S(gl.Type), I(gl.PtrDepth), I(gl.IsArray), I(gl.IsStatic), I(gl.HasInit), S(g.modName(gl.ModuleID)),
      S(f.Path + ":" + gl.Line), I(modConc[gl.ModuleID] || 0)]);
  }
  return ret(["name_", "type_", "ptr", "arr", "stat", "init", "module", "at", "mod_conc_fns"], rows, p, dI(8), dI(2), cmpS(0));
};

Q.qQ8 = (g, p) => ret(["name", "sw", "depth", "calls", "cases", "sloc", "fan_in", "at"],
  symRows(g, p, (s) => s.SwitchInLoop > 0,
    (s) => [S(s.Name), I(s.SwitchInLoop), I(s.MaxLoopDepth), I(s.CallInLoop), I(s.NCases), I(s.Sloc), I(s.FanIn), S(g.at(s))], false),
  p, dI(1), dI(3));

Q.qQ9 = (g, p) => ret(["name", "strlens", "depth", "loops", "fan_in", "at"],
  symRows(g, p, (s) => s.StrlenInLoop > 0,
    (s) => [S(s.Name), I(s.StrlenInLoop), I(s.MaxLoopDepth), I(s.NLoops), I(s.FanIn), S(g.at(s))], false),
  p, dI(2), dI(1));

Q.qQ10 = (g, p) => ret(["name", "r_null", "r_neg", "r_0", "r_val", "r_void", "ret_type", "fan_in", "at"],
  symRows(g, p, (s) => isFn(s) && (s.RetNull > 0) + (s.RetNeg > 0) + (s.RetZero > 0) >= 2,
    (s) => [S(s.Name), I(s.RetNull), I(s.RetNeg), I(s.RetZero), I(s.RetVal), I(s.RetVoid), optS(s.ReturnType, s.HasReturnType), I(s.FanIn),
      S(g.at(s))], false),
  p, dI(7), cmpS(0));

Q.qQ11 = (g, p) => {
  const named = g.namedAsValue(), rows = [];
  for (const s of g.syms()) {
    if (s.FanIn !== 0 || s.IsPublic !== 0 || s.IsTest !== 0 || s.IsEntrypoint !== 0 || s.IsOverride !== 0 || s.IsAbstract !== 0) continue;
    if (named(s)) continue;
    if (s.Kind !== "function" && s.Kind !== "method" && s.Kind !== "closure") continue;
    if (s.Name === "(anonymous)" || s.Name === "<module>") continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0 || f.IsGenerated !== 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), S(s.Kind), I(s.Sloc), I(s.Cyclomatic), I(s.NExternalCalls), S(g.at(s))]);
  }
  return ret(["name", "kind", "sloc", "cyclo", "ext_calls", "at"], rows, p, dI(2));
};

Q.qQ12 = (g, p) => {
  const bySym = new Map();
  for (const i of g.hazCats("reentrancy")) { const h = g.Hazards[i]; addUniq(bySym, h.SymbolID, h.Pattern); }
  return ret(["name", "module", "n_patterns", "patterns", "reentrancy_calls", "concurrency_calls", "atomics", "fan_in", "cyclo", "at"],
    symRows(g, p, (s) => (bySym.get(s.ID) || []).length > 0 && s.NConcurrency > 0, (s) => {
      const pats = bySym.get(s.ID);
      return [S(s.Name), S(g.modName(s.ModuleID)), I(pats.length), S(groupConcat(pats)), I(s.NReentrancy), I(s.NConcurrency), I(s.NAtomic),
        I(s.FanIn), I(s.Cyclomatic), S(g.at(s))];
    }), p, dI(5), dI(2), dI(7));
};

Q.qQ13 = (g, p) => {
  const bySym = new Map(), total = new Map();
  for (const h of g.hazardsByPattern(convFns)) { addUniq(bySym, h.SymbolID, h.Pattern); inc(total, h.SymbolID, h.N); }
  return ret(["name", "module", "n_conversions", "conversions", "conversion_calls", "io_calls", "stdio_calls", "memory_calls", "returns_value",
    "fan_in", "at"],
  symRows(g, p, (s) => bySym.has(s.ID) && !(s.NIO <= 0 && s.NStdio <= 0), (s) => [S(s.Name), S(g.modName(s.ModuleID)),
    I(bySym.get(s.ID).length), S(groupConcat(bySym.get(s.ID))), I(total.get(s.ID) || 0), I(s.NIO), I(s.NStdio), I(s.NMemory), I(s.RetVal),
    I(s.FanIn), S(g.at(s))]), p, dI(4), dI(5), dI(9));
};

Q.qQ14 = (g, p) => {
  const [list, total] = g.hazardsBy("", strcpyFns);
  return ret(["name", "n_patterns", "patterns", "dangerous_calls", "memory_calls", "io_calls", "fan_in", "cyclo", "at"],
    symRows(g, p, (s) => list.has(s.ID), (s) => [S(s.Name), I(list.get(s.ID).length), S(groupConcat(list.get(s.ID))),
      I(total.get(s.ID) || 0), I(s.NMemory), I(s.NIO), I(s.FanIn), I(s.Cyclomatic), S(g.at(s))]), p, dI(3), dI(6));
};

Q.qQ15 = (g, p) => {
  const [list, total] = g.hazardsBy("", printfFns);
  for (const v of list.values()) v.sort();
  return ret(["name", "n_patterns", "patterns", "format_calls", "io_calls", "stdio_calls", "fan_in", "cyclo", "at"],
    symRows(g, p, (s) => list.has(s.ID), (s) => [S(s.Name), I(list.get(s.ID).length), S(groupConcat(list.get(s.ID))),
      I(total.get(s.ID) || 0), I(s.NIO), I(s.NStdio), I(s.FanIn), I(s.Cyclomatic), S(g.at(s))]), p, dI(3), dI(6));
};

Q.qQ16 = (g, p) => {
  const rows = [];
  for (const s of g.symsName()) {
    if (s.NAlloc <= 0 || s.NFree !== 0 || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(s.NAlloc), I(s.NFree), I(s.NAlloc - s.NFree), I(s.NMemory), I(s.FanIn), I(s.Cyclomatic), I(s.NReturns), S(g.at(s))]);
  }
  return ret(["name", "allocs", "frees", "imbalance", "memory_ops", "fan_in", "cyclo", "returns", "at"], rows, p, dI(1), dI(5));
};

Q.qQ17 = (g, p) => ret(["name", "allocs", "frees", "excess_frees", "return_paths", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => !(s.NFree <= s.NAlloc || s.NFree <= 1), (s) => [S(s.Name), I(s.NAlloc), I(s.NFree), I(s.NFree - s.NAlloc),
    I(s.NReturns), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]), p, dI(3), dI(6));

Q.qQ18 = (g, p) => ret(["name", "memory_ops", "null_checks", "allocs", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => s.NMemory > 5 && s.NNullCheck === 0, (s) => [S(s.Name), I(s.NMemory), I(s.NNullCheck), I(s.NAlloc), I(s.Cyclomatic),
    I(s.FanIn), S(g.at(s))]), p, dI(1), dI(5));

Q.qQ19 = (g, p) => ret(["name", "arith_ops", "integer_ops", "comparisons", "loops", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => s.NArith > 20, (s) => [S(s.Name), I(s.NArith), I(s.NInteger), I(s.NCmp), I(s.NLoops), I(s.Cyclomatic), I(s.FanIn),
    S(g.at(s))]), p, dI(1), dI(6));

Q.qQ20 = (g, p) => {
  const ung = g.shapeDense("div_unguarded"), all = g.shapeDense("div_by_var");
  return ret(["name", "unguarded_divs", "var_divs", "div_in_loop", "n_params", "cyclo", "fan_in", "at"],
    symRows(g, p, (s) => ung[s.ID] > 0, (s) => [S(s.Name), I(ung[s.ID]), I(all[s.ID]), I(s.DivInLoop), I(s.NParams), I(s.Cyclomatic),
      I(s.FanIn), S(g.at(s))]), p, dI(1), dI(6), cmpS(0));
};

Q.qQ21 = (g, p) => ret(["name", "switches", "cases", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => s.NSwitch > 0, (s) => [S(s.Name), I(s.NSwitch), I(s.NCases), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]), p, dI(1), dI(4));

Q.qQ22 = (g, p) => ret(["name", "concurrency_ops", "atomic_ops", "reentrancy_calls", "memory_ops", "fan_in", "cyclo", "at"],
  symRows(g, p, (s) => s.NConcurrency > 0, (s) => [S(s.Name), I(s.NConcurrency), I(s.NAtomic), I(s.NReentrancy), I(s.NMemory), I(s.FanIn),
    I(s.Cyclomatic), S(g.at(s))]), p, dI(1), dI(5));

Q.qQ23 = (g, p) => {
  const [list] = g.hazardsBy("", sigUnsafe);
  return ret(["name", "n_unsafe_patterns", "unsafe_patterns", "io_calls", "memory_calls", "fan_in", "at"],
    symRows(g, p, (s) => {
      if (!list.has(s.ID)) return false;
      const low = asciiLower(s.Name);
      return low.includes("sig") || low.includes("handler") || low.includes("signal");
    }, (s) => [S(s.Name), I(list.get(s.ID).length), S(groupConcat(list.get(s.ID))), I(s.NIO), I(s.NMemory), I(s.FanIn), S(g.at(s))]),
    p, dI(1), dI(5));
};

Q.qQ24 = (g, p) => {
  const nox = g.shapeDense("loop_forever_noexit"), all = g.shapeDense("loop_forever");
  return ret(["name", "no_exit_loops", "forever_loops", "returns", "cyclo", "fan_in", "sloc", "at"],
    symRows(g, p, (s) => nox[s.ID] > 0 && isFn(s), (s) => [S(s.Name), I(nox[s.ID]), I(all[s.ID]), I(s.NReturns), I(s.Cyclomatic), I(s.FanIn),
      I(s.Sloc), S(g.at(s))]), p, dI(1), dI(5), cmpS(0));
};

Q.qQ25 = (g, p) => {
  const [ign, who] = g.discards();
  const x = g.six(), cand = [];
  for (const nm of ign.keys()) for (const id of g.ids(nm)) cand.push(g.Symbols[id - 1]);
  cand.sort((a, b) => x.rank[a.ID] - x.rank[b.ID]);
  const rows = [];
  for (const s of cand) {
    const n = ign.get(s.Name) || 0;
    if (n <= 0 || !isFn(s) || x.fnDefs[s.ID] !== 1) continue;
    const rt = trimSpace(s.ReturnType);
    if (rt === "" || rt === "void" || rt.endsWith(" void")) continue;
    if (!g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    const sites = Math.max(s.NCallsites, n);
    rows.push([S(s.Name), I(n), I(sites), I(pct(n, sites)), I(who.get(s.Name) || 0), S(jtrunc(rt, 24)), S(g.at(s))]);
  }
  return ret(["name", "ignored_sites", "call_sites", "pct_ignored", "callers_ignoring", "returns", "at"], rows, p, dI(1), dI(3), cmpS(0));
};

Q.qQ26 = (g, p) => {
  const bySym = g.six().macro;
  return ret(["name", "uses", "params", "body_len", "multiline", "at"],
    symRows(g, p, (s) => { const m = bySym[s.ID]; return !!m && m.IsFunctionlike === 1 && m.NUses > 3; }, (s) => {
      const m = bySym[s.ID];
      return [S(s.Name), I(m.NUses), I(m.NParams), I(m.BodyLen), I(m.IsMultiline), S(g.at(s))];
    }, false), p, dI(1));
};

Q.qQ27 = (g, p) => ret(["name", "fnptr_calls", "dyn_calls", "direct_calls", "fan_in", "sloc", "at"],
  symRows(g, p, (s) => s.NFnptrCalls + s.NDynamicCalls > 0 && isFn(s), (s) => [S(s.Name), I(s.NFnptrCalls), I(s.NDynamicCalls),
    I(Math.max(0, s.NCalls - s.NFnptrCalls - s.NDynamicCalls)), I(s.FanIn), I(s.Sloc), S(g.at(s))]), p, dSum(1, 2), cmpS(0));

Q.qQ28 = (g, p) => {
  const byFile = new Map(), order = [];
  for (const im of g.Imports) {
    if (im.Kind !== "include") continue;
    let a = byFile.get(im.FileID);
    if (!a) { byFile.set(im.FileID, (a = { user: 0, sys: 0 })); order.push(im.FileID); }
    if (im.IsRelative === 1) a.user++; else a.sys++;
  }
  const rows = [];
  for (const fid of order) {
    const f = g.fileOf2(fid);
    if (!f || !g.modOK(f.ModuleID, p)) continue;
    const a = byFile.get(fid);
    rows.push([S(f.Path), I(a.user), I(a.sys), I(pct(a.user, a.user + a.sys)), I(f.Sloc)]);
  }
  return ret(["path", "user_headers", "sys_headers", "pct_user", "sloc"], rows, p, cmpI(3), dSum(1, 2));
};

Q.qQ29 = (g, p) => {
  const out = g.edgeCSR(false), rows = [];
  for (const a of g.Edges) {
    if (a.CallerID >= a.CalleeID) continue;
    let rev = 0;
    for (const j of out.of(a.CalleeID)) if (g.Edges[j].CalleeID === a.CallerID) rev = g.Edges[j].NCalls;
    if (rev === 0) continue;
    const sa = g.sym(a.CallerID), sb = g.sym(a.CalleeID);
    if (!sa || !sb || !g.modOK(sa.ModuleID, p)) continue;
    rows.push([S(sa.Name), S(sb.Name), S(g.at(sa)), S(g.at(sb)), I(a.NCalls), I(rev)]);
  }
  return ret(["fn_a", "fn_b", "at_a", "at_b", "a_calls_b", "b_calls_a"], rows, p, dSum(4, 5));
};

Q.qQ30 = (g, p) => {
  const rows = [];
  for (const gl of g.Globals) {
    if (gl.IsStatic !== 0 || gl.IsConst !== 0 || !g.modOK(gl.ModuleID, p)) continue;
    const f = g.fileOf2(gl.FileID);
    if (!f) continue;
    rows.push([S(f.Path), S(gl.Name), S(gl.Type), I(gl.Line), I(gl.HasInit), I(gl.IsVolatile), I(gl.IsAtomic), I(gl.PtrDepth)]);
  }
  return ret(["path", "name", "type", "line", "has_init", "is_volatile", "is_atomic", "ptr_depth"], rows, p, dI(6), dI(5), cmpS(0), cmpI(3));
};

Q.qQ31 = (g, p) => {
  const nf = g.Files.length + 1, okF = (id) => id >= 0 && id < nf;
  const nFns = new Int32Array(nf), nSyms = new Int32Array(nf), inbound = new Uint8Array(nf);
  for (const s of g.Symbols) { if (!okF(s.FileID)) continue; nSyms[s.FileID]++; if (isFn(s)) nFns[s.FileID]++; }
  for (const e of g.Edges) { const s = g.sym(e.CalleeID); if (s && okF(s.FileID)) inbound[s.FileID] = 1; }
  const pairs = new Set(), includedBy = new Int32Array(nf);
  for (const im of g.Imports) {
    if (im.Kind === "include" && im.HasTargetID && okF(im.TargetID)) {
      const k = im.TargetID * 4294967296 + im.FileID;
      if (!pairs.has(k)) { pairs.add(k); includedBy[im.TargetID]++; }
    }
  }
  const rows = [];
  for (const f of g.Files) {
    if (!okF(f.ID) || includedBy[f.ID] === 0 || f.NSymbols <= 0 || nFns[f.ID] <= 0 || inbound[f.ID]) continue;
    if (!g.modOK(f.ModuleID, p)) continue;
    rows.push([S(f.Path), I(f.Sloc), I(nSyms[f.ID]), I(nFns[f.ID]), I(0), I(includedBy[f.ID])]);
  }
  return ret(["header", "sloc", "n_syms", "n_fns", "inbound_calls", "included_by"], rows, p, dI(5), dI(1));
};

Q.qQ32 = (g, p) => {
  const rows = [];
  for (const r of g.Reach) {
    const s = g.sym(r.SymbolID);
    if (!s || !isFn(s) || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(r.NTransitive), I(s.FanIn), I(s.NCalls), I(s.Sloc), S(g.at(s))]);
  }
  return ret(["name", "transitive_callers", "fan_in", "n_calls", "sloc", "at"], rows, p, dI(1), cmpS(0));
};

Q.qQ33 = (g, p) => {
  const nTop = g.perSym(), nPtr = g.perSym();
  const ok = (id) => id >= 0 && id < nTop.length;
  for (const l of g.Layout) { if (!ok(l.SymbolID)) continue; if (l.Depth === 0) nTop[l.SymbolID]++; if (l.PtrDepth > 0) nPtr[l.SymbolID]++; }
  const rows = [];
  for (const ss of g.SSize) {
    if (ss.Exact !== 1 || ss.TotalSize < 64) continue;
    const s = g.sym(ss.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(ss.TotalSize), I(ss.TailPad), I(ss.TotalPad), I(ss.NLines64), I(ss.Exact), I(ok(ss.SymbolID) ? nTop[ss.SymbolID] : 0),
      I(ok(ss.SymbolID) ? nPtr[ss.SymbolID] : 0), S(g.at(s))]);
  }
  return ret(["name", "total_size", "tail_pad", "total_pad", "n_lines_64", "exact", "n_fields", "n_ptr_fields", "at"], rows, p, dI(1), dI(3));
};

Q.qQ34 = (g, p) => {
  const nf = g.Files.length + 1, okF = (id) => id >= 0 && id < nf;
  const eg = new Int32Array(nf), ec = new Int32Array(nf), tot = new Int32Array(nf), nSym = new Int32Array(nf);
  for (const gl of g.Globals) if (gl.IsStatic === 0 && gl.IsConst === 0 && okF(gl.FileID)) eg[gl.FileID]++;
  for (const s of g.Symbols) {
    if (!okF(s.FileID)) continue;
    nSym[s.FileID]++;
    if (isFn(s)) { ec[s.FileID] += s.NExternalCalls; tot[s.FileID] += s.NCalls; }
  }
  const rows = [];
  for (const f of g.Files) {
    if (!okF(f.ID) || nSym[f.ID] === 0 || (ec[f.ID] === 0 && eg[f.ID] === 0) || !g.modOK(f.ModuleID, p)) continue;
    rows.push([S(f.Path), I(eg[f.ID]), I(ec[f.ID]), I(tot[f.ID]), tot[f.ID] !== 0 ? I(pct(ec[f.ID], tot[f.ID])) : NUL]);
  }
  return ret(["path", "extern_globals", "extern_calls", "total_calls", "pct_external"], rows, p, dI(2));
};

Q.qQ35 = (g, p) => {
  const rows = [];
  let cur = "", open = false, sigs = [], files = [], paths = [];
  const flush = () => { if (open && sigs.length > 1) rows.push([S(cur), I(sigs.length), I(files.length), S(groupConcat(paths))]); };
  for (const s of g.symsName()) {
    if (!isFn(s) || !s.HasSignature || s.Signature === "") continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsGenerated !== 0 || !g.modOK(s.ModuleID, p)) continue;
    if (!open || s.Name !== cur) { flush(); cur = s.Name; open = true; sigs = []; files = []; paths = []; }
    if (!sigs.includes(s.Signature)) sigs.push(s.Signature);
    if (!files.includes(f.ID)) { files.push(f.ID); paths.push(f.Path); }
  }
  flush();
  return ret(["name", "n_sigs", "n_files", "where_defined"], rows, p, dI(2), dI(1), cmpS(0));
};

Q.qQ36 = (g, p) => {
  const n1 = g.Symbols.length + 1, first = new Int32Array(n1), nFiles = new Uint8Array(n1);
  for (const e of g.Edges) {
    const cs = g.sym(e.CallerID);
    if (!cs || e.CalleeID < 0 || e.CalleeID >= n1) continue;
    const id = e.CalleeID;
    if (nFiles[id] === 0) { first[id] = cs.FileID; nFiles[id] = 1; } else if (first[id] !== cs.FileID) nFiles[id] = 2;
  }
  const rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.IsStatic !== 0) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsGenerated !== 0 || !g.modOK(s.ModuleID, p)) continue;
    const n = nFiles[s.ID];
    if (s.FanIn <= 0 || n > 1) continue;
    rows.push([S(s.Name), S(f.Path), I(s.LineStart), I(s.FanIn), I(n)]);
  }
  return ret(["name", "path", "line_start", "fan_in", "n_caller_files"], rows, p, dI(3), cmpS(0));
};

Q.qQ37 = (g, p) => {
  const rows = [];
  for (const i of g.hazRows(riskyApis)) {
    const h = g.Hazards[i], s = g.sym(h.SymbolID);
    if (!s) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsGenerated !== 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(f.Path), S(s.Name), S(h.Pattern), S(h.Category), I(h.N), I(h.FirstLine), I(s.FanIn)]);
  }
  return ret(["path", "caller", "api", "category", "sites", "first_line", "fan_in"], rows, p, dI(6), dI(4));
};

Q.qQ38 = (g, p) => {
  const rows = [];
  for (const sc of g.Secrets) {
    const s = g.sym(sc.SymbolID);
    if (!s) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0 || f.IsGenerated !== 0 || !g.modOK(s.ModuleID, p)) continue;
    const v = sc.Value;
    if (v.startsWith("/") || v.includes("|") || v.includes("%")) continue;
    rows.push([S(s.Name), S(v), I(sc.Line), S(f.Path + ":" + sc.Line)]);
  }
  return ret(["name", "candidate", "line", "at"], rows, p, dLenStr(1));
};

Q.qQ39 = (g, p) => {
  const byPath = new Map(), rows = [];
  for (const f of g.Files) byPath.set(f.Path, f.ID);
  for (const c of g.Cycles) {
    const fid = byPath.get(c.APath);
    if (fid === undefined) { rows.push([S(c.APath), S(c.BPath), I(c.Length), S(c.Members)]); continue; }
    if (!g.modOK(g.Files[fid - 1].ModuleID, p)) continue;
    rows.push([S(c.APath), S(c.BPath), I(c.Length), S(c.Members)]);
  }
  return ret(["header", "partner", "length", "members"], rows, p, cmpI(2), cmpS(0));
};

Q.qQ40 = (g, p) => ret(["name", "const_casts", "casts_total", "fan_in", "at"],
  symRows(g, p, (s) => s.NConstCast > 0, (s) => [S(s.Name), I(s.NConstCast), I(s.NCast), I(s.FanIn), S(g.at(s))]), p, dI(1), dI(3));

Q.qQ41 = (g, p) => {
  const x = g.six(), rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.FanIn !== 0 || s.IsTest !== 0) continue;
    const f = g.fileOf(s.ID);
    if (!f || !g.modOK(s.ModuleID, p)) continue;
    const n = s.IsStatic === 0 ? x.bodyN[s.ID] : x.bodyInFile[s.ID];
    if (n === 0) continue;
    rows.push([S(s.Name), S(f.Path), I(s.LineStart), I(s.Sloc), I(n)]);
  }
  return ret(["name", "path", "line_start", "sloc", "addr_taken"], rows, p, dI(4), dI(3), cmpS(0));
};

Q.qQ42 = (g, p) => {
  const rows = [];
  for (const d of g.Decls) {
    let cnt = 0;
    for (const sid of g.ids(d.Name)) { const sy = g.sym(sid); if (isFn(sy) && (sy.IsStatic === 0 || sy.FileID === d.FileID)) cnt++; }
    if (cnt !== 0) continue;
    const f = g.fileOf2(d.FileID);
    if (!f || f.IsGenerated !== 0 || !g.modOK(f.ModuleID, p)) continue;
    rows.push([S(d.Name), S(f.Path), I(d.Line), I(0)]);
  }
  return ret(["name", "path", "line", "defined_count"], rows, p, cmpS(0), cmpS(1));
};

Q.qQ43 = (g, p) => ret(["name", "pairs", "io_calls", "fan_in", "at"],
  symRows(g, p, (s) => s.NToctou > 0, (s) => [S(s.Name), I(s.NToctou), I(s.NIO), I(s.FanIn), S(g.at(s))]), p, dI(1), dI(3));

Q.qQ44 = (g, p) => ret(["name", "acquires", "releases", "imbalance", "return_paths", "gotos", "fan_in", "cyclo", "at"],
  symRows(g, p, (s) => isFn(s) && s.NLockAcquire > s.NLockRelease, (s) => [S(s.Name), I(s.NLockAcquire), I(s.NLockRelease),
    I(s.NLockAcquire - s.NLockRelease), I(s.NReturns), I(s.NGotos), I(s.FanIn), I(s.Cyclomatic), S(g.at(s))]), p, dI(3), dI(6), cmpS(0));

Q.qQ45 = (g, p) => ret(["name", "acquires", "io_calls", "mem_calls", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => isFn(s) && s.NLockAcquire > 0 && s.NIO > 0, (s) => [S(s.Name), I(s.NLockAcquire), I(s.NIO), I(s.NMemory),
    I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]), p, dI(2), dI(1), cmpS(0));

Q.qQ46 = (g, p) => {
  const want = ["memcpy", "memmove", "strncpy", "strncat", "snprintf"], rows = [];
  for (const m of g.Memops) {
    if (!want.includes(m.Fn) || m.SizeBuf === "" || m.SizeBuf === m.DstTail) continue;
    if (m.Src.includes(m.SizeBuf) || m.SizeArg.includes("*") || m.SizeArg.includes("+")) continue;
    const s = g.sym(m.SymbolID);
    if (!s) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(m.Fn), S(m.Dst), S(m.Src), S(m.SizeArg), S(m.SizeBuf), S(s.Name), I(s.FanIn), S(f.Path + ":" + m.Line)]);
  }
  return ret(["op", "dst", "src", "size_arg", "size_buf", "caller", "fan_in", "at"], rows, p, dI(6), cmpLine(7), cmpS(0));
};

Q.qQ47 = (g, p) => {
  const rows = [];
  for (const a of g.Allocs) {
    if (a.SizeExpr.includes("sizeof") || a.SizeExpr === "") continue;
    const s = g.sym(a.SymbolID);
    if (!s) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(a.Fn), S(a.SizeExpr), S(s.Name), I(s.FanIn), I(s.NAllocsite), S(f.Path + ":" + a.Line)]);
  }
  return ret(["fn", "size_expr", "caller", "fan_in", "alloc_sites", "at"], rows, p, dI(3), cmpLine(5), cmpS(0));
};

Q.qQ48 = (g, p) => {
  const mutable = new Map();
  for (const gl of g.Globals) if (gl.IsConst === 0) inc(mutable, gl.FileID);
  const wr = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.NGlobalWrite <= 0 || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    wr.push({ id: s.ID, mod: s.ModuleID, writes: s.NGlobalWrite, fan: s.FanIn, name: s.Name });
  }
  wr.sort((a, b) => (a.writes !== b.writes ? b.writes - a.writes : a.name !== b.name ? (a.name > b.name ? -1 : 1) : a.id - b.id));
  const rank = new Map(), byMod = new Map();
  for (const x of wr) { let l = byMod.get(x.mod); if (!l) byMod.set(x.mod, (l = [])); l.push(x); }
  for (const m of g.Modules) {
    const part = (byMod.get(m.ID) || []).slice();
    part.sort((a, b) => (a.writes !== b.writes ? b.writes - a.writes : b.fan - a.fan));
    let n = 0, pw = 0, pf = 0, first = true;
    for (const x of part) {
      if (first || x.writes !== pw || x.fan !== pf) { n++; pw = x.writes; pf = x.fan; first = false; }
      rank.set(x.id, n);
    }
  }
  const rows = [];
  for (const x of wr) {
    const s = g.sym(x.id);
    if (!s) continue;
    const f = g.fileOf(s.ID);
    if (!f) continue;
    rows.push([S(s.Name), I(x.writes), S(g.modName(s.ModuleID)), I(mutable.get(f.ID) || 0), I(s.FanIn), I(s.Cyclomatic), I(rank.get(x.id) || 0),
      S(g.at(s))]);
  }
  return ret(["name", "writes", "module", "mutable_globals_here", "fan_in", "cyclo", "rank_in_module", "at"], rows, p, dI(1), dI(4));
};

Q.qQ49 = (g, p) => ret(["name", "narrow_casts", "arith_ops", "shifts", "io_calls", "fan_in", "at"],
  symRows(g, p, (s) => isFn(s) && s.NNarrowCast > 0 && !(s.NArith <= 0 && s.NShift <= 0 && s.NIO <= 0), (s) => [S(s.Name), I(s.NNarrowCast),
    I(s.NArith), I(s.NShift), I(s.NIO), I(s.FanIn), S(g.at(s))]), p, dI(1), dI(5), cmpS(0));

Q.qQ50 = (g, p) => ret(["name", "sign_cmps", "total_cmps", "n_params", "fan_in", "at"],
  symRows(g, p, (s) => isFn(s) && s.NSignCmp > 0, (s) => [S(s.Name), I(s.NSignCmp), I(s.NCmp), I(s.NParams), I(s.FanIn), S(g.at(s))]),
  p, dI(1), dI(4), cmpS(0));

Q.qQ51 = (g, p) => {
  const bySym = new Map();
  for (const i of g.hazRows(["vsnprintf", "vsprintf", "vfprintf", "vprintf", "syslog"])) { const h = g.Hazards[i]; addUniq(bySym, h.SymbolID, h.Pattern); }
  return ret(["name", "fmt_calls", "stdio_calls", "fan_in", "via", "at"],
    symRows(g, p, (s) => isFn(s) && s.IsVariadic === 1 && s.NVariadicFmt > 0, (s) => {
      const l = bySym.get(s.ID);
      return [S(s.Name), I(s.NVariadicFmt), I(s.NStdio), I(s.FanIn), l && l.length ? S(groupConcat(l)) : NUL, S(g.at(s))];
    }), p, dI(3), dI(1), cmpS(0));
};

Q.qQ52 = (g, p) => {
  const x = g.six();
  const vetoed = (s) => (s.IsStatic === 0 ? x.bodyN[s.ID] > 0 : x.bodyInFile[s.ID] > 0);
  const rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.FanIn !== 0 || vetoed(s)) continue;
    let n = 0, plain = 0;
    const names = [];
    for (const r of x.refs.get(s.Name) || []) {
      if (s.IsStatic === 0 || r.file === s.FileID) {
        n++;
        if (r.kind === "ref") plain++;
        const f = g.fileOf2(r.file);
        if (f) names.push(f.Basename);
      }
    }
    if (n === 0 || !g.notTest(s) || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(n), I(plain), S(groupConcat(distinct(names))), I(s.Sloc), I(s.Cyclomatic), S(g.at(s))]);
  }
  return ret(["name", "table_refs", "plain_refs", "tables_in", "sloc", "cyclo", "at"], rows, p, dI(1), dI(4), cmpS(0));
};

Q.qQ53 = (g, p) => ret(["name", "allocs", "frees", "return_paths", "branches", "gotos", "cyclo", "fan_in", "at"],
  symRows(g, p, (s) => isFn(s) && s.NAllocsite > 0 && s.NFree > 0 && s.NReturns >= 3 && s.NReturns > s.NAllocsite, (s) => [S(s.Name),
    I(s.NAllocsite), I(s.NFree), I(s.NReturns), I(s.NBranches), I(s.NGotos), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]), p, dI(3), dI(1), cmpS(0));

Q.qQ54 = (g, p) => ret(["name", "self_realloc", "allocs", "frees", "returns_", "fan_in", "at"],
  symRows(g, p, (s) => s.NReallocSelf > 0, (s) => [S(s.Name), I(s.NReallocSelf), I(s.NAlloc), I(s.NFree), I(s.NReturns), I(s.FanIn),
    S(g.at(s))], false), p, dI(1), dI(5));

Q.qQ55 = (g, p) => {
  const rows = [];
  for (const a of g.Allocs) {
    if (!a.SizeExpr.includes("strlen") || a.SizeExpr.includes("+")) continue;
    const s = g.sym(a.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    const f = g.fileOf(s.ID);
    if (!f) continue;
    rows.push([S(s.Name), S(a.Fn), S(a.SizeExpr), I(s.NAlloc), I(s.FanIn), S(f.Path + ":" + a.Line)]);
  }
  return ret(["name", "fn", "size", "allocs", "fan_in", "at"], rows, p, dI(4), cmpS(0));
};
