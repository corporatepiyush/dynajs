// C23 lexer and hide-set preprocessor. Source text is held as byte strings
// (one char per byte), so every offset and length below is a byte count.
import * as fs from "dyna:file";
import { decode } from "dyna:bytes";

export const K = { ID: 0, NUM: 1, CHAR: 2, STR: 3, PUNCT: 4, HDR: 5, EOF: 6, NL: 7 };

const PUNCTS = ["%:%:", "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
  "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "<:", ":>", "<%", "%>", "%:",
  "[", "]", "(", ")", "{", "}", ".", "&", "*", "+", "-", "~", "!", "/", "%", "<", ">", "^", "|", "?", ":", ";", "=", ",", "#"];
export const PUNCT_SET = new Set(PUNCTS);
const DIGRAPH = { "<:": "[", ":>": "]", "<%": "{", "%>": "}", "%:": "#", "%:%:": "##" };
const PUNCT_RE = /%:%:|<<=|>>=|\.\.\.|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\*=|\/=|%=|\+=|-=|&=|\^=|\|=|##|<:|:>|<%|%>|%:|[\[\](){}.&*+\-~!\/%<>^|?:;=,#]/y;
const IDENT_RE = /[A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*/y;
const HEX = /[0-9a-fA-F]/;
const PUNCT_BY_FIRST = Array.from({ length: 256 }, () => null);
for (const p of PUNCTS) { const c = p.charCodeAt(0); (PUNCT_BY_FIRST[c] ??= []).push(p); }

export function readBytesString(path) {
  return decode(fs.readBytes(new fs.Path(path)), "iso-8859-1");
}

function isIdentPart(c) {
  return c === 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c >= 0x80 || (c >= 48 && c <= 57);
}
const isDigitC = (c) => c >= 48 && c <= 57;

// Splice backslash-newlines and fold CR/CRLF, remembering original lines.
function splice(src) {
  const n = src.length;
  let out = "";
  const origLine = [];
  const lineStar = [0];
  let ol = 1, i = 0, chunk = 0;
  const parts = [];
  let outLen = 0;
  while (i < n) {
    const c = src.charCodeAt(i);
    if (c === 92 && i + 1 < n) {
      const d = src.charCodeAt(i + 1);
      if (d === 10) { ol++; i += 2; continue; }
      if (d === 13) { const adv = i + 2 < n && src.charCodeAt(i + 2) === 10 ? 3 : 2; ol++; i += adv; continue; }
    }
    if (c === 10) {
      parts.push("\n"); origLine.push(ol); outLen++; i++; lineStar.push(outLen); ol++;
    } else if (c === 13) {
      parts.push("\n"); origLine.push(ol); outLen++; i++;
      if (i < n && src.charCodeAt(i) === 10) i++;
      lineStar.push(outLen); ol++;
    } else {
      let j = i + 1;
      while (j < n) { const e = src.charCodeAt(j); if (e === 10 || e === 13 || e === 92) break; j++; }
      parts.push(src.slice(i, j));
      for (let k = i; k < j; k++) origLine.push(ol);
      outLen += j - i; i = j;
    }
  }
  out = parts.join("");
  return { src: out, origLine, lineStar };
}

export function stripBOM(s) {
  return s.charCodeAt(0) === 0xef && s.charCodeAt(1) === 0xbb && s.charCodeAt(2) === 0xbf ? s.slice(3) : s;
}

export function needsSplice(s) {
  return s.indexOf("\r") >= 0 || /\\[\r\n]/.test(s);
}

// Lex a whole file into tokens (EOF last). errs gets {msg, line}.
export function lexWhole(data, errs) {
  let src = stripBOM(data);
  let origLine = null, lineStar;
  if (needsSplice(src)) {
    const sp = splice(src);
    src = sp.src; origLine = sp.origLine; lineStar = sp.lineStar;
  } else {
    lineStar = [0];
    let p = src.indexOf("\n");
    while (p >= 0) { lineStar.push(p + 1); p = src.indexOf("\n", p + 1); }
  }
  const n = src.length;
  const toks = [];
  let pos = 0, curLine = 0, afterHash = false, includeCtx = false;
  const lineAt = (off) => {
    if (origLine === null) {
      if (off >= n) { off = n - 1; if (off < 0) return 1; }
      let lo = 0, hi = lineStar.length;
      while (lo < hi) { const mid = (lo + hi) >> 1; if (lineStar[mid] <= off) lo = mid + 1; else hi = mid; }
      return lo < 1 ? 1 : lo;
    }
    if (off < origLine.length) return origLine[off];
    return origLine.length ? origLine[origLine.length - 1] : 1;
  };
  let li = 0;
  const LS = lineStar.length;
  const err = (off, msg) => { if (errs && errs.length < 4096) errs.push({ msg, line: lineAt(off) }); };
  const onDirectiveLine = () => {
    let i = src.lastIndexOf("\n", pos - 1) + 1;
    while (i < n && (src.charCodeAt(i) === 32 || src.charCodeAt(i) === 9)) i++;
    return i < n && src.charCodeAt(i) === 35;
  };
  const scanQuoted = (q) => {
    while (pos < n && src.charCodeAt(pos) !== q) pos++;
    const qn = q === 39 ? "char const" : "string literal";
    if (pos >= n) { err(pos, "unterminated " + qn); return; }
    pos++;
    while (pos < n) {
      const c = src.charCodeAt(pos);
      if (c === 92) { pos += 2; continue; }
      if (c === 10) { err(pos, "unterminated " + qn); return; }
      if (c === q) { pos++; return; }
      pos++;
    }
    err(pos, "unterminated " + qn);
  };
  for (;;) {
    const p0 = pos;
    // skip whitespace and comments up to (not including) a newline
    while (pos < n) {
      const c = src.charCodeAt(pos);
      if (c === 10) break;
      if (c === 32 || c === 9 || c === 11 || c === 12 || c === 13) { pos++; continue; }
      if (c === 47) {
        const d = src.charCodeAt(pos + 1);
        if (d === 47) { const e = src.indexOf("\n", pos); pos = e < 0 ? n : e; continue; }
        if (d === 42) {
          const e = src.indexOf("*/", pos + 2);
          if (e < 0) { err(pos, "unterminated /* comment"); pos = n; } else pos = e + 2;
          continue;
        }
      }
      break;
    }
    const ws = pos > p0;
    while (curLine + 1 < lineStar.length && lineStar[curLine + 1] <= pos) curLine++;
    if (pos >= n) {
      let off = n - 1, l = 1, co = 1;
      if (off >= 0) {
        while (li + 1 < LS && lineStar[li + 1] <= off) li++;
        l = origLine === null ? li + 1 : origLine[off]; co = off - lineStar[li] + 1;
      }
      toks.push({ k: 6, t: "", off: n, end: n, line: l, col: co, sl: curLine, ws, p: "" });
      return toks;
    }
    const start = pos;
    const c = src.charCodeAt(pos);
    if (c === 10) {
      pos++;
      while (li + 1 < LS && lineStar[li + 1] <= start) li++;
      const l = origLine === null ? li + 1 : origLine[start], co = start - lineStar[li] + 1;
      afterHash = false; includeCtx = false;
      toks.push({ k: 7, t: "\n", off: pos - 1, end: pos, line: l, col: co, sl: curLine, ws, p: "" });
      continue;
    }
    const c1 = src.charCodeAt(pos + 1), c2 = src.charCodeAt(pos + 2);
    let k, p = "";
    let m4 = "";
    if (c === 39 || (c === 76 && c1 === 39) || (c === 117 && (c1 === 39 || (c1 === 56 && c2 === 39))) || (c === 85 && c1 === 39)) {
      k = 2; scanQuoted(39);
    } else if (c === 34 || (c === 76 && c1 === 34) || (c === 117 && (c1 === 34 || (c1 === 56 && c2 === 34))) || (c === 85 && c1 === 34)) {
      k = 3; scanQuoted(34);
    } else if (c === 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c >= 0x80) {
      k = 0;
      for (;;) {
        IDENT_RE.lastIndex = pos;
        if (IDENT_RE.test(src)) pos = IDENT_RE.lastIndex;
        if (src.charCodeAt(pos) === 92 && (src[pos + 1] === "u" || src[pos + 1] === "U")) {
          pos += 2;
          const nd = src[pos - 1] === "U" ? 8 : 4;
          for (let q = 0; q < nd && pos < n && HEX.test(src[pos]); q++) pos++;
          continue;
        }
        break;
      }
    } else if (isDigitC(c) || (c === 46 && isDigitC(c1))) {
      k = 1; pos++;
      while (pos < n) {
        const d = src.charCodeAt(pos);
        if (isIdentPart(d) || d === 46) {
          if ((d === 101 || d === 69 || d === 112 || d === 80) && (src[pos + 1] === "+" || src[pos + 1] === "-")) {
            pos++; if (pos < n) pos++;
            continue;
          }
          pos++; continue;
        }
        if (d === 39 && pos + 1 < n && isIdentPart(src.charCodeAt(pos + 1))) { pos++; continue; }
        break;
      }
    } else if (includeCtx && c === 60) {
      k = 5; pos++;
      while (pos < n) {
        const d = src.charCodeAt(pos);
        if (d === 62) { pos++; break; }
        if (d === 10) { err(start, "unterminated header name"); break; }
        pos++;
      }
    } else {
      const cands = c < 256 ? PUNCT_BY_FIRST[c] : null;
      let m = null;
      if (cands !== null) for (let q = 0; q < cands.length; q++) { const s = cands[q]; if (s.length === 1 || src.startsWith(s, pos)) { m = s; break; } }
      if (m === null) {
        if (!onDirectiveLine()) err(pos, "stray byte in source");
        pos++;
        continue;
      }
      k = 4; pos += m.length;
      p = m.length > 1 ? DIGRAPH[m] || m : m;
      m4 = m;
    }
    const t = k === 4 ? m4 : src.slice(start, pos);
    while (li + 1 < LS && lineStar[li + 1] <= start) li++;
    const l = origLine === null ? li + 1 : origLine[start], co = start - lineStar[li] + 1;
    const tok = { k, t, off: start, end: pos, line: l, col: co, sl: curLine, ws, p };
    toks.push(tok);
    if (k === 4) {
      if (p === "#") afterHash = true;
      else if (p === ")" || p === "," || p === ";") { includeCtx = false; afterHash = false; }
    } else if (k === 5) includeCtx = false;
    else if (k === 0) {
      if (afterHash) {
        if (t === "include" || t === "include_next" || t === "embed" || t === "embed_next") { includeCtx = true; afterHash = false; continue; }
        afterHash = false;
      }
      if (t === "__has_include" || t === "__has_include_next" || t === "__has_embed") includeCtx = true;
    }
  }
}

export function tokLineText(toks) {
  let s = "";
  for (let i = 0; i < toks.length; i++) {
    if (i > 0 && toks[i].ws) s += " ";
    s += toks[i].t;
  }
  return s;
}

// ---------------------------------------------------------------- paths
export function cleanPath(p) {
  const abs = p.startsWith("/");
  const out = [];
  for (const seg of p.split("/")) {
    if (seg === "" || seg === ".") continue;
    if (seg === "..") { if (out.length && out[out.length - 1] !== "..") out.pop(); else if (!abs) out.push(".."); continue; }
    out.push(seg);
  }
  const s = (abs ? "/" : "") + out.join("/");
  return s === "" ? "." : s;
}
export const dirname = (p) => { const i = p.lastIndexOf("/"); return i < 0 ? "." : i === 0 ? "/" : p.slice(0, i); };
export const basename = (p) => { const i = p.lastIndexOf("/"); return i < 0 ? p : p.slice(i + 1); };
export function relPath(base, target) {
  const b = cleanPath(base).split("/").filter(Boolean), t = cleanPath(target).split("/").filter(Boolean);
  let i = 0;
  while (i < b.length && i < t.length && b[i] === t[i]) i++;
  const r = [...Array(b.length - i).fill(".."), ...t.slice(i)].join("/");
  return r === "" ? "." : r;
}

const SYS_PREFIX = ["sys/", "netinet/", "arpa/", "net/", "linux/", "mach/", "machine/", "bits/", "asm/", "netinet6/", "rpc/"];
const STD_HEADERS = new Set(`assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h signal.h
 stdalign.h stdarg.h stdatomic.h stdbit.h stdbool.h stdckdint.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h string.h tgmath.h threads.h
 time.h uchar.h wchar.h wctype.h aio.h cpio.h dirent.h dlfcn.h fcntl.h fnmatch.h ftw.h glob.h grp.h iconv.h langinfo.h libgen.h monetary.h mqueue.h ndbm.h netdb.h nl_types.h
 poll.h pthread.h pwd.h regex.h sched.h search.h semaphore.h spawn.h strings.h stropts.h syslog.h tar.h termios.h trace.h ulimit.h unistd.h
 utime.h utmpx.h wordexp.h getopt.h alloca.h malloc.h endian.h byteswap.h execinfo.h ifaddrs.h io.h direct.h process.h windows.h winsock2.h
 ws2tcpip.h crtdbg.h`.split(/\s+/).filter(Boolean));
const isSystemHeader = (t) => SYS_PREFIX.some((p) => t.startsWith(p)) || STD_HEADERS.has(t);

// ---------------------------------------------------------------- shared state for one run
export class Tree {
  constructor(rootAbs, allAbs, sources) {
    this.root = rootAbs;
    this.sources = sources;          // abs -> byte string (catalogued, non-empty)
    this.byBase = new Map();
    for (const p of allAbs) {
      const b = basename(p);
      let l = this.byBase.get(b);
      if (!l) this.byBase.set(b, (l = []));
      l.push(p);
    }
    for (const l of this.byBase.values()) l.sort();
    this.lexCache = new Map();
    this.statCache = new Map();
    this.incCache = new Map();
    this.extraRead = new Map();
    this.lexTokens = 0;
  }
  read(abs) {
    const s = this.sources.get(abs);
    if (s !== undefined) return s;
    if (this.extraRead.has(abs)) return this.extraRead.get(abs);
    let r = null;
    try {
      const st = fs.stat(new fs.Path(abs));
      if (!st.isDir && st.size <= 32 << 20) r = readBytesString(abs);
    } catch (e) { r = null; }
    this.extraRead.set(abs, r);
    return r;
  }
  isFile(p) {
    let v = this.statCache.get(p);
    if (v === undefined) {
      try { v = !fs.stat(new fs.Path(p)).isDir; } catch (e) { v = false; }
      this.statCache.set(p, v);
    }
    return v;
  }
  treeLookup(fromDir, tgt) {
    if (tgt === "" || tgt.includes("..")) return "";
    tgt = cleanPath(tgt);
    const suffix = "/" + tgt;
    let best = "", bestShared = -1;
    for (const c of this.byBase.get(basename(tgt)) || []) {
      if (!c.endsWith(suffix)) continue;
      let sh = 0;
      for (let i = 0; i < fromDir.length && i < c.length && fromDir[i] === c[i]; i++) if (fromDir[i] === "/") sh++;
      if (sh > bestShared || (sh === bestShared && c.length < best.length)) { best = c; bestShared = sh; }
    }
    return best;
  }
  // Shared lexing of a header; undefined once the cache is full (8M tokens).
  lexShared(abs) {
    let lf = this.lexCache.get(abs);
    if (lf === undefined) {
      if (this.lexTokens >= 8 << 20) return undefined;
      const data = this.read(abs);
      if (data === null) lf = null;
      else { const errs = []; lf = { data, toks: lexWhole(data, errs), errs }; this.lexTokens += lf.toks.length; }
      this.lexCache.set(abs, lf);
    }
    return lf;
  }
}

// ---------------------------------------------------------------- #if
const I64 = (v) => BigInt.asIntN(64, v);
const U64MAX = -1n, I64MAX = 9223372036854775807n, I64MIN = -9223372036854775808n;
const COND_LIMITS = {
  CHAR_BIT: 8n, SCHAR_MAX: 127n, SCHAR_MIN: -128n, UCHAR_MAX: 255n, CHAR_MAX: 127n, CHAR_MIN: -128n,
  SHRT_MAX: 32767n, SHRT_MIN: -32768n, USHRT_MAX: 65535n,
  INT_MAX: 2147483647n, INT_MIN: -2147483648n, UINT_MAX: 4294967295n,
  LONG_MAX: I64MAX, LONG_MIN: I64MIN, ULONG_MAX: U64MAX, LLONG_MAX: I64MAX, LLONG_MIN: I64MIN, ULLONG_MAX: U64MAX,
  INT8_MAX: 127n, INT16_MAX: 32767n, INT32_MAX: 2147483647n, INT64_MAX: I64MAX,
  INT8_MIN: -128n, INT16_MIN: -32768n, INT32_MIN: -2147483648n, INT64_MIN: I64MIN,
  UINT8_MAX: 255n, UINT16_MAX: 65535n, UINT32_MAX: 4294967295n, UINT64_MAX: U64MAX,
  INTPTR_MAX: I64MAX, INTPTR_MIN: I64MIN, UINTPTR_MAX: U64MAX, INTMAX_MAX: I64MAX, INTMAX_MIN: I64MIN, UINTMAX_MAX: U64MAX,
  SIZE_MAX: U64MAX, SSIZE_MAX: I64MAX, PTRDIFF_MAX: I64MAX, PTRDIFF_MIN: I64MIN,
  __WORDSIZE: 64n, __SIZEOF_POINTER__: 8n, __SIZEOF_INT__: 4n, __SIZEOF_SHORT__: 2n, __SIZEOF_LONG_LONG__: 8n, __SIZEOF_SIZE_T__: 8n,
  __LP64__: 1n, _LP64: 1n,
  SIZEOF_CHAR: 1n, SIZEOF_SHORT: 2n, SIZEOF_INT: 4n, SIZEOF_LONG: 8n, SIZEOF_LONG_LONG: 8n, SIZEOF_LONG_INT: 8n, SIZEOF_LONG_LONG_INT: 8n,
  SIZEOF_VOID_P: 8n, SIZEOF_VOIDP: 8n, SIZEOF_SIZE_T: 8n, SIZEOF_OFF_T: 8n, SIZEOF_TIME_T: 8n, SIZEOF_PTRDIFF_T: 8n,
  SIZEOF_INTPTR_T: 8n, SIZEOF_UINTPTR_T: 8n, SIZEOF_FLOAT: 4n, SIZEOF_DOUBLE: 8n, SIZEOF_INT64_T: 8n, SIZEOF_INT32_T: 4n,
  ALIGNOF_SHORT: 2n, ALIGNOF_INT: 4n, ALIGNOF_LONG: 8n, ALIGNOF_LONG_LONG_INT: 8n, ALIGNOF_DOUBLE: 8n, ALIGNOF_INT64_T: 8n,
  MAXIMUM_ALIGNOF: 8n,
};

export function stripNumSuffix(s) {
  let i = s.length;
  while (i > 0 && "uUlLzZiI".includes(s[i - 1])) i--;
  return s.slice(0, i);
}

// C integer literal with wrapping 64-bit value; null when malformed.
export function parseCInt(s) {
  s = stripNumSuffix(s.replaceAll("'", ""));
  if (s === "") return null;
  let neg = false, body = s, base = 10n;
  if (body[0] === "+") body = body.slice(1);
  else if (body[0] === "-") { neg = true; body = body.slice(1); }
  if (body.startsWith("0x") || body.startsWith("0X")) { base = 16n; body = body.slice(2); }
  else if (body.startsWith("0b") || body.startsWith("0B")) { base = 2n; body = body.slice(2); }
  else if (body.length > 1 && body[0] === "0") { base = 8n; body = body.slice(1); }
  let v = 0n;
  for (let i = 0; i < body.length; i++) {
    const c = body.charCodeAt(i);
    let d;
    if (c >= 48 && c <= 57) d = c - 48;
    else if (c >= 97 && c <= 122) d = c - 97 + 10;
    else if (c >= 65 && c <= 90) d = c - 65 + 10;
    else return null;
    if (BigInt(d) >= base) return null;
    v = I64(v * base + BigInt(d));
  }
  return neg ? I64(-v) : v;
}

function readCharVal(s, i) {
  if (s[i] === "\\" && i + 1 < s.length) {
    const e = s[i + 1];
    const simple = { n: 10, t: 9, r: 13, a: 7, b: 8, f: 12, v: 11, "\\": 92, "'": 39, '"': 34, "?": 63, e: 27 };
    if (e in simple) return [BigInt(simple[e]), 2];
    if (e === "x") {
      let v = 0n, j = i + 2;
      while (j < s.length && HEX.test(s[j])) { v = I64(v * 16n + BigInt(parseInt(s[j], 16))); j++; }
      return [v, j - i];
    }
    if (e === "u" || e === "U") {
      const nd = e === "U" ? 8 : 4;
      let v = 0n, j = i + 2;
      for (let k = 0; k < nd && j < s.length && HEX.test(s[j]); k++) { v = v * 16n + BigInt(parseInt(s[j], 16)); j++; }
      return [v, j - i];
    }
    if (e >= "0" && e <= "7") {
      let v = 0n, j = i + 1;
      for (let k = 0; k < 3 && j < s.length && s[j] >= "0" && s[j] <= "7"; k++) { v = v * 8n + BigInt(s.charCodeAt(j) - 48); j++; }
      return [v, j - i];
    }
    return [BigInt(s.charCodeAt(i + 1)), 2];
  }
  const c = s.charCodeAt(i);
  if (c < 0x80) return [BigInt(c), 1];
  let r = BigInt(c & 0x3f), nb = 1;
  if ((c & 0xe0) === 0xc0) nb = 2; else if ((c & 0xf0) === 0xe0) nb = 3; else if ((c & 0xf8) === 0xf0) nb = 4;
  let j = i + 1;
  for (let k = 1; k < nb && j < s.length; k++) { r = (r << 6n) | BigInt(s.charCodeAt(j) & 0x3f); j++; }
  return [r, j - i];
}

function charValue(s) {
  if (s.length < 2) return 0n;
  if (s[0] === "'") s = s.slice(1);
  else if (s[0] === "u" && s[1] === "8") s = s.slice(3);
  else if (s[0] === "u" || s[0] === "U" || s[0] === "L") s = s.slice(2);
  if (s.endsWith("'")) s = s.slice(0, -1);
  let v = 0n, i = 0;
  while (i < s.length) { const [c, adv] = readCharVal(s, i); v = I64((v << 8n) | (c & 0xffn)); i += adv; }
  return v;
}

class Eval {
  constructor(toks, pp, line) { this.toks = toks; this.pp = pp; this.line = line; this.pos = 0; this.bad = false; }
  peek() { return this.pos < this.toks.length ? this.toks[this.pos] : null; }
  accept(p) { const t = this.peek(); if (t && t.k === 4 && t.p === p) { this.pos++; return true; } return false; }
  cond() {
    const c = this.logor();
    if (this.accept("?")) {
      const a = this.cond();
      if (this.accept(":")) { const b = this.cond(); return c !== 0n ? a : b; }
      this.bad = true; return 0n;
    }
    return c;
  }
  logor() { let v = this.logand(); while (this.accept("||")) { const r = this.logand(); v = v !== 0n || r !== 0n ? 1n : 0n; } return v; }
  logand() { let v = this.bitor(); while (this.accept("&&")) { const r = this.bitor(); v = v !== 0n && r !== 0n ? 1n : 0n; } return v; }
  bitor() { let v = this.bitxor(); while (this.accept("|")) v = v | this.bitxor(); return v; }
  bitxor() { let v = this.bitand(); while (this.accept("^")) v = v ^ this.bitand(); return v; }
  bitand() { let v = this.equality(); while (this.accept("&")) v = v & this.equality(); return v; }
  equality() {
    let v = this.relational();
    for (;;) {
      if (this.accept("==")) { v = v === this.relational() ? 1n : 0n; continue; }
      if (this.accept("!=")) { v = v !== this.relational() ? 1n : 0n; continue; }
      return v;
    }
  }
  relational() {
    let v = this.shift();
    for (;;) {
      if (this.accept("<=")) { v = v <= this.shift() ? 1n : 0n; continue; }
      if (this.accept(">=")) { v = v >= this.shift() ? 1n : 0n; continue; }
      if (this.accept("<")) { v = v < this.shift() ? 1n : 0n; continue; }
      if (this.accept(">")) { v = v > this.shift() ? 1n : 0n; continue; }
      return v;
    }
  }
  shift() {
    let v = this.additive();
    for (;;) {
      if (this.accept("<<")) { const r = BigInt.asUintN(64, this.additive()); v = r >= 64n ? 0n : I64(v << r); continue; }
      if (this.accept(">>")) { const r = BigInt.asUintN(64, this.additive()); v = r >= 64n ? (v < 0n ? -1n : 0n) : v >> r; continue; }
      return v;
    }
  }
  additive() {
    let v = this.mult();
    for (;;) {
      if (this.accept("+")) { v = I64(v + this.mult()); continue; }
      if (this.accept("-")) { v = I64(v - this.mult()); continue; }
      return v;
    }
  }
  mult() {
    let v = this.unary();
    for (;;) {
      if (this.accept("*")) { v = I64(v * this.unary()); continue; }
      if (this.accept("/")) {
        const d = this.unary();
        if (d === 0n) { this.pp.errf(this.line, "division by zero in #if"); this.bad = true; return 0n; }
        v = I64(v / d); continue;
      }
      if (this.accept("%")) {
        const d = this.unary();
        if (d === 0n) { this.pp.errf(this.line, "mod by zero in #if"); this.bad = true; return 0n; }
        v = I64(v % d); continue;
      }
      return v;
    }
  }
  unary() {
    if (this.accept("!")) return this.unary() !== 0n ? 0n : 1n;
    if (this.accept("~")) return I64(~this.unary());
    if (this.accept("-")) return I64(-this.unary());
    if (this.accept("+")) return this.unary();
    return this.primary();
  }
  primary() {
    const t = this.peek();
    if (t) {
      if (t.k === 1) {
        this.pos++;
        const v = parseCInt(t.t);
        if (v !== null) return v;
        this.pp.errf(this.line, "bad number in #if: " + t.t);
        this.bad = true; return 0n;
      }
      if (t.k === 2) { this.pos++; return charValue(t.t); }
      if (t.k === 0) {
        this.pos++;
        if (t.t === "true") return 1n;
        if (t.t === "false") return 0n;
        return Object.hasOwn(COND_LIMITS, t.t) ? COND_LIMITS[t.t] : 0n;
      }
      if (t.k === 4 && t.p === "(") {
        this.pos++;
        const v = this.cond();
        if (!this.accept(")")) { this.bad = true; return 0n; }
        return v;
      }
    }
    this.bad = true; return 0n;
  }
}

function dequote(s) {
  if (s.startsWith("u8")) s = s.slice(2);
  if (s.startsWith("u")) s = s.slice(1);
  if (s.startsWith("U")) s = s.slice(1);
  if (s.startsWith("L")) s = s.slice(1);
  let a = 0, b = s.length;
  while (a < b && s[a] === '"') a++;
  while (b > a && s[b - 1] === '"') b--;
  return s.slice(a, b);
}

function goQuote(s) {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c === 34) out += '\\"';
    else if (c === 92) out += "\\\\";
    else if (c >= 32 && c < 127) out += s[i];
    else if (c === 10) out += "\\n";
    else if (c === 9) out += "\\t";
    else if (c === 13) out += "\\r";
    else if (c < 32 || c === 127) out += "\\x" + c.toString(16).padStart(2, "0");
    else out += s[i];
  }
  return out + '"';
}

// For every token index, the index of the first newline or EOF token at or after it (cached on the token array).
const LINE_END = new WeakMap();
function lineEnds(toks) {
  let le = LINE_END.get(toks);
  if (le === undefined) {
    const n = toks.length;
    le = new Int32Array(n + 1);
    le[n] = n;
    for (let i = n - 1; i >= 0; i--) { const k = toks[i].k; le[i] = k === 7 || k === 6 ? i : le[i + 1]; }
    LINE_END.set(toks, le);
  }
  return le;
}

export class PanicError extends Error {}

const withPos = (t, use) => ({ k: t.k, t: t.t, off: use.off, end: use.end, line: use.line, col: use.col, sl: use.sl, ws: t.ws, p: t.p });

// ---------------------------------------------------------------- preprocessor (one unit)
export class Preprocessor {
  constructor(tree, rootAbs, predef) {
    this.tree = tree;
    this.root = rootAbs;
    this.rootDir = dirname(rootAbs);
    this.macros = new Map();
    for (const m of predef) this.macros.set(m.name, { ...m, uses: 0 });
    this.allMacros = [];
    this.errs = [];
    this.files = [];       // {path, id, data}
    this.includes = [];    // {fromID, target, resolved, sys, line}
    this.cfgs = [];        // {fileID, directive, expr, line}
    this.cond = [];
    this.onceSet = new Set();
    this.onStack = new Set();
    this.nextID = 0;
    this.lineDelta = 0;
    this.fileName = "";
    this.rootName = "";
    this.nExpand = 0;
    this.outToks = [];
    this.outFile = [];
  }
  relName(abs) {
    const r = relPath(this.rootDir, abs);
    if (!r.startsWith("..")) return r;
    const r2 = relPath(this.tree.root, abs);
    if (!r2.startsWith("..")) return r2;
    return basename(abs);
  }
  errf(line, msg) {
    if (this.rootName !== "" && this.fileName !== "" && this.fileName !== this.rootName) msg += " (in " + this.fileName + ":" + line + ")";
    if (this.errs.length < 4096) this.errs.push({ msg, line });
  }
  condActive() {
    for (const c of this.cond) if (!c.active) return false;
    return true;
  }
  loadFile(abs, shared) {
    if (this.files.length >= 8000) return null;
    let toks;
    const lf = shared ? this.tree.lexShared(abs) : undefined;
    if (lf !== undefined) {
      if (lf === null) return null;
      toks = lf.toks;
      const id = ++this.nextID;
      for (const e of lf.errs) if (this.errs.length < 4096) this.errs.push({ msg: e.msg + " (in " + this.relName(abs) + ":" + e.line + ")", line: e.line });
      const src = { path: abs, id, toks, data: lf.data, pos: 0, shared: true };
      this.files.push(src);
      return src;
    }
    const data = this.tree.read(abs);
    if (data === null) return null;
    const id = ++this.nextID;
    toks = lexWhole(data, this.errs);
    const src = { path: abs, id, toks, data, pos: 0, shared: false };
    this.files.push(src);
    return src;
  }
  run() {
    this.fileName = this.rootName = this.relName(this.root);
    const src = this.loadFile(this.root, false);
    if (src) this.process(src, 1);
    let eof = { k: 6, t: "", off: 0, end: 0, line: 1, col: 1, sl: 0, ws: false, p: "" }, eofFile = this.nextID;
    if (this.files.length) {
      const last = this.files[this.files.length - 1];
      eof = last.toks[last.toks.length - 1];
      eofFile = last.id;
    }
    this.outToks.push(eof);
    this.outFile.push(eofFile);
  }
  process(src, depth) {
    if (this.onceSet.has(src.path) || depth > 100) return;
    this.onStack.add(src.path);
    const saved = this.fileName;
    this.fileName = this.relName(src.path);
    const toks = src.toks, n = toks.length, fid = src.id;
    const outT = this.outToks, outF = this.outFile, macros = this.macros;
    const lineEnd = lineEnds(toks);
    let active = this.condActive();
    while (src.pos < n) {
      const ls = src.pos;
      const t = toks[ls];
      if (t.k === 6) break;
      const le = lineEnd[ls];
      src.pos = le < n && toks[le].k === 7 ? le + 1 : le;
      if (le === ls) continue;
      if (t.k === 4 && t.p === "#") {
        this.directive(src, toks.slice(ls, le), t.line, depth);
        active = this.condActive();
        continue;
      }
      if (!active) continue;
      let may = false;
      for (let i = ls; i < le; i++) {
        const x = toks[i];
        if (x.k !== 0) continue;
        const s = x.t;
        if (s === "__LINE__" || s === "__FILE__" || macros.has(s)) { may = true; break; }
      }
      if (may) {
        const line = toks.slice(ls, le);
        if (this.openInvocation(line)) {
          for (let q = 0; q < 4000 && src.pos < n; q++) {
            const nt = toks[src.pos];
            if (nt.k === 6 || (nt.k === 4 && nt.p === "#")) break;
            const e = lineEnd[src.pos];
            for (let z = src.pos; z < e; z++) line.push(toks[z]);
            src.pos = e < n && toks[e].k === 7 ? e + 1 : e;
            if (!this.openInvocation(line)) break;
          }
        }
        const ex = this.expand(line);
        for (const x of ex) { outT.push(x); outF.push(fid); }
      } else {
        for (let i = ls; i < le; i++) { outT.push(toks[i]); outF.push(fid); }
      }
    }
    this.onStack.delete(src.path);
    this.fileName = saved;
  }
  lineMayExpand(line) {
    for (const t of line) {
      if (t.k !== 0) continue;
      if (t.t === "__LINE__" || t.t === "__FILE__" || this.macros.has(t.t)) return true;
    }
    return false;
  }
  openInvocation(toks) {
    let depth = 0, inv = false;
    for (let i = 0; i < toks.length; i++) {
      const t = toks[i];
      if (!inv) {
        if (t.k !== 0 || i + 1 >= toks.length) continue;
        const nx = toks[i + 1];
        if (nx.k !== 4 || nx.p !== "(") continue;
        const m = this.macros.get(t.t);
        if (m && m.isFn) { inv = true; depth = 0; }
        continue;
      }
      if (t.k !== 4) continue;
      if (t.p === "(") depth++;
      else if (t.p === ")") { depth--; if (depth === 0) inv = false; }
    }
    return inv;
  }
  directive(src, line, dirLine, depth) {
    if (line.length < 2) return;
    const name = line[1];
    if (name.k !== 0) { this.errf(name.line, "bad directive name"); return; }
    const rest = line.slice(2);
    const dir = name.t;
    switch (dir) {
      case "define": this.doDefine(src, line); break;
      case "undef":
        if (rest.length && rest[0].k === 0 && this.condActive()) this.macros.delete(rest[0].t);
        break;
      case "include": case "include_next": this.doInclude(src, rest, dirLine, depth); break;
      case "if": {
        const active = this.condActive();
        const val = active ? this.evalCond(src, rest, dirLine) : 0n;
        this.cfg(src, "if", rest, dirLine);
        const on = active && val !== 0n;
        this.cond.push({ parent: active, taken: on, active: on, seenElse: false });
        break;
      }
      case "ifdef": case "ifndef": {
        let val = false;
        if (this.condActive()) {
          const def = rest.length > 0 && rest[0].k === 0 && this.macros.has(rest[0].t);
          val = dir === "ifndef" ? !def : def;
        }
        this.cfg(src, dir, rest, dirLine);
        const on = this.condActive() && val;
        this.cond.push({ parent: this.condActive(), taken: on, active: on, seenElse: false });
        break;
      }
      case "elif": {
        if (!this.cond.length) { this.errf(dirLine, "#elif without #if"); return; }
        const st = this.cond[this.cond.length - 1];
        this.cfg(src, "elif", rest, dirLine);
        if (st.seenElse) { this.errf(dirLine, "#elif after #else"); return; }
        st.active = false;
        if (st.parent && !st.taken && this.evalCond(src, rest, dirLine) !== 0n) { st.active = true; st.taken = true; }
        break;
      }
      case "elifdef": case "elifndef": {
        if (!this.cond.length) { this.errf(dirLine, "#" + dir + " without #if"); return; }
        const st = this.cond[this.cond.length - 1];
        this.cfg(src, dir, rest, dirLine);
        if (st.seenElse) return;
        st.active = false;
        if (st.parent && !st.taken) {
          let def = rest.length > 0 && rest[0].k === 0 && this.macros.has(rest[0].t);
          if (dir === "elifndef") def = !def;
          st.active = def; st.taken = def;
        }
        break;
      }
      case "else": {
        if (!this.cond.length) { this.errf(dirLine, "#else without #if"); return; }
        const st = this.cond[this.cond.length - 1];
        this.cfgs.push({ fileID: src.id, directive: "else", expr: "", line: dirLine });
        st.seenElse = true;
        st.active = st.parent && !st.taken;
        st.taken = st.taken || st.active;
        break;
      }
      case "endif":
        this.cfgs.push({ fileID: src.id, directive: "endif", expr: "", line: dirLine });
        if (!this.cond.length) { this.errf(dirLine, "#endif without #if"); return; }
        this.cond.pop();
        break;
      case "line": this.doLine(rest, dirLine); break;
      case "error":
        if (this.condActive()) this.errf(dirLine, "#error: " + tokLineText(rest));
        break;
      case "warning": break;
      case "pragma":
        if (rest.length && rest[0].k === 0 && rest[0].t === "once") this.onceSet.add(src.path);
        break;
      case "embed": case "embed_next": break;
      default: this.errf(dirLine, "unknown directive #" + dir);
    }
  }
  cfg(src, dir, rest, line) {
    this.cfgs.push({ fileID: src.id, directive: dir, expr: tokLineText(rest), line });
  }
  doDefine(src, line) {
    if (line.length < 3 || line[2].k !== 0) { this.errf(line[0].line, "malformed #define"); return; }
    const nm = line[2];
    const m = { name: nm.t, fileID: src.id, line: nm.line, isFn: false, params: [], isVariadic: false, vaName: "", body: [], raw: "", uses: 0, multi: false };
    let i = 3;
    if (i < line.length && line[i].k === 4 && line[i].p === "(" && !line[i].ws) {
      i++;
      const params = [];
      let cur = "", depthP = 1;
      while (i < line.length && depthP > 0) {
        const t = line[i];
        if (t.k === 4 && t.p === "(") { depthP++; cur += "("; }
        else if (t.k === 4 && t.p === ")") { depthP--; if (depthP !== 0) cur += ")"; }
        else if (t.k === 4 && t.p === "," && depthP === 1) { const s = cur.trim(); if (s !== "") params.push(s); cur = ""; }
        else if (t.k === 4 && t.p === "..." && depthP === 1) { m.isVariadic = true; const c = cur.trim(); if (c !== "") m.vaName = c; cur = ""; }
        else if (t.k !== 7) cur += t.t;
        i++;
      }
      if (depthP !== 0) { this.errf(nm.line, "unbalanced macro parameter list"); return; }
      const s = cur.trim();
      if (s !== "") params.push(s);
      m.isFn = true;
      m.params = params;
      if (m.isVariadic && m.vaName === "") m.vaName = "__VA_ARGS__";
    }
    if (i < line.length) m.body = line.slice(i);
    const after = line.slice(3);
    m.raw = tokLineText(after);
    for (const t of after) if (t.k !== 7 && t.line > nm.line) { m.multi = true; break; }
    this.allMacros.push(m);
    if (this.condActive()) this.macros.set(m.name, m);
  }
  doInclude(src, rest, dirLine, depth) {
    if (!rest.length) { this.errf(dirLine, "empty #include"); return; }
    let tgt = "", sys = false;
    const take = (t) => {
      if (t.k === 5) { tgt = t.t.replace(/^</, "").replace(/>$/, ""); sys = true; }
      else if (t.k === 3) tgt = dequote(t.t);
    };
    if (rest[0].k === 5 || rest[0].k === 3) take(rest[0]);
    else { const ex = this.expand(rest); if (ex.length) take(ex[0]); }
    if (tgt === "") { if (this.condActive()) this.errf(dirLine, "malformed #include"); return; }
    const resolved = this.resolveInclude(src, tgt, sys);
    this.includes.push({ fromID: src.id, target: tgt, resolved, sys, line: dirLine });
    if (resolved === "" || !this.condActive()) return;
    if (this.onceSet.has(resolved) || this.onStack.has(resolved)) return;
    const nxt = this.loadFile(resolved, true);
    if (!nxt) return;
    this.process(nxt, depth + 1);
  }
  resolveInclude(src, tgt, sys) {
    const dir = dirname(src.path);
    const key = this.rootDir + "\0" + dir + "\0" + tgt + "\0" + (sys ? 1 : 0);
    const cache = this.tree.incCache;
    const hit = cache.get(key);
    if (hit !== undefined) return hit;
    const cands = [];
    if (!sys) cands.push(cleanPath(dir + "/" + tgt));
    if (sys || this.rootDir !== dir) cands.push(cleanPath(this.rootDir + "/" + tgt));
    let res = "";
    for (const c of cands) if (this.tree.isFile(c)) { res = c; break; }
    if (res === "" && !(sys && isSystemHeader(tgt))) res = this.tree.treeLookup(dir, tgt);
    cache.set(key, res);
    return res;
  }
  doLine(rest, dirLine) {
    if (!rest.length) return;
    const ex = this.expand(rest);
    if (!ex.length) return;
    const v = /^[+-]?(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*|0[bB][01]+)$/.test(stripNumSuffix(ex[0].t)) ? parseCInt(ex[0].t) : null;
    if (v !== null) this.lineDelta = Number(v) - dirLine - 1;
    if (ex.length > 2 && ex[1].k === 3) this.fileName = dequote(ex[1].t);
  }
  evalCond(src, toks, line) {
    const pre = this.resolveCondOps(src, toks, line);
    const ex = this.expand(pre);
    const e = new Eval(ex, this, line);
    const v = e.cond();
    if (e.bad) { this.errf(line, "bad #if expression"); return 0n; }
    return v;
  }
  resolveCondOps(src, toks, line) {
    const out = [];
    for (let i = 0; i < toks.length; i++) {
      const t = toks[i];
      if (t.k !== 0) { out.push(t); continue; }
      if (t.t === "defined") {
        let name = "", j = i + 1;
        if (j < toks.length && toks[j].k === 4 && toks[j].p === "(") {
          j++;
          if (j < toks.length && toks[j].k === 0) { name = toks[j].t; j++; }
          if (j < toks.length && toks[j].k === 4 && toks[j].p === ")") j++;
        } else if (j < toks.length && toks[j].k === 0) { name = toks[j].t; j++; }
        if (name === "") { this.errf(line, "bad defined()"); return out; }
        out.push({ ...t, k: 1, t: this.macros.has(name) ? "1" : "0" });
        i = j - 1;
        continue;
      }
      if (t.t === "__has_include" || t.t === "__has_include_next" || t.t === "__has_embed") {
        const j = i + 1;
        if (!(j < toks.length && toks[j].k === 4 && toks[j].p === "(")) { out.push(t); continue; }
        let depth = 1, k = j + 1;
        const inner = [];
        while (k < toks.length && depth > 0) {
          if (toks[k].k === 4 && toks[k].p === "(") depth++;
          else if (toks[k].k === 4 && toks[k].p === ")") { depth--; if (depth === 0) break; }
          inner.push(toks[k]); k++;
        }
        let val = 0;
        if (inner.length) {
          let tgt = "", sys = false;
          const take = (x) => {
            if (x.k === 5) { tgt = x.t.replace(/^</, "").replace(/>$/, ""); sys = true; }
            else if (x.k === 3) tgt = dequote(x.t);
          };
          if (inner[0].k === 5 || inner[0].k === 3) take(inner[0]);
          else { const ex = this.expand(inner); if (ex.length) take(ex[0]); }
          if (this.resolveInclude(src, tgt, sys) !== "") val = 1;
        }
        out.push({ ...t, k: 1, t: String(val) });
        i = k;
        continue;
      }
      out.push(t);
    }
    return out;
  }
  // ------------------------------------------------------------ expansion
  expand(inp) {
    const macros = this.macros, n = inp.length;
    let s = 0;
    for (; s < n; s++) {
      const t = inp[s];
      if (t.k === 0 && (macros.has(t.t) || t.t === "__LINE__" || t.t === "__FILE__")) break;
    }
    const work = new Array(n - s);
    for (let i = s; i < n; i++) work[n - 1 - i] = { t: inp[i], h: null };
    const out = this.expandWork(work);
    return s === 0 ? out : inp.slice(0, s).concat(out);
  }
  expandETS(ets) {
    const work = new Array(ets.length);
    for (let i = 0; i < ets.length; i++) work[ets.length - 1 - i] = ets[i];
    return this.expandWork(work);
  }
  expandWork(work) {
    const out = [];
    while (work.length) {
      const et = work.pop();
      const t = et.t;
      if (t.k !== 0 || hideHas(et.h, t.t)) { out.push(t); continue; }
      const m = this.macros.get(t.t);
      if (m === undefined) {
        if (t.t === "__LINE__") {
          let v = t.line + this.lineDelta; if (v < 1) v = 1;
          out.push({ ...t, k: 1, t: String(v) });
        } else if (t.t === "__FILE__") out.push({ ...t, k: 3, t: goQuote(this.fileName) });
        else out.push(t);
        continue;
      }
      if (++this.nExpand > 4194304) { out.push(t); continue; }
      if (!m.isFn) {
        m.uses++;
        const nh = { name: m.name, parent: et.h };
        const sub = this.subst(m, null, t);
        for (let k = sub.length - 1; k >= 0; k--) work.push({ t: sub[k], h: nh });
        continue;
      }
      let j = work.length - 1;
      while (j >= 0 && work[j].t.k === 7) j--;
      if (j < 0 || work[j].t.k !== 4 || work[j].t.p !== "(") { out.push(t); continue; }
      const r = collectArgs(work, j);
      if (r === null) { out.push(t); continue; }
      work.length = r.rest;
      m.uses++;
      const nh = { name: m.name, parent: et.h };
      const sub = this.subst(m, r.args, t);
      for (let k = sub.length - 1; k >= 0; k--) work.push({ t: sub[k], h: nh });
    }
    return out;
  }
  subst(m, args, use) {
    const body = m.body;
    const out = [];
    let vaArgs = [];
    const np = m.params.length;
    if (m.isVariadic && args !== null && args.length > np) { vaArgs = args.slice(np); args = args.slice(0, np); }
    if (m.isVariadic && args !== null && args.length < np) { args = args.slice(); while (args.length < np) args.push([]); }
    const argAt = (ai) => {
      if (args === null || ai >= args.length) throw new PanicError("macro argument index out of range");
      return args[ai];
    };
    for (let bi = 0; bi < body.length; bi++) {
      const bt = body[bi];
      if (bt.k === 4 && bt.p === "#" && m.isFn && bi + 1 < body.length) {
        const pt = body[bi + 1];
        if (pt.k === 0) {
          const ai = m.params.indexOf(pt.t);
          if (ai >= 0) { out.push(stringize(argAt(ai), use)); bi++; continue; }
        }
      }
      if (bt.k === 0 && bt.t === "__VA_OPT__" && m.isVariadic && bi + 1 < body.length && body[bi + 1].k === 4 && body[bi + 1].p === "(") {
        let depth = 1, k = bi + 2;
        while (k < body.length && depth > 0) {
          if (body[k].k === 4 && body[k].p === "(") depth++;
          else if (body[k].k === 4 && body[k].p === ")") { depth--; if (depth === 0) break; }
          k++;
        }
        const inner = body.slice(bi + 2, Math.min(k, body.length));
        if (vaArgs.some((a) => a.length > 0)) {
          const innerM = { name: "(va-opt)", body: inner, params: [], isFn: false, isVariadic: false, vaName: "" };
          for (const x of this.subst(innerM, args, use)) out.push(x);
        }
        bi = k;
        continue;
      }
      if (bt.k === 4 && bt.p === "##") {
        let right = null;
        if (bi + 1 < body.length) {
          const nt = body[bi + 1];
          let isParam = false;
          if (nt.k === 0) {
            const ai = m.params.indexOf(nt.t);
            if (ai >= 0) { right = argAt(ai).map((e) => e.t); isParam = true; }
            else if (m.isVariadic && nt.t === m.vaName) {
              right = joinVA(vaArgs.map((a) => a.map((e) => e.t)), use);
              isParam = true;
              if (out.length && out[out.length - 1].k === 4 && out[out.length - 1].p === ",") {
                if (right.length === 0) out.pop();
                else for (const x of right) out.push(x);
                bi++;
                continue;
              }
            }
          }
          if (right === null && !isParam) right = [nt];
          bi++;
        }
        if (right === null) right = [];
        if (out.length && right.length) {
          out[out.length - 1] = pasteTok(out[out.length - 1], right[0], use);
          for (let q = 1; q < right.length; q++) out.push(right[q]);
        } else if (right.length) for (const x of right) out.push(x);
        continue;
      }
      if (bt.k === 0) {
        const ai = m.params.indexOf(bt.t);
        if (ai >= 0) { for (const x of this.expandETS(argAt(ai))) out.push(x); continue; }
        if (m.isVariadic && bt.t === m.vaName) {
          const exp = vaArgs.map((va) => this.expandETS(va));
          for (const x of joinVA(exp, use)) out.push(x);
          continue;
        }
      }
      out.push(bt);
    }
    for (let k = 0; k < out.length; k++) out[k] = withPos(out[k], use);
    return out;
  }
}

function hideHas(h, n) {
  for (; h !== null; h = h.parent) if (h.name === n) return true;
  return false;
}

// Arguments of a function-like invocation whose "(" sits at work[j]; the
// work list is a stack (top = end). Returns {args, rest} or null.
function collectArgs(work, j) {
  const args = [];
  let cur = [], depth = 1, i = j - 1, done = false;
  while (i >= 0 && !done) {
    const et = work[i];
    const t = et.t;
    i--;
    if (t.k === 6) return null;
    if (t.k === 4 && t.p === "(") { depth++; cur.push(et); }
    else if (t.k === 4 && t.p === ")") {
      depth--;
      if (depth === 0) { args.push(cur); done = true; } else cur.push(et);
    } else if (t.k === 4 && t.p === "," && depth === 1) { args.push(cur); cur = []; }
    else if (t.k === 7) { /* dropped */ }
    else cur.push(et);
  }
  if (!done) return null;
  return { args, rest: i + 1 };
}

function joinVA(vaArgs, use) {
  const out = [];
  vaArgs.forEach((a, k) => {
    if (k > 0) out.push({ ...use, k: 4, p: ",", t: "," });
    for (const x of a) out.push(x);
  });
  return out;
}

function stringize(arg, use) {
  let s = '"';
  arg.forEach((et, i) => {
    const t = et.t;
    if (i > 0 && t.ws) s += " ";
    let x = t.t;
    if (t.k === 3) x = x.replaceAll("\\", "\\\\").replaceAll('"', '\\"');
    else if (t.k === 2) x = x.replaceAll("\\", "\\\\");
    s += x;
  });
  return { ...use, k: 3, t: s + '"', p: "" };
}

function pasteTok(l, r, use) {
  const txt = l.t + r.t;
  let k = 0, p = "";
  if (txt === "") k = 0;
  else {
    const c = txt.charCodeAt(0);
    if (isDigitC(c) || (c === 46 && txt.length > 1 && isDigitC(txt.charCodeAt(1)))) k = 1;
    else if (c === 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c >= 0x80) k = 0;
    else if (PUNCT_SET.has(txt)) { k = 4; p = DIGRAPH[txt] || txt; }
  }
  return { ...use, k, t: txt, p };
}

export function predefinedMacros(projectDefines) {
  const base = [["__STDC__", "1"], ["__STDC_VERSION__", "202311L"], ["__STDC_HOSTED__", "1"], ["__c23parser", "1"],
    ["__GNUC__", "4"], ["__GNUC_MINOR__", "2"], ["__GNUC_PATCHLEVEL__", "1"], ["__CHAR_BIT__", "8"], ["__SIZEOF_LONG__", "8"],
    ["__SIZEOF_PTRDIFF_T__", "8"], ["__SIZE_TYPE__", "unsigned long"], ["__PTRDIFF_TYPE__", "long"],
    ["LITTLE_ENDIAN", "1234"], ["BIG_ENDIAN", "4321"], ["BYTE_ORDER", "1234"], ["__BYTE_ORDER", "1234"],
    ["__ORDER_LITTLE_ENDIAN__", "1234"], ["__BYTE_ORDER__", "1234"], ["__SIZEOF_INT128__", "16"], ["__ORDER_BIG_ENDIAN__", "4321"], ["__ORDER_PDP_ENDIAN__", "3412"],
    ["__linux__", "1"], ["linux", "1"], ["__unix__", "1"], ["__unix", "1"], ["__APPLE__", "1"], ["__MACH__", "1"],
    ["__x86_64__", "1"], ["__x86_64", "1"], ["__amd64__", "1"], ["__amd64", "1"], ["__LITTLE_ENDIAN__", "1"], ["__ARM_ARCH", "8"],
    ["__has_include", "1"], ["__has_include_next", "1"], ["__has_c_attribute", "0"], ["__has_embed", "0"]];
  const out = [];
  const obj = (name, text) => {
    const body = lexWhole(text, null).filter((t) => t.k !== 6).slice(0, 64);
    return { name, isFn: false, params: [], isVariadic: false, vaName: "", body, raw: text, line: 0, fileID: 0, uses: 0, multi: false };
  };
  for (const [n, v] of base) out.push(obj(n, v));
  for (const [n, v] of projectDefines) {
    out.push(obj(n, v));
    if (n === "CONFIG_IO_URING") {
      const m = obj("io_uring_for_each_cqe", "for (head = 0; cqe; head++)");
      m.isFn = true; m.params = ["ring", "head", "cqe"];
      out.push(m);
    }
  }
  return out;
}
