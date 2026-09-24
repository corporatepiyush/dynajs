/* test_csv_formula_injection.js -- T2: TO_CSV escapeFormulas (plan S4) */
import { DataFrame } from "dyna:dataframe";
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };
const df = new DataFrame({ a: ["=1+1", "@cmd", "+SUM(X)", "-2+3", "\tTabStart", "plain"] , b: [1,2,3,4,5,6] });
const raw = df.TO_CSV();
ok(raw.includes("=1+1") && raw.includes("@cmd"), "default: formulas pass through (byte-compat)");
const esc = df.TO_CSV({ escapeFormulas: true });
const lines = esc.split("\n");
ok(lines[1].startsWith("'=1+1"), "escaped = starts with '");
ok(lines[2].startsWith("'@cmd"), "escaped @ starts with '");
ok(lines[3].startsWith("'+SUM"), "escaped + starts with '");
ok(lines[4].startsWith("'-2+3"), "escaped - starts with '");
ok(lines[5].startsWith("'\tTabStart") || lines[5].startsWith("'\t"), "escaped tab");
ok(lines[6] === "plain,6", "plain untouched");
ok(esc.includes("plain"), "non-formula intact");
print("test_csv_formula_injection: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_csv_formula_injection: " + fail + " failures");
