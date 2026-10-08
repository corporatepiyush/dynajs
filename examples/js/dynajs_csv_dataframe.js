// A tabular pipeline: parse CSV, lift it into a columnar DataFrame, run the
// group-by / mask / spread-injection-guard verbs, and serialize back to CSV.
// Everything comes from dyna:csv + dyna:dataframe + engine globals (no --std).
import { parse as csvParse, stringify as csvStringify } from "dyna:csv";
import { DataFrame, isDataFrame } from "dyna:dataframe";

let failures = 0;
function check(cond, what) {
    if (cond) { print("  ok  " + what); return; }
    failures++;
    print("  FAIL " + what);
}

// ------------------------------------------------------------------
// 1. CSV in memory: parse shapes rows to the first row's width, and
//    stringify([headers, ...rows]) round-trips parse by design.
// ------------------------------------------------------------------
const text = [
    "sensor,temp,ok",
    "a,20,true",
    "b,21.5,true",
    "a,19.5,true",
    "b,22,true",
    "a,18,true",
    "b,25.5,true",
].join("\n");

const t = csvParse(text);
check(t.headers.join(",") === "sensor,temp,ok", "headers parse in order");
check(t.totalRows === 6, "six data rows");

const roundTripped = csvParse(csvStringify([t.headers, ...t.rows]));
check(roundTripped.headers.join("|") === t.headers.join("|"), "stringify([headers, ...rows]) round-trips the header");
check(roundTripped.rows.every((r, i) => r.join("|") === t.rows[i].join("|")),
      "and every data row survives the round trip");

// ------------------------------------------------------------------
// 2. Columnar lift: TypedArray columns are ALIASED (zero-copy), string
//    columns are dictionary-encoded.
// ------------------------------------------------------------------
const temps = new Float64Array([20.0, 21.5, 19.5, 22.0, 18.0, 25.5]);
const sensors = ["a", "b", "a", "b", "a", "b"];
const df = new DataFrame({ temp: temps, sensor: sensors });

check(isDataFrame(df), "isDataFrame brands engine-built frames");
check(df.ROWS === 6 && df.COLS === 2, "shape is 6x2");
check(df.COLUMNS.join(",") === "temp,sensor", "column order follows the record");
check(df.DTYPES().temp === "f64" && df.DTYPES().sensor === "str", "dtypes: f64 for Float64Array, str for string[]");

temps[5] = 30.0;   // documented ZERO-COPY: mutating the source mutates the frame
check(df.TO_COLUMNS().temp[5] === 30.0, "the TypedArray column aliases the source array");
temps[5] = 25.5;   // restore

// ------------------------------------------------------------------
// 3. Masks and group-bys, with hand-computed expectations.
// ------------------------------------------------------------------
const hot = df.GT("temp", 20);                       // strict >
check(Array.from(hot).join("") === "010101", "GT(20) masks exactly rows 1,3,5");
check(df.FILTER(hot).ROWS === 3, "FILTER keeps exactly the masked rows");

const means = df.GROUP_BY_MEAN("sensor", "temp");
const byKey = new Map(means.keys.map((k, i) => [k, means.values[i]]));
check(byKey.get("b") === 23, "mean of [21.5, 22, 25.5] is exactly 23");
check(Math.abs(byKey.get("a") - 57.5 / 3) < 1e-12, "mean of [20, 19.5, 18] is 57.5/3");

const inWindow = df.BETWEEN("temp", 19.5, 22);        // inclusive both ends
check(Array.from(inWindow).join("") === "111100", "BETWEEN is inclusive at lo and hi");

// ------------------------------------------------------------------
// 4. Spread-sheet injection guard: TO_CSV({escapeFormulas}) prefixes '
//    to cells starting with =/+/-/@ (CSV injection defense).
// ------------------------------------------------------------------
const notes = new DataFrame({
    id: new Int32Array([1, 2, 3, 4]),
    label: ["ok", "=cmd|' /C calc", "+SUM(A1)", "@x"],
});
const raw = notes.TO_CSV();
const guarded = notes.TO_CSV({ escapeFormulas: true });
check(raw.includes("=cmd"), "raw CSV keeps the formula cell verbatim");
check(guarded.includes("'=cmd") && guarded.includes("'+SUM(A1)") && guarded.includes("'@x"),
      "escapeFormulas quotes every =/+/-/@ cell");
check(!raw.includes("'=cmd"), "and the default stays honest");

// ------------------------------------------------------------------
// 5. Back to wire: TO_CSV is RFC 4180, so csv.parse() reads it back.
// ------------------------------------------------------------------
const outCsv = df.TO_CSV();
const back = csvParse(outCsv);
check(back.headers.join(",") === "sensor,temp" || back.headers.join(",") === "temp,sensor",
      "TO_CSV emits a header line csv.parse understands");
check(back.rows.length === 6, "all six rows serialize");
const tempsBack = (back.headers.indexOf("temp") + 1)
    ? back.rows.map((r) => Number(r[back.headers.indexOf("temp")]))
    : [];
check(tempsBack.every((v, i) => v === [20, 21.5, 19.5, 22, 18, 25.5][i]),
      "temperature values round-trip numerically exact");

print(failures === 0
    ? "dynajs_csv_dataframe: all checks passed"
    : "dynajs_csv_dataframe: " + failures + " check(s) FAILED");
if (failures) throw new Error(failures + " check(s) failed");
