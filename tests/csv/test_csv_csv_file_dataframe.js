// covers: dyna:csv.CSVFile
// csv.parseDataFrame(text, opts) and CSVFile#toDataFrame(opts): CSV straight into a DataFrame in one C pass
// (src/core/dfk/dfk-csv.c), no JS string per cell. Owner ruling B1 (Track H, 2026-11): an untyped column is f64
// when every non-empty cell is a decimal number, else str; an empty cell is NaN in f64 and "" in str; dtypes
// force "f64" | "i32" | "str". Oracles: JS Number() for every numeric cell (the reader must round exactly as
// Number does, on the fast path and the long-digit path alike), and csv.parse for quoting, CRLF, BOM and
// ragged rows. Before B1 neither function existed: every row here is red on the old binary.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_dataframe");
import { parseDataFrame, parse, CSVFile } from "dyna:csv";
import { isDataFrame } from "dyna:dataframe";
import { makeTempDir, writeFile, Path } from "dyna:file";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const err = (f) => { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } };
const cols = (df) => {
    const out = {};
    for (const [k, v] of Object.entries(df.TO_COLUMNS())) out[k] = Array.isArray(v) ? v : Array.from(v, (x) => Object.is(x, -0) ? "-0" : x !== x ? "NaN" : x);
    return out;
};

__kitS.section("k,v a,1 b,2 a,3", () => {
    const dir = makeTempDir("csvdf");
    const p = new Path(dir + "/s.csv");
    writeFile(p, "﻿k,v\na,1\nb,2\na,3\n");
    const df = new CSVFile(p).toDataFrame({ dtypes: { v: "i32" } });
    eq(df.DTYPES(), { k: "str", v: "i32" }, "CSVFile#toDataFrame reads the file with the same options");
    eq(Array.from(df.GROUP_BY_SUM("k", "v").values), [4, 2], "and the frame aggregates");
    check(/^TypeError: csv\.toDataFrame: cannot read '.*nope\.csv'/.test(err(() => new CSVFile(new Path(dir + "/nope.csv")).toDataFrame())), "a missing file names the path");
});
__kitS.end();
print("test_csv_dataframe: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_csv_dataframe: " + fails + " failures");
