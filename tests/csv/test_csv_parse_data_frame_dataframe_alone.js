// covers: dyna:csv.parseDataFrame
// csv.parseDataFrame in a program that never imports dyna:dataframe: the frame must still carry the DataFrame
// prototype. dyna:dataframe registers its class when the module is first imported, so the CSV reader makes sure
// the class and its prototype exist in this context before it builds a frame (dyn_df_ensure_class); without
// that, the result was an object with no verbs ("not a function") that also leaked at teardown.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_parse_data_frame_dataframe_alone");
import { parseDataFrame } from "dyna:csv";

await __kitS.section("block L8", async () => {
let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
let df = null, msg = "";
try { df = parseDataFrame("k,v\na,1\nb,2\na,3\n"); } catch (e) { msg = String(e); }
check(df !== null, "parseDataFrame works without importing dyna:dataframe (" + msg + ")");
check(df && typeof df.SUM === "function" && df.SUM("v") === 6, "and the frame has its verbs");
check(df && df.constructor && df.constructor.name === "DataFrame", "and its constructor is DataFrame");
const { DataFrame, isDataFrame } = await import("dyna:dataframe");
check(isDataFrame(df) && df instanceof DataFrame, "importing dyna:dataframe afterwards exports the same class");
check(new DataFrame({ x: new Float64Array(3) }).ROWS === 3, "and its constructor still works");
print("test_csv_dataframe_alone: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_csv_dataframe_alone: " + fails + " failures");
});
__kitS.end();
