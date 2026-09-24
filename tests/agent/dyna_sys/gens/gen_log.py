#!/usr/bin/env python3
"""gen_log.py — dyna:log probes: golden JSON lines, level filtering, children,
text format, size rollover, Debug matching (via child process for stderr)."""
from probe_lib import emit

IMP = '''import { Logger, Debug } from "dyna:log";
import { Path, readFile, makeDir, removeAll, readDir, stat } from "dyna:file";
import { Exec, pid as getPid, setEnv, getEnv } from "dyna:sys";
'''

emit("log", "json", IMP, r'''
const D = new Path(Path.cwd(), "scratch", "log_json-" + getPid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const dest = new Path(D, "app.log");

const log = new Logger({ level: "trace", name: "app", dest: String(dest), timestamp: false });
log.trace("t"); log.debug("d"); log.info("i"); log.warn("w"); log.error("e"); log.fatal("f");
log.flush();

const lines = readFile(dest).trim().split("\n");
assert_eq(lines.length, 6, "all six levels emitted at level=trace");
const parsed = lines.map((l) => JSON.parse(l));
assert_eq(parsed.map((p) => p.level).join(","), "trace,debug,info,warn,error,fatal", "level order");
assert_eq(parsed[0].msg, "t", "msg field");
assert_eq(parsed[0].name, "app", "name field");

// level filtering: warn threshold drops info and below
const dest2 = new Path(D, "warn.log");
const lg2 = new Logger({ level: "warn", name: "app", dest: String(dest2), timestamp: false });
lg2.info("nope"); lg2.warn("yes"); lg2.error("yes2");
lg2.flush();
const l2 = readFile(dest2).trim().split("\n");
assert_eq(l2.length, 2, "level filter drops below-threshold lines");
assert_eq(JSON.parse(l2[0]).level, "warn", "first surviving level");

// silent drops everything
const dest3 = new Path(D, "silent.log");
const lg3 = new Logger({ level: "silent", name: "a", dest: String(dest3), timestamp: false });
lg3.error("swallowed");
lg3.flush();
assert_eq(readFile(dest3).trim(), "", "silent emits nothing");

// enabled() + level setter
assert_eq(lg2.enabled("error"), true, "enabled(error) at warn");
assert_eq(lg2.enabled("info"), false, "enabled(info) false at warn");
assert_throws(() => lg2.enabled("bogus"), null, "enabled(unknown) throws");
assert_throws(() => { lg2.level = "bogus"; }, null, "level setter rejects unknown");
lg2.level = "debug";
assert_eq(lg2.level, "debug", "level setter + getter");

// timestamp shapes
const destE = new Path(D, "epoch.log");
const le = new Logger({ level: "info", dest: String(destE) });  // default epoch
le.info("x");
le.flush();
const pe = JSON.parse(readFile(destE).trim());
assert_eq(typeof pe.time, "number", "epoch timestamp is a number");
assert_true(pe.time > 1600000000000, "epoch ms plausible");

// base fields + pid + hostname + child fields; caller keys cannot forge frame keys
const destB = new Path(D, "base.log");
const lb = new Logger({ level: "info", dest: String(destB), timestamp: false,
                        base: { env: "dev" }, pid: true, hostname: true });
lb.warn({ msg: "forged", level: "fatal" }, "real message");
const child = lb.child({ route: "/api" }, { level: "error" });
child.error({ id: 7 }, "child msg");
child.info("dropped-by-child-level");
lb.flush();
const bl = readFile(destB).trim().split("\n").map((l) => JSON.parse(l));
assert_eq(bl.length, 2, "child level override filtered the info line");
assert_eq(bl[0].msg, "real message", "caller cannot forge msg");
assert_eq(bl[0].level, "warn", "caller cannot forge level");
assert_eq(bl[0].env, "dev", "base field present");
assert_eq(typeof bl[0].pid, "number", "pid added");
assert_eq(typeof bl[0].hostname, "string", "hostname added");
assert_eq(bl[1].route, "/api", "child field present");

// message shapes: (fields, msg), (err, msg), (err, fields, msg)
const destS = new Path(D, "shapes.log");
const ls = new Logger({ level: "info", dest: String(destS), timestamp: false });
const boom = new Error("boom");
boom.code = "EBOOM";
ls.info({ k: 1 }, "fields then msg");
ls.error(boom, "err then msg");
ls.error(boom, { k: 2 }, "err, fields, msg");
ls.flush();
const sl = readFile(destS).trim().split("\n").map((l) => JSON.parse(l));
assert_eq(sl[0].k, 1, "(fields, msg) shape");
assert_eq(sl[1].err.message, "boom", "(err, msg) serializes {type,message,stack}");
assert_eq(sl[1].err.code, "EBOOM", "extra enumerable props carried");
assert_eq(sl[2].k, 2, "(err, fields, msg) shape");
assert_true(typeof sl[1].err.stack === "string", "stack serialized");

// unknown constructor options refuse rather than silently misbehave
assert_throws(() => new Logger({ timestamp: "bogus" }), null, "unknown timestamp string throws");
assert_throws(() => new Logger({ level: "bogus" }), null, "unknown level throws");
{ const b0 = new Logger({ level: "info", dest: String(new Path(D, "buf0.log")), timestamp: false, buffer: 0 }); b0.info("ok"); }
assert_eq(JSON.parse(readFile(new Path(D, "buf0.log")).trim()).msg, "ok", "buffer:0 is legal (unbuffered)");

removeAll(D);
summary("log.json");
''')

emit("log", "text", IMP, r'''
const D = new Path(Path.cwd(), "scratch", "log_text-" + getPid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const dest = new Path(D, "text.log");
const log = new Logger({ level: "info", name: "api", dest: String(dest),
                         timestamp: "iso", format: "text" });
log.warn({ req: 42 }, "slow request");
log.flush();
const line = readFile(dest).trim();
// documented shape: ISO timestamp, 5-col level, name, message, k=v
assert_true(/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z /.test(line), "iso timestamp prefix: " + JSON.stringify(line.slice(0, 30)));
// the maintained contract (tests/test_log.js) pins the LOWER-CASE 5-col
// level; the API.md example's "WARN" is a stale doc example (ticketed)
assert_true(line.indexOf(" warn  ") >= 0, "5-column level aligned lower-case: " + JSON.stringify(line));
assert_true(line.indexOf("api:") >= 0, "name present");
assert_true(line.indexOf("slow request") >= 0, "message present");
assert_true(line.indexOf("req=42") >= 0, "fields as k=v");
removeAll(D);
summary("log.text");
''')

emit("log", "rollover", IMP, r'''
const D = new Path(Path.cwd(), "scratch", "log_rot-" + getPid() + "-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const dest = new Path(D, "app.log");
// rotate at ~600 bytes; each line is ~60 bytes -> rotation after ~10 lines
const log = new Logger({ level: "info", dest: String(dest), timestamp: false,
                         buffer: true,
                         rollover: { size: 600, count: 12 } });
for (let i = 0; i < 60; i++) log.info("line-" + i + "-" + "x".repeat(40));
log.flush();
const names = readDir(D).map((e) => e.name).sort();
assert_true(names.length >= 4, "rotation produced files: " + JSON.stringify(names));
const rotated = names.filter((n) => n !== "app.log");
assert_true(rotated.every((n) => /\.log$/.test(n) && /\.\d+\./.test(n)), "rotated names carry .N numbering: " + JSON.stringify(rotated));
// retention keeps count=12: nothing was pruned, so every line must survive
let total = 0;
for (const n of names) {
  const content = readFile(new Path(D, n)).trim();
  if (!content) continue;
  for (const l of content.split("\n")) { JSON.parse(l); total++; }
}
assert_eq(total, 60, "all 60 lines intact across rotations, none torn (JSON-parseable)");
// retention actually prunes: count=1 keeps at most active+1
const dest2 = new Path(D, "cap.log");
const lg2 = new Logger({ level: "info", dest: String(dest2), timestamp: false,
                         rollover: { size: 300, count: 1 } });
for (let i = 0; i < 40; i++) lg2.info("row-" + i + "-" + "y".repeat(40));
lg2.flush();
const kept = readDir(D).map((e) => e.name).filter((n) => n.startsWith("cap."));
assert_true(kept.length <= 2, "count=1 retention keeps at most active+1, got " + kept.length);
assert_eq(log.enabled("info"), true, "logger still usable after rollover");
log.flush();
removeAll(D);
summary("log.rollover");
''')

emit("log", "debug", IMP, r'''
// Debug writes to STDERR of THIS process, so the oracle runs a child dynajs
// and inspects its captured stderr. The child script is generated next to us.
const engine = getEnv("DYN_DYNAJS") || "../../../dynajs";
const child = new Path(Path.cwd(), "probes", "log", "debug_child.js");

function runWithDebug(debugVal) {
  const r = Exec(engine, [String(child)], { env: debugVal === null ? {} : { DEBUG: debugVal } });
  // under sanitizer builds the runtime may prepend a symbolizer warning to
  // stderr; strip pid-prefixed runtime chatter so the oracle stays clean
  const lines = r.stderr.split("\n").filter((l) => l && l.indexOf("==") !== 0);
  r.stderr = lines.join("\n");
  return r;
}

let r;
r = runWithDebug(null);
assert_eq(r.stderr.trim(), "", "no DEBUG: silent");
r = runWithDebug("svc:parse");
assert_eq(r.stderr.trim(), "svc:parse tokens: 42", "exact namespace enabled: got " + JSON.stringify(r.stderr));
r = runWithDebug("*");
assert_true(r.stderr.indexOf("svc:parse tokens: 42") >= 0, "star enables");
r = runWithDebug("other:*");
assert_eq(r.stderr.trim(), "", "unmatched namespace silent");
r = runWithDebug("svc:*");
assert_true(r.stderr.indexOf("tokens") >= 0, "wildcard suffix matches");
r = runWithDebug("*,-svc:parse");
assert_eq(r.stderr.trim(), "", "negation after star wins (last match wins)");
r = runWithDebug("-svc:parse,svc:*");
assert_true(r.stderr.indexOf("tokens") >= 0, "later positive re-enables (last match wins)");
summary("log.debug");
''')

# the child is emitted as its own file (no harness; plain prints to stderr)
import os
child = '''import { Debug } from "dyna:log";
const dbg = Debug("svc:parse");
dbg("tokens:", 42);
'''
os.makedirs(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "probes", "log"), exist_ok=True)
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "probes", "log", "debug_child.js"), "w") as f:
    f.write(child)

print("gen_log: 4 probes + child")
