// Machine output: encoding/csv with UseCRLF, and the indented JSON array.
import { runeAt, runeWidth, isUniSpace } from "./text.js";

// Go strconv.FormatFloat(f, 'g', -1, 64)
export function goG(f) {
  if (Number.isNaN(f)) return "NaN";
  if (!Number.isFinite(f)) return f > 0 ? "+Inf" : "-Inf";
  if (f === 0) return Object.is(f, -0) ? "-0" : "0";
  const e = f.toExponential();
  const [mant, ex] = e.split("e");
  const exp = Number(ex);
  if (exp < -4 || exp >= 6) {
    const a = Math.abs(exp);
    return mant + "e" + (exp < 0 ? "-" : "+") + (a < 10 ? "0" + a : String(a));
  }
  return String(f);
}

export const fieldString = (v) => (v === null || v === undefined ? "" : "i" in v ? String(v.i) : "f" in v ? goG(v.f) : v.s);

function csvField(s) {
  let need = false;
  if (s === "") need = false;
  else if (s === "\\.") need = true;
  else if (/[\n\r",]/.test(s)) need = true;
  else need = isUniSpace(runeAt(s, 0));
  if (!need) return s;
  let o = '"';
  for (const ch of s) {
    if (ch === '"') o += '""';
    else if (ch === "\r") continue;
    else if (ch === "\n") o += "\r\n";
    else o += ch;
  }
  return o + '"';
}

export function csv(out, cols, rows) {
  out.push(cols.map(csvField).join(",") + "\r\n");
  for (const r of rows) out.push(r.map((v) => csvField(fieldString(v))).join(",") + "\r\n");
}

function jsonQuote(s) {
  let o = '"';
  for (let i = 0; i < s.length;) {
    const r = runeAt(s, i);
    i += runeWidth(s, i);
    if (r === 34) o += '\\"';
    else if (r === 92) o += "\\\\";
    else if (r === 8) o += "\\b";
    else if (r === 12) o += "\\f";
    else if (r === 10) o += "\\n";
    else if (r === 13) o += "\\r";
    else if (r === 9) o += "\\t";
    else if (r < 0x20) o += "\\u" + r.toString(16).padStart(4, "0");
    else if (r === 60 || r === 62 || r === 38) o += "\\u00" + r.toString(16);
    else if (r < 0x80) o += String.fromCharCode(r);
    else if (r <= 0xffff) o += "\\u" + r.toString(16).padStart(4, "0");
    else { const x = r - 0x10000; o += "\\u" + (0xd800 + (x >> 10)).toString(16) + "\\u" + (0xdc00 + (x & 0x3ff)).toString(16); }
  }
  return o + '"';
}

const jsonVal = (v) => (v === null || v === undefined ? "null" : "i" in v ? String(v.i) : "f" in v ? JSON.stringify(v.f) : jsonQuote(v.s));

export function json(out, cols, rows) {
  if (!rows.length) { out.push("[]\n"); return; }
  const objs = rows.map((r) => "  {\n" + cols.map((c, i) => "    " + JSON.stringify(c) + ": " + jsonVal(r[i])).join(",\n") + "\n  }");
  out.push("[\n" + objs.join(",\n") + "\n]\n");
}
