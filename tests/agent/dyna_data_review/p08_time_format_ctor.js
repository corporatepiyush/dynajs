import { Format } from "dyna:time";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";

// non-string layouts must throw TypeError, not coerce silently
TH(function () { new Format(12345); }, "TypeError", "number layout throws");
TH(function () { new Format(0); }, "TypeError", "zero layout throws");
TH(function () { new Format(null); }, "TypeError", "null layout throws");
TH(function () { new Format(undefined); }, "TypeError", "undefined layout throws");
TH(function () { new Format({}); }, "TypeError", "object layout throws");
TH(function () { new Format(["2006"]); }, "TypeError", "array layout throws");
TH(function () { new Format(true); }, "TypeError", "boolean layout throws");
TH(function () { new Format(10n); }, "TypeError", "BigInt layout throws");
TH(function () { new Format(); }, "TypeError", "missing layout still throws");
// string layouts (Go reference style) still work: 2006-01-02 = Y-m-d
const f = new Format("2006-01-02");
EQ(f.format(Date.UTC(2026, 0, 2) / 1000), "2026-01-02", "valid layout formats");
const g = new Format("");
OK(typeof g.format(0) === "string", "empty-string layout accepted (is a string)");
DONE("p08_time_format_ctor");
