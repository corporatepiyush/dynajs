// Text heuristics over a function body's bytes (comments and literals blanked):
import { trimSpace, jtrunc } from "./text.js";
// the counters that feed the rare symbol columns and the hazards table.
const C = (s, i) => s.charCodeAt(i);
export const isIdentStart = (c) => c === 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90);
export const isIdentPart = (c) => isIdentStart(c) || (c >= 48 && c <= 57) || c >= 0x80;
const isWordByte = isIdentPart;
const isDigit = (c) => c >= 48 && c <= 57;
export const isHardWS = (c) => c === 32 || c === 9 || c === 13 || c === 10;
export const isStripSpaceB = (c) => c === 32 || c === 9 || c === 13 || c === 10 || c === 11 || c === 12 ||
  c === 0x1c || c === 0x1d || c === 0x1e || c === 0x1f || c === 0x85 || c === 0xa0;

export function trimSpaceB(s) {
  let a = 0, b = s.length;
  while (a < b && isStripSpaceB(C(s, a))) a++;
  while (b > a && isStripSpaceB(C(s, b - 1))) b--;
  return s.slice(a, b);
}
export const isWordAt = (b, i, s) => i >= 0 && i + s.length <= b.length && b.startsWith(s, i);
export const wordBefore = (b, i) => i > 0 && isWordByte(C(b, i - 1));
export const wordAfter = (b, i) => i < b.length && isWordByte(C(b, i));
export function identEnd(b, i) {
  if (i >= b.length || !isIdentStart(C(b, i))) return i;
  let j = i + 1;
  while (j < b.length && isIdentPart(C(b, j))) j++;
  return j;
}
const wEnd = (b, i) => { while (i < b.length && isWordByte(C(b, i))) i++; return i; };
export const skipHardWS = (b, i) => { while (i < b.length && isHardWS(C(b, i))) i++; return i; };
const skipHT = (b, i) => { while (i < b.length && (C(b, i) === 32 || C(b, i) === 9)) i++; return i; };
const indexByteFrom = (b, ch, from) => (from < 0 ? -1 : b.indexOf(ch, from));
export function countStr(b, s) {
  if (!s.length) return 0;
  let n = 0;
  for (let i = b.indexOf(s); i >= 0; i = b.indexOf(s, i + s.length)) n++;
  return n;
}
export const hasStr = (b, s) => b.includes(s);
export const asciiLower = (s) => s.replace(/[A-Z]+/g, (m) => m.toLowerCase());

export function countWord(b, w) {
  let n = 0;
  for (let i = b.indexOf(w); i >= 0; i = b.indexOf(w, i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + w.length)) continue;
    n++;
    i += w.length - 1;
  }
  return n;
}

export function countCast(b) {
  if (!b.includes("*")) return 0;
  let n = 0;
  for (let i = b.indexOf("("); i >= 0 && i + 1 < b.length; i = b.indexOf("(", i + 1)) {
    let j = skipHardWS(b, i + 1);
    if (j + 5 < b.length && b.startsWith("const", j) && isHardWS(C(b, j + 5))) j = skipHardWS(b, j + 5);
    const e = identEnd(b, j);
    if (e === j) continue;
    let k = skipHardWS(b, e), star = 0;
    while (k < b.length && C(b, k) === 42) { star++; k++; }
    if (star === 0) continue;
    k = skipHardWS(b, k);
    if (k < b.length && C(b, k) === 41) { n++; i = k; }
  }
  return n;
}

const RE_MULSZ = /\*|sizeof/g;
export function countMulSizeof(b) {
  if (!b.includes("sizeof")) return 0;
  let n = 0;
  const re = RE_MULSZ;
  re.lastIndex = 0;
  let m;
  while ((m = re.exec(b)) !== null) {
    const i = m.index;
    re.lastIndex = i + 1;
    if (C(b, i) === 42) {
      const j = skipHardWS(b, i + 1);
      if (isWordAt(b, j, "sizeof")) { n++; re.lastIndex = j + 6; }
      continue;
    }
    if (!wordAfter(b, i + 6)) {
      const j = skipHardWS(b, i + 6);
      if (j < b.length && C(b, j) === 40) {
        let k = indexByteFrom(b, ")", j + 1);
        if (k >= 0) {
          k = skipHardWS(b, k + 1);
          if (k < b.length && C(b, k) === 42) { n++; re.lastIndex = k + 1; }
        }
      }
    }
  }
  return n;
}

const FIXED_BUF = ["unsigned char", "uint8_t", "int8_t", "char"];
const RE_FIXED = /(?<![A-Za-z0-9_\x80-\xff])(?:unsigned|uint8_t|int8_t|char)(?![A-Za-z0-9_\x80-\xff])/g;
export function countFixedBuffer(b) {
  if (!b.includes("[")) return 0;
  let n = 0, m;
  const re = RE_FIXED;
  re.lastIndex = 0;
  while ((m = re.exec(b)) !== null) {
    const e = m.index + m[0].length;
    let j = skipHardWS(b, e);
    const k = identEnd(b, j);
    if (k === j) continue;
    j = skipHardWS(b, k);
    if (j >= b.length || C(b, j) !== 91) continue;
    j = skipHT(b, j + 1);
    const st = j;
    while (j < b.length && isDigit(C(b, j))) j++;
    if (j === st) continue;
    const q = skipHT(b, j);
    if (q < b.length && C(b, q) === 93) { n++; re.lastIndex = q + 1; }
  }
  return n;
}

const RE_VLA = /(?<![A-Za-z0-9_\x80-\xff])(?:char|int|uint8_t|double|float)(?![A-Za-z0-9_\x80-\xff])/g;
export function countVLAPat(b) {
  if (!b.includes("[")) return 0;
  let n = 0, mm;
  const re = RE_VLA;
  re.lastIndex = 0;
  while ((mm = re.exec(b)) !== null) {
    const e = mm.index + mm[0].length;
    let j = skipHardWS(b, e);
    const k = identEnd(b, j);
    if (k === j) continue;
    j = skipHardWS(b, k);
    if (j >= b.length || C(b, j) !== 91) continue;
    j = skipHT(b, j + 1);
    if (j >= b.length || !(C(b, j) === 95 || (C(b, j) >= 97 && C(b, j) <= 122))) continue;
    let m = identEnd(b, j);
    m = skipHardWS(b, m);
    if (m < b.length && C(b, m) === 93) { n++; re.lastIndex = m + 1; }
  }
  return n;
}

export function countSignedCmp(b) {
  let n = 0;
  for (let i = b.indexOf("int"); i >= 0; i = b.indexOf("int", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 3)) continue;
    let j = skipHardWS(b, i + 3), k = wEnd(b, j);
    if (k === j) continue;
    j = skipHardWS(b, k);
    if (j >= b.length || C(b, j) !== 61 || (j + 1 < b.length && C(b, j + 1) === 61)) continue;
    j = skipHardWS(b, j + 1);
    k = wEnd(b, j);
    if (k === j) continue;
    j = skipHardWS(b, k);
    if (j >= b.length || C(b, j) !== 45) continue;
    j = skipHardWS(b, j + 1);
    k = wEnd(b, j);
    if (k > j) n++;
  }
  return n;
}

function lockWord(w) {
  switch (w) {
    case "lock": case "trylock": case "rdlock": case "wrlock": case "tryrdlock": case "trywrlock":
    case "timedlock": case "timedrdlock": case "timedwrlock": case "spinlock": case "acquire": return 1;
    case "unlock": case "release": return 2;
    case "mutex": case "mtx": case "rwlock": case "spin": case "sem": case "semaphore": case "futex": case "lck":
    case "locked": case "unlocked": return 3;
  }
  return 0;
}

// [shaped, acquires, releases]
export function lockShape(name) {
  const words = [];
  let start = 0;
  const flush = (end) => { if (end > start && words.length < 12) words.push(asciiLower(name.slice(start, end))); start = end; };
  for (let i = 0; i < name.length; i++) {
    const c = C(name, i);
    if (c === 95) { flush(i); start = i + 1; }
    else if (c >= 65 && c <= 90 && i > 0 && C(name, i - 1) >= 97 && C(name, i - 1) <= 122) flush(i);
  }
  flush(name.length);
  let hard = false, acq = false, rel = false, verbAcq = false, verbRel = false;
  for (const w of words) {
    switch (lockWord(w)) {
      case 1: if (w === "acquire") verbAcq = true; else { acq = true; hard = true; } break;
      case 2: if (w === "release") verbRel = true; else { rel = true; hard = true; } break;
      case 3: hard = true; break;
    }
  }
  if (!hard) {
    switch (name) {
      case "flock": case "lockf": case "flockfile": case "ftrylockfile": return [true, true, false];
      case "funlockfile": return [true, false, true];
    }
    return [false, false, false];
  }
  if (rel || verbRel) return [true, false, true];
  if (words.length > 1 && words[0] === "sem") {
    switch (words[words.length - 1]) {
      case "wait": case "trywait": case "timedwait": return [true, true, false];
      case "post": return [true, false, true];
    }
  }
  return [true, acq || verbAcq, false];
}

const NARROW = ["uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t"];
export function countNarrowCasts(b) {
  let n = 0;
  for (let i = b.indexOf("("); i >= 0 && i + 1 < b.length; i = b.indexOf("(", i + 1)) {
    let j = skipHardWS(b, i + 1);
    let hit = false;
    for (const t of NARROW) if (isWordAt(b, j, t) && !wordAfter(b, j + t.length)) { hit = true; break; }
    if (!hit) continue;
    for (const t of NARROW) if (isWordAt(b, j, t)) { j += t.length; break; }
    j = skipHardWS(b, j);
    if (j >= b.length || C(b, j) !== 41) continue;
    j = skipHardWS(b, j + 1);
    if (j < b.length && C(b, j) === 40) j = skipHardWS(b, j + 1);
    if (j < b.length && (isIdentStart(C(b, j)) || C(b, j) === 38 || C(b, j) === 42)) { n++; i = j; }
  }
  return n;
}

function retNegArm(b, i) {
  const c = C(b, i);
  if (c === 60) {
    if (i + 1 < b.length && C(b, i + 1) === 61) {
      const j = skipHardWS(b, i + 2);
      if (j + 1 < b.length && C(b, j) === 45 && C(b, j + 1) === 49) return j + 2 - i;
      return 0;
    }
    const j = skipHardWS(b, i + 1);
    if (j < b.length && C(b, j) === 48) return j + 1 - i;
  } else if (c === 61 || c === 33) {
    if (i + 1 < b.length && C(b, i + 1) === 61) {
      const j = skipHardWS(b, i + 2);
      if (j + 1 < b.length && C(b, j) === 45 && C(b, j + 1) === 49) return j + 2 - i;
    }
  } else if (c === 45) {
    if (i > 0 && isWordByte(C(b, i - 1))) return 0;
    const j = skipHardWS(b, i + 1);
    if (j < b.length && C(b, j) === 49) {
      const k = skipHardWS(b, j + 1);
      if (isWordAt(b, k, "==") || isWordAt(b, k, "!=")) return k + 2 - i;
    }
  }
  return 0;
}
const armCount = (b, arm, re) => {
  let n = 0, m;
  re.lastIndex = 0;
  while ((m = re.exec(b)) !== null) {
    const i = m.index, k = arm(b, i);
    if (k > 0) { n++; re.lastIndex = i + k; } else re.lastIndex = i + 1;
  }
  return n;
};
const RE_RETNEG = /[<=!-]/g;
const RE_DOMAIN = /(?<![A-Za-z0-9_\x80-\xff])(?:if|fabs|clamp)(?![A-Za-z0-9_\x80-\xff])|[<>]/g;

function domainArm(b, i) {
  const c = C(b, i);
  if (isIdentStart(c) && !wordBefore(b, i)) {
    const w = b.slice(i, identEnd(b, i));
    if (w === "if" || w === "fabs") {
      const j = skipHardWS(b, i + w.length);
      if (j < b.length && C(b, j) === 40) return j + 1 - i;
    }
  }
  if (isWordAt(b, i, "clamp") && !wordBefore(b, i) && !wordAfter(b, i + 5)) return 5;
  if (c !== 62 && c !== 60) return 0;
  if (i + 1 < b.length && C(b, i + 1) === 61) return 2;
  const j = skipHardWS(b, i + 1);
  if (j >= b.length) return 0;
  const d = C(b, j);
  if (d === 48) return j + 1 - i;
  if (d === 49 && c === 60) return j + 1 - i;
  if (d === 45 && c === 62 && j + 1 < b.length && C(b, j + 1) === 49) return j + 2 - i;
  return 0;
}

function gUchar(b) {
  if (!b.includes("unsigned")) return 0;
  let n = 0;
  for (let i = b.indexOf("("); i >= 0 && i + 1 < b.length; i = b.indexOf("(", i + 1)) {
    let j = skipHardWS(b, i + 1);
    if (!isWordAt(b, j, "unsigned")) continue;
    j = skipHardWS(b, j + 8);
    if (!isWordAt(b, j, "char")) continue;
    j = skipHardWS(b, j + 4);
    if (j < b.length && C(b, j) === 41) { n++; i = j; }
  }
  return n;
}
function gErrnoZero(b) {
  let n = 0;
  for (let i = b.indexOf("errno"); i >= 0; i = b.indexOf("errno", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 5)) continue;
    const j = skipHardWS(b, i + 5);
    if (j < b.length && C(b, j) === 61 && (j + 1 >= b.length || C(b, j + 1) !== 61)) {
      const k = skipHardWS(b, j + 1);
      if (k < b.length && C(b, k) === 48) n++;
    }
  }
  return n;
}
const RE_ENDPTR = /endptr|&/g;
function gEndptr(b) {
  let n = 0, m;
  const re = RE_ENDPTR;
  re.lastIndex = 0;
  while ((m = re.exec(b)) !== null) {
    const i = m.index;
    re.lastIndex = i + 1;
    if (C(b, i) !== 38) { n++; re.lastIndex = i + 6; continue; }
    const j = skipHardWS(b, i + 1);
    let k = wEnd(b, j);
    if (k > j) {
      k = skipHardWS(b, k);
      if (k < b.length && C(b, k) === 41) {
        k = skipHardWS(b, k + 1);
        if (k < b.length && C(b, k) === 59) { n++; re.lastIndex = k + 1; }
      }
    }
  }
  return n;
}
function gStackszArray(b) {
  if (!b.includes("STKSZ") && !b.includes("PTHREAD_STACK_MIN")) return 0;
  let n = 0;
  for (let i = 0; i < b.length; i++) {
    if (!isWordByte(C(b, i))) continue;
    const j = skipHardWS(b, identEnd(b, i));
    if (j >= b.length || C(b, j) !== 91) continue;
    const k = skipHT(b, j + 1);
    for (const w of ["SIGSTKSZ", "MINSIGSTKSZ", "PTHREAD_STACK_MIN"]) {
      if (isWordAt(b, k, w)) { const m = skipHT(b, k + w.length); if (m < b.length && C(b, m) === 93) n++; }
    }
  }
  return n;
}
const RE_PTROVF = /(?<![A-Za-z0-9_\x80-\xff])[A-Za-z_][A-Za-z0-9_\x80-\xff]*[\t\n\r ]*\+/g;
function gPtrOvf(b) {
  if (!b.includes("<")) return 0;
  let n = 0, mm;
  const re = RE_PTROVF;
  re.lastIndex = 0;
  while ((mm = re.exec(b)) !== null) {
    const i = mm.index, name = b.slice(i, identEnd(b, i));
    let j = skipHardWS(b, mm.index + mm[0].length);
    re.lastIndex = j;
    let k = j;
    while (k < b.length && isWordByte(C(b, k))) k++;
    if (k === j) continue;
    k = skipHardWS(b, k);
    if (k >= b.length || C(b, k) !== 60) continue;
    k = skipHardWS(b, k + 1);
    if (isWordAt(b, k, name) && !wordAfter(b, k + name.length)) n++;
  }
  return n;
}
function gCallocTransposed(b) {
  let n = 0;
  for (let i = b.indexOf("calloc"); i >= 0; i = b.indexOf("calloc", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 6)) continue;
    let j = skipHardWS(b, i + 6);
    if (j >= b.length || C(b, j) !== 40) continue;
    j = skipHT(b, j + 1);
    if (isWordAt(b, j, "sizeof")) n++;
  }
  return n;
}
function gVaArgArray(b) {
  let n = 0;
  for (let i = b.indexOf("va_arg"); i >= 0; i = b.indexOf("va_arg", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 6)) continue;
    const j = skipHardWS(b, i + 6);
    if (j >= b.length || C(b, j) !== 40) continue;
    let region = indexByteFrom(b, ";", j + 1);
    if (region < 0) region = b.length;
    for (let q = j + 1; q < region; q++) {
      if (q + 1 < region && C(b, q) === 40) break;
      if (C(b, q) === 91) { const r = indexByteFrom(b, "]", q + 1); if (r > 0 && r < region) n++; break; }
    }
  }
  return n;
}

export function countCall(b, ...names) {
  let n = 0;
  for (const w of names) {
    for (let i = b.indexOf(w); i >= 0; i = b.indexOf(w, i + 1)) {
      if (wordBefore(b, i) || wordAfter(b, i + w.length)) continue;
      const p = skipHardWS(b, i + w.length);
      if (p < b.length && C(b, p) === 40) n++;
    }
  }
  return n;
}
function countCall2(b, prefix) {
  let n = 0;
  for (let i = b.indexOf(prefix); i >= 0; i = b.indexOf(prefix, i + 1)) {
    if (wordBefore(b, i)) continue;
    const e = identEnd(b, i + prefix.length);
    const j = skipHardWS(b, e);
    if (j < b.length && C(b, j) === 40) { n++; i = e - 1; }
  }
  return n;
}

export function countGuard(g, b) {
  switch (g) {
    case "ret_neg": return armCount(b, retNegArm, RE_RETNEG);
    case "domain": return armCount(b, domainArm, RE_DOMAIN);
    case "uchar": return gUchar(b);
    case "errno_zero": return gErrnoZero(b);
    case "endptr": return gEndptr(b);
    case "va_end": return countCall(b, "va_end");
    case "map_failed": return countWord(b, "MAP_FAILED");
    case "monotonic": return countWord(b, "CLOCK_MONOTONIC") + countWord(b, "CLOCK_MONOTONIC_RAW") + countWord(b, "CLOCK_BOOTTIME");
    case "stacksz_array": return gStackszArray(b);
    case "ptr_ovf_check": return gPtrOvf(b);
    case "calloc_sizeof_first": return gCallocTransposed(b);
    case "va_arg_array": return gVaArgArray(b);
  }
  return 0;
}

const flagWords = (b, ...ws) => ws.reduce((n, w) => n + countWord(b, w), 0);
function scanUringRes(b) {
  let n = 0;
  for (let i = b.indexOf("res"); i >= 0; i = b.indexOf("res", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 3)) continue;
    const j = skipHardWS(b, i + 3);
    if (j < b.length && C(b, j) === 60) { const k = skipHardWS(b, j + 1); if (k < b.length && C(b, k) === 48) n++; }
    if (i >= 2 && C(b, i - 1) === 45 && C(b, i - 2) === 62) n++;
    if (i >= 1 && C(b, i - 1) === 46) n++;
  }
  return n;
}
function scanUringUdata(b) {
  let n = countCall2(b, "io_uring_sqe_set_data");
  for (let i = b.indexOf("user_data"); i >= 0; i = b.indexOf("user_data", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 9)) continue;
    const j = skipHardWS(b, i + 9);
    if (j < b.length && C(b, j) === 61 && (j + 1 >= b.length || C(b, j + 1) !== 61)) {
      const k = skipHardWS(b, j + 1);
      if (k < b.length && C(b, k) !== 59) n++;
    }
  }
  return n;
}
function scanStreamOps(b) {
  let n = 0;
  for (let i = 0; i + 15 <= b.length; i++) {
    if (!isWordAt(b, i, "io_uring_prep_") || wordBefore(b, i)) continue;
    const p = i + 15;
    for (const suf of ["sendmsg", "sendzc", "send", "recvmsg", "recv", "send_zc"]) {
      if (!isWordAt(b, p, suf)) continue;
      const e = p + suf.length;
      if (wordAfter(b, e)) continue;
      const j = skipHardWS(b, e);
      if (j < b.length && C(b, j) === 40) { n++; i = e - 1; }
      break;
    }
  }
  return n;
}
function scanUringRing(b) {
  const inClass = (c) => isWordByte(c) || c === 46 || c === 62 || c === 45;
  let n = 0, i = 0;
  while (i < b.length) {
    if (!isWordAt(b, i, "ring")) { i++; continue; }
    let re = i + 4;
    while (re < b.length && inClass(C(b, re)) && !isHardWS(C(b, re))) re++;
    let matched = false;
    for (let g = Math.min(re - i - 4, 16); g >= 0; g--) {
      const q = i + 4 + g;
      if (q + 4 > re && q + 4 > b.length) continue;
      if (!isWordAt(b, q, "head") && !isWordAt(b, q, "tail")) continue;
      if (!wordAfter(b, q + 4)) continue;
      if (q === 0 || !isWordByte(C(b, q - 1))) { n++; matched = true; i = q + 4 - 1; break; }
    }
    if (!matched) i++;
  }
  return n;
}
const BARRIER = new Set(["io_uring_smp_load_acquire", "io_uring_smp_store_release", "smp_load_acquire", "smp_store_release",
  "atomic_load_explicit", "atomic_store_explicit", "atomic_load", "atomic_store", "__atomic_load_n", "__atomic_store_n",
  "__atomic_load", "__atomic_store"]);
function scanUringBarrier(b) {
  let n = 0;
  for (let i = 0; i < b.length; i++) {
    if (!isIdentStart(C(b, i)) || wordBefore(b, i)) continue;
    const e = identEnd(b, i);
    if (!BARRIER.has(b.slice(i, e))) continue;
    const j = skipHardWS(b, e);
    if (j < b.length && C(b, j) === 40) { n++; i = e - 1; }
  }
  return n;
}
export function scanEvFlags(b) {
  return {
    etReg: flagWords(b, "EPOLLET", "EV_CLEAR"),
    oneshotReg: flagWords(b, "EPOLLONESHOT", "EV_ONESHOT", "EV_DISPATCH"),
    writeReady: flagWords(b, "EPOLLOUT", "EVFILT_WRITE"),
    errFlag: flagWords(b, "EPOLLERR", "EPOLLHUP", "EV_EOF", "EV_ERROR"),
    rearm: flagWords(b, "EPOLL_CTL_MOD", "EV_ENABLE"),
    dereg: flagWords(b, "EPOLL_CTL_DEL", "EV_DELETE", "EV_DISABLE"),
    eagain: flagWords(b, "EAGAIN", "EWOULDBLOCK"),
    eintr: countWord(b, "EINTR"),
    uringSqpoll: countWord(b, "IORING_SETUP_SQPOLL"),
    nonblockSet: flagWords(b, "O_NONBLOCK", "SOCK_NONBLOCK", "FIONBIO", "F_SETFL"),
    uringSqe: countCall(b, "io_uring_get_sqe"),
    uringSeen: countCall(b, "io_uring_cqe_seen") + countCall(b, "io_uring_cq_advance"),
    uringUdata: scanUringUdata(b),
    uringLink: countWord(b, "IOSQE_IO_LINK"),
    uringStreamOps: scanStreamOps(b),
    uringTeardown: countCall(b, "io_uring_queue_exit"),
    kqTimer: countWord(b, "EVFILT_TIMER"),
    kqReceipt: countWord(b, "EV_RECEIPT"),
    eventDestroy: countCall(b, "close", "io_uring_queue_exit") + countCall2(b, "io_uring_unregister"),
    uringRing: scanUringRing(b),
    uringBarrier: scanUringBarrier(b),
    uringRes: scanUringRes(b),
  };
}

export const countWeakRandom = (b) => countCall(b, "rand", "random", "srand", "srandom", "drand48", "lrand48", "mrand48");
export function countShiftVar(b) {
  let n = 0, i = 0, nl = b.indexOf("<<"), nr = b.indexOf(">>");
  for (;;) {
    if (nl >= 0 && nl < i) nl = b.indexOf("<<", i);
    if (nr >= 0 && nr < i) nr = b.indexOf(">>", i);
    if (nl < 0 && nr < 0) break;
    i = nl < 0 ? nr : nr < 0 ? nl : Math.min(nl, nr);
    const j = skipHardWS(b, i + 2);
    if (j < b.length && (C(b, j) === 40 || isIdentStart(C(b, j)))) { n++; i = j; }
    i++;
  }
  return n;
}
export function countReallocSelf(b) {
  let n = 0;
  for (let i = 0; i < b.length; i++) {
    if (!isIdentStart(C(b, i)) || wordBefore(b, i)) continue;
    const e = identEnd(b, i), name = b.slice(i, e);
    let j = skipHardWS(b, e);
    if (j >= b.length || C(b, j) !== 61 || (j + 1 < b.length && C(b, j + 1) === 61)) continue;
    j = skipHardWS(b, j + 1);
    const run = identEnd(b, j);
    let ok = false;
    for (let k = run; k >= j; k--) {
      if (!isWordAt(b, k, "realloc") || wordAfter(b, k + 7)) continue;
      let p = skipHardWS(b, k + 7);
      if (p >= b.length || C(b, p) !== 40) continue;
      p = skipHardWS(b, p + 1);
      if (!isWordAt(b, p, name) || wordAfter(b, p + name.length)) continue;
      const q = skipHardWS(b, p + name.length);
      if (q < b.length && C(b, q) === 44) { ok = true; break; }
    }
    if (ok) n++;
  }
  return n;
}
export function countAssertSide(b) {
  let n = 0;
  const alt = (i) => {
    if (isWordAt(b, i, "strcpy") || isWordAt(b, i, "memcpy") || isWordAt(b, i, "malloc") || isWordAt(b, i, "printf")) return true;
    if (i + 1 < b.length && C(b, i) === 61 && C(b, i + 1) !== 61) {
      const prev = i > 0 ? C(b, i - 1) : 0;
      if (prev === 61 || prev === 33) return false;
      if (prev === 60 || prev === 62) return i > 1 && C(b, i - 2) === prev;
      return true;
    }
    return i + 1 < b.length && ((C(b, i) === 43 && C(b, i + 1) === 43) || (C(b, i) === 45 && C(b, i + 1) === 45));
  };
  for (let i = b.indexOf("assert"); i >= 0; i = b.indexOf("assert", i + 1)) {
    if (wordBefore(b, i) || wordAfter(b, i + 6)) continue;
    const p = skipHardWS(b, i + 6);
    if (p >= b.length || C(b, p) !== 40) continue;
    let region = indexByteFrom(b, ";", p + 1);
    if (region < 0) region = b.length;
    let found = -1;
    for (let q = p + 1; q < region; q++) if (alt(q)) { found = q; break; }
    if (found < 0) continue;
    let end = -1;
    for (let q = region - 1; q > found; q--) if (C(b, q) === 41) { end = q; break; }
    if (end < 0) continue;
    n++;
    i = end;
  }
  return n;
}
export function scanFrees(b, fn) {
  for (let j = b.indexOf("free"); j >= 0; j = b.indexOf("free", j + 1)) {
    if (wordAfter(b, j + 4)) continue;
    let s0 = j;
    while (s0 > 0 && isWordByte(C(b, s0 - 1))) s0--;
    if (!isIdentStart(C(b, s0))) continue;
    let p = skipHardWS(b, j + 4);
    if (p < b.length && C(b, p) === 40) {
      p = skipHardWS(b, p + 1);
      const q = identEnd(b, p);
      if (q > p && q < b.length && C(b, q) === 41) fn(b.slice(p, q), q + 1);
    }
  }
}
export function derefAfter(t, name) {
  for (let i = 0; i < t.length; i++) {
    if (isWordAt(t, i, name)) {
      if (i === 0 || (C(t, i - 1) !== 46 && !isWordByte(C(t, i - 1)))) {
        const j = skipHardWS(t, i + name.length);
        if (j < t.length && (C(t, j) === 91 || (C(t, j) === 45 && j + 1 < t.length && C(t, j + 1) === 62))) return true;
      }
    }
    if (C(t, i) === 42) {
      const j = skipHardWS(t, i + 1);
      if (isWordAt(t, j, name)) {
        const k = j + name.length;
        if (!wordAfter(t, k)) {
          const mm = skipHardWS(t, k);
          if (mm >= t.length || C(t, mm) !== 61 || (mm + 1 < t.length && C(t, mm + 1) === 61)) return true;
        }
      }
    }
  }
  return false;
}

// Comments and string/char literals blanked to spaces (newlines kept).
const BB_NEXT = /[\/"']/g, BB_DQ = /["\\\n]/g, BB_SQ = /['\\\n]/g;
export function blankBuf(src) {
  const n = src.length;
  const parts = [];
  let last = 0, i = 0;
  const blank = (a, e) => {
    if (e > n) e = n;
    parts.push(src.slice(last, a));
    parts.push(src.slice(a, e).replace(/[^\n]/g, " "));
    last = e;
  };
  while (i < n) {
    BB_NEXT.lastIndex = i;
    if (BB_NEXT.exec(src) === null) break;
    i = BB_NEXT.lastIndex - 1;
    const c = C(src, i);
    if (c === 47 && i + 1 < n && C(src, i + 1) === 42) {
      const j = i + 2 >= n ? -1 : src.indexOf("*/", i + 2);
      const end = j >= 0 ? j + 2 : n;
      blank(i, end);
      i = end;
    } else if (c === 47 && i + 1 < n && C(src, i + 1) === 47) {
      const j = src.indexOf("\n", i);
      const end = j >= 0 ? j : n;
      blank(i, end);
      i = end;
    } else if (c === 34 || c === 39) {
      const re = c === 34 ? BB_DQ : BB_SQ;
      let j = i + 1, closed = -1;
      while (j < n) {
        re.lastIndex = j;
        if (re.exec(src) === null) { j = n; break; }
        j = re.lastIndex - 1;
        const d = C(src, j);
        if (d === 92) { j += 2; continue; }
        if (d === c) closed = j;
        break;
      }
      let end = j;
      if (closed >= 0) end = closed + 1;
      else if (end < n && C(src, end) === 92) end = n;
      blank(i, end);
      i = end;
    } else i++;
  }
  parts.push(src.slice(last));
  return parts.join("");
}

export function nlOffsets(b) {
  const o = [];
  for (let i = b.indexOf("\n"); i >= 0; i = b.indexOf("\n", i + 1)) o.push(i + 1);
  return o;
}
function lineText(b, nl, n) {
  let start = 0;
  if (n > 1) { if (n - 2 >= nl.length) return null; start = nl[n - 2]; }
  const end = n - 1 < nl.length ? nl[n - 1] : b.length;
  if (end < start) return null;
  return b.slice(start, end);
}
export function leadingComment(b, nl, lineStart) {
  let n = 0, i = lineStart - 2;
  while (i >= 0) {
    const s = trimSpaceB(lineText(b, nl, i + 1) ?? "");
    if (s.length === 0 && n === 0) { i--; continue; }
    if (s.startsWith("/*") || s.startsWith("*") || s.startsWith("//")) { n++; i--; continue; }
    break;
  }
  return [n > 0, n];
}

export function replaceWord(s, w, rep) {
  let out = "", i = 0;
  while (i < s.length) {
    if (isWordAt(s, i, w) && (i === 0 || !isIdentPart(C(s, i - 1))) && (i + w.length >= s.length || !isIdentPart(C(s, i + w.length)))) {
      out += rep;
      i += w.length;
      continue;
    }
    out += s[i];
    i++;
  }
  return out;
}

export function firstIdent(s) {
  for (let i = 0; i < s.length; i++) {
    if (isIdentStart(C(s, i))) {
      let e = i + 1;
      while (e < s.length && isIdentPart(C(s, e))) e++;
      return s.slice(i, e);
    }
  }
  return null;
}
export function lastPathTail(dst) {
  let last = "", start = 0;
  for (let i = 0; i <= dst.length; i++) {
    if (i === dst.length || ".>[]".includes(dst[i])) { last = dst.slice(start, i); start = i + 1; }
  }
  const t = trimSpaceB(last);
  return t === "" ? dst : t;
}

const MARKER_KINDS = new Set(["TODO", "FIXME", "XXX", "HACK", "BUG", "NOTE", "WARNING", "OPTIMIZE", "REVIEW", "DEPRECATED",
  "SAFETY", "PANIC", "UNSAFE"]);
const c23Start = (c) => c === 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c >= 0x80;
const c23Part = (c) => c23Start(c) || (c >= 48 && c <= 57);
const identEndAt = (b, i) => { while (i < b.length && c23Part(C(b, i))) i++; return i; };

const MARK_WORDS = [...MARKER_KINDS];
export function scanMarkersRaw(src, fn) {
  const starts = new Set();
  for (const w of MARK_WORDS) for (let i = src.indexOf(w); i >= 0; i = src.indexOf(w, i + w.length)) starts.add(src.lastIndexOf("\n", i) + 1);
  if (starts.size === 0) return;
  const order = [...starts].sort((a, b) => a - b);
  let ln = 1, pos = 0;
  for (const ls of order) {
    for (let nl = src.indexOf("\n", pos); nl >= 0 && nl < ls; nl = src.indexOf("\n", nl + 1)) ln++;
    pos = ls;
    const e = src.indexOf("\n", ls);
    scanMarkerLine(src.slice(ls, e < 0 ? src.length : e), ln, fn);
  }
}
function scanMarkerLine(line, n, fn) {
  if (!(line.includes("//") || line.includes("--") || line.includes("#") || line.includes("*"))) return;
  for (let p = 0; p < line.length;) {
    if (!c23Start(C(line, p))) { p++; continue; }
    if (p > 0 && c23Part(C(line, p - 1))) { p = identEndAt(line, p); continue; }
    const e = identEndAt(line, p);
    const w = line.slice(p, e);
    if (MARKER_KINDS.has(w)) {
      let q = e;
      while (q < line.length && (C(line, q) === 32 || C(line, q) === 9)) q++;
      if (q < line.length && (line[q] === ":" || line[q] === "-" || line[q] === "(")) {
        fn(w, n, jtrunc(trimSpace(line), 200));
        return;
      }
    }
    p = e;
  }
}
