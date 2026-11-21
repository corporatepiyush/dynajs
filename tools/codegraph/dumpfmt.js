// The --dump text format: typed fields, rows sorted bytewise per table.
export const NULL = null;
export class Float { constructor(v) { this.v = v; } }

const HEXD = "0123456789ABCDEF";
const NEEDS_ESC = /[\x00-\x1f\\\x7f]/;
export function field(v) {
  if (v === null || v === undefined) return "\\N";
  if (typeof v === "number") return Number.isInteger(v) ? "i:" + v : "f:" + v;
  if (typeof v === "bigint") return "i:" + v;
  if (v instanceof Float) return "f:" + v.v;
  if (typeof v === "boolean") return v ? "i:1" : "i:0";
  if (!NEEDS_ESC.test(v)) return "s:" + v;
  let out = "s:";
  let last = 0;
  for (let i = 0; i < v.length; i++) {
    const c = v.charCodeAt(i);
    let rep;
    if (c === 92) rep = "\\\\";
    else if (c === 10) rep = "\\n";
    else if (c === 9) rep = "\\t";
    else if (c === 13) rep = "\\r";
    else if (c < 32 || c === 127) rep = "\\x" + HEXD[c >> 4] + HEXD[c & 15];
    else continue;
    out += v.slice(last, i) + rep;
    last = i + 1;
  }
  return out + v.slice(last);
}

export function row(fields) {
  return fields.length ? "R " + fields.map(field).join(" ") : "R ";
}

export function table(name, ncols, rows) {
  const lines = rows.map(row);
  lines.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  return `T ${name} ${ncols} ${lines.length}\n` + (lines.length ? lines.join("\n") + "\n" : "") + `E ${name}\n`;
}
