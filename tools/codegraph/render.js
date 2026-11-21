// Text rendering: cells, aligned tables, Go-compatible number formatting.
import { runeWidth, runeCount } from "./text.js";
import { Float } from "./dumpfmt.js";

export const MAX_CELL = 72;
const FFFD = "\xef\xbf\xbd";

// %.<d>f with round-half-even on the exact binary value (as Go prints).
export function fixed(x, d = 2) {
  if (!Number.isFinite(x)) return x > 0 ? "+Inf" : x < 0 ? "-Inf" : "NaN";
  const exact = Math.abs(x).toFixed(Math.min(100, d + 30));
  const dot = exact.indexOf(".");
  const tail = exact.slice(dot + 1 + d);
  let r = Math.abs(x).toFixed(d);
  if (/^50*$/.test(tail)) {
    const kept = exact.slice(0, dot + 1 + d);
    const last = kept.charCodeAt(kept.length - 1) - 48;
    r = last % 2 === 0 ? kept : Math.abs(x).toFixed(d);
    if (d === 0) r = r.replace(/\.$/, "");
  }
  return (x < 0 || Object.is(x, -0) ? "-" : "") + r;
}

// The first n runes of s, invalid bytes re-encoded as U+FFFD.
function runePrefix(s, n) {
  let out = "", c = 0;
  for (let i = 0; i < s.length && c < n; c++) {
    const w = runeWidth(s, i);
    out += w === 1 && s.charCodeAt(i) >= 0x80 ? FFFD : s.slice(i, i + w);
    i += w;
  }
  return out;
}

export function cellText(v) {
  if (v === null || v === undefined) return "-";
  if (typeof v === "object" && !(v instanceof Float)) return "i" in v ? String(v.i) : "f" in v ? fixed(v.f, 2) : v.s;
  if (v instanceof Float) return fixed(v.v, 2);
  if (typeof v === "number" || typeof v === "bigint") return String(v);
  if (typeof v === "boolean") return v ? "1" : "0";
  return v;
}

export function cell(v) {
  if (v === null || v === undefined) return "-";
  const t = cellText(v);
  if (t.length <= MAX_CELL || runeCount(t) <= MAX_CELL) return t;
  return runePrefix(t, MAX_CELL - 3) + "...";
}

export function render(out, rows, cols) {
  if (rows.length === 0) { out.push(" (no rows)\n"); return; }
  const nc = cols.length;
  const widths = cols.map(runeCount);
  const nr = rows.length, body = new Array(nr), lens = new Array(nr);
  for (let k = 0; k < nr; k++) {
    const r = rows[k], m = r.length, cs = new Array(m), ls = new Array(m);
    for (let j = 0; j < m; j++) {
      const c = cell(r[j]), n = runeCount(c);
      cs[j] = c; ls[j] = n;
      if (j < nc && n > widths[j]) widths[j] = n;
    }
    body[k] = cs; lens[k] = ls;
  }
  const maxW = Math.max(...widths);
  const spaces = [""];
  for (let i = 1; i <= maxW; i++) spaces.push(spaces[i - 1] + " ");
  const line = (cells, ls) => {
    let s = "";
    for (let i = 0; i < cells.length; i++) {
      s += " " + cells[i];
      const pad = widths[i] - ls[i];
      if (pad > 0) s += spaces[pad];
    }
    out.push(s + "\n");
  };
  line(cols, cols.map(runeCount));
  const dash = widths.map((w) => "-".repeat(w));
  line(dash, widths);
  for (let k = 0; k < nr; k++) line(body[k], lens[k]);
}
