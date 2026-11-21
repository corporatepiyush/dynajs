// flags: --std
// select_driver.js -- select() navigation/cancel/refusal probes (stdin is a pipe).
import { run as drive, bytes, check, done } from "./drive.js";

const run = (mode, payload, t) => drive("probe_select.js", mode, bytes(payload), (l) => l.startsWith("A=") || l.startsWith("<TIMEOUT>"), t);
const J = JSON.stringify;
const is = (got, want) => J(got) === J(want);
const one = (got, pre) => got.length === 1 && got[0].startsWith(pre);

let got = run("three", "\x1b[B\x1b[B\r");
check("S1", "down x2 + enter picks 'three'", is(got, ['A="three"']), J(got));
got = run("three", "\x1b[A\r");
check("S2", "up from the top wraps to the last option", is(got, ['A="three"']), J(got));
got = run("three", "\x1b");
check("S3", "bare Escape cancels -> null", is(got, ["A=null"]), J(got));
got = run("three", "\x03");
check("S4", "Ctrl-C cancels -> null", is(got, ["A=null"]), J(got));
got = run("three", "");
check("S5", "EOF cancels -> null", is(got, ["A=null"]), J(got));
got = run("three", "xy\x1b[Zq\r");
check("S6", "unknown keys are ignored (incl shift+tab), enter picks top", is(got, ['A="one"']), J(got));
got = run("three", "\x1b\r");
check("S7", "ESC-enter (meta+enter) is ignored, not a selection", is(got, ["A=null"]), J(got));
got = run("nonstring-num", "\r");
check("S8", "option 42 refused (TypeError naming the index), not stringified", one(got, "A=TypeError") && got[0].includes("option 1"), J(got));
got = run("nonstring-null", "\r");
check("S9", "option null refused (TypeError)", one(got, "A=TypeError"), J(got));
got = run("nonstring-obj", "\r");
check("S10", "option {} refused (TypeError)", one(got, "A=TypeError"), J(got));
got = run("empty", "\r");
check("S11", "empty options refused", one(got, "A=TypeError"), J(got));
got = run("toobig", "\r");
check("S12", "65537 options -> RangeError", one(got, "A=RangeError"), J(got));
got = run("max", "\r", 60000);
check("S13", "65536 options at the documented max works", is(got, ['A="q"']), J(got));
got = run("getter-snapshot", "\x1b[B\r");
check("S14", "menu answers from the snapshot, not a live getter", is(got, ['A="second" reads=1']), J(got));
done("select_driver");
