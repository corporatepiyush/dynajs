// Derivation: one claimed file's rows from its unit's AST and preprocessor.
import { N, F, PUNCT_TABLE, PCODE, goParseInt } from "./parse.js";
import { stripBOM, parseCInt, stripNumSuffix, cleanPath } from "./pp.js";
import { jtrunc, squeeze, runeCount, trimSpace } from "./text.js";
import { classifyLines, runeLen } from "./discover.js";
import * as H from "./heur.js";
import data from "./data.json" with { type: "json" };

const P = PCODE;
const MK_VISITED = 1, MK_CALLEE = 2, MK_AMP = 4;
const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const hazardFuncs = (n) => (has(data.hazardFuncs, n) ? data.hazardFuncs[n] : "");
const funcToNS = (n) => (has(data.funcToNS, n) ? data.funcToNS[n] : "");
const MAGIC_OK = new Set(Object.keys(data.magicOK));
const LIBC_ALLOC = new Set(Object.keys(data.libcAllocNames).filter((k) => data.libcAllocNames[k]));
const EVENT_WAIT = new Set(Object.keys(data.eventWaitFn).filter((k) => data.eventWaitFn[k]));
const EVENT_CREATE = new Set(Object.keys(data.eventCreateFn).filter((k) => data.eventCreateFn[k]));
const BUILTIN_PREFIXES = data.builtinPrefixes;
const TYPE_SIZE = data.typeSizeTable, TYPE_ALIGN = data.typeAlignTable;
const PRINTF_FNS = data.printfFns;
const GUARD_NAMES = data.guardNames;
const GUARD_NS = data.guardNS;
const EV_TIMEOUT_IDX = data.evTimeoutIdx, EV_BATCH_IDX = data.evBatchIdx;
const MEMOP_FNS = new Set(["memcpy", "memmove", "strcpy", "strncpy", "strcat", "strncat", "memset", "sprintf", "snprintf"]);
const GUARD_COL = { ret_neg: "NRetNegCheck", domain: "NDomainGuard", uchar: "NUcharCast", errno_zero: "NErrnoZero", endptr: "NEndptr",
  va_end: "NVaEnd", map_failed: "NMapFailed", monotonic: "NMonotonicClock", stacksz_array: "NStackszArray",
  ptr_ovf_check: "NPtrOvfCheck", calloc_sizeof_first: "NCallocTransposed", va_arg_array: "NVaArgArr" };
const b2i = (b) => (b ? 1 : 0);
const PTR = 8;

// ---- numbers and literals
function magicFor(tok) {
  const v = goParseInt(stripNumSuffix(tok.replaceAll("'", "")));
  return v !== null && MAGIC_OK.has(String(v));
}
function isFloatTok(t) {
  const body = stripNumSuffix(t.replaceAll("'", ""));
  if (body.length > 1 && body[0] === "0" && (body[1] === "x" || body[1] === "X")) return /[pP.]/.test(body.slice(2));
  return /[.eE]/.test(body);
}
function numKind(t) {
  const body = t.toLowerCase();
  if (body.startsWith("0x")) return "hex";
  if (body.startsWith("0b")) return "bin";
  if (body.length > 1 && body[0] === "0" && !isFloatTok(t)) return "oct";
  if (isFloatTok(t)) return "float";
  return "int";
}
function dequoteLit(s) {
  for (const p of ["u8", "u", "U", "L"]) if (s.startsWith(p)) s = s.slice(p.length);
  if (s.length >= 2 && s[0] === '"') s = s.slice(1, -1);
  return s;
}
function secretShaped(s) {
  if (s.length < 12 || s.includes(" ")) return false;
  const low = H.asciiLower(s);
  return ["passw", "secret", "api_key", "apikey", "token", "credential", "private_key", "auth_key", "access_key"].some((k) => low.includes(k));
}
const isIntrinsicCall = (name) => BUILTIN_PREFIXES.some((p) => name.startsWith(p));
const isFreeName = (n) => n === "free" || (n.length > 4 && n.slice(-4).toLowerCase() === "free");
function isAllocName(name) {
  if (LIBC_ALLOC.has(name)) return true;
  const low = H.asciiLower(name);
  return ["alloc", "free", "strdup", "memdup"].some((s) => low.length > s.length && low.endsWith(s));
}
const allocatesName = (n) => isAllocName(n) && !isFreeName(n);
const isEventOp = (fn) => fn.startsWith("epoll_") || fn.startsWith("io_uring_") || EVENT_WAIT.has(fn) || EVENT_CREATE.has(fn) || fn === "kqueue" || fn === "EV_SET";
const eventFamily = (fn) => (fn.startsWith("epoll_") ? "epoll" : fn.startsWith("io_uring_") ? "io_uring" : "kqueue");

export function splitTop(s) {
  const out = [];
  let depth = 0, start = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === "(" || c === "[") depth++;
    else if (c === ")" || c === "]") depth--;
    else if (c === "," && depth === 0) { out.push(trimSpace(s.slice(start, i))); start = i + 1; }
  }
  if (trimSpace(s.slice(start)) !== "" || out.length > 0) out.push(trimSpace(s.slice(start)));
  return out;
}
function memopArgs(fn, a) {
  let dst = "", src = "", sz = "";
  if (fn === "memset") { if (a.length > 0) dst = a[0]; if (a.length > 2) sz = a[2]; }
  else if (fn === "snprintf" || fn === "sprintf") { if (a.length > 0) dst = a[0]; if (a.length > 1) sz = a[1]; if (a.length > 2) src = a[2]; }
  else { if (a.length > 0) dst = a[0]; if (a.length > 1) src = a[1]; if (a.length > 2) sz = a[2]; }
  return [dst, src, sz];
}
function sizeofOperand(s) {
  s = trimSpace(s);
  if (!s.startsWith("sizeof")) return null;
  const rest = trimSpace(s.slice(6));
  if (rest.startsWith("(")) {
    let inner = rest.slice(1);
    if (inner.endsWith(")")) inner = inner.slice(0, -1);
    inner = trimSpace(inner);
    if (inner.includes("struct ") || inner.includes("union ") || inner.includes("enum ")) return null;
    return inner;
  }
  return rest === "" ? null : rest;
}
const countCh = (s, ch) => s.split(ch).length - 1;
function lockArgName(arg) {
  let a = trimSpace(arg);
  while (a.length && (a[0] === "&" || a[0] === "(" || a[0] === " ")) a = a.slice(1);
  while (a.length && (a[a.length - 1] === ")" || a[a.length - 1] === " ") && countCh(a, "(") < countCh(a, ")")) a = a.slice(0, -1);
  if (H.firstIdent(a) === null) return "";
  return a.replaceAll(" ", "");
}

// ---- type sizes
function aggBase(t) {
  for (const w of ["const", "volatile", "_Atomic", "register"]) t = H.replaceWord(t, w, " ");
  return H.trimSpaceB(squeeze(t, ""));
}
function typeSizeOf(ctype, ptr, alen) {
  let base = 0, align = 0, exact = 0;
  if (ptr > 0) { base = PTR; align = PTR; exact = 1; }
  else {
    let t = ctype;
    for (const w of ["const", "volatile", "struct", "union", "enum", "_Atomic", "register"]) t = H.replaceWord(t, w, " ");
    t = H.trimSpaceB(squeeze(t, ""));
    if (has(TYPE_SIZE, t)) {
      base = TYPE_SIZE[t];
      align = has(TYPE_ALIGN, t) ? TYPE_ALIGN[t] : base < 8 ? base : 8;
      exact = 1;
    }
  }
  if (alen > 0 && base !== 0) return [base * alen, align, exact];
  if (alen < 0) return [0, align, 0];
  return [base, align, exact];
}
function fldSize(f) {
  let [sz, al, ex] = typeSizeOf(f.ftype, f.ptr, f.alen);
  if (f.kn && f.ptr === 0) {
    sz = f.ksz; al = f.kal; ex = f.kex;
    if (f.alen > 0) sz *= f.alen;
    else if (f.alen < 0) { sz = 0; ex = 0; }
  }
  return [sz, al, ex];
}
const I32 = (x) => x | 0;
function packedSize(flds) {
  const m = [];
  for (const f of flds) {
    if (f.depth !== 0 || f.inUnion !== 0) continue;
    const [sz, al, ex] = fldSize(f);
    if (ex === 0 || al <= 0) return 0;
    m.push([sz, al]);
  }
  m.sort((a, b) => b[1] - a[1]);
  let off = 0, maxAl = 1;
  for (const [sz, al] of m) {
    const r = off % al;
    if (r !== 0) off = I32(off + al - r);
    off = I32(off + sz);
    maxAl = Math.max(maxAl, al);
  }
  const r = off % maxAl;
  if (r !== 0) off = I32(off + maxAl - r);
  return off;
}
function layoutStruct(flds, isUnion) {
  let off = 0, maxAlign = 1, allExact = 1;
  const out = [];
  for (const f of flds) {
    if (f.depth !== 0 || f.inUnion !== 0) { out.push([f.ordinal, -1, 0, 0, 0]); continue; }
    let [sz, al, ex] = fldSize(f);
    if (ex === 0) allExact = 0;
    if (al === 0) al = 1;
    if (al > maxAlign) maxAlign = al;
    let pad = 0;
    if (!isUnion) { pad = I32(-off) % al; if (pad < 0) pad += al; }
    const thisOff = isUnion ? 0 : I32(off + pad);
    out.push([f.ordinal, thisOff, sz, pad, ex]);
    if (isUnion) { if (sz > off) off = sz; } else off = I32(off + pad + sz);
  }
  let tail = 0;
  if (maxAlign > 0) { tail = I32(-off) % maxAlign; if (tail < 0) tail += maxAlign; }
  return [out, I32(off + tail), tail, allExact, maxAlign];
}

function signedIntType(t) {
  if (t === "" || t === undefined || /[*[(]/.test(t)) return false;
  let seen = false;
  for (const f of t.split(/[\t\n\v\f\r ]+/).filter(Boolean)) {
    switch (f) {
      case "const": case "volatile": case "register": case "static": case "auto": break;
      case "signed": seen = true; break;
      case "int": case "long": case "short": case "ssize_t": case "ptrdiff_t": case "off_t": case "intptr_t":
      case "intmax_t": case "int8_t": case "int16_t": case "int32_t": case "int64_t": case "__int64": seen = true; break;
      default: return false;
    }
  }
  return seen;
}
function unsignedSizeType(t) {
  if (t === "" || t === undefined || /[*[(]/.test(t)) return false;
  let wide = false;
  for (const f of t.split(/[\t\n\v\f\r ]+/).filter(Boolean)) {
    switch (f) {
      case "const": case "volatile": case "register": case "static": case "auto": case "int": case "long": break;
      case "unsigned": case "size_t": case "uintptr_t": case "uintmax_t": case "uint32_t": case "uint64_t": case "u_int":
      case "u_long": case "uint": case "ulong": case "rsize_t": wide = true; break;
      default: return false;
    }
  }
  return wide;
}

const RARE_KEYS = ["SwitchInLoop", "LibmInLoop", "DivInLoop", "StrlenInLoop", "RetNull", "RetNeg", "RetZero", "RetVal", "RetVoid",
  "NFnptrCalls", "NMacroCalls", "NExternalCalls", "NFree", "NConstCast", "NToctou", "NLockAcquire", "NLockRelease", "NNarrowCast",
  "NSignCmp", "NVariadicFmt", "NMemcpy", "NAllocsite", "NGlobalWrite", "NErrno", "NWeakRandom", "NShiftVar", "NReallocSelf", "NVla",
  "NGetenv", "NAssertSide", "NFreeThenUse", "NEpoll", "NUring", "NKqueue", "NEventWait", "NEtReg", "NOneshotReg", "NWriteReady",
  "NErrFlag", "NRearm", "NDereg", "NEagain", "NEintr", "NUringRes", "NUringRing", "NUringBarrier", "NUringSqpoll", "NNonblockSet",
  "NEventCreate", "NEventDestroy", "NUringSqe", "NUringSeen", "NUringUdata", "NUringLink", "NUringStreamOps", "NUringTeardown",
  "NEvTimeoutIndefinite", "NEvTimeoutZero", "NEvBatchOne", "NKqTimer", "NKqTimerZeroData", "NKqReceipt", "NRetNegCheck",
  "NDomainGuard", "NUcharCast", "NErrnoZero", "NEndptr", "NVaEnd", "NMapFailed", "NMonotonicClock", "NStackszArray", "NPtrOvfCheck",
  "NCallocTransposed", "NVaArgArr"];
export const SYM_KEYS = ["NParams", "IsPublic", "IsStatic", "IsTest", "IsEntrypoint", "IsGenerated", "Sloc", "NCommentLines", "HasDoc",
  "Cyclomatic", "Cognitive", "MaxNesting", "NTokens", "NOperators", "NOperands", "NDistinctOperators", "NDistinctOperands", "NLoops",
  "NBranches", "NReturns", "NSwitch", "NCases", "NLabels", "NGotos", "MaxLoopDepth", "CallInLoop", "AllocInLoop", "IOInLoop",
  "LockInLoop", "BranchInLoop", "NLocals", "NCmp", "NShift", "NArith", "NFloatLit", "NMagic", "NNullCheck", "NCalls", "NDynamicCalls",
  "NUnresolvedCalls", "FanIn", "FanOut", "NCallsites", "IsRecursive", "RiskScore", "NMemory", "NAlloc", "NIO", "NStdio", "NExec",
  "NLibm", "NInteger", "NConcurrency", "NReentrancy", "IsInline", "IsVariadic", "NPtrLocals", "NDeref", "NCast", "NSizeof",
  "NIntrinsic", "NAtomic", "NRestrict", "NLikely", "NBuiltin", ...RARE_KEYS];

// A symbol row with every counter at 0.
function newSym(o) {
  const s = { ID: 0, FileID: 0, ModuleID: 0, Name: "", QualName: "", Kind: "", LineStart: 0, Signature: "", HasSignature: false,
    ReturnType: "", HasReturnType: false };
  for (const k of SYM_KEYS) s[k] = 0;
  return Object.assign(s, o);
}

class Walk {
  constructor(marks, ep) {
    this.mk = marks; this.ep = ep;
    for (const k of ["cyclomatic", "cognitive", "maxNesting", "nLoops", "nBranches", "nSwitch", "nCases", "nLabels", "nGotos",
      "nReturns", "nDeref", "nCast", "nSizeof", "nArith", "nCmp", "nShift", "nLogical", "nTernary", "nAssign", "nCompoundAssign",
      "nIncdec", "nFloatLit", "nMagic", "nNullCheck", "nPtrLocals", "nVla", "nOperators", "nOperands", "nDistOps", "nDistOper",
      "hasVolatile", "nStaticAssert", "nAtomic", "nIntrinsic", "nLikely", "nBuiltin", "nGetenv", "nErrno", "maxLoopDepth",
      "callInLoop", "allocInLoop", "libmInLoop", "strlenInLoop", "ioInLoop", "lockInLoop", "switchInLoop", "branchInLoop",
      "divInLoop", "retNull", "retNeg", "retZero", "retVal", "retVoid", "nStringLit", "nAddrof", "nArrow", "nDot", "nSubscript",
      "nBitop", "nGlobalWrite", "nSignCmp", "frameBytes", "frameUnsized", "frameMax", "nDivByVar", "nForever", "nForeverNoX"]) this[k] = 0;
    this.locals = []; this.lits = []; this.addrs = []; this.calls = [];
    this.opSeen = new Set(); this.operSeen = new Set();
    this.toctouChk = null; this.toctouOpn = null; this.castIdents = [];
    this.shadow = null; this.types = null; this.refs = []; this.wantRefs = false; this.fileScope = false;
    this.divisors = []; this.zeroTested = null; this.discards = null; this.discardOrd = [];
  }
  has(i, bit) { const v = this.mk[i]; return (v & ~7) === this.ep && (v & bit) !== 0; }
  set(i, bit) { if ((this.mk[i] & ~7) !== this.ep) this.mk[i] = this.ep; this.mk[i] |= bit; }
  noteOp(s) { this.nOperators++; if (!this.opSeen.has(s)) { this.opSeen.add(s); this.nDistOps++; } }
  noteOper(s) { this.nOperands++; if (!this.operSeen.has(s)) { this.operSeen.add(s); this.nDistOper++; } }
  takeRefs() {
    const out = [];
    for (const r of this.refs) {
      if (this.has(r.node, MK_CALLEE)) continue;
      out.push({ Name: jtrunc(r.name, 80), Line: r.line, Kind: this.has(r.node, MK_AMP) ? "addr" : "ref" });
    }
    return out;
  }
}

// Per-unit state shared by every file derived from it.
export class UnitState {
  constructor(fa) {
    this.fa = fa;
    this.marks = new Int32Array(fa.nodes.length + 1);
    this.ep = 0;
    this.funcSet = null;
  }
  newEpoch() { this.ep += 8; return this.ep; }
}

export class DerivedFile {
  constructor(us, pp, raw, srcID, file) {
    this.us = us; this.fa = us.fa; this.pp = pp; this.srcRaw = raw; this.srcID = srcID;
    this.file = file;
    this.globalFileID = file.id; this.modID = file.moduleID; this.isTestF = file.isTest; this.isGenF = file.isGenerated;
    this.relPath = "";
    this.docLines = 0;
    for (const t of ["symbols", "params", "fields", "locals", "lits", "markers", "attrs", "imports", "hazards", "enums", "layout",
      "ssize", "decls", "addrs", "secrets", "allocs", "memops", "macros", "globals", "cfgs", "locks", "evops", "apiuses",
      "pendNames", "pendLines", "pendSym", "pendFile", "pendModule", "fnNames", "fnSyms", "fnFiles", "fnStatic", "fnModules",
      "macroName"]) this[t] = [];
    this.funcSymByNode = new Map();
    this.curFuncNode = 0; this.lastFuncNode = 0;
    this.tagSz = null; this.typeSz = null; this.lastTag = 0;
    this.declared = new Set(); this.macroSID = new Map();
    this.incSeen = new Set(); this.incRows = [];
    this.fnSpans = null; this.glSet = null; this.fnNameSet = new Set();
    this.blank = H.blankBuf(raw);
    this.nlRaw = H.nlOffsets(raw);
    this.buildOffMap();
  }
  name(i) { return this.fa.name(i); }
  internSym(s) { this.symbols.push(s); return this.symbols.length; }

  buildOffMap() {
    const raw = this.srcRaw;
    const base = raw.length - stripBOM(raw).length;
    const src = raw.slice(base);
    this.offAt = []; this.offAdd = [];
    if (!/\r|\\\n|\\\r/.test(src)) { if (base !== 0) { this.offAt = [0]; this.offAdd = [base]; } return; }
    let last = -(1 << 30), k = 0, oo = 0;
    const n = src.length;
    for (let i = 0; i < n;) {
      const c = src.charCodeAt(i);
      if (c === 92 && i + 1 < n) {
        const d = src.charCodeAt(i + 1);
        if (d === 10) { oo += 2; i += 2; continue; }
        if (d === 13) { const adv = i + 2 < n && src.charCodeAt(i + 2) === 10 ? 3 : 2; oo += adv; i += adv; continue; }
      }
      const dd = oo - k + base;
      if (dd !== last) { this.offAt.push(k); this.offAdd.push(dd); last = dd; }
      k++;
      oo++; i++;
      if (c === 13 && i < n && src.charCodeAt(i) === 10) { oo++; i++; }
    }
  }
  rawOff(off) {
    const at = this.offAt;
    if (at.length === 0 || off < at[0]) return off;
    let lo = 0, hi = at.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (at[mid] <= off) lo = mid + 1; else hi = mid; }
    return off + this.offAdd[lo - 1];
  }
  rawRange(off, end) {
    if (end <= off) { const r = this.rawOff(off); return [r, r]; }
    return [this.rawOff(off), this.rawOff(end - 1) + 1];
  }
  rawSpan(off, end, cap) {
    [off, end] = this.rawRange(off, end);
    if (off < 0 || end < off || end > this.srcRaw.length) return "";
    return jtrunc(squeeze(this.srcRaw.slice(off, end), ""), cap);
  }
  funcSpans() {
    if (this.fnSpans === null) {
      this.fnSpans = [];
      const ns = this.fa.nodes;
      for (let i = 0; i < ns.length; i++) {
        const n = ns[i];
        if (n.k === 2/*FuncDef*/ && (n.file === 0 || n.file === this.srcID)) this.fnSpans.push([n.off, n.end, i]);
      }
    }
    return this.fnSpans;
  }
  insideFunc(off) {
    if (this.fnStart === undefined) {
      const sp = [...this.funcSpans()].sort((a, b) => a[0] - b[0]);
      this.fnStart = sp.map((s) => s[0]);
      this.fnMaxEnd = [];
      let mx = -Infinity;
      for (const s of sp) { if (s[1] > mx) mx = s[1]; this.fnMaxEnd.push(mx); }
    }
    const st = this.fnStart;
    let lo = 0, hi = st.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (st[mid] <= off) lo = mid + 1; else hi = mid; }
    return lo > 0 && this.fnMaxEnd[lo - 1] > off;
  }
  globalSet() {
    if (this.glSet === null) {
      const m = new Set(this.globals.map((g) => g.Name));
      const fa = this.fa;
      for (const n of fa.nodes) {
        if (n.k !== 3/*Decl*/ || n.file === 0 || n.file === this.srcID) continue;
        const d = fa.decls[n.a - 1];
        if (d.kind === 0 && d.nameIdx !== 0 && (d.storage & 8/*Extern*/) !== 0) m.add(fa.name(d.nameIdx));
      }
      this.glSet = m;
    }
    return this.glSet;
  }
  tuFunc(name) {
    if (this.us.funcSet === null) {
      const fa = this.fa, m = new Set();
      for (const f of fa.funcs) m.add(fa.name(f.nameIdx));
      for (const d of fa.decls) if (d.kind === 1 && d.nameIdx !== 0) m.add(fa.name(d.nameIdx));
      this.us.funcSet = m;
    }
    return this.us.funcSet.has(name);
  }
  newWalk() { return new Walk(this.us.marks, this.us.newEpoch()); }

  derive() {
    const fa = this.fa, ns = fa.nodes;
    for (let ni = 0; ni < ns.length; ni++) {
      const n = ns[ni];
      if (n.file !== 0 && n.file !== this.srcID) continue;
      switch (n.k) {
        case 6/*StructSpec*/: case 7/*UnionSpec*/: case 8/*EnumSpec*/: {
          const td = fa.tags[n.a - 1];
          if (td.defined) this.deriveTag(td, n.a);
          break;
        }
        case 3/*Decl*/: {
          const d = fa.decls[n.a - 1];
          if (this.insideFunc(n.off)) break;
          if (d.kind === 2) this.deriveTypedef(d, n);
          else if (d.kind === 1) this.derivePrototype(d, n);
          else if (d.nameIdx !== 0) this.deriveGlobal(d, n);
          break;
        }
        case 2/*FuncDef*/:
          this.curFuncNode = ni;
          this.deriveFunction(fa.funcs[n.a - 1], n);
          break;
      }
    }
    for (let ni = 0; ni < ns.length; ni++) {
      const n = ns[ni];
      if (n.k !== 3/*Decl*/ || (n.file !== 0 && n.file !== this.srcID)) continue;
      const d = fa.decls[n.a - 1];
      if (d.init === 0 || this.insideFunc(n.off)) continue;
      const w = this.newWalk();
      w.wantRefs = true; w.fileScope = true;
      this.walkStmt(w, d.init, 1, 0);
      for (const l of w.lits) { l.FileID = this.srcID; l.ID = this.lits.length + 1; this.lits.push(l); }
      for (const a of w.takeRefs()) { a.FileID = this.srcID; a.ID = this.addrs.length + 1; a.SymbolID = 0; a.HasSym = false; this.addrs.push(a); }
    }
    for (const m of this.pp.allMacros) if (m.fileID === this.srcID) this.deriveMacro(m);
    H.scanMarkersRaw(this.srcRaw, (kind, line, text) => this.markers.push({ FileID: this.srcID, Kind: kind, Line: line, Text: text, SymbolID: 0, HasSym: false }));
    for (const c of this.pp.cfgs) {
      if (c.fileID !== this.srcID || c.directive === "endif") continue;
      const expr = jtrunc(trimSpace(c.expr), 160);
      this.cfgs.push({ FileID: this.srcID, Directive: c.directive, Expr: expr, Line: c.line,
        IsConfig: b2i(expr.includes("CONFIG_") || expr.includes("HAVE_") || expr.includes("USE_")) });
    }
    for (const inc of this.pp.includes) {
      if (inc.fromID !== this.srcID) continue;
      this.imports.push({ FileID: this.srcID, Target: inc.target, Kind: "include", Line: inc.line, IsExternal: 1, NNames: 1, IsRelative: b2i(!inc.sys) });
      if (!this.incSeen.has(inc.line)) { this.incSeen.add(inc.line); this.incRows.push(inc); }
    }
    this.deriveAttrs();
  }

  deriveAttrs() {
    for (const a of this.fa.attrs) {
      const row = { FileID: this.srcID, Name: jtrunc(a.name, 80), Args: jtrunc(a.args, 200), HasArgs: a.args !== "", Line: a.line,
        HasSymbolID: false, SymbolID: 0 };
      let best = 0;
      for (const s of this.funcSpans()) if (a.off >= s[0] && a.off < s[1]) { best = s[2]; break; }
      if (this.funcSymByNode.has(best)) { row.HasSymbolID = true; row.SymbolID = this.funcSymByNode.get(best); }
      this.attrs.push(row);
    }
  }

  deriveTag(td, tagIdx) {
    const fa = this.fa;
    let name = fa.name(td.nameIdx);
    if (name === "") name = "(anon@" + td.line + ")";
    const kind = td.tag;
    const localsym = this.internSym(newSym({ FileID: this.srcID, Name: name, QualName: name, Kind: kind, LineStart: td.line, IsPublic: 1 }));
    if (kind === "struct" || kind === "union") {
      const isUnion = kind === "union";
      const flds = [];
      let ord = 0, nestN = null, hide = null, inUni = null, lastAnon = 0;
      const anonAt = new Map(), spans = [];
      for (let ci = 0; ci < td.nChild; ci++) {
        const sn = fa.nodes[td.child0 + ci];
        if (sn.k !== 6/*StructSpec*/ && sn.k !== 7/*UnionSpec*/) continue;
        if (sn.a === 0 || sn.a === tagIdx) continue;
        const it = fa.tags[sn.a - 1];
        if (!it.defined || it.child0 < td.child0 || it.child0 + it.nChild > td.child0 + td.nChild) continue;
        spans.push({ lo: it.child0 - td.child0, hi: it.child0 - td.child0 + it.nChild, at: ci, union: sn.k === 7/*UnionSpec*/ });
        if (it.nameIdx === 0) anonAt.set(ci, sn.a);
      }
      if (spans.length) {
        nestN = new Int32Array(td.nChild); hide = new Uint8Array(td.nChild); inUni = new Uint8Array(td.nChild);
        for (const sp of spans) for (let k = sp.lo; k < sp.hi; k++) nestN[k]++;
        const seen = new Uint8Array(td.nChild);
        for (const sp of spans) {
          for (let k = sp.lo; k < sp.hi; k++) if (!seen[k]) { seen[k] = 1; inUni[k] = sp.union ? 1 : 0; }
          let named = true;
          for (let k = sp.at + 1; k < td.nChild; k++) {
            const fn = fa.nodes[td.child0 + k];
            if (fn.k !== 9/*Field*/ || nestN[k] !== nestN[sp.at]) continue;
            named = fa.fields[fn.a - 1].nameIdx !== 0;
            break;
          }
          if (named) for (let k = sp.lo; k < sp.hi; k++) hide[k] = 1;
        }
      }
      for (let ci = 0; ci < td.nChild; ci++) {
        if (anonAt.has(ci) && (nestN === null || nestN[ci] === 0)) lastAnon = anonAt.get(ci);
        if (hide !== null && hide[ci]) continue;
        const fn = fa.nodes[td.child0 + ci];
        if (fn.k !== 9/*Field*/) continue;
        const fd = fa.fields[fn.a - 1];
        const fname = fa.name(fd.nameIdx), ftype = fa.name(fd.ptrTypeIdx);
        this.fields.push({ SymbolID: localsym, Ordinal: ord, Name: jtrunc(fname, 80), Type: jtrunc(ftype, 160), Line: fd.line,
          IsConst: b2i(fd.isConst), IsMutable: b2i(!fd.isConst), IsNullable: b2i(fd.ptr > 0), IsCollection: b2i(fd.arrayLen !== 0),
          TypeDepth: fd.ptr });
        const f = { ordinal: ord, ftype: fa.name(fd.typeIdx), fname, ptr: fd.ptr, alen: fd.arrayLen, isFnptr: b2i(fd.isFnptr),
          depth: 0, inUnion: 0, line: fd.line, kn: false, ksz: 0, kal: 0, kex: 0 };
        if (nestN !== null && nestN[ci] > 0) { f.depth = nestN[ci]; f.inUnion = inUni[ci]; }
        if (fd.ptr === 0 && !fd.isFnptr && f.depth === 0) {
          const base = aggBase(f.ftype);
          let v;
          if (base === "struct" || base === "union") v = this.tagSz?.get(lastAnon);
          else if (base === "enum" || base.startsWith("enum ")) v = [4, 4, 1];
          else v = this.typeSz?.get(base);
          if (v) { f.kn = true; [f.ksz, f.kal, f.kex] = v; }
        }
        flds.push(f);
        ord++;
      }
      const [lay, tot, tail, ex, mal] = layoutStruct(flds, isUnion);
      let tpad = tail;
      for (let i = 0; i < lay.length; i++) {
        if (i < flds.length) tpad += lay[i][3];
        this.layout.push({ SymbolID: localsym, Ordinal: lay[i][0], ByteOff: lay[i][1], ByteSize: lay[i][2], PadBefore: lay[i][3],
          Exact: lay[i][4], PtrDepth: flds[i].ptr, ArrayLen: flds[i].alen, IsFnptr: flds[i].isFnptr, Depth: flds[i].depth,
          InUnion: flds[i].inUnion });
      }
      this.ssize.push({ SymbolID: localsym, TotalSize: tot, TailPad: tail, TotalPad: I32(tpad), MaxAlign: mal, Exact: ex,
        NLines64: tot > 0 ? Math.trunc((tot + 63) / 64) : 0 });
      if (this.tagSz === null) { this.tagSz = new Map(); this.typeSz = new Map(); }
      this.tagSz.set(tagIdx, [tot, mal, ex]);
      this.lastTag = tagIdx;
      if (!isUnion && ex === 1) {
        const best = packedSize(flds);
        if (best > 0 && best < tot) this.hazards.push({ SymbolID: localsym, Pattern: "packed_size", Category: "shape", N: best, FirstLine: 0 });
      }
      if (td.nameIdx !== 0) this.typeSz.set(kind + " " + fa.name(td.nameIdx), [tot, mal, ex]);
    } else if (kind === "enum") {
      let ord = 0;
      for (let ci = 0; ci < td.nChild; ci++) {
        const en = fa.nodes[td.child0 + ci];
        if (en.k !== 10/*Enumerator*/) continue;
        const ed = fa.enums[en.a - 1];
        this.enums.push({ SymbolID: localsym, Ordinal: ord++, Name: jtrunc(fa.name(ed.nameIdx), 80), HasValue: ed.hasValue,
          Value: ed.hasValue ? jtrunc(fa.name(ed.valueIdx), 60) : "", NFields: 0 });
      }
    }
  }

  deriveTypedef(d, n) {
    const name = this.name(d.nameIdx);
    if (name === "") return;
    const ty = this.name(d.typeIdx);
    if (d.ptr === 0 && d.arrayLen === 0 && this.typeSz !== null) {
      const base = aggBase(ty);
      if (base === "struct" || base === "union") { const v = this.tagSz.get(this.lastTag); if (v) this.typeSz.set(name, v); }
      else if (base === "enum" || base.startsWith("enum ")) this.typeSz.set(name, [4, 4, 1]);
      else { const v = this.typeSz.get(base); if (v) this.typeSz.set(name, v); }
    }
    this.symbols.push(newSym({ FileID: this.srcID, Name: name, QualName: name, Kind: "typedef", LineStart: n.line,
      ReturnType: jtrunc(ty, 120), HasReturnType: true, IsPublic: 1 }));
  }
  derivePrototype(d, n) {
    const name = this.name(d.nameIdx);
    if (name === "") return;
    this.declared.add(name);
    this.decls.push({ FileID: this.srcID, Name: name, Line: n.line });
  }
  deriveGlobal(d, n) {
    const name = this.name(d.nameIdx);
    if (name === "") return;
    const ty = this.name(d.typeIdx);
    this.globals.push({ FileID: this.srcID, Name: name, Type: jtrunc(squeeze(ty, ""), 120), Line: n.line,
      IsStatic: b2i(d.storage & 4/*Static*/), IsConst: b2i((d.storage & 256/*Constexpr*/) !== 0 || ty.includes("const")),
      IsVolatile: b2i(ty.includes("volatile")), IsAtomic: b2i(ty.includes("_Atomic")), IsArray: b2i(d.arrayLen !== 0),
      PtrDepth: d.ptr, HasInit: b2i(d.init !== 0) });
    if (this.glSet !== null) this.glSet.add(name);
  }
  deriveMacro(m) {
    const nParams = m.params.filter((p) => trimSpace(p) !== "").length;
    const sig = jtrunc("define " + m.name + " " + m.raw, 200);
    const sid = this.internSym(newSym({ FileID: this.srcID, Name: m.name, QualName: m.name, Kind: "macro", LineStart: m.line,
      Signature: sig, HasSignature: true }));
    this.macroName.push(m.name);
    if (!this.macroSID.has(m.name)) this.macroSID.set(m.name, sid);
    this.macros.push({ SymbolID: sid, IsFunctionlike: b2i(m.isFn), NParams: nParams, Body: jtrunc(m.raw, 500), HasBody: true,
      BodyLen: runeCount(m.raw), IsMultiline: b2i(m.multi), NUses: m.uses });
  }

  deriveFunction(fd, n) {
    const fa = this.fa;
    this.lastFuncNode = this.curFuncNode;
    const name = fa.name(fd.nameIdx), sig = fa.name(fd.sigIdx), ret = fa.name(fd.retIdx);
    const isStatic = (fd.storage & 4/*Static*/) !== 0;
    const qual = isStatic ? this.relPath + ":" + name : name;
    const sym = newSym({ FileID: this.srcID, Name: name, QualName: jtrunc(qual, 400), Kind: "function", LineStart: n.line,
      Signature: jtrunc(sig, 400), HasSignature: true, ReturnType: jtrunc(ret, 120), HasReturnType: true,
      IsStatic: b2i(isStatic), IsInline: b2i(fd.storage & 16/*Inline*/), IsPublic: b2i(!isStatic),
      IsEntrypoint: b2i(name === "main" || name === "LLVMFuzzerTestOneInput"), IsVariadic: b2i(fd.variadic), NParams: fd.nParams,
      IsTest: this.isTestF, IsGenerated: this.isGenF });
    const w = this.newWalk();
    w.wantRefs = true;
    w.toctouChk = new Set(); w.toctouOpn = new Set();
    w.shadow = new Set(); w.types = new Map();
    w.zeroTested = new Set(); w.discards = new Map();
    for (let pi = 0; pi < fd.nParams; pi++) {
      const pd = fa.params[fa.nodes[fd.param0 + pi].a - 1];
      const nm = fa.name(pd.nameIdx);
      if (nm !== "") { w.shadow.add(nm); w.types.set(nm, fa.name(pd.typeIdx)); }
    }
    if (fd.body !== 0) this.walkStmt(w, fd.body, 1, 0);
    Object.assign(sym, {
      Cyclomatic: w.cyclomatic + 1, Cognitive: w.cognitive, MaxNesting: w.maxNesting, NLoops: w.nLoops, NBranches: w.nBranches,
      NSwitch: w.nSwitch, NCases: w.nCases, NLabels: w.nLabels, NGotos: w.nGotos, NReturns: w.nReturns, NDeref: w.nDeref,
      NCast: w.nCast, NSizeof: w.nSizeof, NArith: w.nArith, NCmp: w.nCmp, NShift: w.nShift, NMagic: w.nMagic,
      NFloatLit: w.nFloatLit, NPtrLocals: w.nPtrLocals, MaxLoopDepth: w.maxLoopDepth, NIntrinsic: w.nIntrinsic, NAtomic: w.nAtomic,
      NLikely: w.nLikely, NBuiltin: w.nBuiltin, NGetenv: w.nGetenv, NErrno: w.nErrno, NVla: w.nVla, NOperators: w.nOperators,
      NOperands: w.nOperands, NTokens: w.nOperators + w.nOperands, NGlobalWrite: w.nGlobalWrite, NDistinctOperators: w.nDistOps,
      NDistinctOperands: w.nDistOper, NNullCheck: w.nNullCheck, CallInLoop: w.callInLoop, AllocInLoop: w.allocInLoop,
      IOInLoop: w.ioInLoop, LockInLoop: w.lockInLoop, BranchInLoop: w.branchInLoop, SwitchInLoop: w.switchInLoop,
      LibmInLoop: w.libmInLoop, DivInLoop: w.divInLoop, StrlenInLoop: w.strlenInLoop, RetNull: w.retNull, RetNeg: w.retNeg,
      RetZero: w.retZero, RetVal: w.retVal, RetVoid: w.retVoid,
    });
    if (fd.body !== 0) {
      const [b0, b1] = this.rawRange(fa.nodes[fd.body].off, fa.nodes[fd.body].end);
      if (b0 >= 0 && b1 <= this.srcRaw.length && b1 >= b0) { const cl = classifyLines(this.srcRaw.slice(b0, b1)); sym.Sloc = cl.code; sym.NCommentLines = cl.cmt; }
    }
    let nDyn = 0;
    for (const c of w.calls) if (c.fnptr) nDyn++;
    sym.NCalls = w.calls.length; sym.NDynamicCalls = nDyn; sym.NFnptrCalls = nDyn; sym.NCallsites = sym.NCalls;
    const sid = this.internSym(sym);
    sym.ID = sid;
    this.funcSymByNode.set(this.lastFuncNode, sid);
    let np = 0, sawEllipsis = false;
    for (let pi = 0; pi < fd.nParams; pi++) {
      const pd = fa.params[fa.nodes[fd.param0 + pi].a - 1];
      const pname = fa.name(pd.nameIdx), ptype = fa.name(pd.typeIdx);
      if (ptype === "" && pname === "void") continue;
      if (pname === "..." || pd.variadic) {
        this.params.push(paramRow(sid, np, { IsVariadic: 1, IsMutable: 1, Type: "..." }));
        sym.IsVariadic = 1;
        sawEllipsis = true;
        continue;
      }
      const pa = paramRow(sid, np, { Type: jtrunc(ptype, 120), IsRef: b2i(pd.ptr > 0), IsMutable: b2i(!pd.isConst),
        IsNullable: b2i(pd.ptr > 0), TypeDepth: pd.ptr });
      if (pname !== "") { pa.Name = jtrunc(pname, 80); pa.HasName = true; }
      this.params.push(pa);
      if (pd.ptr > 0) sym.NPtrParams = (sym.NPtrParams || 0) + 1;
      np++;
    }
    sym.NParams = np;
    if (fd.variadic && !sawEllipsis) this.params.push(paramRow(sid, np, { IsVariadic: 1, IsMutable: 1, Type: "..." }));
    sym.NLocals = w.locals.length;
    w.locals.forEach((l, i) => { l.SymbolID = sid; l.Ordinal = i; this.locals.push(l); });
    let nLit = 0;
    for (const l of w.lits) {
      if (l.Kind === "int" || l.Kind === "hex" || l.Kind === "oct" || l.Kind === "bin") { if (nLit >= 200) continue; nLit++; }
      l.SymbolID = sid; l.HasSym = true; l.FileID = this.srcID; l.ID = this.lits.length + 1;
      this.lits.push(l);
    }
    for (const a of w.addrs) { a.SymbolID = sid; a.HasSym = true; a.FileID = this.srcID; a.ID = this.addrs.length + 1; a.Kind = "addr"; this.addrs.push(a); }
    for (const a of w.takeRefs()) { a.SymbolID = sid; a.HasSym = true; a.FileID = this.srcID; a.ID = this.addrs.length + 1; a.Kind = "fn" + a.Kind; this.addrs.push(a); }
    const x = this.applyCallHeuristics(sid, w);
    Object.assign(sym, { NMemcpy: x.nMemcpy, NAlloc: x.nAlloc, NAllocsite: x.nAllocsite, NFree: x.nFree, NLockAcquire: x.nLockAcq,
      NLockRelease: x.nLockRel, NEpoll: x.nEpoll, NUring: x.nUring, NKqueue: x.nKqueue, NEventCreate: x.nEventCreate,
      NEventWait: x.nEventWait, NMemory: x.nMemcpy + x.nAlloc + x.nFree, NEvTimeoutIndefinite: x.nTimeoutInf,
      NEvTimeoutZero: x.nTimeoutZero, NEvBatchOne: x.nBatchOne, NKqTimerZeroData: x.nKqTimerZero });
    this.textHeuristics(sym, w, fd, n);
    const byName = new Map(), order = [];
    for (const c of w.calls) {
      if (c.fnptr) continue;
      if (!byName.has(c.name)) { order.push(c.name); byName.set(c.name, []); }
      byName.get(c.name).push(c.line);
    }
    for (const nm of order) {
      this.pendNames.push(nm); this.pendLines.push(byName.get(nm)); this.pendSym.push(sid);
      this.pendFile.push(this.globalFileID); this.pendModule.push(this.modID);
    }
    this.fnNames.push(name); this.fnNameSet.add(name);
    this.fnSyms.push(sid); this.fnFiles.push(this.globalFileID); this.fnStatic.push(isStatic); this.fnModules.push(this.modID);
  }

  textHeuristics(sym, w, fd, n) {
    if (fd.body === 0) return;
    const fa = this.fa;
    const [b0, b1] = this.rawRange(fa.nodes[fd.body].off, fa.nodes[fd.body].end);
    if (b0 < 0 || b1 > this.srcRaw.length || b1 < b0) return;
    const rawBody = this.srcRaw.slice(b0, b1);
    const blankBody = b1 <= this.blank.length ? this.blank.slice(b0, b1) : rawBody;
    const [doc, nd] = H.leadingComment(this.srcRaw, this.nlRaw, n.line);
    if (doc) { sym.HasDoc = 1; this.docLines += nd; }
    const h0 = this.rawOff(n.off);
    if (h0 >= 0 && h0 <= b0) {
      const def = b1 <= this.blank.length ? this.blank.slice(h0, b1) : this.srcRaw.slice(h0, b1);
      if (def.includes("restrict")) sym.NRestrict = H.countWord(def, "restrict") + H.countWord(def, "__restrict") + H.countWord(def, "__restrict__");
    }
    if (blankBody.includes("8_t") || blankBody.includes("16_t") || blankBody.includes("32_t")) sym.NNarrowCast = H.countNarrowCasts(blankBody);
    const known = new Set(w.locals.map((l) => l.Name));
    const paramDs = [];
    for (let pi = 0; pi < fd.nParams; pi++) {
      const pd = fa.params[fa.nodes[fd.param0 + pi].a - 1];
      paramDs.push(pd);
      const nm = fa.name(pd.nameIdx);
      if (nm !== "") known.add(nm);
    }
    sym.NSignCmp = w.nSignCmp;
    if (blankBody.includes("rand")) sym.NWeakRandom = H.countWeakRandom(blankBody);
    if (blankBody.includes("<<") || blankBody.includes(">>")) sym.NShiftVar = H.countShiftVar(blankBody);
    if (blankBody.includes("realloc")) sym.NReallocSelf = H.countReallocSelf(blankBody);
    if (blankBody.includes("assert")) sym.NAssertSide = H.countAssertSide(blankBody);
    if (blankBody.includes("free")) {
      let k = 0;
      H.scanFrees(blankBody, (nm, after) => { if (after < blankBody.length && H.derefAfter(blankBody.slice(after), nm)) k++; });
      sym.NFreeThenUse = k;
    }
    let nCC = 0;
    for (const cn of w.castIdents) {
      if (!known.has(cn)) continue;
      for (const l of w.locals) if (l.Name === cn && l.IsConst === 1) { nCC++; break; }
      for (const pd of paramDs) if (fa.name(pd.nameIdx) === cn && pd.isConst) { nCC++; break; }
    }
    sym.NConstCast = nCC;
    let tt = 0;
    for (const k of w.toctouChk) if (w.toctouOpn.has(k)) tt++;
    sym.NToctou = tt;
    if (fd.variadic) {
      let nf = 0;
      for (const c of w.calls) if (PRINTF_FNS.includes(c.name)) nf++;
      sym.NVariadicFmt = nf;
    }
    let nEv = 0;
    for (let i = this.evStart; i < this.evops.length; i++) if (this.evops[i].SymbolID === sym.ID) nEv++;
    if (nEv > 0) {
      const fl = H.scanEvFlags(blankBody);
      Object.assign(sym, { NEtReg: fl.etReg, NOneshotReg: fl.oneshotReg, NWriteReady: fl.writeReady, NErrFlag: fl.errFlag,
        NRearm: fl.rearm, NDereg: fl.dereg, NEagain: fl.eagain, NEintr: fl.eintr, NUringRes: fl.uringRes, NUringSqpoll: fl.uringSqpoll,
        NNonblockSet: fl.nonblockSet, NEventDestroy: fl.eventDestroy, NUringSqe: fl.uringSqe, NUringSeen: fl.uringSeen,
        NUringUdata: fl.uringUdata, NUringLink: fl.uringLink, NUringStreamOps: fl.uringStreamOps, NUringTeardown: fl.uringTeardown,
        NKqTimer: fl.kqTimer, NKqReceipt: fl.kqReceipt, NUringRing: fl.uringRing, NUringBarrier: fl.uringBarrier });
    }
    const usedNS = new Set();
    for (let i = this.apiStart; i < this.apiuses.length; i++) { const a = this.apiuses[i]; if (a.SymbolID === sym.ID) usedNS.add(a.NS); }
    if (usedNS.size > 0) {
      for (const gn of GUARD_NAMES) {
        if (!Object.keys(GUARD_NS[gn] || {}).some((k) => usedNS.has(k))) continue;
        sym[GUARD_COL[gn]] = H.countGuard(gn, blankBody);
      }
    }
    const emitHz = (pat, cat, k) => { if (k > 0) this.hazards.push({ SymbolID: sym.ID, Pattern: pat, Category: cat, N: k, FirstLine: 0 }); };
    emitHz("ptr_cast", "integer", H.countCast(blankBody));
    if (blankBody.includes("<<") || blankBody.includes(">>")) emitHz("shift", "integer", H.countStr(blankBody, "<<") + H.countStr(blankBody, ">>"));
    if (blankBody.includes("sizeof")) emitHz("mul_sizeof", "integer", H.countMulSizeof(blankBody));
    emitHz("fixed_buffer", "memory", H.countFixedBuffer(blankBody));
    emitHz("vla", "memory", H.countVLAPat(blankBody));
    emitHz("signed_cmp", "integer", H.countSignedCmp(blankBody));
    let unguarded = 0;
    for (const d of w.divisors) if (!w.zeroTested.has(d)) unguarded++;
    emitHz("frame_bytes", "shape", Math.min(w.frameBytes, 1 << 30));
    emitHz("frame_unsized", "shape", w.frameUnsized);
    emitHz("frame_max_local", "shape", w.frameMax);
    emitHz("div_by_var", "shape", w.nDivByVar);
    emitHz("div_unguarded", "shape", unguarded);
    emitHz("loop_forever", "shape", w.nForever);
    emitHz("loop_forever_noexit", "shape", w.nForeverNoX);
    for (const nm of w.discardOrd) emitHz(jtrunc(nm, 80), "discard", w.discards.get(nm));
  }

  applyCallHeuristics(sid, w) {
    this.evStart = this.evops.length; this.apiStart = this.apiuses.length;
    const x = { nMemcpy: 0, nAlloc: 0, nAllocsite: 0, nAllocNoSizeof: 0, nFree: 0, nLockAcq: 0, nLockRel: 0, nEpoll: 0, nUring: 0,
      nKqueue: 0, nEventCreate: 0, nEventWait: 0, nTimeoutInf: 0, nTimeoutZero: 0, nBatchOne: 0, nKqTimerZero: 0 };
    const hazN = new Map(), hazFirst = new Map();
    let apiRows = 0;
    for (const c of w.calls) {
      const name = c.name;
      const isMem = MEMOP_FNS.has(name), isAl = isAllocName(name), isEv = isEventOp(name);
      let [, acq, rel] = H.lockShape(name);
      let argText = "", args = null;
      if (isMem || isAl || acq || rel || isEv) {
        if (c.argsTxt === null) {
          const o = { s: "", len: 0 }, cn = c.argsNode;
          for (let i = 0; i < cn.b; i++) { if (i > 0) { o.s += ", "; o.len += 2; } this.exprText(o, cn.c + i, 0); }
          c.argsTxt = o.s;
        }
        argText = c.argsTxt === "" ? this.rawSpan(c.argsOff, c.argsEnd, 200) : jtrunc(c.argsTxt, 200);
        args = splitTop(argText);
      }
      if (isMem) {
        const [dst, src, sarg] = memopArgs(name, args);
        let sizeBuf = "";
        const op = sizeofOperand(sarg);
        if (op !== null) { const nm = H.firstIdent(op); if (nm !== null) sizeBuf = nm; }
        else { const nm = H.firstIdent(sarg); if (nm !== null && sarg !== "" && sarg[0] !== "-" && !(sarg[0] >= "0" && sarg[0] <= "9")) sizeBuf = nm; }
        let dt = H.lastPathTail(dst);
        if (dt === "") dt = dst;
        this.memops.push({ ID: this.memops.length + 1, SymbolID: sid, FileID: this.srcID, Fn: name, Dst: jtrunc(dst, 120),
          Src: jtrunc(src, 120), SizeArg: jtrunc(sarg, 120), SizeBuf: jtrunc(sizeBuf, 80), DstTail: jtrunc(dt, 80), Line: c.line });
        x.nMemcpy++;
      }
      if (isAl) {
        if (LIBC_ALLOC.has(name)) x.nAlloc++;
        if (!c.fnptr && !isFreeName(name)) {
          const sz = jtrunc(squeeze(argText, ""), 150);
          this.allocs.push({ ID: this.allocs.length + 1, SymbolID: sid, FileID: this.srcID, Fn: name, SizeExpr: sz, Line: c.line });
          x.nAllocsite++;
          if (!sz.includes("sizeof")) x.nAllocNoSizeof++;
        }
      }
      if (name === "free" && !c.fnptr) x.nFree++;
      if (acq || rel) {
        if ((name === "flock" || name === "lockf") && (argText.includes("LOCK_UN") || argText.includes("F_ULOCK"))) { acq = false; rel = true; }
        if (acq) x.nLockAcq++;
        if (rel) x.nLockRel++;
        if (args.length > 0) {
          const nm = lockArgName(args[0]);
          if (nm !== "") this.locks.push({ ID: this.locks.length + 1, SymbolID: sid, FileID: this.srcID, Name: jtrunc(nm, 80), Line: c.line });
        }
      }
      if (isEv) {
        const a = jtrunc(squeeze(argText, ""), 180);
        this.evops.push({ ID: this.evops.length + 1, SymbolID: sid, FileID: this.srcID, Family: eventFamily(name), Fn: name, Args: a, Line: c.line });
        if (name.startsWith("epoll_")) x.nEpoll++;
        else if (name.startsWith("io_uring_")) x.nUring++;
        else x.nKqueue++;
        if (EVENT_CREATE.has(name)) x.nEventCreate++;
        if (EVENT_WAIT.has(name)) x.nEventWait++;
        if (has(EV_TIMEOUT_IDX, name)) {
          const ti = EV_TIMEOUT_IDX[name];
          const parts = splitTop(argText);
          const bi = EV_BATCH_IDX[name];
          if (parts.length > bi && parts[bi] === "1") x.nBatchOne++;
          if (parts.length > ti && parts.length > bi && parts[bi] !== "0") {
            const tmo = parts[ti];
            if (tmo === "NULL" || tmo === "-1") x.nTimeoutInf++;
            else if (tmo === "0") x.nTimeoutZero++;
          }
        } else if (name === "EV_SET") {
          const parts = splitTop(argText);
          if (parts.length > 5 && parts[2].includes("EVFILT_TIMER") && parts[5] === "0") x.nKqTimerZero++;
        }
      }
      const ns = funcToNS(name);
      if (ns !== "" && apiRows < 12) {
        this.apiuses.push({ ID: this.apiuses.length + 1, SymbolID: sid, FileID: this.srcID, NS: ns, Fn: name, Line: c.line });
        apiRows++;
      }
      const cat = hazardFuncs(name);
      if (cat !== "") {
        const k = name + "\0" + cat;
        if (!hazN.get(k)) hazFirst.set(k, c.line);
        hazN.set(k, (hazN.get(k) || 0) + 1);
      }
    }
    for (const c of w.calls) {
      const cat = hazardFuncs(c.name);
      const k = c.name + "\0" + cat;
      if (cat === "" || !hazN.get(k)) continue;
      this.hazards.push({ SymbolID: sid, Pattern: jtrunc(c.name, 80), Category: cat, N: hazN.get(k), FirstLine: hazFirst.get(k) });
      hazN.set(k, 0);
    }
    for (const l of w.lits) {
      if (l.Kind === "_secret") this.secrets.push({ ID: this.secrets.length + 1, SymbolID: sid, HasSym: true, FileID: this.srcID, Value: l.Value, Line: l.Line });
    }
    return x;
  }

  // ---- body walk helpers
  noteTested(w, idx, depth) {
    if (idx === 0 || depth > 8 || w.zeroTested === null) return;
    const nd = this.fa.nodes[idx];
    if (nd.k === 28/*Ident*/) w.zeroTested.add(this.name(nd.a));
    else if (nd.k === 49/*Paren*/ || nd.k === 38/*Unary*/) this.noteTested(w, nd.a, depth + 1);
    else if (nd.k === 39/*Binary*/) { this.noteTested(w, nd.a, depth + 1); this.noteTested(w, nd.b, depth + 1); }
  }
  constTrue(idx) {
    for (let hop = 0; idx !== 0 && hop < 4; hop++) {
      const nd = this.fa.nodes[idx];
      if (nd.k === 49/*Paren*/) idx = nd.a;
      else if (nd.k === 29/*NumLit*/) { const v = parseCInt(this.name(nd.a)); return v !== null && v !== 0n; }
      else if (nd.k === 32/*BoolLit*/) return nd.f === 1;
      else return false;
    }
    return false;
  }
  leavesLoop(idx) {
    const ns = this.fa.nodes;
    if (idx === 0) return false;
    let lo = idx, hi = idx + 1;
    const nd = ns[idx];
    if (nd.k === 13/*CompoundStmt*/) { lo = nd.a; hi = nd.a + nd.b; }
    else {
      lo = idx;
      while (lo > 0 && ns[lo - 1].off >= nd.off && ns[lo - 1].end <= nd.end && ns[lo - 1].k !== 13/*CompoundStmt*/) lo--;
    }
    for (let i = lo; i < hi && i < ns.length; i++) {
      const n = ns[i];
      if (n.k === 23/*Break*/ || n.k === 22/*Return*/ || n.k === 25/*Goto*/) return true;
      if (n.k === 34/*Call*/) {
        const c = ns[n.a];
        if (c.k === 28/*Ident*/) {
          switch (this.name(c.a)) {
            case "exit": case "_exit": case "_Exit": case "abort": case "longjmp": case "siglongjmp": case "pthread_exit":
            case "quick_exit": case "thrd_exit": case "__builtin_unreachable": case "__builtin_trap": return true;
          }
        }
      }
    }
    return false;
  }
  noteDivisor(w, idx) {
    for (let hop = 0; idx !== 0 && hop < 4; hop++) {
      const nd = this.fa.nodes[idx];
      if (nd.k === 49/*Paren*/) { idx = nd.a; continue; }
      if (nd.k === 29/*NumLit*/ || nd.k === 30/*CharLit*/ || nd.k === 44/*SizeofT*/ || nd.k === 45/*SizeofE*/ || nd.k === 46/*AlignofT*/) return;
      if (nd.k === 28/*Ident*/) { w.nDivByVar++; w.divisors.push(this.name(nd.a)); return; }
      break;
    }
    w.nDivByVar++;
  }
  noteLoop(w, cond, body, forever) {
    if (!forever && !this.constTrue(cond)) return;
    w.nForever++;
    if (!this.leavesLoop(body)) w.nForeverNoX++;
  }
  isNullLit(idx) {
    if (idx === 0) return false;
    const nd = this.fa.nodes[idx];
    if (nd.k === 28/*Ident*/) { const nm = this.name(nd.a); return nm === "NULL" || nm === "nullptr"; }
    if (nd.k === 49/*Paren*/) return this.isNullLit(nd.a);
    return false;
  }
  signedOperand(w, idx) {
    if (idx === 0 || w.types === null) return false;
    const nd = this.fa.nodes[idx];
    if (nd.k === 49/*Paren*/) return this.signedOperand(w, nd.a);
    if (nd.k === 28/*Ident*/) return signedIntType(w.types.get(this.name(nd.a)) ?? "");
    return false;
  }
  sizeOperand(w, idx, depth) {
    if (idx === 0 || depth > 6 || w.types === null) return false;
    const nd = this.fa.nodes[idx];
    switch (nd.k) {
      case 44/*SizeofT*/: case 45/*SizeofE*/: return true;
      case 49/*Paren*/: return this.sizeOperand(w, nd.a, depth + 1);
      case 28/*Ident*/: return unsignedSizeType(w.types.get(this.name(nd.a)) ?? "");
      case 34/*Call*/: {
        const c = this.fa.nodes[nd.a];
        if (c.k === 28/*Ident*/) {
          switch (this.name(c.a)) { case "strlen": case "strnlen": case "wcslen": case "strspn": case "strcspn": case "fread": case "fwrite": return true; }
        }
        break;
      }
      case 39/*Binary*/:
        if (nd.f === 38/*+*/ || nd.f === 39/*-*/ || nd.f === 37/***/ || nd.f === 42/*/*/) return this.sizeOperand(w, nd.a, depth + 1) || this.sizeOperand(w, nd.b, depth + 1);
    }
    return false;
  }
  writesGlobal(w, idx) {
    const ns = this.fa.nodes;
    for (let hop = 0; idx !== 0 && hop < 32; hop++) {
      const nd = ns[idx];
      if (nd.k === 28/*Ident*/) { const nm = this.name(nd.a); return nm !== "" && !w.shadow?.has(nm) && this.globalSet().has(nm); }
      if (nd.k === 35/*Member*/) idx = nd.b;
      else if (nd.k === 36/*Index*/ || nd.k === 49/*Paren*/) idx = nd.a;
      else return false;
    }
    return false;
  }
  retShape(idx) {
    const nd = this.fa.nodes[idx];
    switch (nd.k) {
      case 28/*Ident*/: { const nm = this.name(nd.a); if (nm === "NULL" || nm === "nullptr") return 1; break; }
      case 33/*NullPtr*/: return 1;
      case 49/*Paren*/: if (nd.a !== 0) return this.retShape(nd.a); break;
      case 38/*Unary*/:
        if (nd.f === 39/*-*/ && nd.a !== 0) {
          const c = this.fa.nodes[nd.a];
          if (c.k === 29/*NumLit*/) { const tx = this.name(c.a); if (tx.length > 0 && /^[0-9]+$/.test(tx)) return 2; }
        }
        break;
      case 29/*NumLit*/: if (this.name(nd.a) === "0") return 3; break;
    }
    return 0;
  }
  exprText(out, idx, depth) {
    const fa = this.fa;
    if (idx === 0 || idx >= fa.nodes.length || depth > 24 || out.len > 400) return;
    const n = fa.nodes[idx];
    const put = (s) => { out.s += s; out.len += s.length; };
    const sub = (i) => this.exprText(out, i, depth + 1);
    const typ = (i) => { if (i === 0 || i > fa.types.length) return; put(squeeze(fa.name(fa.types[i - 1].textIdx), "")); };
    switch (n.k) {
      case 28/*Ident*/: case 29/*NumLit*/: case 30/*CharLit*/: case 31/*StrLit*/: case 32/*BoolLit*/: put(fa.name(n.a)); break;
      case 33/*NullPtr*/: put("nullptr"); break;
      case 49/*Paren*/: put("("); sub(n.a); put(")"); break;
      case 39/*Binary*/: case 40/*Assign*/: sub(n.a); put(" " + PUNCT_TABLE[n.f & 0xff] + " "); sub(n.b); break;
      case 42/*Comma*/: sub(n.a); put(", "); sub(n.b); break;
      case 38/*Unary*/: put(PUNCT_TABLE[n.f & 0xff]); sub(n.a); break;
      case 37/*Postfix*/: sub(n.a); put(PUNCT_TABLE[n.f & 0xff]); break;
      case 41/*Cond*/: sub(n.a); put(" ? "); sub(n.b); put(" : "); sub(n.c); break;
      case 35/*Member*/: sub(n.b); put(n.f & 1 ? "->" : "."); put(fa.name(n.a)); break;
      case 36/*Index*/: sub(n.a); put("["); sub(n.b); put("]"); break;
      case 34/*Call*/:
        sub(n.a); put("(");
        for (let i = 0; i < n.b; i++) { if (i > 0) put(", "); sub(n.c + i); }
        put(")");
        break;
      case 43/*Cast*/: put("("); typ(n.a); put(")"); sub(n.b); break;
      case 44/*SizeofT*/: put("sizeof("); typ(n.a); put(")"); break;
      case 46/*AlignofT*/: put("alignof("); typ(n.a); put(")"); break;
      case 45/*SizeofE*/: put("sizeof "); sub(n.a); break;
      case 50/*CompoundLit*/: put("("); typ(n.a); put("){...}"); break;
      default: put("...");
    }
  }

  walkStmt(w, idx, scope, loop) {
    const fa = this.fa, ns = fa.nodes;
    if (idx === 0 || idx >= ns.length) return;
    if (w.has(idx, MK_VISITED)) return;
    w.set(idx, MK_VISITED);
    const n = ns[idx];
    if (scope > w.maxNesting) w.maxNesting = scope;
    if (loop > w.maxLoopDepth) w.maxLoopDepth = loop;
    switch (n.k) {
      case 13/*CompoundStmt*/:
        for (let i = 0; i < n.b; i++) this.walkStmt(w, n.a + i, scope + 1, loop);
        break;
      case 14/*DeclStmt*/:
        for (let i = 0; i < n.b; i++) {
          const c = ns[n.a + i];
          if (c.k !== 3/*Decl*/) continue;
          const d = fa.decls[c.a - 1];
          const typ = fa.name(d.typeIdx);
          const isConst = typ.includes("const") || (d.storage & 256/*Constexpr*/) !== 0;
          const lr = { Line: c.line, Name: jtrunc(fa.name(d.nameIdx), 80), Type: jtrunc(squeeze(typ, ""), 120), IsConst: b2i(isConst),
            IsMutable: b2i(!isConst), HasInit: b2i(d.init !== 0), InLoop: b2i(loop > 0), ScopeDepth: scope, IsUntyped: 0 };
          w.locals.push(lr);
          if (w.shadow !== null && (d.storage & 8/*Extern*/) === 0) w.shadow.add(lr.Name);
          if (w.types !== null) w.types.set(lr.Name, d.arrayLen !== 0 ? typ + "[]" : typ);
          if (typ.includes("volatile")) w.hasVolatile++;
          if ((d.storage & (4/*Static*/ | 8/*Extern*/ | 128/*ThreadLoc*/)) === 0) {
            const f = { ftype: typ, ptr: d.ptr, alen: d.arrayLen, kn: false, ksz: 0, kal: 0, kex: 0 };
            if (d.ptr === 0 && this.typeSz !== null) {
              const v = this.typeSz.get(aggBase(typ));
              if (v) { f.kn = true; [f.ksz, f.kal, f.kex] = v; }
            }
            const [sz, , exa] = fldSize(f);
            if (exa === 1 && sz > 0) { w.frameBytes += sz; w.frameMax = Math.max(w.frameMax, sz); }
            else w.frameUnsized++;
          }
          if (d.ptr > 0) w.nPtrLocals++;
          if (d.arrayLen === -1) w.nVla++;
          if (d.init !== 0) this.walkStmt(w, d.init, scope, loop);
        }
        break;
      case 15/*ExprStmt*/:
        if (n.a !== 0) {
          const c = ns[n.a];
          if (c.k === 34/*Call*/ && w.discards !== null) {
            const cal = ns[c.a];
            if (cal.k === 28/*Ident*/) {
              const nm = fa.name(cal.a);
              if (this.tuFunc(nm)) {
                if (!w.discards.get(nm)) w.discardOrd.push(nm);
                w.discards.set(nm, (w.discards.get(nm) || 0) + 1);
              }
            }
          }
          this.walkStmt(w, n.a, scope, loop);
        }
        break;
      case 16/*If*/:
        w.cyclomatic++; w.nBranches++;
        if (loop > 0) w.branchInLoop++;
        if (scope - 1 > 1) w.cognitive += scope - 1; else w.cognitive++;
        w.noteOper("if");
        this.noteTested(w, n.a, 0);
        this.walkStmt(w, n.a, scope, loop);
        this.walkStmt(w, n.b, scope, loop);
        if (n.c !== 0) this.walkStmt(w, n.c, scope, loop);
        break;
      case 17/*Switch*/:
        w.nSwitch++; w.nBranches++;
        if (loop > 0) w.switchInLoop++;
        if (scope - 1 > 1) w.cognitive += scope - 1; else w.cognitive++;
        w.noteOper("switch");
        this.walkStmt(w, n.a, scope, loop);
        this.walkStmt(w, n.b, scope, loop);
        break;
      case 18/*Case*/:
        if (n.a !== 0) {
          w.cyclomatic++; w.nBranches++; w.nCases++;
          if (loop > 0) w.branchInLoop++;
          w.noteOper("case");
          this.walkStmt(w, n.a, scope, loop);
        }
        this.walkStmt(w, n.b, scope, loop);
        break;
      case 19/*While*/:
        w.nLoops++; w.cyclomatic++; if (scope - 1 > 1) w.cognitive += scope - 1; else w.cognitive++;
        w.noteOper("while");
        this.noteTested(w, n.a, 0);
        this.noteLoop(w, n.a, n.b, false);
        this.walkStmt(w, n.a, scope, loop);
        this.walkStmt(w, n.b, scope, loop + 1);
        break;
      case 20/*Do*/:
        w.nLoops++; w.cyclomatic++; if (scope - 1 > 1) w.cognitive += scope - 1; else w.cognitive++;
        w.noteOper("do");
        this.noteTested(w, n.b, 0);
        this.noteLoop(w, n.b, n.a, false);
        this.walkStmt(w, n.a, scope, loop + 1);
        this.walkStmt(w, n.b, scope, loop);
        break;
      case 21/*For*/:
        w.nLoops++; w.cyclomatic++; if (scope - 1 > 1) w.cognitive += scope - 1; else w.cognitive++;
        w.noteOper("for");
        this.noteTested(w, n.b, 0);
        this.noteLoop(w, n.b, n.d, n.b === 0);
        if (n.a !== 0) this.walkStmt(w, n.a, scope, loop);
        if (n.b !== 0) this.walkStmt(w, n.b, scope, loop);
        if (n.c !== 0) this.walkStmt(w, n.c, scope, loop);
        this.walkStmt(w, n.d, scope, loop + 1);
        break;
      case 22/*Return*/:
        w.nReturns++;
        w.noteOper("return");
        if (n.a === 0) w.retVoid++;
        else {
          switch (this.retShape(n.a)) { case 1: w.retNull++; break; case 2: w.retNeg++; break; case 3: w.retZero++; break; default: w.retVal++; }
          this.walkStmt(w, n.a, scope, loop);
        }
        break;
      case 25/*Goto*/: w.nGotos++; w.noteOper("goto"); break;
      case 26/*Label*/: w.nLabels++; if (n.b !== 0) this.walkStmt(w, n.b, scope, loop); break;
      case 12/*StaticAssert*/: w.nStaticAssert++; break;
      case 39/*Binary*/: {
        const f = n.f;
        w.noteOp(PUNCT_TABLE[f & 0xff]);
        if (f === 42/*/*/ && loop > 0) w.divInLoop++;
        if (f === 42/*/*/ || f === 43/*%*/) this.noteDivisor(w, n.b);
        if (f === 36/*&*/ || f === 47/*|*/ || f === 46/*^*/ || f === 7/*<<*/ || f === 8/*>>*/) w.nBitop++;
        if (f === 13/*&&*/ || f === 14/*||*/) { w.cyclomatic++; w.nLogical++; }
        else if (f === 38/*+*/ || f === 39/*-*/ || f === 37/***/ || f === 42/*/*/ || f === 43/*%*/) w.nArith++;
        else if (f === 11/*==*/ || f === 12/*!=*/ || f === 44/*<*/ || f === 45/*>*/ || f === 9/*<=*/ || f === 10/*>=*/) w.nCmp++;
        else if (f === 7/*<<*/ || f === 8/*>>*/) w.nShift++;
        if ((f === 11/*==*/ || f === 12/*!=*/) && (this.isNullLit(n.a) || this.isNullLit(n.b))) w.nNullCheck++;
        if (f === 44/*<*/ || f === 45/*>*/ || f === 9/*<=*/ || f === 10/*>=*/) {
          if ((this.signedOperand(w, n.a) && this.sizeOperand(w, n.b, 0)) || (this.sizeOperand(w, n.a, 0) && this.signedOperand(w, n.b))) w.nSignCmp++;
        }
        this.walkStmt(w, n.a, scope, loop); this.walkStmt(w, n.b, scope, loop);
        break;
      }
      case 40/*Assign*/: {
        const f = n.f;
        w.noteOp(PUNCT_TABLE[f & 0xff]);
        if (f === 51/*=*/) w.nAssign++; else w.nCompoundAssign++;
        if (f === 20/*&=*/ || f === 22/*|=*/ || f === 21/*^=*/ || f === 1/*<<=*/ || f === 2/*>>=*/) w.nBitop++;
        if (f === 16/*/=*/ && loop > 0) w.divInLoop++;
        if (f === 16/*/=*/ || f === 17/*%=*/) this.noteDivisor(w, n.b);
        if (this.writesGlobal(w, n.a)) w.nGlobalWrite++;
        this.walkStmt(w, n.a, scope, loop); this.walkStmt(w, n.b, scope, loop);
        break;
      }
      case 41/*Cond*/:
        this.noteTested(w, n.a, 0);
        w.nTernary++; w.cyclomatic++;
        w.noteOp("?");
        this.walkStmt(w, n.a, scope, loop); this.walkStmt(w, n.b, scope, loop); this.walkStmt(w, n.c, scope, loop);
        break;
      case 38/*Unary*/: {
        const f = n.f;
        if (f === 37/***/) { w.nDeref++; w.noteOp("*"); }
        else if (f === 5/*++*/ || f === 6/*--*/) {
          w.nIncdec++;
          w.noteOp(PUNCT_TABLE[f]);
          if (this.writesGlobal(w, n.a)) w.nGlobalWrite++;
        } else if (f === 36/*&*/) {
          w.noteOp("&");
          const o = ns[n.a];
          if (o.k === 28/*Ident*/) {
            w.nAddrof++;
            w.set(n.a, MK_AMP);
            const na = fa.name(o.a);
            if (na !== "" && !this.fnNameSet.has(na)) w.addrs.push({ Name: jtrunc(na, 80), Line: ns[n.a].line });
          }
        } else if (f === 40/*~*/) w.nBitop++;
        else w.noteOp(PUNCT_TABLE[f & 0xff]);
        if (n.a !== 0) this.walkStmt(w, n.a, scope, loop);
        break;
      }
      case 37/*Postfix*/:
        w.nIncdec++;
        w.noteOp(PUNCT_TABLE[n.f & 0xff]);
        if (this.writesGlobal(w, n.a)) w.nGlobalWrite++;
        this.walkStmt(w, n.a, scope, loop);
        break;
      case 34/*Call*/: {
        const callee = ns[n.a];
        let name = "", fnptr = false;
        if (callee.k === 28/*Ident*/) { name = fa.name(callee.a); w.set(n.a, MK_CALLEE); } else fnptr = true;
        let argsOff = 0, argsEnd = 0, argsTxt = "";
        if (n.b > 0) {
          const first = ns[n.c], last = ns[n.c + n.b - 1];
          argsOff = first.off; argsEnd = last.end;
          if (first.off <= callee.off) {
            argsTxt = null;
          } else {
            const e = n.end - 1;
            if (e > argsEnd) {
              const r = this.rawOff(e);
              if (r >= 0 && r < this.srcRaw.length && this.srcRaw[r] === ")") argsEnd = e;
            }
          }
        }
        w.calls.push({ name, fnptr, node: idx, argsOff, argsEnd, argsTxt, argsNode: n, line: n.line });
        if (loop > 0) {
          w.callInLoop++;
          if (allocatesName(name)) w.allocInLoop++;
          switch (name) {
            case "sqrt": case "exp": case "log": case "log2": case "log10": case "pow": case "sin": case "cos": case "tan": case "atan":
            case "atan2": case "tgamma": case "lgamma": case "erf": case "fmod": case "cbrt": case "hypot": case "floor": case "ceil":
            case "round": w.libmInLoop++; break;
            case "strlen": case "strnlen": w.strlenInLoop++; break;
          }
          const hz = hazardFuncs(name);
          if (hz === "io") w.ioInLoop++;
          else if (hz === "concurrency") w.lockInLoop++;
        }
        if (n.b > 0) {
          const a0 = ns[n.c];
          if (a0.k === 28/*Ident*/) {
            const an = fa.name(a0.a);
            switch (name) {
              case "access": case "faccessat": case "stat": case "lstat": w.toctouChk?.add(an); break;
              case "open": case "openat": case "fopen": case "creat": w.toctouOpn?.add(an); break;
            }
          }
        }
        if (name === "getenv" || name === "secure_getenv") w.nGetenv++;
        if (isIntrinsicCall(name)) w.nIntrinsic++;
        if (name.startsWith("__atomic_") || name.startsWith("__sync_") || name.startsWith("__c11_atomic_") || name.startsWith("atomic_")) w.nAtomic++;
        if (name === "__builtin_expect" || name === "__builtin_expect_with_probability" || name === "likely" || name === "unlikely") w.nLikely++;
        if (name.startsWith("__builtin_")) w.nBuiltin++;
        this.walkStmt(w, n.a, scope, loop);
        for (let i = 0; i < n.b; i++) this.walkStmt(w, n.c + i, scope, loop);
        break;
      }
      case 35/*Member*/:
        if (n.f & 1) { w.nDeref++; w.nArrow++; } else w.nDot++;
        w.noteOp(".");
        this.walkStmt(w, n.b, scope, loop);
        break;
      case 36/*Index*/: w.nSubscript++; w.noteOp("["); this.walkStmt(w, n.a, scope, loop); this.walkStmt(w, n.b, scope, loop); break;
      case 43/*Cast*/:
        w.nCast++;
        w.noteOp("cast");
        if (n.b !== 0 && ns[n.b].k === 28/*Ident*/) w.castIdents.push(fa.name(ns[n.b].a));
        this.walkStmt(w, n.b, scope, loop);
        break;
      case 44/*SizeofT*/: case 46/*AlignofT*/: w.nSizeof++; w.noteOper("sizeof"); break;
      case 45/*SizeofE*/: w.nSizeof++; w.noteOper("sizeof"); this.walkStmt(w, n.a, scope, loop); break;
      case 51/*VaArg*/: w.noteOper("__builtin_va_arg"); this.walkStmt(w, n.a, scope, loop); break;
      case 28/*Ident*/: {
        const nm = fa.name(n.a);
        if (nm === "errno") w.nErrno++;
        w.noteOper(nm);
        if (w.wantRefs && (w.fileScope || (!w.shadow?.has(nm) && this.tuFunc(nm)))) w.refs.push({ node: idx, name: nm, line: n.line });
        break;
      }
      case 29/*NumLit*/: {
        const tok = fa.name(n.a);
        w.noteOper(tok);
        const kind = numKind(tok);
        let magic = !magicFor(tok);
        if (kind === "float") { w.nFloatLit++; magic = true; }
        if (magic) w.nMagic++;
        w.lits.push({ Kind: kind, Value: jtrunc(tok, 40), Line: n.line, IsMagic: b2i(magic), SymbolID: 0, HasSym: false });
        break;
      }
      case 30/*CharLit*/: {
        const tok = fa.name(n.a);
        w.noteOper(tok);
        w.lits.push({ Kind: "char", Value: jtrunc(tok, 40), Line: n.line, IsMagic: 0, SymbolID: 0, HasSym: false });
        break;
      }
      case 31/*StrLit*/: {
        const tok = fa.name(n.a);
        w.nStringLit++;
        w.noteOper(tok);
        const dq = dequoteLit(tok);
        w.lits.push({ Kind: "string", Value: jtrunc(dq, 200), Line: n.line, IsMagic: 0, SymbolID: 0, HasSym: false });
        if (secretShaped(dq)) w.lits.push({ Kind: "_secret", Value: jtrunc(dq, 200), Line: n.line, IsMagic: 0, SymbolID: 0, HasSym: false });
        break;
      }
      case 48/*StmtExpr*/: if (n.a !== 0) this.walkStmt(w, n.a, scope, loop); break;
      case 49/*Paren*/: w.noteOp("("); if (n.a !== 0) this.walkStmt(w, n.a, scope, loop); break;
      case 42/*Comma*/: w.noteOp(","); this.walkStmt(w, n.a, scope, loop); this.walkStmt(w, n.b, scope, loop); break;
      case 50/*CompoundLit*/: this.walkStmt(w, n.b, scope, loop); break;
      case 47/*Generic*/:
        w.noteOper("_Generic");
        this.walkStmt(w, n.a, scope, loop);
        for (let i = 0; i < n.c; i++) { const asc = fa.assocs[n.b + i]; if (asc.expr !== 0) this.walkStmt(w, asc.expr, scope, loop); }
        break;
      case 52/*InitList*/:
        for (let i = 0; i < n.b; i++) { const it = ns[n.a + i]; if (it.b !== 0) this.walkStmt(w, it.b, scope, loop); }
        break;
    }
  }
}

function paramRow(sid, pos, o) {
  return Object.assign({ SymbolID: sid, Pos: pos, Name: "", HasName: false, Type: "", IsVariadic: 0, IsRef: 0, IsMutable: 0,
    IsNullable: 0, TypeDepth: 0 }, o);
}

// The per-file output that crosses to the merging thread.
export function fileOut(df, absID, perr, nErrs, nMissing) {
  const incCtx = new Map();
  for (const inc of df.incRows) if (!incCtx.has(inc.line)) incCtx.set(inc.line, inc.resolved);
  const gfid = df.globalFileID;
  for (const s of df.symbols) { s.FileID = gfid; s.ModuleID = df.modID; }
  const fix = (rows) => { for (const r of rows) if ("FileID" in r) r.FileID = gfid; return rows; };
  for (const im of df.imports) {
    im.FileID = gfid;
    im.TargetID = 0; im.HasTargetID = false;
    const r = incCtx.get(im.Line);
    if (r) { const tid = absID.get(cleanPath(r)); if (tid !== undefined) { im.TargetID = tid; im.HasTargetID = true; im.IsExternal = 0; } }
  }
  for (const g of df.globals) g.ModuleID = df.modID;
  const funcs = df.fnNames.map((nm, k) => ({ name: nm, sid: df.fnSyms[k], fid: df.fnFiles[k], mid: df.fnModules[k], static: df.fnStatic[k] }));
  return {
    fid: gfid, perr, nErrs, nMissing, docLines: df.docLines,
    symbols: df.symbols, params: df.params, fields: df.fields, locals: df.locals, literals: fix(df.lits), markers: fix(df.markers),
    attrs: fix(df.attrs), imports: df.imports, hazards: df.hazards, enums: df.enums, layout: df.layout, ssize: df.ssize,
    decls: fix(df.decls), addrs: fix(df.addrs), secrets: fix(df.secrets), allocs: fix(df.allocs), memops: fix(df.memops),
    macros: df.macros, macroNames: df.macroName, globals: fix(df.globals), cfgs: fix(df.cfgs), locks: fix(df.locks),
    evops: fix(df.evops), apiuses: fix(df.apiuses),
    pendSID: df.pendSym, pendFID: df.pendFile, pendMID: df.pendModule, pendName: df.pendNames, pendLine: df.pendLines, funcs,
  };
}
