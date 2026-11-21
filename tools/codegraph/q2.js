// Defect queries Q56-Q119.
import { I, S, limit, orderBy, cmpI, dI, cmpS, dSum, dWeighted, pct, groupConcat, inc, pushTo } from "./qcore.js";
import { Q } from "./q1.js";

const isFn = (s) => s.Kind === "function";
const ret = (cols, rows, p, ...keys) => { orderBy(rows, ...keys); return [cols, limit(rows, p)]; };
function scan(g, p, list, pred, row, notTest = false) {
  const rows = [];
  for (const s of list) {
    if (!pred(s)) continue;
    if (notTest && !g.notTest(s)) continue;
    if (!g.modOK(s.ModuleID, p)) continue;
    rows.push(row(s));
  }
  return rows;
}

Q.qQ56 = (g, p) => {
  const openFns = ["open", "openat", "creat", "fopen", "fdopen", "opendir", "socket", "accept", "accept4", "mmap", "popen", "tmpfile"];
  const closeFns = ["close", "fclose", "closedir", "munmap", "pclose", "shutdown"];
  const o = new Map(), c = new Map();
  for (const i of g.hazCats("io", "stdio")) {
    const h = g.Hazards[i];
    if (openFns.includes(h.Pattern)) inc(o, h.SymbolID, h.N);
    if (closeFns.includes(h.Pattern)) inc(c, h.SymbolID, h.N);
  }
  const rows = [];
  for (const sid of [...o.keys()].sort((a, b) => a - b)) {
    if (o.get(sid) <= (c.get(sid) || 0)) continue;
    const s = g.sym(sid);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(o.get(sid)), I(c.get(sid) || 0), I(s.NReturns), I(s.NGotos), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]);
  }
  return ret(["name", "opens", "closes", "returns_", "gotos", "cyclo", "fan_in", "at"], rows, p, dWeighted([[1, 1], [-1, 2]]), dI(6));
};

Q.qQ57 = (g, p) => ret(["name", "allocs", "null_checks", "returns_", "cyclo", "fan_in", "at"],
  scan(g, p, g.syms(), (s) => isFn(s) && s.NAlloc > 0 && s.NNullCheck === 0, (s) => [S(s.Name), I(s.NAlloc), I(s.NNullCheck), I(s.NReturns),
    I(s.Cyclomatic), I(s.FanIn), S(g.at(s))]), p, dI(1), dI(5), cmpS(0));

Q.qQ58 = (g, p) => ret(["name", "vlas", "io", "mem", "int_", "nest", "fan_in", "rec", "at"],
  scan(g, p, g.syms(), (s) => s.NVla > 0 && !(s.NIO <= 0 && s.NMemory <= 0 && s.IsRecursive !== 1), (s) => [S(s.Name), I(s.NVla), I(s.NIO),
    I(s.NMemory), I(s.NInteger), I(s.MaxNesting), I(s.FanIn), I(s.IsRecursive), S(g.at(s))]), p, dI(1), dI(2), dI(6));

Q.qQ59 = (g, p) => ret(["name", "uses", "frees", "sloc", "returns_", "fan_in", "at"],
  scan(g, p, g.syms(), (s) => s.NFreeThenUse > 0, (s) => [S(s.Name), I(s.NFreeThenUse), I(s.NFree), I(s.Sloc), I(s.NReturns), I(s.FanIn),
    S(g.at(s))]), p, dI(1), dI(5));

Q.qQ60 = (g, p) => ret(["name", "weak_rand", "io", "exec", "fan_in", "exported", "at"],
  scan(g, p, g.syms(), (s) => s.NWeakRandom > 0, (s) => [S(s.Name), I(s.NWeakRandom), I(s.NIO), I(s.NExec), I(s.FanIn), I(s.IsPublic),
    S(g.at(s))]), p, dI(1), dI(4));

Q.qQ61 = (g, p) => ret(["name", "var_shifts", "casts", "io", "int_", "cyclo", "fan_in", "at"],
  scan(g, p, g.symsName(), (s) => s.NShiftVar > 0, (s) => [S(s.Name), I(s.NShiftVar), I(s.NCast), I(s.NIO), I(s.NInteger), I(s.Cyclomatic),
    I(s.FanIn), S(g.at(s))]), p, dI(1), dI(3), dI(6));

Q.qQ62 = (g, p) => {
  const fallible = new Map();
  for (const i of g.hazCats("io", "stdio", "alloc", "memory")) inc(fallible, g.Hazards[i].SymbolID, g.Hazards[i].N);
  return ret(["name", "errno_reads", "fallible_calls", "returns_", "cyclo", "fan_in", "at"],
    scan(g, p, g.symsName(), (s) => isFn(s) && s.NErrno > 0, (s) => [S(s.Name), I(s.NErrno), I(fallible.get(s.ID) || 0), I(s.NReturns),
      I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], true), p, dI(1), dI(2));
};

Q.qQ63 = (g, p) => ret(["name", "env_reads", "mem", "exec", "io", "fan_in", "at"],
  scan(g, p, g.syms(), (s) => s.NGetenv > 0, (s) => [S(s.Name), I(s.NGetenv), I(s.NMemory), I(s.NExec), I(s.NIO), I(s.FanIn), S(g.at(s))]),
  p, dI(1), dI(3), dI(5));

Q.qQ64 = (g, p) => ret(["name", "bad_asserts", "sloc", "calls", "fan_in", "in_test_file", "at"],
  scan(g, p, g.syms(), (s) => s.NAssertSide > 0, (s) => [S(s.Name), I(s.NAssertSide), I(s.Sloc), I(s.NCalls), I(s.FanIn),
    I(g.fileOf(s.ID).IsTest), S(g.at(s))], true), p, dI(1), dI(4));

Q.qQ65 = (g, p) => {
  const byPair = new Map(), per = new Map();
  g.Locks.forEach((l, i) => pushTo(per, l.SymbolID, i));
  for (const sid of [...per.keys()].sort((a, b) => a - b)) {
    const idxs = per.get(sid);
    for (const a of idxs) {
      for (const b of idxs) {
        const la = g.Locks[a], lb = g.Locks[b];
        if (la.Name === lb.Name || la.Line >= lb.Line) continue;
        let lo = la.Name, hi = lb.Name, dirFirst = 0;
        if (hi < lo) { [lo, hi] = [hi, lo]; dirFirst = 1; }
        const k = lo + "\x00" + hi;
        let lp = byPair.get(k);
        if (!lp) byPair.set(k, (lp = { lo, hi, aFns: new Set(), bFns: new Set(), aNames: [], bNames: [] }));
        const s = g.sym(sid), nm = s ? s.Name : "";
        if (dirFirst === 0) { lp.aFns.add(sid); if (!lp.aNames.includes(nm)) lp.aNames.push(nm); }
        else { lp.bFns.add(sid); if (!lp.bNames.includes(nm)) lp.bNames.push(nm); }
      }
    }
  }
  const keys = [...byPair.values()].sort((x, y) => (x.lo !== y.lo ? (x.lo < y.lo ? -1 : 1) : x.hi < y.hi ? -1 : x.hi > y.hi ? 1 : 0));
  const rows = [];
  for (const lp of keys) {
    const af = lp.aFns.size, bf = lp.bFns.size;
    if (af === 0 || bf === 0) continue;
    let modOK = false;
    for (const sid of [...lp.aFns, ...lp.bFns]) { const s = g.sym(sid); if (s && g.modOK(s.ModuleID, p)) modOK = true; }
    if (!modOK) continue;
    rows.push([S(lp.lo), S(lp.hi), I(af), I(bf), S(groupConcat(lp.aNames)), S(groupConcat(lp.bNames))]);
  }
  return ret(["lock_a", "lock_b", "a_first_fns", "b_first_fns", "a_first", "b_first"], rows, p, dSum(2, 3), dI(2));
};

Q.qQ66 = (g, p) => {
  const modc = g.perMod();
  for (const s of g.Symbols) if (s.NConcurrency > 0 && s.ModuleID >= 0 && s.ModuleID < modc.length) modc[s.ModuleID]++;
  const byKey = new Map();
  for (const l of g.Layout) {
    if (l.InUnion !== 0 || l.Depth !== 0 || l.ByteOff < 0 || l.Exact !== 1) continue;
    const k = l.SymbolID * 4294967296 + Math.trunc(l.ByteOff / 64);
    let e = byKey.get(k);
    if (!e) byKey.set(k, (e = { n: 0, lo: l.ByteOff, hi: l.ByteOff }));
    e.n++;
    if (l.ByteOff < e.lo) e.lo = l.ByteOff;
    if (l.ByteOff > e.hi) e.hi = l.ByteOff;
  }
  const size = g.perSym(), pad = g.perSym();
  for (const ss of g.SSize) if (ss.SymbolID >= 0 && ss.SymbolID < size.length) { size[ss.SymbolID] = ss.TotalSize; pad[ss.SymbolID] = ss.TotalPad; }
  const keys = [...byKey].filter(([, v]) => v.n >= 2).map(([k]) => k).sort((a, b) => a - b);
  const rows = [];
  for (const k of keys) {
    const sid = Math.floor(k / 4294967296), s = g.sym(sid);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    const e = byKey.get(k);
    rows.push([S(s.Name), I(e.n), I(k % 4294967296), I(e.lo), I(e.hi), I(size[sid] || 0), I(pad[sid] || 0), I(modc[s.ModuleID] || 0), S(g.at(s))]);
  }
  return ret(["struct_name", "sharing_fields", "cache_line", "first_off", "last_off", "size", "pad", "conc_fns_in_module", "at"], rows, p,
    dI(1), dI(7), dI(5));
};

Q.qQ67 = (g, p) => {
  const rows = [];
  for (const m of g.Memops) {
    if ((m.Fn !== "strncpy" && m.Fn !== "strncat") || !m.SizeArg.includes("sizeof")) continue;
    const s = g.sym(m.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    const f = g.fileOf(s.ID);
    if (!f) continue;
    rows.push([S(s.Name), S(m.Fn), S(m.Dst), S(m.SizeArg), I(s.FanIn), S(f.Path + ":" + m.Line)]);
  }
  return ret(["name", "fn", "dst", "size", "fan_in", "at"], rows, p, dI(4), cmpS(0));
};

Q.qQ68 = (g, p) => {
  const copied = g.perSym();
  for (const m of g.Memops) if ((m.Fn === "memcpy" || m.Fn === "memmove" || m.Fn === "memset") && m.SymbolID >= 0 && m.SymbolID < copied.length) copied[m.SymbolID]++;
  const rows = [];
  for (const ss of g.SSize) {
    if (ss.TotalPad <= 0 || ss.Exact !== 1) continue;
    const s = g.sym(ss.SymbolID);
    if (!s || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(ss.TotalSize), I(ss.TotalPad), I(pct(ss.TotalPad, ss.TotalSize)), I(copied[ss.SymbolID] > 0 ? 1 : 0), S(g.at(s))]);
  }
  return ret(["struct_name", "size", "pad", "pct_pad", "copied_whole", "at"], rows, p, dI(2), dI(4), dI(1));
};

Q.qQ69 = (g, p) => {
  const named = g.namedAsValue(), rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.IsStatic !== 1 || s.FanIn !== 0 || named(s) || !g.modOK(s.ModuleID, p)) continue;
    const f = g.fileOf(s.ID);
    if (!f) continue;
    rows.push([S(s.Name), I(s.Sloc), I(s.Cyclomatic), I(s.NCalls), S(f.Path), S(g.at(s))]);
  }
  return ret(["name", "sloc", "cyclo", "calls", "only_file", "at"], rows, p, dI(1), dI(2), cmpS(0));
};

Q.qQ70 = (g, p) => {
  const declN = g.six().declN, rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.IsStatic !== 0 || s.IsInline !== 0) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.Ext !== ".c" || declN[s.ID] > 0 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(s.FanIn), I(s.Sloc), I(s.IsPublic), S(g.at(s))]);
  }
  return ret(["name", "fan_in", "sloc", "external", "at"], rows, p, dI(1), dI(2), cmpS(0));
};

Q.qQ71 = (g, p) => {
  const incl = g.perFile(), incBy = g.perFile(), guarded = g.perFile();
  const ok = (id) => id >= 0 && id < incl.length;
  for (const im of g.Imports) { if (ok(im.FileID)) incl[im.FileID]++; if (im.HasTargetID && ok(im.TargetID)) incBy[im.TargetID]++; }
  for (const cb of g.Cfgs) if (cb.Line <= 12 && cb.Directive === "ifndef" && ok(cb.FileID)) guarded[cb.FileID]++;
  const rows = [];
  for (const f of g.Files) {
    if (f.Ext !== ".h" || guarded[f.ID] > 0 || !g.modOK(f.ModuleID, p)) continue;
    rows.push([S(f.Path), I(f.Sloc), I(f.NSymbols), I(incl[f.ID]), I(incBy[f.ID])]);
  }
  return ret(["path", "sloc", "syms", "includes", "included_by"], rows, p, dI(4), dI(1));
};

Q.qQ72 = (g, p) => {
  const K = (a, b) => a * 4294967296 + b;
  const seenE = new Set(), deg = new Map(), direct = [];
  for (const e of g.Edges) {
    const a = g.sym(e.CallerID), b = g.sym(e.CalleeID);
    if (!a || !b || a.ModuleID === 0 || b.ModuleID === 0 || a.ModuleID === b.ModuleID) continue;
    const k = K(a.ModuleID, b.ModuleID);
    if (!seenE.has(k)) { seenE.add(k); direct.push([a.ModuleID, b.ModuleID]); inc(deg, a.ModuleID); }
  }
  const succ = new Map();
  for (const [a, b] of direct) pushTo(succ, a, b);
  const best = new Map();
  for (const [a, b] of direct) best.set(K(a, b), 1);
  let cur = [...direct.map(([a, b]) => [a, b, 1])];
  for (let depth = 2; depth <= 4; depth++) {
    const next = new Map();
    for (const [a, b, d] of cur) {
      if (d !== depth - 1) continue;
      for (const to of succ.get(b) || []) { const nk = K(a, to); if (!best.has(nk)) next.set(nk, [a, to, depth]); }
    }
    for (const [k, v] of next) best.set(k, v[2]);
    cur = [...next.values()];
    if (next.size === 0) break;
  }
  const keys = [];
  for (const k of best.keys()) { const a = Math.floor(k / 4294967296), b = k % 4294967296; if (a < b) keys.push([a, b]); }
  keys.sort((x, y) => x[0] - y[0] || x[1] - y[1]);
  const rows = [];
  for (const [a, b] of keys) {
    const out = best.get(K(a, b)), back = best.get(K(b, a));
    if (out === undefined || back === undefined || !g.modOK(a, p)) continue;
    rows.push([S(g.modName(a)), S(g.modName(b)), I(out), I(back), I(deg.get(a) || 0), I(deg.get(b) || 0)]);
  }
  return ret(["from_module", "to_module", "hops_out", "hops_back", "out_edges", "back_edges"], rows, p, dSum(2, 3), dI(4));
};

Q.qQ73 = (g, p) => {
  const prefixOf = (s) => { const u = s.Name.indexOf("_"); return u >= 1 ? s.Name.slice(0, u + 1) : ""; };
  const nf = g.Files.length + 1, counts = new Array(nf);
  for (const s of g.Symbols) {
    if (!isFn(s) || s.FileID < 0 || s.FileID >= nf) continue;
    const pr = prefixOf(s);
    if (pr === "") continue;
    const l = counts[s.FileID] || (counts[s.FileID] = []);
    const e = l.find((c) => c.pr === pr);
    if (e) e.n++; else l.push({ pr, n: 1 });
  }
  const dom = new Array(nf);
  for (let fid = 0; fid < nf; fid++) {
    let d = { pr: "", n: 0 };
    for (const c of counts[fid] || []) if (c.n > d.n || (c.n === d.n && c.pr > d.pr)) d = c;
    dom[fid] = d;
  }
  const rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.FileID < 0 || s.FileID >= nf) continue;
    const pr = prefixOf(s), d = dom[s.FileID];
    if (pr === "" || pr === d.pr || d.n < 3 || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), S(d.pr), I(d.n), I(s.FanIn), I(s.Sloc), S(g.at(s))]);
  }
  return ret(["name", "dominant_prefix", "prefix_users", "fan_in", "sloc", "at"], rows, p, dI(2), dI(3));
};

Q.qQ74 = (g, p) => {
  const untested = new Uint8Array(g.Symbols.length + 1);
  for (const e of g.Edges) {
    const cs = g.sym(e.CallerID);
    if (!cs || e.CalleeID < 0 || e.CalleeID >= untested.length) continue;
    const f = g.fileOf(cs.ID);
    if ((f && f.IsTest === 1) || cs.IsTest === 1) untested[e.CalleeID] = 1;
  }
  const rows = [];
  for (const s of g.syms()) {
    if (!isFn(s) || s.FanIn < 3) continue;
    const f = g.fileOf(s.ID);
    if (!f || f.IsTest !== 0 || s.IsTest !== 0 || untested[s.ID] || !g.modOK(s.ModuleID, p)) continue;
    rows.push([S(s.Name), I(s.FanIn), I(s.Cyclomatic), I(s.Sloc), S(g.modName(s.ModuleID) || "(none)"), S(g.at(s))]);
  }
  return ret(["name", "fan_in", "cyclo", "sloc", "module", "at"], rows, p, dI(1), dI(2), cmpS(0));
};

Q.qQ75 = (g, p) => ret(["name", "io_calls", "depth", "loops", "calls", "fan_in", "at"],
  scan(g, p, g.syms(), (s) => s.IOInLoop > 0, (s) => [S(s.Name), I(s.IOInLoop), I(s.MaxLoopDepth), I(s.NLoops), I(s.CallInLoop), I(s.FanIn),
    S(g.at(s))]), p, dI(1), dI(2));

Q.qQ76 = (g, p) => ret(["name", "lock_ops", "depth", "acquires", "releases", "fan_in", "at"],
  scan(g, p, g.syms(), (s) => s.LockInLoop > 0, (s) => [S(s.Name), I(s.LockInLoop), I(s.MaxLoopDepth), I(s.NLockAcquire), I(s.NLockRelease),
    I(s.FanIn), S(g.at(s))]), p, dI(1), dI(2));

Q.qQ77 = (g, p) => ret(["name", "branches", "depth", "switches", "calls", "sloc", "fan_in", "at"],
  scan(g, p, g.symsName(), (s) => s.BranchInLoop > 0 && s.MaxLoopDepth >= 1, (s) => [S(s.Name), I(s.BranchInLoop), I(s.MaxLoopDepth),
    I(s.SwitchInLoop), I(s.CallInLoop), I(s.Sloc), I(s.FanIn), S(g.at(s))]), p, dI(1), dI(2));

const sq = (cols, keep, fill, ...keys) => (g, p) => g.simpleQ(cols, keep, p, (s) => fill(g, s), ...keys);
Q.qQ78 = sq(["name", "et_regs", "epoll_calls", "kqueue_calls", "eagain_checks", "loops", "cyclo", "fan_in", "at"],
  (s) => s.NEtReg > 0 && s.NEagain === 0, (g, s) => [S(s.Name), I(s.NEtReg), I(s.NEpoll), I(s.NKqueue), I(s.NEagain), I(s.NLoops),
    I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(6));
Q.qQ79 = sq(["name", "waits", "epoll_calls", "uring_calls", "kqueue_calls", "eintr_checks", "loops", "cyclo", "fan_in", "at"],
  (s) => s.NEventWait > 0 && s.NEintr === 0, (g, s) => [S(s.Name), I(s.NEventWait), I(s.NEpoll), I(s.NUring), I(s.NKqueue), I(s.NEintr),
    I(s.NLoops), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(7));
Q.qQ80 = sq(["name", "write_ready", "epoll_calls", "kqueue_calls", "deregistrations", "loops", "fan_in", "at"],
  (s) => s.NWriteReady > 0 && s.NDereg === 0, (g, s) => [S(s.Name), I(s.NWriteReady), I(s.NEpoll), I(s.NKqueue), I(s.NDereg), I(s.NLoops),
    I(s.FanIn), S(g.at(s))], dI(1), dI(6));
Q.qQ81 = sq(["name", "epoll_calls", "kqueue_calls", "err_flags", "waits", "cyclo", "fan_in", "at"],
  (s) => (s.NEpoll > 0 || s.NKqueue > 0) && s.NErrFlag === 0, (g, s) => [S(s.Name), I(s.NEpoll), I(s.NKqueue), I(s.NErrFlag), I(s.NEventWait),
    I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], dSum(1, 2), dI(6));
Q.qQ82 = sq(["name", "oneshot_regs", "rearms", "epoll_calls", "kqueue_calls", "cyclo", "fan_in", "at"],
  (s) => s.NOneshotReg > 0 && s.NRearm === 0, (g, s) => [S(s.Name), I(s.NOneshotReg), I(s.NRearm), I(s.NEpoll), I(s.NKqueue), I(s.Cyclomatic),
    I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(6));
Q.qQ83 = sq(["name", "uring_calls", "res_checks", "waits", "errno_reads", "cyclo", "fan_in", "at"],
  (s) => s.NUring > 0 && s.NUringRes === 0, (g, s) => [S(s.Name), I(s.NUring), I(s.NUringRes), I(s.NEventWait), I(s.NErrno), I(s.Cyclomatic),
    I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(6));
Q.qQ84 = sq(["name", "uring_calls", "errno_reads", "res_checks", "cyclo", "fan_in", "at"],
  (s) => s.NUring > 0 && s.NErrno > 0, (g, s) => [S(s.Name), I(s.NUring), I(s.NErrno), I(s.NUringRes), I(s.Cyclomatic), I(s.FanIn),
    S(g.at(s))], cmpI(1), cmpI(2));
Q.qQ85 = sq(["name", "uring_calls", "sqpoll_setups", "frees", "allocs", "cyclo", "at"],
  (s) => s.NUring > 0 && s.NUringSqpoll > 0 && s.NFree > 0, (g, s) => [S(s.Name), I(s.NUring), I(s.NUringSqpoll), I(s.NFree), I(s.NAlloc),
    I(s.Cyclomatic), S(g.at(s))], cmpI(2), cmpI(3));
Q.qQ86 = sq(["name", "ring_touches", "uring_calls", "barriers", "cyclo", "fan_in", "at"],
  (s) => s.NUringRing > 0 && s.NUringBarrier === 0, (g, s) => [S(s.Name), I(s.NUringRing), I(s.NUring), I(s.NUringBarrier), I(s.Cyclomatic),
    I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(5));
Q.qQ87 = sq(["name", "waits", "allocs", "allocs_in_loop", "loop_depth", "calls_in_loop", "cyclo", "fan_in", "at"],
  (s) => s.NEventWait > 0 && s.NAlloc > 0, (g, s) => [S(s.Name), I(s.NEventWait), I(s.NAlloc), I(s.AllocInLoop), I(s.MaxLoopDepth),
    I(s.CallInLoop), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(2), cmpI(3));
Q.qQ88 = sq(["name", "indefinite", "zero_tmo", "waits", "epoll_calls", "kqueue_calls", "loops", "cyclo", "at"],
  (s) => s.NEvTimeoutIndefinite > 0 || s.NEvTimeoutZero > 0, (g, s) => [S(s.Name), I(s.NEvTimeoutIndefinite), I(s.NEvTimeoutZero),
    I(s.NEventWait), I(s.NEpoll), I(s.NKqueue), I(s.NLoops), I(s.Cyclomatic), S(g.at(s))], dSum(1, 2), cmpI(7));
Q.qQ89 = sq(["name", "et_regs", "nonblock", "eagain_checks", "epoll_calls", "kqueue_calls", "cyclo", "fan_in", "at"],
  (s) => s.NEtReg > 0 && s.NNonblockSet === 0, (g, s) => [S(s.Name), I(s.NEtReg), I(s.NNonblockSet), I(s.NEagain), I(s.NEpoll), I(s.NKqueue),
    I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(6));
Q.qQ90 = sq(["name", "creates", "destroys", "epoll_calls", "uring_calls", "kqueue_calls", "returns_", "cyclo", "fan_in", "at"],
  (s) => s.NEventCreate > 0 && s.NEventDestroy === 0, (g, s) => [S(s.Name), I(s.NEventCreate), I(s.NEventDestroy), I(s.NEpoll), I(s.NUring),
    I(s.NKqueue), I(s.NReturns), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], dI(1), dI(8));
Q.qQ91 = sq(["name", "sqe_gets", "null_checks", "uring_calls", "cyclo", "fan_in", "at"],
  (s) => s.NUringSqe > 0 && s.NNullCheck === 0, (g, s) => [S(s.Name), I(s.NUringSqe), I(s.NNullCheck), I(s.NUring), I(s.Cyclomatic),
    I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(5));
Q.qQ92 = sq(["name", "cqe_seen", "waits", "uring_calls", "sqe_gets", "cyclo", "fan_in", "at"],
  (s) => s.NUring > 0 && s.NEventWait > 0 && s.NUringSeen === 0, (g, s) => [S(s.Name), I(s.NUringSeen), I(s.NEventWait), I(s.NUring),
    I(s.NUringSqe), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(2), cmpI(3));
Q.qQ93 = sq(["name", "uncorrelated", "sqe_gets", "user_data_sets", "waits", "cyclo", "fan_in", "at"],
  (s) => s.NUringSqe >= 2 && s.NUringSqe > s.NUringUdata, (g, s) => [S(s.Name), I(s.NUringSqe - s.NUringUdata), I(s.NUringSqe),
    I(s.NUringUdata), I(s.NEventWait), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(2));
Q.qQ94 = sq(["name", "stream_ops", "linked", "sqe_gets", "loop_depth", "cyclo", "fan_in", "at"],
  (s) => s.NUringStreamOps > 0 && s.NUringLink === 0, (g, s) => [S(s.Name), I(s.NUringStreamOps), I(s.NUringLink), I(s.NUringSqe),
    I(s.MaxLoopDepth), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(6));
Q.qQ95 = sq(["name", "ring_inits", "exits", "uring_calls", "any_release", "cyclo", "fan_in", "at"],
  (s) => s.NUring > 0 && s.NEventCreate > 0 && s.NUringTeardown === 0, (g, s) => [S(s.Name), I(s.NEventCreate), I(s.NUringTeardown),
    I(s.NUring), I(s.NEventDestroy), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], cmpI(1), cmpI(2));
Q.qQ96 = sq(["name", "timers", "zero_data", "kqueue_calls", "cyclo", "fan_in", "at"],
  (s) => s.NKqTimerZeroData > 0, (g, s) => [S(s.Name), I(s.NKqTimer), I(s.NKqTimerZeroData), I(s.NKqueue), I(s.Cyclomatic), I(s.FanIn),
    S(g.at(s))], cmpI(2), cmpI(5));
Q.qQ97 = sq(["name", "receipt_batches", "err_flags", "kqueue_calls", "cyclo", "fan_in", "at"],
  (s) => s.NKqReceipt > 0 && s.NErrFlag === 0, (g, s) => [S(s.Name), I(s.NKqReceipt), I(s.NErrFlag), I(s.NKqueue), I(s.Cyclomatic), I(s.FanIn),
    S(g.at(s))], cmpI(1), cmpI(5));
Q.qQ98 = sq(["name", "waits", "io_in_loop", "lock_in_loop", "calls_in_loop", "loop_depth", "cyclo", "fan_in", "at"],
  (s) => s.NEventWait > 0 && (s.IOInLoop > 0 || s.LockInLoop > 0), (g, s) => [S(s.Name), I(s.NEventWait), I(s.IOInLoop), I(s.LockInLoop),
    I(s.CallInLoop), I(s.MaxLoopDepth), I(s.Cyclomatic), I(s.FanIn), S(g.at(s))], dSum(2, 3), cmpI(7));
Q.qQ119 = sq(["name", "size_one_waits", "waits", "epoll_calls", "kqueue_calls", "cyclo", "fan_in", "at"],
  (s) => s.NEvBatchOne > 0, (g, s) => [S(s.Name), I(s.NEvBatchOne), I(s.NEventWait), I(s.NEpoll), I(s.NKqueue), I(s.Cyclomatic), I(s.FanIn),
    S(g.at(s))], cmpI(1), cmpI(6));

export function apiByID(g) {
  return g.memo("apiByID", () => { const m = new Map(); g.APIUses.forEach((u, i) => pushTo(m, u.SymbolID, i)); return m; });
}
export const hasAPIUse = (g, sid, fns) => (apiByID(g).get(sid) || []).some((i) => fns.includes(g.APIUses[i].Fn));
function apiQ(cols, fns, keep, fill, ...keys) {
  return (g, p) => {
    const byFn = g.memo("apiByFn", () => { const m = new Map(); for (const u of g.APIUses) pushTo(m, u.Fn, u.SymbolID); return m; });
    const mark = new Uint8Array(g.Symbols.length + 1);
    for (const fn of fns) for (const id of byFn.get(fn) || []) if (id > 0 && id < mark.length) mark[id] = 1;
    const rows = [];
    for (const s of g.syms()) {
      if (!mark[s.ID] || !keep(g, s) || !g.modOK(s.ModuleID, p)) continue;
      rows.push(fill(g, s));
    }
    orderBy(rows, ...keys, cmpS(0));
    return [cols, limit(rows, p)];
  };
}
const nameRow = (...ks) => (g, s) => [S(s.Name), ...ks.map((k) => I(s[k])), S(g.at(s))];
Q.qQ99 = apiQ(["name", "sloc", "cyclo", "fan_in", "at"], ["snprintf", "vsnprintf"], (g, s) => s.NRetNegCheck === 0,
  nameRow("Sloc", "Cyclomatic", "FanIn"), dI(1), dI(3));
Q.qQ100 = apiQ(["name", "sloc", "cyclo", "fan_in", "at"], ["fread", "fwrite", "fgets", "fscanf", "vfscanf", "fprintf", "fputs", "fputc"],
  (g, s) => s.NRetNegCheck === 0 && s.NNullCheck === 0, nameRow("Sloc", "Cyclomatic", "FanIn"), dI(1), dI(3));
Q.qQ101 = apiQ(["name", "errno_reads", "endptr_checks", "cyclo", "fan_in", "at"], ["strtol", "strtoll", "strtoul", "strtoull", "strtod",
  "strtof", "strtold", "strtoimax", "strtoumax"], (g, s) => !(s.NErrnoZero > 0 && s.NEndptr > 0),
nameRow("NErrno", "NEndptr", "Cyclomatic", "FanIn"), dI(3), dI(4));
Q.qQ102 = apiQ(["name", "cyclo", "params", "fan_in", "at"], ["isalnum", "isalpha", "isblank", "iscntrl", "isdigit", "isgraph", "islower",
  "isprint", "ispunct", "isspace", "isupper", "isxdigit", "tolower", "toupper"], (g, s) => s.NUcharCast === 0,
nameRow("Cyclomatic", "NParams", "FanIn"), dI(3), dI(1));
Q.qQ103 = apiQ(["name", "loops", "cyclo", "fan_in", "at"], ["nanosleep", "usleep", "sleep"], (g, s) => s.NEintr === 0,
  nameRow("NLoops", "Cyclomatic", "FanIn"), dI(3), dI(2));
Q.qQ104 = apiQ(["name", "depth", "io_in_loop", "cyclo", "fan_in", "at"], ["send", "sendto", "sendmsg", "write", "writev", "pwritev"],
  (g, s) => s.NRetNegCheck === 0, nameRow("MaxLoopDepth", "IOInLoop", "Cyclomatic", "FanIn"), dI(4), dI(1));
Q.qQ105 = apiQ(["name", "returns_", "cyclo", "fan_in", "at"], ["mmap"], (g, s) => !hasAPIUse(g, s.ID, ["munmap"]),
  nameRow("NReturns", "Cyclomatic", "FanIn"), dI(3), dI(2));
Q.qQ106 = apiQ(["name", "cyclo", "returns_", "fan_in", "at"], ["waitpid", "wait", "waitid", "wait3", "wait4"], (g, s) => s.NRetNegCheck === 0,
  nameRow("Cyclomatic", "NReturns", "FanIn"), cmpI(3), cmpI(1));
Q.qQ107 = apiQ(["name", "cyclo", "fan_in", "at"], ["scanf", "fscanf", "sscanf", "vscanf", "vfscanf", "vsscanf"], (g, s) => s.NRetNegCheck === 0,
  nameRow("Cyclomatic", "FanIn"), dI(2), dI(1));
Q.qQ108 = apiQ(["name", "depth", "cyclo", "fan_in", "at"], ["sqrt", "log", "log10", "log2", "log1p", "logb", "asin", "acos", "atanh", "acosh",
  "tgamma", "lgamma", "pow"], (g, s) => s.NDomainGuard === 0, nameRow("MaxLoopDepth", "Cyclomatic", "FanIn"), dI(3), dI(1));
Q.qQ109 = apiQ(["name", "cyclo", "conc_in_fn", "fan_in", "at"], ["localtime", "gmtime", "ctime", "asctime"], () => true,
  nameRow("Cyclomatic", "NConcurrency", "FanIn"), cmpI(3), cmpI(1));
Q.qQ110 = apiQ(["name", "cyclo", "fan_in", "at"], ["signal"], (g, s) => !hasAPIUse(g, s.ID, ["sigaction"]), nameRow("Cyclomatic", "FanIn"),
  dI(2), dI(1));
Q.qQ111 = apiQ(["name", "cyclo", "fan_in", "at"], ["mmap"], (g, s) => s.NMapFailed === 0 && s.NNullCheck === 0, nameRow("Cyclomatic", "FanIn"),
  dI(2), dI(1));
Q.qQ112 = apiQ(["name", "depth", "cyclo", "fan_in", "at"], ["accept", "accept4"], (g, s) => s.NNonblockSet === 0,
  nameRow("MaxLoopDepth", "Cyclomatic", "FanIn"), cmpI(3), cmpI(1));
Q.qQ113 = apiQ(["name", "cyclo", "returns_", "fan_in", "at"], ["va_start"], (g, s) => s.NVaEnd === 0, nameRow("Cyclomatic", "NReturns", "FanIn"),
  cmpI(3), cmpI(1));
Q.qQ114 = apiQ(["name", "sized_arrays", "cyclo", "fan_in", "at"], [], (g, s) => s.NStackszArray > 0,
  nameRow("NStackszArray", "Cyclomatic", "FanIn"), cmpI(1), cmpI(3));
Q.qQ115 = apiQ(["name", "broken_checks", "cyclo", "fan_in", "at"], ["memcpy", "memmove", "memset", "malloc", "realloc", "strlen", "strcpy"],
  (g, s) => s.NPtrOvfCheck > 0, nameRow("NPtrOvfCheck", "Cyclomatic", "FanIn"), cmpI(1), cmpI(3));
Q.qQ116 = apiQ(["name", "transposed", "cyclo", "fan_in", "at"], ["calloc"], (g, s) => s.NCallocTransposed > 0,
  nameRow("NCallocTransposed", "Cyclomatic", "FanIn"), cmpI(1), cmpI(3));
Q.qQ117 = apiQ(["name", "array_va_args", "cyclo", "fan_in", "at"], ["va_arg", "va_start"], (g, s) => s.NVaArgArr > 0,
  nameRow("NVaArgArr", "Cyclomatic", "FanIn"), cmpI(1), cmpI(3));
Q.qQ118 = apiQ(["name", "monotonic_uses", "cyclo", "depth", "fan_in", "at"], ["gettimeofday", "time", "ftime", "clock"],
  (g, s) => s.NMonotonicClock === 0 && !hasAPIUse(g, s.ID, ["clock_gettime"]), nameRow("NMonotonicClock", "Cyclomatic", "MaxLoopDepth", "FanIn"),
  dI(4), dI(2));
