// --dump: every table, rows sorted bytewise, in the oracle's table order.
import { field, Float } from "./dumpfmt.js";
import { encode, decode } from "dyna:bytes";
import { SYM_KEYS } from "./derive.js";

const N = null;
const opt = (v, ok) => (ok ? v : N);

// Python repr of a float64, as the oracle's cgRepr prints it.
export function cgRepr(f) {
  if (f === Infinity) return "inf";
  if (f === -Infinity) return "-inf";
  if (Number.isNaN(f)) return "nan";
  const neg = f < 0 || Object.is(f, -0);
  if (neg) f = -f;
  if (f === 0) return neg ? "-0.0" : "0.0";
  const m = f.toExponential();
  const ep = m.indexOf("e");
  const digits = m.slice(0, ep).replace(".", "");
  const decpt = Number(m.slice(ep + 1)) + 1;
  let s = neg ? "-" : "";
  if (decpt < -3 || decpt > 16) {
    s += digits[0];
    if (digits.length > 1) s += "." + digits.slice(1);
    const e = decpt - 1;
    return s + "e" + (e < 0 ? "-" : "+") + String(Math.abs(e)).padStart(2, "0");
  }
  if (decpt <= 0) return s + "0." + "0".repeat(-decpt) + digits;
  if (decpt >= digits.length) return s + digits + "0".repeat(decpt - digits.length) + ".0";
  return s + digits.slice(0, decpt) + "." + digits.slice(decpt);
}
const F = (v) => new Float(cgRepr(v));

function table(out, name, ncols, rows) {
  const lines = new Array(rows.length);
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    let s = "R";
    if (r.length === 0) s += " ";
    for (const v of r) s += " " + field(v);
    lines[i] = s;
  }
  lines.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  out.push(`T ${name} ${ncols} ${lines.length}\n`);
  for (const l of lines) out.push(l + "\n");
  out.push(`E ${name}\n`);
}

const META_ORDER = ["addr_taken_pruned", "calls_resolved", "files_failed", "files_parsed", "files_skipped", "grammar_note",
  "imports_resolved", "lang", "layout_model", "makefiles_read", "parse_mode", "parser", "root", "schema_version", "sqlite", "target"];

// Column order of the symbols table: the 174 columns, zeros where C has no use.
const Z = 0;
const SYM_COLS = ["ID", "FileID", "ModuleID", null, "Name", "QualName", "Kind", "LineStart", Z, Z, "Signature", "ReturnType", "NParams",
  Z, Z, Z, Z, "IsPublic", "IsStatic", Z, Z, Z, Z, Z, "IsTest", Z, "IsEntrypoint", "IsGenerated", "Sloc", "NCommentLines", "HasDoc",
  "Cyclomatic", "Cognitive", "MaxNesting", "NTokens", "NOperators", "NOperands", "NDistinctOperators", "NDistinctOperands", "NLoops",
  "NBranches", "NReturns", "NSwitch", "NCases", Z, Z, Z, Z, Z, Z, "NLabels", "NGotos", "MaxLoopDepth", "CallInLoop", "AllocInLoop",
  "IOInLoop", Z, "LockInLoop", Z, Z, Z, "BranchInLoop", "NLocals", "NCmp", "NShift", "NArith", Z, "NFloatLit", "NMagic", "NNullCheck",
  Z, Z, "NCalls", "NDynamicCalls", "NUnresolvedCalls", "FanIn", "FanOut", "NCallsites", "IsRecursive", "RiskScore", "NMemory", "NAlloc",
  "NIO", "NStdio", "NExec", "NLibm", "NInteger", "NConcurrency", "NReentrancy", "IsInline", "IsVariadic", "NPtrLocals", "NDeref", "NCast",
  "NSizeof", "NIntrinsic", "NAtomic", "NRestrict", "NLikely", "NBuiltin", ...SYM_KEYS.slice(SYM_KEYS.indexOf("SwitchInLoop"))];

export function symbolRow(s) {
  const r = new Array(SYM_COLS.length);
  for (let i = 0; i < SYM_COLS.length; i++) {
    const c = SYM_COLS[i];
    if (c === null) r[i] = N;
    else if (c === 0) r[i] = 0;
    else if (c === "Signature") r[i] = opt(s.Signature, s.HasSignature);
    else if (c === "ReturnType") r[i] = opt(s.ReturnType, s.HasReturnType);
    else r[i] = s[c];
  }
  return r;
}

function utf8Runes(s) {
  if (!/[\x80-\xff]/.test(s)) return s;
  try { return decode(encode(s, "iso-8859-1"), "utf-8"); } catch (e) { return s; }
}
const RE_ASCII_TOK = /[A-Za-z0-9]+/g;
export function tokenCount(s) {
  if (!/[\x80-\xff]/.test(s)) { const m = s.match(RE_ASCII_TOK); return m === null ? 0 : m.length; }
  s = utf8Runes(s);
  let n = 0, inw = false;
  for (const ch of s) {
    if (/[\p{L}\p{Nd}]/u.test(ch)) { inw = true; continue; }
    if (inw) { n++; inw = false; }
  }
  if (inw) n++;
  return n;
}
function svarint(v) {
  const tmp = [];
  do { tmp.push(v & 0x7f); v = Math.floor(v / 128); } while (v !== 0);
  for (let i = 0; i < tmp.length - 1; i++) tmp[i] |= 0x80;
  return tmp.reverse();
}
function reprBytes(b) {
  const hasS = b.includes(39), hasD = b.includes(34);
  const q = hasS && !hasD ? 34 : 39;
  let s = "b" + String.fromCharCode(q);
  for (let i = 0; i < b.length; i++) {
    const c = b[i];
    if (c >= 0x20 && c < 0x7f) {
      if (c === q && i + 1 < b.length && b[i + 1] === q) { s += "\\" + String.fromCharCode(q); continue; }
      s += String.fromCharCode(c);
      continue;
    }
    if (c === 9) s += "\\t";
    else if (c === 10) s += "\\n";
    else if (c === 13) s += "\\r";
    else if (c === 92) s += "\\\\";
    else s += "\\x" + c.toString(16).padStart(2, "0");
  }
  return s + String.fromCharCode(q);
}

// The symbols table, each line built straight from the column list.
function symbolsTable(out, g) {
  const lines = new Array(g.symbols.length);
  const nc = SYM_COLS.length;
  for (let k = 0; k < g.symbols.length; k++) {
    const sy = g.symbols[k];
    let line = "R";
    for (let i = 0; i < nc; i++) {
      const c = SYM_COLS[i];
      if (c === null) line += " \\N";
      else if (c === 0) line += " i:0";
      else if (c === "Signature") line += " " + field(sy.HasSignature ? sy.Signature : null);
      else if (c === "ReturnType") line += " " + field(sy.HasReturnType ? sy.ReturnType : null);
      else if (c === "Name" || c === "QualName" || c === "Kind") line += " " + field(sy[c]);
      else line += " i:" + sy[c];
    }
    lines[k] = line;
  }
  lines.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  out.push(`T symbols 174 ${lines.length}\n`);
  for (const l of lines) out.push(l + "\n");
  out.push("E symbols\n");
}

function fts(out, g) {
  const n = g.symbols.length;
  out.push(`T sym_fts 3 ${n}\n`);
  out.push("R \\N \\N \\N\n".repeat(n));
  out.push("E sym_fts\nT sym_fts_config 2 1\nR s:version i:4\nE sym_fts_config\n");
  const lines = g.symbols.map((s) => {
    const blob = [...svarint(tokenCount(s.Name)), ...svarint(tokenCount(s.QualName)), ...svarint(tokenCount(s.Signature))];
    return "R i:" + s.ID + " " + field(reprBytes(blob));
  });
  lines.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  out.push(`T sym_fts_docsize 2 ${lines.length}\n`);
  for (const l of lines) out.push(l + "\n");
  out.push("E sym_fts_docsize\n");
}

export function dumpText(g) {
  const out = [];
  const T = (name, ncols, rows) => table(out, name, ncols, rows);
  T("addr_taken", 6, g.addrs.map((a) => [a.ID, opt(a.SymbolID, a.HasSym), a.FileID, a.Name, a.Line, a.Kind]));
  T("allocsites", 6, g.allocs.map((a, i) => [i + 1, a.SymbolID, a.FileID, a.Fn, a.SizeExpr, a.Line]));
  T("apiuse", 6, g.apiuses.map((a, i) => [i + 1, a.SymbolID, a.FileID, a.NS, a.Fn, a.Line]));
  T("attributes", 6, g.attrs.map((a, i) => [i + 1, opt(a.SymbolID, a.HasSymbolID), a.FileID, a.Name, opt(a.Args, a.HasArgs), a.Line]));
  T("callsites", 3, g.callsites.map((c) => [c.CallerID, c.CalleeID, c.Line]));
  T("config_blocks", 6, g.cfgs.map((c, i) => [i + 1, c.FileID, c.Directive, c.Expr, c.Line, c.IsConfig]));
  T("declarations", 4, g.decls.map((c, i) => [i + 1, c.FileID, c.Name, c.Line]));
  T("edges", 6, g.edges.map((e) => [e.CallerID, e.CalleeID, e.NCalls, e.SameFile, e.SameModule, e.IsSelf]));
  T("enum_members", 5, g.enums.map((e) => [e.SymbolID, e.Ordinal, e.Name, opt(e.Value, e.HasValue), e.NFields]));
  T("eventops", 7, g.evops.map((e, i) => [i + 1, e.SymbolID, e.FileID, e.Family, e.Fn, e.Args, e.Line]));
  T("fields", 14, g.fields.map((f) => [f.SymbolID, f.Ordinal, f.Name, f.Type, "", f.Line, 0, f.IsConst, f.IsMutable, f.IsNullable,
    f.IsCollection, 0, 0, f.TypeDepth]));
  T("files", 29, g.files.map((f) => [f.id, f.path, f.dir, f.basename, f.ext, f.lang, f.moduleID, f.bytes, f.lines, f.sloc, f.blankLines,
    f.commentLines, f.docLines, f.maxLineLen, f.sha1, f.parsed, f.isTest, f.isGenerated, f.isVendored, f.nParseErrors, f.nMissingNodes,
    F(0), f.nSymbols, f.nFunctions, f.nTypes, f.nImports, f.totalCyclo, f.maxCyclo, f.totalRisk]));
  T("globals", 13, g.globals.map((m, i) => [i + 1, m.FileID, m.ModuleID, m.Name, m.Type, m.Line, m.IsStatic, m.IsConst, m.IsVolatile,
    m.IsAtomic, m.IsArray, m.PtrDepth, m.HasInit]));
  T("hazards", 5, g.hazards.map((h) => [h.SymbolID, h.Pattern, h.Category, h.N, h.FirstLine]));
  T("imports", 13, g.imports.map((im, i) => [i + 1, im.FileID, im.Target, opt(im.TargetID, im.HasTargetID), N, im.Kind, im.Line,
    im.IsExternal, im.IsRelative, 0, 0, 0, im.NNames]));
  T("include_cycles", 5, g.cycles.map((c, i) => [i + 1, c.APath, c.BPath, c.Length, c.Members]));
  T("layout", 11, g.layout.map((l) => [l.SymbolID, l.Ordinal, l.ByteOff, l.ByteSize, l.PadBefore, l.Exact, l.PtrDepth, l.ArrayLen,
    l.IsFnptr, l.Depth, l.InUnion]));
  T("literals", 7, g.literals.map((l, i) => [i + 1, opt(l.SymbolID, l.HasSym), l.FileID, l.Kind, l.Value, l.Line, l.IsMagic]));
  T("locals", 11, g.locals.map((l) => [l.SymbolID, l.Ordinal, l.Name, l.Type, l.Line, l.IsConst, l.IsMutable, l.IsUntyped, l.HasInit,
    l.InLoop, l.ScopeDepth]));
  T("locks", 5, g.locks.map((l, i) => [i + 1, l.SymbolID, l.FileID, l.Name, l.Line]));
  T("macros", 7, g.macros.map((m) => [m.SymbolID, m.IsFunctionlike, m.NParams, opt(m.Body, m.HasBody), m.BodyLen, m.IsMultiline, m.NUses]));
  T("makefile_rules", 7, g.mkRules.map((m, i) => [i + 1, m.Path, m.Rule, m.Line, m.NObjs, m.NSrcs, m.UsesAr]));
  T("markers", 6, g.markers.map((m, i) => [i + 1, m.FileID, opt(m.SymbolID, m.HasSym), m.Kind, m.Line, m.Text]));
  T("memops", 10, g.memops.map((m, i) => [i + 1, m.SymbolID, m.FileID, m.Fn, m.Dst, m.Src, m.SizeArg, m.SizeBuf, m.DstTail, m.Line]));
  T("meta", 2, META_ORDER.filter((k) => g.meta.has(k)).map((k) => [k, g.meta.get(k)]));
  T("modules", 10, g.modules.map((m) => [m.id, m.name, m.kind, m.nFiles || 0, m.nSymbols || 0, m.nPublic || 0, m.sloc || 0, m.fanIn || 0,
    m.fanOut || 0, F(m.instability || 0)]));
  T("params", 13, g.params.map((p) => [p.SymbolID, p.Pos, opt(p.Name, p.HasName), p.Type, N, 0, p.IsVariadic, p.IsRef, p.IsMutable,
    p.IsNullable, 0, 0, p.TypeDepth]));
  T("reach", 3, g.reach.map((r) => [r.SymbolID, r.NTransitive, r.NTransitiveOut]));
  T("secret_candidates", 5, g.secrets.map((s, i) => [i + 1, opt(s.SymbolID, s.HasSym), s.FileID, s.Value, s.Line]));
  T("struct_size", 7, g.ssize.map((s) => [s.SymbolID, s.TotalSize, s.TailPad, s.TotalPad, s.MaxAlign, s.Exact, s.NLines64]));
  fts(out, g);
  symbolsTable(out, g);
  T("unresolved_calls", 4, g.unres.map((u) => [u.CallerID, u.Name, u.N, u.FirstLine]));
  return out.join("");
}
