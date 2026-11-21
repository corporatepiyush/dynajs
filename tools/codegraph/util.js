// Small helpers shared by the query layer.

// SQL LIKE over bytes: % any run, _ any one byte.
export function likeMatch(s, pat) {
  if (pat === "%") return true;
  let si = 0, pi = 0, star = -1, mark = 0;
  while (si < s.length) {
    if (pi < pat.length && (pat[pi] === "_" || pat[pi] === s[si])) { si++; pi++; }
    else if (pi < pat.length && pat[pi] === "%") { star = pi; pi++; mark = si; }
    else if (star >= 0) { pi = star + 1; mark++; si = mark; }
    else return false;
  }
  while (pi < pat.length && pat[pi] === "%") pi++;
  return pi === pat.length;
}

// Go strconv.Quote for the ASCII and Latin-1 byte strings the CLI sees.
export function goQuote(s) {
  let o = '"';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c === 34) o += '\\"';
    else if (c === 92) o += "\\\\";
    else if (c === 10) o += "\\n";
    else if (c === 13) o += "\\r";
    else if (c === 9) o += "\\t";
    else if (c === 7) o += "\\a";
    else if (c === 8) o += "\\b";
    else if (c === 12) o += "\\f";
    else if (c === 11) o += "\\v";
    else if (c < 32 || c === 127) o += "\\x" + c.toString(16).padStart(2, "0");
    else o += s[i];
  }
  return o + '"';
}
