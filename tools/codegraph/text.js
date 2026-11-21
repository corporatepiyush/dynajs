// Byte-string helpers with UTF-8 rune semantics: a valid sequence is one
// rune, every byte of an invalid sequence is a rune of its own.

// Length in bytes of the UTF-8 rune at i (1 for an invalid byte).
export function runeWidth(s, i) {
  const c = s.charCodeAt(i);
  if (c < 0x80) return 1;
  const c1 = s.charCodeAt(i + 1);
  if (c >= 0xc2 && c <= 0xdf) return c1 >= 0x80 && c1 <= 0xbf ? 2 : 1;
  const c2 = s.charCodeAt(i + 2);
  if (c >= 0xe0 && c <= 0xef) {
    const lo = c === 0xe0 ? 0xa0 : 0x80, hi = c === 0xed ? 0x9f : 0xbf;
    return c1 >= lo && c1 <= hi && c2 >= 0x80 && c2 <= 0xbf ? 3 : 1;
  }
  if (c >= 0xf0 && c <= 0xf4) {
    const lo = c === 0xf0 ? 0x90 : 0x80, hi = c === 0xf4 ? 0x8f : 0xbf;
    const c3 = s.charCodeAt(i + 3);
    return c1 >= lo && c1 <= hi && c2 >= 0x80 && c2 <= 0xbf && c3 >= 0x80 && c3 <= 0xbf ? 4 : 1;
  }
  return 1;
}

const ASCII_RE = /^[\x00-\x7f]*$/;

export function runeCount(s) {
  if (ASCII_RE.test(s)) return s.length;
  let n = 0;
  for (let i = 0; i < s.length; i += runeWidth(s, i)) n++;
  return n;
}

// The first n runes of s.
export function jtrunc(s, n) {
  if (s.length <= n) return s;
  let c = 0;
  for (let i = 0; i < s.length; i += runeWidth(s, i)) {
    if (c === n) return s.slice(0, i);
    c++;
  }
  return s;
}

// Decoded code point at i (0xFFFD for an invalid byte).
export function runeAt(s, i) {
  const w = runeWidth(s, i), c = s.charCodeAt(i);
  if (w === 1) return c < 0x80 ? c : 0xfffd;
  if (w === 2) return ((c & 0x1f) << 6) | (s.charCodeAt(i + 1) & 0x3f);
  if (w === 3) return ((c & 0x0f) << 12) | ((s.charCodeAt(i + 1) & 0x3f) << 6) | (s.charCodeAt(i + 2) & 0x3f);
  return ((c & 0x07) << 18) | ((s.charCodeAt(i + 1) & 0x3f) << 12) | ((s.charCodeAt(i + 2) & 0x3f) << 6) | (s.charCodeAt(i + 3) & 0x3f);
}

export const isUniSpace = (r) => (r >= 9 && r <= 13) || r === 32 || r === 0x85 || r === 0xa0 || r === 0x1680 ||
  (r >= 0x2000 && r <= 0x200a) || r === 0x2028 || r === 0x2029 || r === 0x202f || r === 0x205f || r === 0x3000;

// Unicode-aware whitespace trim (both ends).
export function trimSpace(s) {
  let a = 0;
  while (a < s.length) {
    const w = runeWidth(s, a);
    if (!isUniSpace(runeAt(s, a))) break;
    a += w;
  }
  let b = s.length;
  while (b > a) {
    let st = b - 1;
    while (st > a && b - st < 4 && (s.charCodeAt(st) & 0xc0) === 0x80) st--;
    if (runeWidth(s, st) !== b - st) st = b - 1;
    if (!isUniSpace(runeAt(s, st))) break;
    b = st;
  }
  return s.slice(a, b);
}

// Trim any of the characters in cut from both ends (bytes; cut is ASCII).
export function trimChars(s, cut) {
  if (!cut) return s;
  let a = 0, b = s.length;
  while (a < b && cut.includes(s[a])) a++;
  while (b > a && cut.includes(s[b - 1])) b--;
  return s.slice(a, b);
}

const WS_RUN = /[ \t\n\r\v\f\x1c\x1d\x1e]+/g;
// Trim cut, then collapse each whitespace run to one space (ends included).
export function squeeze(s, cut = "") {
  return trimChars(s, cut).replace(WS_RUN, " ");
}

export const isStripSpace = (c) => c === 32 || (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x1f) || c === 0x85 || c === 0xa0;
export function trimStrip(s) {
  let a = 0, b = s.length;
  while (a < b && isStripSpace(s.charCodeAt(a))) a++;
  while (b > a && isStripSpace(s.charCodeAt(b - 1))) b--;
  return s.slice(a, b);
}

export const containsAny = (s, subs) => subs.some((x) => s.includes(x));
