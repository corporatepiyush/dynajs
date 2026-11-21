// C23 recursive-descent parser producing a flat, creation-ordered node
// array. Node order and positions are part of the contract (see
// PSEUDOCODE 5.4): consumers walk "the nodes created while parsing X".
import { squeeze, trimSpace } from "./text.js";
import data from "./data.json" with { type: "json" };

export const N = Object.fromEntries(["Bad", "TU", "FuncDef", "Decl", "Param", "KRDecl", "StructSpec", "UnionSpec", "EnumSpec", "Field",
  "Enumerator", "TypeRef", "StaticAssert", "CompoundStmt", "DeclStmt", "ExprStmt", "If", "Switch", "Case", "While", "Do", "For",
  "Return", "Break", "Continue", "Goto", "Label", "Asm", "Ident", "NumLit", "CharLit", "StrLit", "BoolLit", "NullPtr", "Call",
  "Member", "Index", "Postfix", "Unary", "Binary", "Assign", "Cond", "Comma", "Cast", "SizeofT", "SizeofE", "AlignofT", "Generic",
  "StmtExpr", "Paren", "CompoundLit", "VaArg", "InitList", "InitItem"].map((n, i) => [n, i]));

export const F = { Const: 1, Volatile: 2, Static: 4, Extern: 8, Inline: 16, Auto: 32, Register: 64, ThreadLoc: 128,
  Constexpr: 256, Restrict: 512, Atomic: 1024, Variadic: 2048, Noreturn: 4096, KRStyle: 8192, HasInit: 16384, AttrHere: 32768 };

export const PUNCT_TABLE = ["%:%:", "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
  "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "<:", ":>", "<%", "%>", "%:",
  "[", "]", "(", ")", "{", "}", ".", "&", "*", "+", "-", "~", "!", "/", "%", "<", ">", "^", "|", "?", ":", ";", "=", ",", "#"];
export const PCODE = Object.fromEntries(PUNCT_TABLE.map((p, i) => [p, i]));

const C_KEYWORDS = new Set(Object.keys(data.cKeywords).filter((k) => data.cKeywords[k]));
const C23_KEYWORDS = new Set(Object.keys(data.c23Keywords).filter((k) => data.c23Keywords[k]));
const SYS_TYPEDEFS = new Set(Object.keys(data.sysTypedefs).filter((k) => data.sysTypedefs[k]));
export const isCKeyword = (s) => C_KEYWORDS.has(s);
export const isKeyword = (s) => C23_KEYWORDS.has(s);

const BIN_PREC = { "||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "<": 7, ">": 7, "<=": 7, ">=": 7, "<<": 8, ">>": 8,
  "+": 9, "-": 9, "*": 10, "/": 10, "%": 10 };
const ASSIGN_OPS = new Set(["=", "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "<<=", ">>="]);

const B = { Void: 0, Bool: 1, Char: 2, Short: 3, Int: 4, Long: 5, LongLong: 6, Float: 7, Double: 8, Signed: 9, Unsigned: 10,
  Decimal32: 11, Decimal64: 12, Decimal128: 13, Typedef: 14, Struct: 15, Union: 16, Enum: 17, Typeof: 18, BitInt: 19, Auto: 20, None: 21 };

const TYPE_START = new Set(["__int128", "__int128_t", "__uint128_t", "__typeof__", "__typeof", "const", "volatile", "restrict", "_Atomic", "struct", "union", "enum", "void", "char", "short", "int", "long",
  "float", "double", "signed", "unsigned", "_Bool", "bool", "_Complex", "_Imaginary", "typeof", "typeof_unqual", "_BitInt",
  "_Decimal32", "_Decimal64", "_Decimal128", "constexpr"]);
const TYPE_START_STMT = new Set([...TYPE_START, "alignas", "_Alignas"]);
const TYPE_START_AT = new Set([...TYPE_START, "register"]);
const DECL_START = new Set(["__int128", "__int128_t", "__uint128_t", "__typeof__", "__typeof", "static", "extern", "register", "auto", "typedef", "const", "volatile", "restrict", "_Atomic", "struct",
  "union", "enum", "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "_Bool", "bool", "_Complex",
  "_Imaginary", "typeof", "typeof_unqual", "_BitInt", "_Decimal32", "_Decimal64", "_Decimal128", "constexpr", "thread_local",
  "_Thread_local", "alignas", "_Alignas", "__extension__", "__attribute__", "__attribute", "__inline", "__inline__", "_Noreturn",
  "noreturn", "static_assert", "_Static_assert"]);
const UNKNOWN_TYPE_NEXT = new Set(["__int128", "__int128_t", "__uint128_t", "__typeof__", "__typeof", "const", "volatile", "restrict", "_Atomic", "struct", "union", "enum", "void", "char", "short", "int",
  "long", "float", "double", "signed", "unsigned", "_Bool", "bool", "_Complex", "_Imaginary", "typeof", "typeof_unqual", "_BitInt",
  "_Decimal32", "_Decimal64", "_Decimal128", "constexpr", "alignas", "_Alignas"]);
const PAREN_PARAM = new Set(["__int128", "__int128_t", "__uint128_t", "__typeof__", "__typeof", "const", "volatile", "restrict", "_Atomic", "void", "char", "short", "int", "long", "float", "double",
  "signed", "unsigned", "_Bool", "bool", "struct", "union", "enum", "typeof", "typeof_unqual", "_BitInt", "_Decimal32", "_Decimal64",
  "_Decimal128", "register", "inline"]);
const PTR_QUALS = new Set(["const", "volatile", "restrict", "__restrict", "__restrict__"]);

// Integer literal as Go's strconv.ParseInt(s, 0, 64): base prefixes,
// underscores only between digits after a prefix-aware check; null on error.
export function goParseInt(s) {
  let i = 0, neg = false;
  if (s[0] === "+" || s[0] === "-") { neg = s[0] === "-"; i = 1; }
  let base = 10, body = s.slice(i), prefixed = false;
  if (body.length > 1 && body[0] === "0") {
    const c = body[1].toLowerCase();
    if (c === "x") { base = 16; body = body.slice(2); prefixed = true; }
    else if (c === "b") { base = 2; body = body.slice(2); prefixed = true; }
    else if (c === "o") { base = 8; body = body.slice(2); prefixed = true; }
    else { base = 8; body = body.slice(1); prefixed = true; }
  }
  if (body.length === 0) return prefixed && base === 8 ? 0n : null;
  if (body.includes("_")) {
    // underscores must separate digits (or follow the prefix)
    const full = (prefixed ? "0" : "") + body;
    if (/__|_$/.test(full) || (!prefixed && body[0] === "_")) return null;
    body = body.replaceAll("_", "");
  }
  let v = 0n;
  const B = BigInt(base);
  for (const ch of body) {
    const d = parseInt(ch, 36);
    if (Number.isNaN(d) || d >= base) return null;
    v = v * B + BigInt(d);
  }
  if (neg) v = -v;
  if (v > 9223372036854775807n || v < -9223372036854775808n) return null;
  return v;
}

const attrAhead = (t) => t.p === "[" || t.t === "__attribute__" || t.t === "__attribute";
const DECLSPEC_WORDS = new Set(["typedef", "extern", "static", "auto", "register", "thread_local", "_Thread_local", "__thread", "constexpr", "inline", "__inline", "__inline__", "_Noreturn", "noreturn", "const", "__const", "__const__", "volatile", "__volatile", "__volatile__", "restrict", "__restrict", "__restrict__", "_Atomic", "alignas", "_Alignas", "__extension__", "__attribute__", "__attribute", "asm", "__asm", "__asm__", "void", "_Bool", "bool", "char", "short", "int", "long", "float", "double", "signed", "__signed", "__signed__", "unsigned", "_Decimal32", "_Decimal64", "_Decimal128", "_Complex", "_Imaginary", "_BitInt", "__int128", "__int128_t", "__uint128_t", "typeof", "typeof_unqual", "__typeof", "__typeof__", "struct", "union", "enum"]);
function specWord(p, ts, w, kind) {
  ts.words.push(w); ts.base = ts.words.join(" "); if (kind !== undefined) ts.kind = kind; ts.hasBase = true; ts.saw = true;
  if (p.pos < p.last) p.pos++;
}
const EOF_K = 6, ID = 0, NUM = 1, CHR = 2, STR = 3, PU = 4;

class Scope {
  constructor() { this.typedefs = null; this.tags = null; this.enumConsts = null; }
  setTypedef(n, v) { (this.typedefs ??= new Map()).set(n, v); }
}

export class AST {
  constructor() {
    this.names = []; this.nameIx = new Map();
    this.nodes = [];
    this.funcs = []; this.decls = []; this.params = []; this.tags = []; this.fields = []; this.enums = [];
    this.types = []; this.inits = []; this.assocs = []; this.attrs = [];
  }
  intern(s) {
    let ix = this.nameIx.get(s);
    if (ix === undefined) { this.names.push(s); ix = this.names.length; this.nameIx.set(s, ix); }
    return ix;
  }
  name(ix) { return ix === 0 || ix > this.names.length ? "" : this.names[ix - 1]; }
}

const mkNode = (k) => ({ k, f: 0, a: 0, b: 0, c: 0, d: 0, file: 0, off: 0, end: 0, line: 0, col: 0 });
const nodeAt = (k, t) => ({ k, f: 0, a: 0, b: 0, c: 0, d: 0, file: 0, off: t.off, end: t.end, line: t.line, col: t.col });

function rawText(src, off, end) {
  if (src !== null && off <= src.length && end <= src.length && off <= end) return squeeze(src.slice(off, end), "");
  return "";
}

function baseText(ts) {
  if (!ts.constq && !ts.volatileq && !ts.restrictq && !ts.atomicq) return ts.base.replace(/ +$/, "");
  let s = "";
  if (ts.constq) s += "const ";
  if (ts.volatileq) s += "volatile ";
  if (ts.restrictq) s += "restrict ";
  if (ts.atomicq) s += "_Atomic ";
  return (s + ts.base).replace(/ +$/, "");
}

const newSpecs = () => ({ words: [], base: "", kind: 21/*None*/, tag: "", tagNode: 0, typedef: "", isTypedef: false, hasBase: false,
  constq: false, volatileq: false, restrictq: false, atomicq: false, alignas: false, storage: 0, saw: false });
const newDcl = () => ({ name: "", nameTok: null, hasName: false, ptr: 0, isArray: false, arrayLen: 0n, arrayConst: false, isFn: false,
  fnptr: false, parenName: false, params: [], variadic: false, kr: false, krNames: [], depth: 0, constq: false, volatileq: false });
const stars = (n) => " *".repeat(n);
const arrayLenI = (d) => (!d.isArray ? 0 : d.arrayConst ? Number(BigInt.asIntN(32, d.arrayLen)) : -1);

export class Parser {
  // toks/files: the unit's token stream and per-token file ids; srcs: file id -> bytes
  constructor(fa, toks, files, srcs) {
    this.fa = fa; this.toks = toks; this.files = files; this.srcs = srcs;
    this.pos = 0; this.errs = []; this.nErrors = 0;
    this.scopes = [new Scope()];
    this.funcDepth = 0; this.loopDepth = 0; this.krMode = false;
    this.last = toks.length - 1;
  }
  // ---- token access (by index so the file id is always at hand)
  cur() { return this.toks[this.pos]; }
  peek(n) { const i = this.pos + n; return this.toks[i <= this.last ? i : this.last]; }
  peekIx(n) { const i = this.pos + n; return i <= this.last ? i : this.last; }
  eof() { return this.toks[this.pos].k === EOF_K; }
  advance() { const t = this.toks[this.pos]; if (this.pos < this.last) this.pos++; return t; }
  atP(p) { const t = this.toks[this.pos]; return t.k === PU && t.p === p; }
  atId(s) { const t = this.toks[this.pos]; return t.k === ID && t.t === s; }
  acceptP(p) { if (this.atP(p)) { if (this.pos < this.last) this.pos++; return true; } return false; }
  acceptId(s) { if (this.atId(s)) { if (this.pos < this.last) this.pos++; return true; } return false; }
  srcOf(ix) {
    const s = this.srcs.get(this.files[ix]);
    if (s !== undefined) return s;
    if (this.srcs.size === 1) for (const v of this.srcs.values()) return v;
    return null;
  }
  errAt(ix, msg) {
    if (this.errs.length < 4096) this.errs.push({ line: this.toks[ix].line, file: this.files[ix], msg });
    this.nErrors++;
  }
  syncTo(ix, what) {
    const t = this.toks[ix];
    this.errAt(ix, "parse error near '" + t.t + "' (" + what + "); syncing");
    let depth = 0;
    const line = t.sl;
    while (!this.eof()) {
      const c = this.toks[this.pos];
      if (c.k === PU) {
        if (c.p === "{") depth++;
        else if (c.p === "}") { if (depth === 0) return; depth--; }
        else if (c.p === ";" && depth === 0) { if (this.pos < this.last) this.pos++; return; }
      }
      if (c.sl > line && depth === 0) return;
      if (this.pos < this.last) this.pos++;
    }
  }
  expectP(p, what) {
    if (this.acceptP(p)) return true;
    this.errAt(this.pos, "expected '" + p + "' " + what);
    return false;
  }
  closeParamList() {
    if ((this.toks[this.pos].p === ")" && (this.pos < this.last && this.pos++, true))) return;
    this.errAt(this.pos, "expected ')' to close parameter list; recovering");
    let depth = 0;
    while (!this.eof()) {
      const t = this.toks[this.pos];
      if (t.k === PU) {
        const p = t.p;
        if (p === "(" || p === "[") depth++;
        else if (p === ")" || p === "]") {
          if (depth === 0 && p === ")") { if (this.pos < this.last) this.pos++; return; }
          if (depth > 0) depth--;
        } else if ((p === "{" || p === ";") && depth === 0) return;
      }
      this.advance();
    }
  }
  addNode(n) { this.fa.nodes.push(n); return this.fa.nodes.length - 1; }
  prevEnd() { return this.pos > 0 ? this.toks[this.pos - 1].end : this.toks[this.pos].end; }
  lastSpan() {
    const ns = this.fa.nodes;
    if (!ns.length) return [0, 0];
    const n = ns[ns.length - 1];
    return [n.off, n.end];
  }
  // ---- scopes and type names
  pushScope() { this.scopes.push(new Scope()); }
  popScope() { if (this.scopes.length > 1) this.scopes.pop(); }
  isTypedefName(s) {
    for (let i = this.scopes.length - 1; i >= 0; i--) {
      const m = this.scopes[i].typedefs;
      if (m !== null) { const v = m.get(s); if (v !== undefined) return v; }
    }
    return SYS_TYPEDEFS.has(s);
  }
  addTypedef(n) { this.scopes[this.scopes.length - 1].setTypedef(n, true); }
  addTypedefGuard(n) {
    if (n !== "" && this.scopes.length > 1 && this.isTypedefName(n)) this.scopes[this.scopes.length - 1].setTypedef(n, false);
  }
  addTag(n) { (this.scopes[this.scopes.length - 1].tags ??= new Set()).add(n); }
  addEnumConst(n) { (this.scopes[this.scopes.length - 1].enumConsts ??= new Set()).add(n); }
  plainIdent(t) { return t.k === ID && !C_KEYWORDS.has(t.t) && !this.isTypedefName(t.t); }
  strMacroHere() {
    const t = this.toks[this.pos];
    if (t.k !== ID || C_KEYWORDS.has(t.t)) return false;
    const nx = this.peek(1);
    return !(nx.k === PU && nx.p === "(");
  }
  skipPtrQuals(j) {
    let n = 0;
    for (;;) {
      const x = this.peek(j);
      if (x.k === PU && x.p === "*") { n++; j++; continue; }
      if (x.k === ID && PTR_QUALS.has(x.t)) { j++; continue; }
      return [j, n];
    }
  }
  inferTypedef() {
    const t = this.toks[this.pos];
    if (!this.plainIdent(t)) return false;
    const [j] = this.skipPtrQuals(1);
    const nm = this.peek(j);
    if (nm.k !== ID || C_KEYWORDS.has(nm.t)) return false;
    const nx = this.peek(j + 1);
    if (nx.k !== PU || !(nx.p === "=" || nx.p === ";" || nx.p === "," || nx.p === "[")) return false;
    this.scopes[0].setTypedef(t.t, true);
    return true;
  }
  inferCastType() {
    const t = this.peek(1);
    if (!this.plainIdent(t)) return false;
    let [j, nstars] = this.skipPtrQuals(2);
    let arr = false;
    const x = this.peek(j);
    if (x.k === PU && x.p === "[") {
      let k = j + 1;
      for (let n = 0; n < 6; n++) {
        const y = this.peek(k);
        if (y.k === PU && y.p === "]") break;
        k++;
      }
      const y = this.peek(k);
      if (y.k !== PU || y.p !== "]") return false;
      arr = true;
      j = k + 1;
    }
    const x2 = this.peek(j);
    if (x2.k !== PU || x2.p !== ")") return false;
    const nx = this.peek(j + 1);
    let ok = false;
    if (nx.k === PU && nx.p === "{") ok = true;
    else if (arr) ok = false;
    else if (nstars > 0) ok = true;
    else if (nx.k === NUM || nx.k === STR || nx.k === CHR) ok = true;
    else if (nx.k === ID) ok = !C_KEYWORDS.has(nx.t) || nx.t === "sizeof";
    if (ok) this.scopes[0].setTypedef(t.t, true);
    return ok;
  }
  inferSizeofType() {
    const t = this.peek(1);
    if (!this.plainIdent(t)) return false;
    const [j, nstars] = this.skipPtrQuals(2);
    if (nstars === 0) return false;
    const x = this.peek(j);
    if (x.k !== PU || x.p !== ")") return false;
    this.scopes[0].setTypedef(t.t, true);
    return true;
  }
  // ---- attributes and asm
  recordAttr(name, args, line, off, end) { this.fa.attrs.push({ name, args, line, off, end }); }
  skipBalanced() {
    if (!(this.toks[this.pos].p === "(")) return ["", 0, 0];
    const start = this.advance();
    let depth = 1, b = "";
    const first = start.off;
    while (!this.eof() && depth > 0) {
      const t = this.toks[this.pos];
      if (t.k === PU) {
        if (t.p === "(") depth++;
        else if (t.p === ")") {
          depth--;
          if (depth === 0) { const end = t.end; if (this.pos < this.last) this.pos++; return [b, first, end]; }
          this.advance();
          continue;
        }
      }
      if (b.length > 0 || t.k !== PU || t.p !== "(") {
        if (b.length > 0 && t.ws) b += " ";
        b += t.t;
      }
      this.advance();
    }
    return [b, first, start.end];
  }
  parseAttributes() {
    for (;;) {
      const t = this.toks[this.pos];
      if (t.k === PU && t.p === "[" && this.peek(1).k === PU && this.peek(1).p === "[") {
        if (this.pos < this.last) this.pos++; this.advance();
        let depth = 2, name = "", args = "", gotName = false, scoped = false;
        while (!this.eof() && depth > 0) {
          const c = this.toks[this.pos];
          if (c.k === PU && c.p === "[") depth++;
          else if (c.k === PU && c.p === "]") { depth--; if (this.pos < this.last) this.pos++; continue; }
          if (depth === 2) {
            if (c.k === ID) {
              if (gotName && !scoped) {
                if (args.length > 0 && c.ws) args += " ";
                args += c.t;
              } else { name += c.t; gotName = true; scoped = false; }
            } else if (c.k === PU) {
              if (c.p === ":" && gotName && args.length === 0 && this.peek(1).k === PU && this.peek(1).p === ":") {
                name += "::"; scoped = true; if (this.pos < this.last) this.pos++; this.advance(); continue;
              }
              if (args.length > 0 || gotName) args += c.t;
            } else args += c.t;
          }
          this.advance();
        }
        this.recordAttr(name, args, t.line, t.off, t.end);
        continue;
      }
      if (t.k === ID && (t.t === "__attribute__" || t.t === "__attribute")) {
        if (this.pos < this.last) this.pos++;
        const [args, off, end] = this.skipBalanced();
        this.recordAttr("__attribute__", args, t.line, off, end);
        continue;
      }
      return;
    }
  }
  skipAsm() {
    while ((this.toks[this.pos].t === "asm") || (this.toks[this.pos].t === "__asm__") || (this.toks[this.pos].t === "__asm") || (this.toks[this.pos].t === "volatile") || (this.toks[this.pos].t === "__volatile__") ||
      (this.toks[this.pos].t === "inline") || (this.toks[this.pos].t === "goto")) {
      const ni = this.pos, name = this.advance();
      if ((this.toks[this.pos].p === "(")) {
        const [, off, end] = this.skipBalanced();
        const src = this.srcOf(ni);
        const txt = src !== null && off < src.length && end <= src.length && off < end ? squeeze(src.slice(off, end), "") : "";
        this.recordAttr(name.t, txt, name.line, off, end);
      }
    }
  }
  // ---- declaration specifiers
  parseDeclSpecs() {
    const ts = newSpecs();
    while (!this.eof()) {
      const t = this.toks[this.pos];
      if (t.k !== ID) break;
      if (!DECLSPEC_WORDS.has(t.t)) {
        if (this.isTypedefName(t.t)) {
          ts.base = t.t; ts.kind = 14/*Typedef*/; ts.typedef = t.t; ts.hasBase = true; ts.saw = true; if (this.pos < this.last) this.pos++;
          continue;
        }
        if (!ts.hasBase && this.unknownTypeFollows()) {
          ts.words.push(t.t); ts.base = ts.words.join(" "); ts.kind = 14/*Typedef*/; ts.typedef = t.t; ts.hasBase = true; ts.saw = true;
          if (this.pos < this.last) this.pos++;
          continue;
        }
        return ts;
      }
      switch (t.t) {
        case "typedef": ts.saw = true; ts.typedef = "typedef"; ts.isTypedef = true; if (this.pos < this.last) this.pos++; continue;
        case "extern": ts.storage |= 8/*Extern*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "static": ts.storage |= 4/*Static*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "auto": ts.storage |= 32/*Auto*/; ts.saw = true; ts.kind = 20/*Auto*/; ts.base = "auto"; ts.hasBase = true; if (this.pos < this.last) this.pos++; continue;
        case "register": ts.storage |= 64/*Register*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "thread_local": case "_Thread_local": case "__thread": ts.storage |= 128/*ThreadLoc*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "constexpr": ts.storage |= 256/*Constexpr*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "inline": case "__inline": case "__inline__": ts.storage |= 16/*Inline*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "_Noreturn": case "noreturn": ts.storage |= 4096/*Noreturn*/; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "const": case "__const": case "__const__": ts.constq = true; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "volatile": case "__volatile": case "__volatile__": ts.volatileq = true; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "restrict": case "__restrict": case "__restrict__": ts.restrictq = true; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "_Atomic":
          if (this.peek(1).k === PU && this.peek(1).p === "(") {
            if (this.pos < this.last) this.pos++; this.advance();
            const inner = this.parseTypeName();
            this.expectP(")", "after _Atomic(");
            ts.base = "_Atomic(" + inner.text + ")"; ts.kind = 18/*Typeof*/; ts.hasBase = true; ts.saw = true;
            continue;
          }
          ts.atomicq = true; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "alignas": case "_Alignas": this.advance(); this.skipBalanced(); ts.alignas = true; ts.saw = true; continue;
        case "__extension__": this.advance(); continue;
        case "__attribute__": case "__attribute": {
          if (this.pos < this.last) this.pos++;
          const [args, aoff, aend] = this.skipBalanced();
          this.recordAttr("__attribute__", args, t.line, aoff, aend);
          continue;
        }
        case "asm": case "__asm": case "__asm__": this.advance(); this.skipBalanced(); continue;
        case "__int128": case "__int128_t": case "__uint128_t": specWord(this, ts, "__int128", 6/*LongLong*/); continue;
        case "void": specWord(this, ts, "void", 0/*Void*/); continue;
        case "_Bool": case "bool": specWord(this, ts, "_Bool", 1/*Bool*/); continue;
        case "char": specWord(this, ts, "char", 2/*Char*/); continue;
        case "short": specWord(this, ts, "short", 3/*Short*/); continue;
        case "int": specWord(this, ts, "int", ts.kind === 21/*None*/ ? 4/*Int*/ : ts.kind); continue;
        case "long":
          if (ts.words.length && ts.words[ts.words.length - 1] === "long") { ts.words[ts.words.length - 1] = "long long"; ts.kind = 6/*LongLong*/; }
          else { ts.words.push("long"); ts.kind = 5/*Long*/; }
          ts.base = ts.words.join(" "); ts.hasBase = true; ts.saw = true; if (this.pos < this.last) this.pos++;
          continue;
        case "float": specWord(this, ts, "float", 7/*Float*/); continue;
        case "double": specWord(this, ts, "double", 8/*Double*/); continue;
        case "signed": case "__signed": case "__signed__":
          ts.words.push("signed"); ts.base = ts.words.join(" "); ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "unsigned": specWord(this, ts, "unsigned", 9/*Signed*/); continue;
        case "_Decimal32": case "_Decimal64": case "_Decimal128":
          ts.base = t.t; ts.kind = B[t.t.slice(1)]; ts.hasBase = true; ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "_Complex": case "_Imaginary":
          ts.words.push(t.t); ts.base = ts.words.join(" "); ts.saw = true; if (this.pos < this.last) this.pos++; continue;
        case "_BitInt": {
          if (this.pos < this.last) this.pos++;
          this.expectP("(", "after _BitInt");
          let w = "";
          while (!this.eof() && !(this.toks[this.pos].p === ")")) { w += this.toks[this.pos].t; if (this.pos < this.last) this.pos++; }
          this.expectP(")", "after _BitInt width");
          ts.base = "_BitInt(" + w + ")"; ts.kind = 19/*BitInt*/; ts.hasBase = true; ts.saw = true;
          continue;
        }
        case "typeof": case "typeof_unqual": case "__typeof": case "__typeof__": {
          const unqual = t.t !== "typeof" && t.t !== "__typeof__" && t.t !== "__typeof";
          if (this.pos < this.last) this.pos++;
          this.expectP("(", "after typeof");
          let text = "typeof(";
          if (this.isTypeStart()) {
            text += this.parseTypeName().text;
            this.expectP(")", "to close typeof(");
          } else {
            let depth = 1, b = "";
            while (!this.eof() && depth > 0) {
              const c = this.toks[this.pos];
              if (c.k === PU) {
                if (c.p === "(") depth++;
                else if (c.p === ")") { depth--; if (depth === 0) { if (this.pos < this.last) this.pos++; break; } }
              }
              if (b.length > 0 && c.ws) b += " ";
              b += c.t;
              if (this.pos < this.last) this.pos++;
            }
            text += b;
          }
          if (unqual) text = text.replace("typeof(", "typeof_unqual(");
          ts.base = text + ")"; ts.kind = 18/*Typeof*/; ts.hasBase = true; ts.saw = true;
          continue;
        }
        case "struct": case "union": case "enum": {
          if (this.pos < this.last) this.pos++;
          const node = this.parseTagSpec(t.t);
          ts.tagNode = node;
          ts.kind = t.t === "struct" ? 15/*Struct*/ : t.t === "union" ? 16/*Union*/ : 17/*Enum*/;
          const td = this.fa.tags[this.fa.nodes[node].a - 1];
          ts.base = t.t;
          if (td.nameIdx !== 0) ts.base += " " + this.fa.name(td.nameIdx);
          if (td.hasUnderlying) ts.base += " : " + this.fa.name(td.underlyingIdx);
          ts.tag = t.t; ts.hasBase = true; ts.saw = true;
          continue;
        }
        default:
          if (this.isTypedefName(t.t)) {
            ts.base = t.t; ts.kind = 14/*Typedef*/; ts.typedef = t.t; ts.hasBase = true; ts.saw = true; if (this.pos < this.last) this.pos++;
            continue;
          }
          if (!ts.hasBase && this.unknownTypeFollows()) {
            ts.words.push(t.t); ts.base = ts.words.join(" "); ts.kind = 14/*Typedef*/; ts.typedef = t.t; ts.hasBase = true; ts.saw = true;
            if (this.pos < this.last) this.pos++;
            continue;
          }
          return ts;
      }
    }
    if (attrAhead(this.toks[this.pos])) this.parseAttributes();
    return ts;
  }
  unknownTypeFollows() {
    if (this.krMode) return false;
    const nx = this.peek(1);
    if (nx.k === ID) {
      switch (nx.t) {
        case "__attribute__": case "__attribute": case "__asm__": case "__asm": case "asm": case "__extension__": return false;
      }
      if (UNKNOWN_TYPE_NEXT.has(nx.t)) return true;
      return !isKeyword(nx.t);
    }
    if (nx.k === PU) return nx.p === "*" || nx.p === "[" || nx.p === ";" || nx.p === "=" || nx.p === "(";
    return false;
  }
  parseTagSpec(kind) {
    const kwIx = this.pos;
    if (attrAhead(this.toks[this.pos])) this.parseAttributes();
    const td = { nameIdx: 0, tag: kind, child0: 0, nChild: 0, underlyingIdx: 0, hasUnderlying: false, defined: false, line: 0, lineEnd: 0 };
    if (this.toks[this.pos].k === ID) {
      const nm = this.advance();
      td.nameIdx = this.fa.intern(nm.t);
      td.line = nm.line;
    }
    if (kind === "enum" && (this.toks[this.pos].p === ":")) {
      if (this.pos < this.last) this.pos++;
      td.underlyingIdx = this.fa.intern(this.parseTypeName().text);
      td.hasUnderlying = true;
    }
    if ((this.toks[this.pos].p === "{")) {
      const ob = this.advance();
      td.defined = true;
      if (td.line === 0) td.line = ob.line;
      const child0 = this.fa.nodes.length;
      if (kind === "enum") {
        while (!this.eof() && !(this.toks[this.pos].p === "}")) {
          if (attrAhead(this.toks[this.pos])) this.parseAttributes();
          if ((this.toks[this.pos].p === "}")) break;
          if (this.toks[this.pos].k !== ID) {
            this.errAt(this.pos, "expected enumerator name");
            this.syncTo(this.pos, "enumerator");
            if ((this.toks[this.pos].p === ",")) if (this.pos < this.last) this.pos++;
            continue;
          }
          const ni = this.pos, nm = this.advance();
          const ed = { nameIdx: this.fa.intern(nm.t), valueIdx: 0, hasValue: false, line: nm.line };
          if ((this.toks[this.pos].p === "=" && (this.pos < this.last && this.pos++, true))) {
            this.assignment();
            const [vs, ve] = this.lastSpan();
            if (ve > vs) { ed.hasValue = true; ed.valueIdx = this.fa.intern(rawText(this.srcOf(ni), vs, ve)); }
          }
          const en = nodeAt(10/*Enumerator*/, nm);
          en.file = this.files[ni];
          if (ed.hasValue) en.f = 1;
          this.fa.enums.push(ed);
          en.a = this.fa.enums.length;
          en.b = ed.valueIdx;
          this.addNode(en);
          this.addEnumConst(this.fa.name(ed.nameIdx));
          if (!(this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) break;
        }
        this.expectP("}", "to close enum");
      } else {
        while (!this.eof() && !(this.toks[this.pos].p === "}")) this.parseMember(kind);
        this.expectP("}", "to close " + kind);
      }
      if (attrAhead(this.toks[this.pos])) this.parseAttributes();
      td.child0 = child0;
      td.nChild = this.fa.nodes.length - child0;
      td.lineEnd = this.toks[this.pos].line;
    } else td.lineEnd = td.line;
    this.fa.tags.push(td);
    const n = mkNode(kind === "union" ? 7/*UnionSpec*/ : kind === "enum" ? 8/*EnumSpec*/ : 6/*StructSpec*/);
    n.a = this.fa.tags.length;
    if (td.line !== 0) n.line = td.line;
    n.file = this.files[kwIx];
    const node = this.addNode(n);
    if (td.nameIdx !== 0) this.addTag(this.fa.name(td.nameIdx));
    return node;
  }
  parseMember(kind) {
    const ti = this.pos, t = this.toks[this.pos];
    if ((this.toks[this.pos].p === ";")) { if (this.pos < this.last) this.pos++; return; }
    if ((this.toks[this.pos].t === "_Static_assert") || (this.toks[this.pos].t === "static_assert")) { this.parseStaticAssert(); return; }
    const specs = this.parseDeclSpecs();
    if (!specs.saw) { this.errAt(ti, "bad member declaration"); this.syncTo(ti, "member"); return; }
    const fieldNode = () => { const n = nodeAt(9/*Field*/, t); n.a = this.fa.fields.length; n.file = this.files[ti]; this.addNode(n); };
    if ((this.toks[this.pos].p === ";") && (specs.kind === 15/*Struct*/ || specs.kind === 16/*Union*/)) {
      if (this.pos < this.last) this.pos++;
      const bt = baseText(specs);
      this.fa.fields.push({ nameIdx: 0, typeIdx: this.fa.intern(bt), ptr: 0, ptrTypeIdx: this.fa.intern(bt), arrayLen: 0, bitfield: 0,
        isFnptr: false, depth: this.scopes.length, isConst: false, line: t.line });
      fieldNode();
      return;
    }
    for (;;) {
      if (attrAhead(this.toks[this.pos])) this.parseAttributes();
      const d = this.declarator(specs, false);
      if (attrAhead(this.toks[this.pos])) this.parseAttributes();
      let bf = 0;
      if ((this.toks[this.pos].p === ":" && (this.pos < this.last && this.pos++, true))) { this.assignment(); const [, ve] = this.lastSpan(); if (ve > 0) bf = -1; }
      const bt = baseText(specs);
      const typeIdx = this.fa.intern(bt), ptrTypeIdx = this.fa.intern(bt + stars(d.ptr));
      this.fa.fields.push({ nameIdx: d.name !== "" ? this.fa.intern(d.name) : 0, typeIdx, ptr: d.ptr, ptrTypeIdx, arrayLen: arrayLenI(d), bitfield: bf, isFnptr: d.fnptr, depth: d.depth,
        isConst: specs.constq, line: t.line });
      fieldNode();
      if ((this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) continue;
      this.expectP(";", "after member");
      break;
    }
  }
  parseStaticAssert() {
    const t = this.advance();
    this.expectP("(", "after _Static_assert");
    const n = mkNode(12/*StaticAssert*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    const child0 = this.fa.nodes.length;
    if (!(this.toks[this.pos].p === ")")) this.assignment();
    if ((this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) while (this.toks[this.pos].k === STR) if (this.pos < this.last) this.pos++;
    this.expectP(")", "to close _Static_assert");
    this.expectP(";", "after _Static_assert");
    n.a = child0;
    n.b = this.fa.nodes.length - child0;
    n.end = this.toks[this.pos].end;
    this.addNode(n);
  }
  // ---- declarators
  skipDecorationIdents(specs) {
    if (specs === null || !specs.saw || this.krMode) return;
    const b = specs.base;
    const baseIsDecoration = specs.kind === 14/*Typedef*/ && specs.hasBase && !isKeyword(b) && !this.isTypedefName(b) &&
      /^[A-Za-z_\x80-\xff]/.test(b) && !/[ *([;,]/.test(b);
    for (;;) {
      const t = this.toks[this.pos];
      if (t.k !== ID || isKeyword(t.t) || this.isTypedefName(t.t)) return;
      const n1 = this.peek(1);
      if (n1.k !== ID || isKeyword(n1.t)) return;
      const n2 = this.peek(2);
      if (n2.k !== PU) return;
      if (n2.p === "(") {}
      else if (n2.p === "," || n2.p === ")") { if (!baseIsDecoration) return; }
      else return;
      if (this.pos < this.last) this.pos++;
    }
  }
  declarator(specs, abstract) {
    let d = newDcl();
    for (;;) {
      if (attrAhead(this.toks[this.pos])) this.parseAttributes();
      if (!(this.toks[this.pos].p === "*" && (this.pos < this.last && this.pos++, true))) break;
      d.ptr++;
      for (;;) {
        const t = this.toks[this.pos];
        if (t.k !== ID) break;
        const q = t.t;
        if (q === "const" || q === "__const" || q === "__const__") d.constq = true;
        else if (q === "volatile" || q === "__volatile" || q === "__volatile__") d.volatileq = true;
        else if (!(q === "restrict" || q === "__restrict" || q === "__restrict__" || q === "_Atomic")) break;
        if (this.pos < this.last) this.pos++;
      }
    }
    this.skipDecorationIdents(specs);
    d = this.directDeclarator(d, abstract);
    if (attrAhead(this.toks[this.pos])) this.parseAttributes();
    while ((this.toks[this.pos].t === "__asm__") || (this.toks[this.pos].t === "__asm") || (this.toks[this.pos].t === "asm")) { if (this.pos < this.last) this.pos++; this.skipBalanced(); }
    return d;
  }
  directDeclarator(d, abstract) {
    const t = this.toks[this.pos];
    if (t.k === ID) {
      if (!isKeyword(t.t)) { d.name = t.t; d.nameTok = t; d.hasName = true; if (this.pos < this.last) this.pos++; }
    } else if (t.k === PU && t.p === "(") {
      const nx = this.peek(1);
      if (abstract && ((nx.k === PU && (nx.p === ")" || nx.p === "...")) || (nx.k === ID && PAREN_PARAM.has(nx.t)))) {
        return this.declaratorSuffix(d, abstract);
      }
      this.advance();
      d.depth++;
      const inner = this.declarator(null, abstract);
      this.expectP(")", "to close declarator");
      if (inner.hasName) { d.name = inner.name; d.nameTok = inner.nameTok; d.hasName = true; d.parenName = true; }
      d.ptr += inner.ptr;
      d.depth += inner.depth;
      d.constq = d.constq || inner.constq;
    }
    return this.declaratorSuffix(d, abstract);
  }
  declaratorSuffix(d, abstract) {
    while (!this.eof()) {
      const t = this.toks[this.pos];
      if (t.k === PU && t.p === "[") {
        const nx = this.peek(1);
        if (nx.k === PU && nx.p === "[") { if (attrAhead(this.toks[this.pos])) this.parseAttributes(); continue; }
        this.advance();
        d.isArray = true; d.arrayConst = false; d.arrayLen = 0n;
        if ((this.toks[this.pos].t === "static")) { d.arrayConst = true; if (this.pos < this.last) this.pos++; }
        while (this.toks[this.pos].k === ID) {
          const w = this.toks[this.pos].t;
          if (w === "const" || w === "volatile" || w === "restrict" || w === "__restrict" || w === "__restrict__" || w === "_Atomic" || w === "static") {
            d.arrayConst = d.arrayConst || w === "static";
            if (this.pos < this.last) this.pos++;
            continue;
          }
          break;
        }
        if ((this.toks[this.pos].p === "*") && this.peek(1).k === PU && this.peek(1).p === "]") {
          if (this.pos < this.last) this.pos++;
          d.arrayConst = false; d.arrayLen = -1n;
        } else if (!(this.toks[this.pos].p === "]")) {
          const si = this.pos, start = this.toks[this.pos];
          this.assignment();
          const end = this.prevEnd();
          const s = trimSpace(rawText(this.srcOf(si), start.off, end));
          const v = s === "" ? null : goParseInt(s);
          if (v !== null) { d.arrayLen = v; d.arrayConst = true; } else d.arrayLen = -1n;
        }
        this.expectP("]", "to close array");
        continue;
      }
      if (t.k === PU && t.p === "(") {
        if (this.pos < this.last) this.pos++;
        const [params, variadic, kr] = this.paramList();
        d.isFn = true; d.params = params; d.variadic = variadic; d.kr = kr;
        if (kr) for (const pt of params) d.krNames.push(pt.name);
        d.fnptr = d.parenName && d.ptr > 0;
        continue;
      }
      return d;
    }
    return d;
  }
  paramList() {
    const params = [];
    let variadic = false;
    if ((this.toks[this.pos].p === ")")) { if (this.pos < this.last) this.pos++; return [[], false, false]; }
    if ((this.toks[this.pos].t === "void") && this.peek(1).k === PU && this.peek(1).p === ")") { if (this.pos < this.last) this.pos++; this.advance(); return [[], false, false]; }
    let kr = false;
    while (!this.eof()) {
      if (attrAhead(this.toks[this.pos])) this.parseAttributes();
      if ((this.toks[this.pos].p === "..." && (this.pos < this.last && this.pos++, true))) { this.closeParamList(); return [params, true, kr]; }
      const ti = this.pos, t = this.toks[this.pos];
      const specs = this.parseDeclSpecs();
      if (!specs.saw) {
        if (t.k === ID && !isKeyword(t.t)) {
          if (this.pos < this.last) this.pos++;
          kr = true;
          params.push({ name: t.t, typ: "", ptr: 0, arrayLen: 0, isConst: false, variadic: false, unnamed: false, line: t.line });
          if ((this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) continue;
          this.closeParamList();
          return [params, false, kr];
        }
        this.errAt(ti, "expected parameter declaration");
        if ((this.toks[this.pos].p === ")")) { if (this.pos < this.last) this.pos++; return [params, variadic, kr]; }
        if ((this.toks[this.pos].p === ",")) { if (this.pos < this.last) this.pos++; continue; }
        this.closeParamList();
        return [params, variadic, kr];
      }
      const isVariadicArg = (this.toks[this.pos].p === "...");
      const d = this.declarator(specs, true);
      if (isVariadicArg) variadic = true;
      params.push({ name: d.hasName ? d.name : "", typ: baseText(specs) + stars(d.ptr), ptr: d.ptr, arrayLen: arrayLenI(d),
        isConst: specs.constq, variadic: variadic && d.name === "" && !d.hasName, unnamed: !d.hasName, line: t.line });
      while (this.toks[this.pos].k === ID && !isKeyword(this.toks[this.pos].t)) {
        const nx = this.peek(1);
        if (nx.k === ID && !isKeyword(nx.t)) { if (this.pos < this.last) this.pos++; continue; }
        if (nx.k === PU && nx.p === "(") { if (this.pos < this.last) this.pos++; this.skipBalanced(); continue; }
        if (nx.k === PU && (nx.p === "," || nx.p === ")")) if (this.pos < this.last) this.pos++;
        break;
      }
      if ((this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) continue;
      this.closeParamList();
      return [params, variadic, kr];
    }
    return [params, variadic, kr];
  }
  parseTypeName() {
    const specs = this.parseDeclSpecs();
    const d = this.declarator(specs, true);
    return { text: baseText(specs) + stars(d.ptr), ptr: d.ptr, isFn: d.isFn, isArray: d.isArray };
  }
  // ---- external declarations
  parseTU() {
    const mk = this.addNode(mkNode(1/*TU*/));
    this.fa.nodes[mk].a = this.fa.nodes.length;
    let lastPos = -1, reps = 0;
    while (!this.eof()) {
      if (this.pos === lastPos) {
        reps++;
        if (reps > 4) { this.errAt(this.pos, "no progress; skipping token"); if (this.pos < this.last) this.pos++; reps = 0; continue; }
      } else { reps = 0; lastPos = this.pos; }
      this.parseExternal();
      if (this.fa.nodes.length > 4 << 20) { this.errAt(this.pos, "node budget exceeded"); return; }
    }
    const tu = this.fa.nodes[mk];
    tu.b = this.fa.nodes.length - tu.a;
    tu.end = this.toks[this.last].end;
  }
  parseExternal() {
    let ti = this.pos;
    if ((this.toks[this.pos].p === ";")) { if (this.pos < this.last) this.pos++; return; }
    if ((this.toks[this.pos].p === "}")) { this.errAt(ti, "unexpected '}' at file scope"); if (this.pos < this.last) this.pos++; return; }
    if ((this.toks[this.pos].t === "__extension__")) { if (this.pos < this.last) this.pos++; return; }
    if ((this.toks[this.pos].t === "asm") || (this.toks[this.pos].t === "__asm__") || (this.toks[this.pos].t === "__asm")) { this.skipAsm(); (this.toks[this.pos].p === ";" && (this.pos < this.last && this.pos++, true)); return; }
    if ((this.toks[this.pos].t === "_Static_assert") || (this.toks[this.pos].t === "static_assert")) { this.parseStaticAssert(); return; }
    if ((this.toks[this.pos].t === "extern") && this.peek(1).k === STR) {
      if (this.pos < this.last) this.pos++; this.advance();
      if ((this.toks[this.pos].p === "{" && (this.pos < this.last && this.pos++, true))) {
        while (!this.eof() && !(this.toks[this.pos].p === "}")) this.parseExternal();
        (this.toks[this.pos].p === "}" && (this.pos < this.last && this.pos++, true));
        return;
      }
      this.parseExternal();
      return;
    }
    if (this.plainIdent(this.toks[this.pos]) && this.plainIdent(this.peek(1))) {
      const [j, nstars] = this.skipPtrQuals(2);
      if (nstars > 0) {
        const nm = this.peek(j), after = this.peek(j + 1);
        if (nm.k === ID && !C_KEYWORDS.has(nm.t) && after.k === PU && after.p === "(") {
          this.scopes[0].setTypedef(this.peek(1).t, true);
          if (this.pos < this.last) this.pos++;
          ti = this.pos;
        }
      }
    }
    if (attrAhead(this.toks[this.pos])) this.parseAttributes();
    const specs = this.parseDeclSpecs();
    if (attrAhead(this.toks[this.pos])) this.parseAttributes();
    if (!specs.saw) { this.errAt(ti, "expected declaration"); this.syncTo(ti, "external declaration"); return; }
    if (specs.isTypedef) {
      const d0 = this.declarator(specs, false);
      if (d0.isFn && ((this.toks[this.pos].p === "{") || d0.kr)) { this.parseFuncDef(specs, d0, ti); return; }
      this.parseDeclList(specs, d0, ti);
      return;
    }
    let d = this.declarator(specs, false);
    if (d.isFn && !(this.toks[this.pos].p === "{") && !(this.toks[this.pos].p === ";") && krShaped(d)) {
      const save = this.pos, saveErrs = this.errs.length;
      this.krMode = true;
      const d2 = this.declarator(specs, false);
      this.krMode = false;
      if (d2.isFn && d2.kr) d = d2;
      else {
        this.pos = save;
        this.nErrors = saveErrs;
        this.errs.length = Math.min(saveErrs, this.errs.length);
      }
    }
    if (d.isFn && ((this.toks[this.pos].p === "{") || d.kr)) { this.parseFuncDef(specs, d, ti); return; }
    this.parseDeclList(specs, d, ti);
  }
  parseDeclList(specs, first, ti) {
    let d = first, have = true;
    for (;;) {
      if (!have) { if (attrAhead(this.toks[this.pos])) this.parseAttributes(); d = this.declarator(specs, false); if (attrAhead(this.toks[this.pos])) this.parseAttributes(); }
      have = false;
      const t = this.toks[ti];
      const nd = { nameIdx: 0, typeIdx: this.fa.intern(baseText(specs) + stars(d.ptr)), typename: 0, bitfield: 0, arrayLen: arrayLenI(d),
        ptr: d.ptr, init: 0, storage: specs.storage, kind: 0, line: t.line };
      if (d.hasName) nd.nameIdx = this.fa.intern(d.name);
      const kind = specs.isTypedef ? 2 : d.isFn && !d.fnptr ? 1 : 0;
      nd.kind = kind;
      if (kind === 2 && d.hasName) this.addTypedef(d.name);
      else if (kind === 0 && d.hasName && this.funcDepth > 0) this.addTypedefGuard(d.name);
      if ((this.toks[this.pos].p === "=" && (this.pos < this.last && this.pos++, true))) { nd.init = this.initializer(); nd.storage |= 16384/*HasInit*/; }
      this.fa.decls.push(nd);
      const n = nodeAt(3/*Decl*/, t);
      n.a = this.fa.decls.length;
      if (d.hasName) n.b = nd.nameIdx;
      if (nd.init !== 0) n.f |= 16384/*HasInit*/;
      n.file = this.files[ti];
      this.addNode(n);
      if ((this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) { ti = this.pos; continue; }
      this.expectP(";", "after declaration");
      break;
    }
  }
  parseFuncDef(specs, d, ti) {
    if (d.kr) {
      while (!this.eof() && !(this.toks[this.pos].p === "{")) {
        const ki = this.pos;
        const kspecs = this.parseDeclSpecs();
        if (!kspecs.saw) { this.errAt(ki, "bad K&R declaration"); if (this.pos < this.last) this.pos++; continue; }
        for (;;) {
          if (attrAhead(this.toks[this.pos])) this.parseAttributes();
          const kd = this.declarator(kspecs, false);
          for (const pt of d.params) {
            if (pt.name === kd.name) { pt.typ = baseText(kspecs) + stars(kd.ptr); pt.ptr = kd.ptr; pt.isConst = kspecs.constq; pt.unnamed = false; }
          }
          if (!(this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) break;
        }
        this.expectP(";", "after K&R declaration");
      }
    }
    const ob = this.toks[this.pos];
    this.funcDepth++;
    this.pushScope();
    const p0 = this.fa.nodes.length;
    const sig = funcSignature(specs, d);
    const ret = baseText(specs) + stars(d.ptr);
    d.params.forEach((pt, i) => {
      const pd = { nameIdx: 0, typeIdx: this.fa.intern(pt.typ), ptr: pt.ptr, arrayLen: pt.arrayLen, storage: 0,
        variadic: pt.variadic && i === d.params.length - 1 && d.variadic, unnamed: pt.unnamed, isConst: pt.isConst, line: pt.line };
      if (pt.name !== "") pd.nameIdx = this.fa.intern(pt.name);
      this.fa.params.push(pd);
      const pn = mkNode(4/*Param*/);
      pn.a = this.fa.params.length; pn.line = pt.line;
      this.addNode(pn);
      if (pt.name !== "") this.addTypedefGuard(pt.name);
    });
    const fd = { nameIdx: this.fa.intern(d.name), sigIdx: this.fa.intern(sig), retIdx: this.fa.intern(ret), param0: p0,
      nParams: d.params.length, body: 0, lineEnd: ob.line, storage: specs.storage, variadic: d.variadic, kr: d.kr };
    const body = this.block();
    this.popScope();
    this.funcDepth--;
    fd.body = body;
    if (body !== 0) fd.lineEnd = this.fa.nodes[body].line;
    this.fa.funcs.push(fd);
    const t = this.toks[ti];
    const fn = mkNode(2/*FuncDef*/);
    fn.a = this.fa.funcs.length; fn.file = this.files[ti]; fn.off = t.off; fn.line = t.line; fn.col = t.col;
    fn.end = body !== 0 ? this.fa.nodes[body].end : t.end;
    this.addNode(fn);
  }
  // ---- statements
  block() {
    const ob = this.advance();
    this.pushScope();
    const mk = this.addNode(nodeAt(13/*CompoundStmt*/, ob));
    const n = this.fa.nodes[mk];
    n.a = this.fa.nodes.length;
    while (!(this.toks[this.pos].p === "}") && !this.eof()) this.stmt();
    n.b = this.fa.nodes.length - n.a;
    if ((this.toks[this.pos].p === "}" && (this.pos < this.last && this.pos++, true))) n.end = this.toks[this.pos - 1].end;
    this.popScope();
    return mk;
  }
  isDeclStart() {
    const t = this.toks[this.pos];
    if (t.k !== ID) return false;
    return DECL_START.has(t.t) || this.isTypedefName(t.t);
  }
  isTypeStart() {
    const t = this.toks[this.pos];
    return t.k === ID && (TYPE_START_STMT.has(t.t) || this.isTypedefName(t.t));
  }
  isTypeStartAt(n) {
    const t = this.peek(n);
    return t.k === ID && (TYPE_START_AT.has(t.t) || this.isTypedefName(t.t));
  }
  stmt() {
    let t = this.toks[this.pos];
    if (t.k === PU && t.p === "[") { if (attrAhead(this.toks[this.pos])) this.parseAttributes(); t = this.toks[this.pos]; }
    if ((this.toks[this.pos].p === "{")) return this.block();
    if ((this.toks[this.pos].p === ";")) { if (this.pos < this.last) this.pos++; return this.addNode(nodeAt(15/*ExprStmt*/, t)); }
    if (t.k === ID) {
      switch (t.t) {
        case "if": return this.ifStmt();
        case "switch": return this.switchStmt();
        case "while": return this.whileStmt();
        case "do": return this.doStmt();
        case "for": return this.forStmt();
        case "return": return this.returnStmt();
        case "break": case "continue": {
          if (this.pos < this.last) this.pos++;
          this.expectP(";", "after " + t.t);
          return this.addNode(nodeAt(t.t === "break" ? 23/*Break*/ : 24/*Continue*/, t));
        }
        case "goto": {
          if (this.pos < this.last) this.pos++;
          const n = mkNode(25/*Goto*/);
          n.off = t.off; n.line = t.line; n.col = t.col;
          if ((this.toks[this.pos].p === "&&" && (this.pos < this.last && this.pos++, true))) { if (this.toks[this.pos].k === ID) n.a = this.fa.intern(this.advance().t); }
          else if (this.toks[this.pos].k === ID) n.a = this.fa.intern(this.advance().t);
          else if (this.toks[this.pos].p === "*") { let d = 0; while (this.pos < this.last) { const q = this.toks[this.pos]; if (q.p === "(" || q.p === "[") d++; else if (q.p === ")" || q.p === "]") d--; else if (q.p === ";" && d <= 0) break; this.pos++; } }
          this.expectP(";", "after goto");
          n.end = this.prevEnd();
          return this.addNode(n);
        }
        case "case": case "default": return this.caseStmt();
        case "_Static_assert": case "static_assert": this.parseStaticAssert(); return this.fa.nodes.length - 1;
        case "asm": case "__asm__": case "__asm": {
          const at = this.toks[this.pos];
          this.skipAsm();
          (this.toks[this.pos].p === ";" && (this.pos < this.last && this.pos++, true));
          const n = nodeAt(27/*Asm*/, at);
          n.end = this.prevEnd();
          return this.addNode(n);
        }
        case "__extension__": this.advance(); return this.stmt();
      }
      if (this.peek(1).k === PU && this.peek(1).p === ":") {
        if (this.pos < this.last) this.pos++; this.advance();
        const n = mkNode(26/*Label*/);
        n.a = this.fa.intern(t.t); n.off = t.off; n.line = t.line; n.col = t.col;
        const mk = this.addNode(n);
        n.b = this.stmt();
        n.end = this.prevEnd();
        return mk;
      }
    }
    if (this.isDeclStart() || this.inferTypedef()) {
      const mk = this.tryDeclStatement();
      if (mk !== null) return mk;
    }
    const n = mkNode(15/*ExprStmt*/);
    const mk = this.addNode(n);
    n.a = this.expr();
    this.expectP(";", "after expression statement");
    n.off = t.off; n.end = this.prevEnd(); n.line = t.line; n.col = t.col;
    return mk;
  }
  tryDeclStatement() {
    const fa = this.fa;
    const save = { pos: this.pos, nodes: fa.nodes.length, decls: fa.decls.length, params: fa.params.length, tags: fa.tags.length,
      fields: fa.fields.length, enums: fa.enums.length, types: fa.types.length, inits: fa.inits.length, errs: this.nErrors };
    const ti = this.pos, t = this.toks[this.pos];
    const n = mkNode(14/*DeclStmt*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    const mk = this.addNode(n);
    n.a = fa.nodes.length;
    if (attrAhead(this.toks[this.pos])) this.parseAttributes();
    const specs = this.parseDeclSpecs();
    if (!specs.saw) { this.rollback(save); return null; }
    const d0 = this.declarator(specs, false);
    this.parseDeclList(specs, d0, ti);
    if (this.nErrors > save.errs) { this.rollback(save); return null; }
    n.b = fa.nodes.length - n.a;
    n.end = this.prevEnd();
    return mk;
  }
  rollback(s) {
    const fa = this.fa;
    this.pos = s.pos;
    fa.nodes.length = s.nodes; fa.decls.length = s.decls; fa.params.length = s.params; fa.tags.length = s.tags;
    fa.fields.length = s.fields; fa.enums.length = s.enums; fa.types.length = s.types; fa.inits.length = s.inits;
    this.errs.length = Math.min(s.errs, this.errs.length);
    this.nErrors = s.errs;
  }
  ifStmt() {
    const t = this.advance();
    this.expectP("(", "after if");
    const c = this.expr();
    this.expectP(")", "after if condition");
    const th = this.stmt();
    let el = 0;
    if (this.acceptId("else")) el = this.stmt();
    const n = mkNode(16/*If*/);
    Object.assign(n, { a: c, b: th, c: el, off: t.off, end: this.prevEnd(), line: t.line, col: t.col });
    return this.addNode(n);
  }
  switchStmt() {
    const t = this.advance();
    this.expectP("(", "after switch");
    const c = this.expr();
    this.expectP(")", "after switch condition");
    const b = this.stmt();
    const n = mkNode(17/*Switch*/);
    Object.assign(n, { a: c, b, off: t.off, end: this.prevEnd(), line: t.line, col: t.col });
    return this.addNode(n);
  }
  whileStmt() {
    const t = this.advance();
    const n = mkNode(19/*While*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    const mk = this.addNode(n);
    this.expectP("(", "after while");
    const c = this.expr();
    this.expectP(")", "after while condition");
    this.loopDepth++;
    const b = this.stmt();
    this.loopDepth--;
    n.a = c; n.b = b; n.end = this.prevEnd();
    return mk;
  }
  doStmt() {
    const t = this.advance();
    const n = mkNode(20/*Do*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    const mk = this.addNode(n);
    this.loopDepth++;
    n.a = this.stmt();
    this.loopDepth--;
    if (this.acceptId("while")) {
      this.expectP("(", "after do while");
      const c = this.expr();
      this.expectP(")", "after do while");
      this.expectP(";", "after do-while");
      n.b = c;
    }
    n.end = this.prevEnd();
    return mk;
  }
  forStmt() {
    const t = this.advance();
    this.expectP("(", "after for");
    this.pushScope();
    const n = mkNode(21/*For*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    const mk = this.addNode(n);
    let init = 0;
    if (!(this.toks[this.pos].p === ";")) {
      if (this.isDeclStart() || this.inferTypedef()) {
        const d = this.tryDeclStatement();
        if (d !== null) init = d;
      } else {
        const ie = mkNode(15/*ExprStmt*/);
        const iex = this.addNode(ie);
        ie.a = this.expr();
        this.expectP(";", "after for-init");
        ie.end = this.prevEnd();
        init = iex;
      }
    } else if (this.pos < this.last) this.pos++;
    let cond = 0;
    if (!(this.toks[this.pos].p === ";")) cond = this.expr();
    this.expectP(";", "after for-cond");
    let step = 0;
    if (!(this.toks[this.pos].p === ")")) step = this.expr();
    this.expectP(")", "after for-clauses");
    this.loopDepth++;
    const b = this.stmt();
    this.loopDepth--;
    n.a = init; n.b = cond; n.c = step; n.d = b; n.end = this.prevEnd();
    this.popScope();
    return mk;
  }
  returnStmt() {
    const t = this.advance();
    const n = mkNode(22/*Return*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    if (!(this.toks[this.pos].p === ";")) n.a = this.expr();
    this.expectP(";", "after return");
    n.end = this.prevEnd();
    return this.addNode(n);
  }
  caseStmt() {
    const t = this.advance();
    const n = mkNode(18/*Case*/);
    n.off = t.off; n.line = t.line; n.col = t.col;
    if (t.t === "default") this.expectP(":", "after default");
    else {
      n.a = this.assignment();
      if ((this.toks[this.pos].p === "..." && (this.pos < this.last && this.pos++, true))) this.assignment();
      this.expectP(":", "after case");
    }
    n.b = this.stmt();
    n.end = this.prevEnd();
    return this.addNode(n);
  }
  // ---- expressions
  span2(k, f, l, r) {
    const ns = this.fa.nodes, L = ns[l];
    const n = mkNode(k);
    n.f = f; n.a = l; n.b = r; n.off = L.off; n.end = ns[r].end; n.line = L.line; n.col = L.col;
    return this.addNode(n);
  }
  expr() {
    let l = this.assignment();
    while ((this.toks[this.pos].p === ",")) { if (this.pos < this.last) this.pos++; const r = this.assignment(); l = this.span2(42/*Comma*/, 0, l, r); }
    return l;
  }
  assignment() {
    const c = this.binary(1);
    if ((this.toks[this.pos].p === "?")) {
      if (this.pos < this.last) this.pos++;
      const th = this.expr();
      this.expectP(":", "in ?:");
      const el = this.assignment();
      const ns = this.fa.nodes;
      const n = mkNode(41/*Cond*/);
      Object.assign(n, { a: c, b: th, c: el, off: ns[c].off, end: ns[el].end, line: ns[c].line, col: ns[c].col });
      return this.addNode(n);
    }
    const t = this.toks[this.pos];
    if (t.k === PU && ASSIGN_OPS.has(t.p)) {
      if (this.pos < this.last) this.pos++;
      const r = this.assignment();
      return this.span2(40/*Assign*/, PCODE[t.p], c, r);
    }
    return c;
  }
  binary(minPrec) {
    let l = this.unary();
    for (;;) {
      const t = this.toks[this.pos];
      if (t.k !== PU) return l;
      const prec = BIN_PREC[t.p];
      if (prec === undefined || prec < minPrec) return l;
      if (this.pos < this.last) this.pos++;
      const r = this.binary(prec + 1);
      l = this.span2(39/*Binary*/, PCODE[t.p], l, r);
    }
  }
  unaryNode(t, op, o) {
    const n = nodeAt(38/*Unary*/, t);
    n.f = op; n.a = o; n.end = this.fa.nodes[o].end;
    return this.addNode(n);
  }
  unary() {
    const t = this.toks[this.pos];
    if (t.k === PU) {
      switch (t.p) {
        case "++": case "--": case "+": case "-": case "!": case "~": case "*": case "&": {
          if (this.pos < this.last) this.pos++;
          return this.unaryNode(t, PCODE[t.p], this.unary());
        }
        case "&&":
          this.advance();
          if (this.toks[this.pos].k === ID) {
            const nm = this.advance();
            const n = nodeAt(28/*Ident*/, t);
            n.a = this.fa.intern(nm.t); n.f = 1; n.end = nm.end;
            return this.addNode(n);
          }
          break;
        case "(":
          if (this.peek(1).k !== EOF_K && (this.isTypeStartAt(1) || this.inferCastType())) {
            const save = this.pos;
            if (this.pos < this.last) this.pos++;
            const tn = this.parseTypeName();
            if ((this.toks[this.pos].p === ")" && (this.pos < this.last && this.pos++, true))) {
              if ((this.toks[this.pos].p === "{")) {
                const il = this.initializer();
                const n = nodeAt(50/*CompoundLit*/, t);
                n.a = this.addTypeD(tn); n.b = il; n.end = this.fa.nodes[il].end;
                return this.addNode(n);
              }
              const o = this.unary();
              const n = nodeAt(43/*Cast*/, t);
              n.a = this.addTypeD(tn); n.b = o; n.end = this.fa.nodes[o].end;
              return this.addNode(n);
            }
            this.pos = save;
          }
          break;
      }
    }
    if (t.k === ID) {
      switch (t.t) {
        case "sizeof": {
          if (this.pos < this.last) this.pos++;
          if ((this.toks[this.pos].p === "(") && (this.isTypeStartAt(1) || this.inferSizeofType())) {
            const save = this.pos;
            if (this.pos < this.last) this.pos++;
            const tn = this.parseTypeName();
            if ((this.toks[this.pos].p === ")" && (this.pos < this.last && this.pos++, true))) {
              const n = nodeAt(44/*SizeofT*/, t);
              n.a = this.addTypeD(tn); n.end = this.prevEnd();
              return this.addNode(n);
            }
            this.pos = save;
          }
          const o = this.unary();
          const n = nodeAt(45/*SizeofE*/, t);
          n.a = o; n.end = this.fa.nodes[o].end;
          return this.addNode(n);
        }
        case "_Alignof": case "alignof": case "__alignof": case "__alignof__": {
          if (this.pos < this.last) this.pos++;
          this.expectP("(", "after alignof");
          const tn = this.parseTypeName();
          this.expectP(")", "after alignof type");
          const n = nodeAt(46/*AlignofT*/, t);
          n.a = this.addTypeD(tn); n.end = this.prevEnd();
          return this.addNode(n);
        }
        case "__extension__": this.advance(); return this.unary();
        case "_Generic": return this.genericSelection();
        case "__real": case "__imag": this.advance(); return this.unaryNode(t, PCODE["*"], this.unary());
      }
    }
    return this.postfix();
  }
  addTypeD(tn) {
    this.fa.types.push({ textIdx: this.fa.intern(tn.text), ptr: tn.ptr, array: tn.isArray, arrayLen: 0, isFn: tn.isFn });
    return this.fa.types.length;
  }
  genericSelection() {
    const t = this.advance();
    this.expectP("(", "after _Generic");
    const ctrl = this.assignment();
    const a0 = this.fa.assocs.length;
    let nAss = 0;
    while ((this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) {
      const asc = { typeIdx: 0, isDefault: false, expr: 0 };
      if ((this.toks[this.pos].t === "default")) { if (this.pos < this.last) this.pos++; asc.isDefault = true; }
      else asc.typeIdx = this.addTypeD(this.parseTypeName());
      this.expectP(":", "in _Generic association");
      asc.expr = this.assignment();
      this.fa.assocs.push(asc);
      nAss++;
    }
    this.expectP(")", "to close _Generic");
    const n = nodeAt(47/*Generic*/, t);
    n.a = ctrl; n.b = a0; n.c = nAss; n.end = this.prevEnd();
    return this.addNode(n);
  }
  postfix() {
    let e = this.primary();
    const ns = this.fa.nodes;
    while (!this.eof()) {
      const t = this.toks[this.pos];
      if (t.k !== PU) return e;
      const E = ns[e];
      let n;
      switch (t.p) {
        case "[": {
          if (this.pos < this.last) this.pos++;
          const ix = this.expr();
          this.expectP("]", "to close index");
          n = mkNode(36/*Index*/); n.a = e; n.b = ix; n.end = this.prevEnd();
          break;
        }
        case "(": {
          if (this.pos < this.last) this.pos++;
          const a0 = ns.length;
          let nArgs = 0;
          if (!(this.toks[this.pos].p === ")")) for (;;) { this.assignment(); nArgs++; if (!(this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) break; }
          this.expectP(")", "to close call args");
          n = mkNode(34/*Call*/); n.a = e; n.b = nArgs; n.c = a0; n.end = this.prevEnd();
          break;
        }
        case ".": case "->": {
          if (this.pos < this.last) this.pos++;
          if (this.toks[this.pos].k !== ID) { this.errAt(this.pos, "expected member name"); return e; }
          const nm = this.advance();
          n = mkNode(35/*Member*/); n.a = this.fa.intern(nm.t); n.b = e; n.f = t.p === "->" ? 1 : 0; n.end = nm.end;
          break;
        }
        case "++": case "--":
          this.advance();
          n = mkNode(37/*Postfix*/); n.f = PCODE[t.p]; n.a = e; n.end = this.prevEnd();
          break;
        default: return e;
      }
      n.off = E.off; n.line = E.line; n.col = E.col;
      e = this.addNode(n);
    }
    return e;
  }
  joinStrings(t, txt, end) {
    while (this.toks[this.pos].k === STR || this.strMacroHere()) { const nx = this.advance(); txt += " " + nx.t; end = nx.end; }
    const n = nodeAt(31/*StrLit*/, t);
    n.a = this.fa.intern(txt); n.end = end;
    return this.addNode(n);
  }
  primary() {
    const t = this.toks[this.pos];
    switch (t.k) {
      case NUM: case CHR: {
        if (this.pos < this.last) this.pos++;
        const n = nodeAt(t.k === NUM ? 29/*NumLit*/ : 30/*CharLit*/, t);
        n.a = this.fa.intern(t.t);
        return this.addNode(n);
      }
      case STR: this.advance(); return this.joinStrings(t, t.t, t.end);
      case ID: {
        const nx = this.peek(1);
        if (nx.k === STR && !C_KEYWORDS.has(t.t)) { if (this.pos < this.last) this.pos++; return this.joinStrings(t, t.t, t.end); }
        switch (t.t) {
          case "true": case "false": {
            if (this.pos < this.last) this.pos++;
            const n = nodeAt(32/*BoolLit*/, t);
            n.f = t.t === "true" ? 1 : 0; n.a = this.fa.intern(t.t);
            return this.addNode(n);
          }
          case "nullptr": this.advance(); return this.addNode(nodeAt(33/*NullPtr*/, t));
          case "offsetof": case "__builtin_offsetof":
            if (nx.k === PU && nx.p === "(") {
              if (this.pos < this.last) this.pos++; this.advance();
              const c = this.toks[this.pos];
              if (this.plainIdent(c)) this.scopes[0].setTypedef(c.t, true);
              const tn = this.parseTypeName();
              let depth = 1;
              while (!this.eof() && depth > 0) {
                const c2 = this.toks[this.pos];
                if (c2.k === PU) {
                  if (c2.p === "(") depth++;
                  else if (c2.p === ")") depth--;
                  else if (c2.p === ";" || c2.p === "{" || c2.p === "}") { depth = 0; continue; }
                }
                if (depth > 0) if (this.pos < this.last) this.pos++;
              }
              (this.toks[this.pos].p === ")" && (this.pos < this.last && this.pos++, true));
              const n = nodeAt(44/*SizeofT*/, t);
              n.a = this.addTypeD(tn); n.end = this.prevEnd();
              return this.addNode(n);
            }
            break;
          case "va_arg":
            if (nx.k !== PU || nx.p !== "(") break;
          // falls through
          case "__builtin_va_arg": {
            if (this.pos < this.last) this.pos++;
            this.expectP("(", "after __builtin_va_arg");
            const e = this.assignment();
            this.expectP(",", "in __builtin_va_arg");
            const tn = this.parseTypeName();
            this.expectP(")", "to close __builtin_va_arg");
            const n = nodeAt(51/*VaArg*/, t);
            n.a = e; n.b = this.addTypeD(tn); n.end = this.prevEnd();
            return this.addNode(n);
          }
        }
        this.advance();
        const n = nodeAt(28/*Ident*/, t);
        n.a = this.fa.intern(t.t);
        return this.addNode(n);
      }
      case PU:
        if (t.p === "(") {
          const nx = this.peek(1);
          if (nx.k === PU && nx.p === "{") {
            if (this.pos < this.last) this.pos++; this.advance();
            const b = this.block();
            this.expectP(")", "to close statement expression");
            const n = nodeAt(48/*StmtExpr*/, t);
            n.a = b; n.end = this.prevEnd();
            return this.addNode(n);
          }
          this.advance();
          const e = this.expr();
          this.expectP(")", "to close parenthesis");
          const n = nodeAt(49/*Paren*/, t);
          n.a = e; n.end = this.prevEnd();
          return this.addNode(n);
        }
    }
    this.errAt(this.pos, "unexpected token '" + t.t + "' in expression");
    if (this.pos < this.last) this.pos++;
    return this.addNode(nodeAt(0/*Bad*/, t));
  }
  initializer() {
    if (!(this.toks[this.pos].p === "{")) return this.assignment();
    const ob = this.advance();
    const n = nodeAt(52/*InitList*/, ob);
    const mk = this.addNode(n);
    n.a = this.fa.nodes.length;
    while (!this.eof() && !(this.toks[this.pos].p === "}")) {
      const item = { fieldIdx: 0, index: 0n, hasIndex: false, isList: false };
      let desig = false;
      for (;;) {
        if ((this.toks[this.pos].p === ".")) {
          if (this.pos < this.last) this.pos++;
          if (this.toks[this.pos].k === ID) item.fieldIdx = this.fa.intern(this.advance().t);
          else this.errAt(this.pos, "expected field designator name");
          desig = true;
          continue;
        }
        if ((this.toks[this.pos].p === "[")) {
          if (this.pos < this.last) this.pos++;
          this.assignment();
          if ((this.toks[this.pos].p === "..." && (this.pos < this.last && this.pos++, true))) this.assignment();
          this.expectP("]", "to close index designator");
          item.hasIndex = true;
          desig = true;
          continue;
        }
        break;
      }
      if (desig) this.expectP("=", "after designator");
      const val = this.initializer();
      item.isList = this.fa.nodes[val].k === 52/*InitList*/;
      this.fa.inits.push(item);
      const it = mkNode(53/*InitItem*/);
      it.a = this.fa.inits.length; it.b = val;
      this.addNode(it);
      if (!(this.toks[this.pos].p === "," && (this.pos < this.last && this.pos++, true))) break;
    }
    this.expectP("}", "to close initializer list");
    n.b = this.fa.nodes.length - n.a;
    if (this.pos > 0) n.end = this.toks[this.pos - 1].end;
    return mk;
  }
}

function krShaped(d) {
  if (d.params.length === 0 || d.variadic) return false;
  for (const pt of d.params) {
    if (!pt.unnamed || pt.ptr > 0 || pt.typ.includes(" ") || pt.typ.includes("*") || pt.typ.includes("(")) return false;
  }
  return true;
}

function funcSignature(specs, d) {
  let b = "";
  if (specs.storage & 4/*Static*/) b += "static ";
  if (specs.storage & 16/*Inline*/) b += "inline ";
  if (specs.storage & 8/*Extern*/) b += "extern ";
  b += baseText(specs) + stars(d.ptr) + " " + d.name + "(";
  d.params.forEach((pt, i) => {
    if (i > 0) b += ", ";
    if (pt.typ === "") b += pt.name;
    else if (pt.unnamed) b += pt.typ;
    else b += pt.typ + " " + pt.name;
  });
  if (d.variadic) { if (d.params.length) b += ", "; b += "..."; }
  return b + ")";
}

export function parseUnit(toks, files, srcs) {
  const fa = new AST();
  const p = new Parser(fa, toks, files, srcs);
  p.parseTU();
  return { fa, errs: p.errs };
}
