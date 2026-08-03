// run_all.js — spawn `dynajs` on every example in this directory and aggregate the
// results into one overall PASS/FAIL. dynajs runs a single file at a time, so this
// runner shells out with os.exec (block:true inherits stdio and returns the
// child's exit code).
//
// Usage:
//   dynajs examples/js/run_all.js                 # uses $DYNAJS, else "dynajs" on PATH
//   DYNAJS=/path/to/dynajs dynajs examples/js/run_all.js
//   dynajs examples/js/run_all.js /path/to/dynajs    # explicit interpreter as arg
//
// NB: the async_streams example needs Array.fromAsync (a dynascript addition);
// point DYNAJS at the dynascript dynajs to run the whole suite.

import * as std from "std";
import * as os from "os";

// The example suite, in a sensible reading order. harness.js and this file are
// intentionally excluded.
const EXAMPLES = [
  "json5_parser.js",
  "mini_vm.js",
  "parser_combinators.js",
  "csv_engine.js",
  "regex_router.js",
  "state_machine.js",
  "persistent_list.js",
  "lru_cache.js",
  "event_emitter.js",
  "bignum_crypto.js",
  "reactive_signals.js",
  "bytebuffer_codec.js",
  "promise_scheduler.js",
  "async_streams.js",
];

/** Directory containing this script, derived from how it was invoked. */
function scriptDir() {
  const self = scriptArgs[0] ?? "examples/js/run_all.js";
  const slash = self.lastIndexOf("/");
  return slash < 0 ? "." : self.slice(0, slash);
}

/** Which dynajs to spawn: explicit arg, then $DYNAJS, then the freshly-built
 *  ./dynajs at the repo root (some examples use dynascript-only features such
 *  as Array.fromAsync), then "dynajs" on PATH. */
function resolveDynajs() {
  if (scriptArgs[1]) return scriptArgs[1];
  const env = std.getenv("DYNAJS");
  if (env) return env;
  const [f, err] = [std.open("./dynajs", "r"), 0];
  if (f) { f.close(); return "./dynajs"; }
  return "dynajs";
}

/** print + flush, so our lines interleave correctly with child stdio. */
function log(line = "") {
  print(line);
  std.out.flush();
}

function main() {
  const dir = scriptDir();
  const dynajs = resolveDynajs();

  log(`running ${EXAMPLES.length} examples with: ${dynajs}\n`);

  const results = [];
  for (const file of EXAMPLES) {
    const path = `${dir}/${file}`;
    log(`──────── ${file} ────────`);
    const started = Date.now();
    const code = os.exec([dynajs, path], { block: true });
    const ms = Date.now() - started;
    results.push({ file, code, ms });
    log("");
  }

  // Summary table.
  log("════════ summary ════════");
  let failed = 0;
  for (const { file, code, ms } of results) {
    const status = code === 0 ? "PASS" : `FAIL (exit ${code})`;
    if (code !== 0) failed++;
    log(`  ${status.padEnd(16)} ${file.padEnd(24)} ${ms}ms`);
  }

  const passed = results.length - failed;
  log(`\n  ${passed}/${results.length} suites passed`);

  const ok = failed === 0;
  log(ok ? "\nALL PASS" : "\nSOME FAILED");
  std.exit(ok ? 0 : 1);
}

main();
