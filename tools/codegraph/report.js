// --report and --selfcheck.
import { render } from "./render.js";
import { Float } from "./dumpfmt.js";
import { trimSpace } from "./text.js";

const BAR = "=".repeat(78), DASH = "-".repeat(78);
const padR = (s, n) => (s.length >= n ? s : s + " ".repeat(n - s.length));
const byteCmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const isFnKind = (k) => k === "function" || k === "method" || k === "constructor" || k === "closure";

export function symAt(g, s) {
  const f = g.files[s.FileID - 1];
  return f ? f.path + ":" + s.LineStart : "";
}

function round2(f) { return Math.trunc(f * 100 + (f < 0 ? -0.5 : 0.5)) / 100; }

export function report(out, g) {
  const w = (s) => out.push(s);
  w(`\n${BAR}\nOVERVIEW\n${DASH}\n`);
  for (const k of ["lang", "target", "parser", "root"]) {
    const v = g.meta.get(k);
    if (v) w(` ${padR(k, 14)} ${v}\n`);
  }
  let parsed = 0, sloc = 0;
  for (const f of g.files) if (f.parsed === 1) { parsed++; sloc += f.sloc; }
  w(` ${padR("files", 14)} ${g.files.length} catalogued, ${parsed} parsed, ${sloc} sloc\n`);
  const kindN = new Map();
  for (const s of g.symbols) kindN.set(s.Kind, (kindN.get(s.Kind) || 0) + 1);
  const ks = [...kindN].sort((a, b) => b[1] - a[1] || byteCmp(a[0], b[0])).slice(0, 12);
  w(` ${padR("symbols", 14)} ${ks.map(([k, v]) => k + "=" + v).join(", ")}\n`);
  let unres = 0;
  for (const u of g.unres) unres += u.N;
  w(` ${padR("call graph", 14)} ${g.edges.length} edges, ${g.callsites.length} call sites, ${unres} unresolved\n`);
  w(`\n${BAR}\nHOW MUCH OF THIS TO TRUST\n${DASH}\n`);
  let errFiles = 0, errTotal = 0;
  for (const f of g.files) if (f.nParseErrors > 0) { errFiles++; errTotal += f.nParseErrors; }
  let direct = 0, viaMacro = 0, external = 0, viaPtr = 0;
  for (const s of g.symbols) { viaMacro += s.NMacroCalls; external += s.NExternalCalls; viaPtr += s.NFnptrCalls; }
  for (const ed of g.edges) direct += ed.NCalls;
  const named = direct + viaMacro + external + unres;
  const pctOf = (n, d) => (d === 0 ? "-" : Math.trunc((Math.trunc((n * 1000) / d) + 5) / 10) + "%");
  if (parsed === 0) w(" NOTHING WAS PARSED. Every number below is zero because no file\n was read, not because this repository is empty or clean.\n");
  w(` ${padR("files with parse diagnostics", 30)} ${errFiles} of ${parsed} parsed file(s), ${errTotal} diagnostic(s)\n`);
  if (errFiles > 0) w("   functions near a diagnostic may be missing or merged: --parse-errors\n");
  if (named + viaPtr === 0) w(` ${padR("calls", 30)} none recorded -- this is the absence of data, not a clean result\n`);
  else {
    w(` ${padR("call sites that name a callee", 30)} ${named}\n`);
    w(`   ${padR("followed to a definition", 28)} ${padR(String(direct), 8)} ${pctOf(direct, named)}\n`);
    w(`   ${padR("external by design", 28)} ${padR(String(external), 8)} ${pctOf(external, named)}  (libc and other code outside this tree)\n`);
    w(`   ${padR("macro, not a function", 28)} ${padR(String(viaMacro), 8)} ${pctOf(viaMacro, named)}  (a macro the preprocessor left unexpanded)\n`);
    w(`   ${padR("NOT followed", 28)} ${padR(String(unres), 8)} ${pctOf(unres, named)}  <- the blind share\n`);
    w(` ${padR("calls through a pointer", 30)} ${viaPtr}  (no callee name at all; never in the call graph)\n`);
    w(" Fan-in, reachability and dead-code are computed from the first line only.\n");
    w(" `--metrics graph-blindspots` breaks the blind share down per module, and\n");
    w(" the `calls_resolved` row of the meta table says how much of it is names\n");
    w(" defined more than once, where neither linkage nor a shared prototype\n");
    w(" picks the target.\n");
  }
  w(`\n${BAR}\nBIGGEST MODULES\n${DASH}\n`);
  const mods = g.modules.filter((m) => (m.nFiles || 0) > 0).sort((a, b) => (b.sloc || 0) - (a.sloc || 0) || byteCmp(a.name, b.name)).slice(0, 12);
  render(out, mods.map((m) => [m.name, m.nFiles, m.sloc, m.nSymbols || 0, new Float(round2(m.instability || 0))]), ["name", "files", "sloc", "syms", "instab"]);
  w(`\n${BAR}\nHEAVIEST FUNCTIONS\n${DASH}\n`);
  const fns = g.symbols.filter((s) => isFnKind(s.Kind));
  const heavy = [...fns].sort((a, b) => b.Cyclomatic - a.Cyclomatic).slice(0, 12);
  render(out, heavy.map((s) => [s.Name, s.Sloc, s.Cyclomatic, s.Cognitive, s.MaxNesting, s.FanIn, symAt(g, s)]),
    ["name", "sloc", "cyclo", "cog", "nest", "fan_in", "at"]);
  w(`\n${BAR}\nMOST DEPENDED ON\n${DASH}\n`);
  const dep = [...fns].sort((a, b) => b.FanIn - a.FanIn).slice(0, 12);
  render(out, dep.map((s) => [s.Name, s.FanIn, s.FanOut, s.Cyclomatic, s.Sloc, symAt(g, s)]), ["name", "fan_in", "fan_out", "cyclo", "sloc", "at"]);
  w(`\n${BAR}\nMARKERS LEFT IN THE CODE\n${DASH}\n`);
  const mk = new Map();
  for (const m of g.markers) mk.set(m.Kind, (mk.get(m.Kind) || 0) + 1);
  render(out, [...mk].sort((a, b) => b[1] - a[1] || byteCmp(a[0], b[0])).map(([k, v]) => [k, v]), ["kind", "n"]);
}

function shapeCounts(g, pat) {
  const m = new Map();
  for (const h of g.hazards) if (h.Pattern === pat && h.Category === "shape") m.set(h.SymbolID, (m.get(h.SymbolID) || 0) + h.N);
  return m;
}

// Q25's rows: functions whose result is discarded somewhere.
export function discardRows(g) {
  const ign = new Map(), who = new Map();
  for (const h of g.hazards) if (h.Category === "discard") { ign.set(h.Pattern, (ign.get(h.Pattern) || 0) + h.N); who.set(h.Pattern, (who.get(h.Pattern) || 0) + 1); }
  const byName = new Map();
  for (const s of g.symbols) { let l = byName.get(s.Name); if (!l) byName.set(s.Name, (l = [])); l.push(s); }
  const rows = [];
  for (const [nm, n] of ign) {
    const ss = byName.get(nm) || [];
    const fnDefs = ss.filter((s) => s.Kind === "function").length;
    for (const s of ss) {
      if (n <= 0 || s.Kind !== "function" || fnDefs !== 1) continue;
      const rt = trimSpace(s.ReturnType);
      if (rt === "" || rt === "void" || rt.endsWith(" void")) continue;
      const f = g.files[s.FileID - 1];
      if (!f || f.isTest !== 0) continue;
      rows.push({ s, n, sites: Math.max(s.NCallsites, n), who: who.get(nm) });
    }
  }
  return rows;
}

export function selfCheck(out, g) {
  const rules = [];
  const add = (name) => { const r = { name, checked: 0, bad: 0, first: "" }; rules.push(r); return r; };
  const fail = (r, ok, where) => { r.checked++; if (!ok) { r.bad++; if (r.first === "") r.first = where(); } };
  const isFn = (s) => !!s && s.Kind === "function";
  const sym = (id) => (id < 1 || id > g.symbols.length ? null : g.symbols[id - 1]);
  const fileOf = (id) => { const s = sym(id); return s && s.FileID >= 1 && s.FileID <= g.files.length ? g.files[s.FileID - 1] : null; };
  const at = (s) => () => s.Name + "  " + (fileOf(s.ID) ? fileOf(s.ID).path + ":" + s.LineStart : "");

  const rLoopDepth = add("a function with loops has loop depth >= 1 and <= its loop count");
  const rInLoop = add("nothing is counted `in a loop` in a function that has no loop");
  const rRet = add("the five return shapes add up to the return count");
  const rCyclo = add("cyclomatic complexity is at least 1 + the number of loops");
  const rCmp = add("null checks and signed/size compares are subsets of all compares");
  const rLocals = add("n_locals equals the function's rows in locals");
  const rParams = add("n_params equals the function's rows in params, `...` aside");
  const rVariadic = add("a function is variadic exactly when it has one `...` row, and that row is last");
  const rLine = add("every symbol starts on a line its file has");
  const cnt = (m, k) => m.get(k) || 0;
  const inc = (m, k, v = 1) => m.set(k, (m.get(k) || 0) + v);
  const nLoc = new Map();
  for (const l of g.locals) inc(nLoc, l.SymbolID);
  const nPar = new Map(), nVar = new Map(), lastPos = new Map();
  for (const p of g.params) {
    if (p.IsVariadic === 1) inc(nVar, p.SymbolID); else inc(nPar, p.SymbolID);
    lastPos.set(p.SymbolID, Math.max(cnt(lastPos, p.SymbolID), p.Pos));
  }
  const varLast = new Set();
  for (const p of g.params) if (p.IsVariadic === 1 && p.Pos === cnt(lastPos, p.SymbolID)) varLast.add(p.SymbolID);
  for (const s of g.symbols) {
    const f = fileOf(s.ID);
    fail(rLine, f !== null && s.LineStart >= 1 && (f.lines === 0 || s.LineStart <= f.lines + 1), at(s));
    if (!isFn(s)) continue;
    fail(rLoopDepth, (s.NLoops === 0 && s.MaxLoopDepth === 0) || (s.NLoops > 0 && s.MaxLoopDepth >= 1 && s.MaxLoopDepth <= s.NLoops), at(s));
    const inLoop = s.CallInLoop + s.AllocInLoop + s.IOInLoop + s.LockInLoop + s.BranchInLoop + s.SwitchInLoop + s.LibmInLoop + s.DivInLoop + s.StrlenInLoop;
    fail(rInLoop, s.NLoops > 0 || inLoop === 0, at(s));
    fail(rRet, s.RetNull + s.RetNeg + s.RetZero + s.RetVal + s.RetVoid === s.NReturns, at(s));
    fail(rCyclo, s.Cyclomatic >= 1 + s.NLoops, at(s));
    fail(rCmp, s.NNullCheck <= s.NCmp && s.NSignCmp <= s.NCmp, at(s));
    fail(rLocals, s.NLocals === cnt(nLoc, s.ID), at(s));
    fail(rParams, s.NParams === cnt(nPar, s.ID), at(s));
    fail(rVariadic, (s.IsVariadic === 0 && cnt(nVar, s.ID) === 0) || (s.IsVariadic === 1 && cnt(nVar, s.ID) === 1 && varLast.has(s.ID)), at(s));
  }

  const rEdgeEnds = add("both ends of every call edge are functions that exist");
  const rEdgeFlags = add("an edge's is_self/same_file/same_module agree with its two ends");
  const rFan = add("fan_in and fan_out equal the distinct callers/callees in edges");
  const rSites = add("an edge has >= 1 call and no more distinct call lines than calls");
  const rEdgeOne = add("no two edges join the same caller and callee");
  const outE = new Map();
  for (const ed of g.edges) { let l = outE.get(ed.CallerID); if (!l) outE.set(ed.CallerID, (l = [])); l.push(ed.CalleeID); }
  for (let id = 1; id <= g.symbols.length; id++) {
    const es = outE.get(id);
    if (!es) continue;
    fail(rEdgeOne, new Set(es).size === es.length, at(g.symbols[id - 1]));
  }
  const fin = new Map(), fout = new Map(), sites = new Map();
  for (const c of g.callsites) inc(sites, c.CallerID * 4294967296 + c.CalleeID);
  for (const ed of g.edges) {
    const a = sym(ed.CallerID), b = sym(ed.CalleeID);
    const where = () => (a ? a.Name : "?") + " -> " + (b ? b.Name : "?");
    fail(rEdgeEnds, isFn(a) && isFn(b), where);
    if (!a || !b) continue;
    fail(rEdgeFlags, (ed.IsSelf === 1) === (a.ID === b.ID) && (ed.SameFile === 1) === (a.FileID === b.FileID) &&
      (ed.SameModule === 1) === (a.ModuleID === b.ModuleID), where);
    fail(rSites, ed.NCalls >= 1 && cnt(sites, a.ID * 4294967296 + b.ID) <= ed.NCalls, where);
    if (ed.IsSelf === 0) { inc(fout, a.ID); inc(fin, b.ID); }
  }
  for (const s of g.symbols) if (isFn(s)) fail(rFan, s.FanIn === cnt(fin, s.ID) && s.FanOut === cnt(fout, s.ID), at(s));
  const rReach = add("transitive callers are at least as many as direct callers");
  for (const r of g.reach) { const s = sym(r.SymbolID); if (isFn(s)) fail(rReach, r.NTransitive >= s.FanIn, at(s)); }

  const rFileLines = add("code + comment + blank lines do not exceed a file's line count");
  const rFileFns = add("a file's n_functions equals its function symbols");
  const rFileDoc = add("doc lines are comment lines: doc_lines <= comment_lines");
  const fileFns = new Map();
  for (const s of g.symbols) if (isFn(s)) inc(fileFns, s.FileID);
  for (const f of g.files) {
    const where = () => f.path;
    fail(rFileLines, f.sloc + f.commentLines + f.blankLines <= f.lines + 1 && f.sloc >= 0, where);
    fail(rFileFns, f.nFunctions === cnt(fileFns, f.id), where);
    fail(rFileDoc, f.docLines >= 0 && f.docLines <= f.commentLines, where);
  }

  const rSize = add("struct: pad < size, size % align == 0, lines64 == ceil(size/64)");
  const rLayFit = add("every laid-out member lies inside its struct");
  const rLayOrder = add("struct members do not overlap; union members all start at 0");
  const rLayPad = add("member sizes + member padding + tail padding == struct size");
  const rPacked = add("packed size == member bytes rounded up to the alignment");
  const size = new Map();
  for (const ss of g.ssize) size.set(ss.SymbolID, ss);
  const packed = shapeCounts(g, "packed_size");
  const lay = new Map();
  for (const l of g.layout) {
    if (l.Depth !== 0 || l.InUnion !== 0 || l.ByteOff < 0) continue;
    const ss = size.get(l.SymbolID), s = sym(l.SymbolID);
    if (!ss || !s || ss.Exact !== 1) continue;
    fail(rLayFit, l.ByteOff + l.ByteSize <= ss.TotalSize, at(s));
    let a = lay.get(l.SymbolID);
    if (!a) lay.set(l.SymbolID, (a = { end: 0, pad: 0, sum: 0, overlap: false }));
    if (s.Kind === "union") { if (l.ByteOff !== 0) a.overlap = true; continue; }
    if (l.ByteOff < a.end) a.overlap = true;
    a.end = (l.ByteOff + l.ByteSize) | 0;
    a.pad = (a.pad + l.PadBefore) | 0;
    a.sum = (a.sum + l.ByteSize) | 0;
  }
  for (const ss of g.ssize) {
    const s = sym(ss.SymbolID);
    if (!s || ss.Exact !== 1) continue;
    fail(rSize, ss.TotalPad >= 0 && (ss.TotalSize === 0 || ss.TotalPad < ss.TotalSize) && ss.MaxAlign >= 1 &&
      ss.TotalSize % ss.MaxAlign === 0 && ss.NLines64 === Math.trunc((ss.TotalSize + 63) / 64), at(s));
    const a = lay.get(ss.SymbolID);
    if (!a) continue;
    fail(rLayOrder, !a.overlap, at(s));
    if (s.Kind === "struct") {
      fail(rLayPad, a.pad + ss.TailPad === ss.TotalPad && a.sum + ss.TotalPad === ss.TotalSize, at(s));
      let want = a.sum;
      const r = want % ss.MaxAlign;
      if (r !== 0) want += ss.MaxAlign - r;
      const pk = packed.has(ss.SymbolID) ? packed.get(ss.SymbolID) : ss.TotalSize;
      fail(rPacked, pk === want, at(s));
    }
  }

  const rNest = add("a named nested struct/union member keeps its fields to itself");
  const byTag = new Map();
  for (const f of g.fields) { let l = byTag.get(f.SymbolID); if (!l) byTag.set(f.SymbolID, (l = [])); l.push(f); }
  const first = new Map();
  const fkey = (file, line, name) => file + "\0" + line + "\0" + name;
  for (const [id, fs] of byTag) {
    fs.sort((a, b) => a.Ordinal - b.Ordinal);
    const t = sym(id);
    if (t && fs[0].Name !== "") { const k = fkey(t.FileID, fs[0].Line, fs[0].Name); let l = first.get(k); if (!l) first.set(k, (l = [])); l.push(id); }
  }
  for (const [id, fs] of byTag) {
    const o = sym(id);
    if (!o) continue;
    let leak = false;
    for (let i = 0; i < fs.length && !leak; i++) {
      for (const tid of first.get(fkey(o.FileID, fs[i].Line, fs[i].Name)) || []) {
        const ts = byTag.get(tid);
        if (tid === id || i + ts.length >= fs.length) continue;
        let same = true;
        for (let k = 0; k < ts.length; k++) {
          if (ts[k].Name !== fs[i + k].Name || ts[k].Line !== fs[i + k].Line || ts[k].Type !== fs[i + k].Type) { same = false; break; }
        }
        const t = sym(tid), m = fs[i + ts.length];
        if (same && m.Name !== "" && (m.Type === t.Kind || m.Type === t.Kind + " " + t.Name)) { leak = true; break; }
      }
    }
    fail(rNest, !leak, at(o));
  }

  const rHaz = add("every hazard row counts >= 1 occurrence in a symbol that exists");
  for (const h of g.hazards) fail(rHaz, h.N >= 1 && sym(h.SymbolID) !== null, () => h.Pattern + " on symbol " + h.SymbolID);
  const rFrame = add("a function's largest local is no larger than its whole frame");
  const frame = shapeCounts(g, "frame_bytes"), big = shapeCounts(g, "frame_max_local");
  for (const [id, b] of big) { const s = sym(id); if (s) fail(rFrame, b <= (frame.get(id) || 0), at(s)); }
  const rDisc = add("a result is not ignored at more call sites than there are calls");
  for (const r of discardRows(g)) fail(rDisc, r.n <= r.sites, () => r.s.Name);

  out.push(`\n${BAR}\nSELF-CHECK\n${DASH}\n`);
  out.push(" Each rule ties two numbers computed by different code. A violation is a\n bug in this tool, never a property of the code it read.\n\n");
  let nBad = 0;
  const rows = rules.map((r) => {
    let v = r.checked === 0 ? "n/a" : "ok";
    if (r.bad > 0) { v = "VIOLATED"; nBad++; }
    return [v, r.bad, r.checked, r.name, r.first];
  });
  render(out, rows, ["verdict", "bad", "checked", "rule", "first violation"]);
  out.push(`\n ${nBad} of ${rules.length} rules violated\n`);
  return nBad;
}
