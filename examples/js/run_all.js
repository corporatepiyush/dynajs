// run_all.js — spawn `dynajs` on every example in this directory and aggregate
// the results into one overall PASS/FAIL.
//
// Usage:
//   dynajs examples/js/run_all.js                 # uses $DYNAJS, else ./dynajs, else PATH
//   DYNAJS=/path/to/dynajs dynajs examples/js/run_all.js
//   dynajs examples/js/run_all.js /path/to/dynajs    # explicit interpreter as arg
//
// TWO THINGS THIS RUNNER GETS RIGHT THAT THE PREVIOUS ONE DID NOT:
//
//   1. IT RUNS EVERY EXAMPLE. The list used to name 14 files while the
//      directory held 26 — the twelve dyna:* examples were in no runner at all,
//      so nothing executed them and nothing said so. A hand-maintained list of
//      what a harness covers goes stale silently, and its own comment is what
//      stops anyone re-checking. This one DISCOVERS the directory and fails if
//      a file it cannot classify shows up, so adding an example cannot quietly
//      skip it.
//   2. EVERY CHILD HAS TWO TIME BOUNDS, and a timeout is reported apart from a
//      failure. `ulimit -t` bounds CPU seconds and catches a spin; a process
//      blocked on a socket or a read burns no CPU at all, so only the wall
//      clock catches that one. "It never finished" and "it returned the wrong
//      answer" lead to different investigations, so they get different labels.

import * as std from "std";
import * as os from "os";

/** Pure-ECMAScript examples: these run on any build. */
const PORTABLE = [
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

/** Examples that import a dyna:* native module. They need a binary built with
 *  CONFIG_NATIVE_MODULES=y; against a default build they are SKIPPED with a
 *  reason, never counted as passing. */
const NATIVE = [
  "dynajs_bytes.js",
  "dynajs_compress.js",
  "dynajs_dictionary.js",
  "dynajs_dataframe.js",
  "dynajs_mathx.js",
  "dynajs_ml.js",
  "dynajs_ml_pipeline.js",
  "dynajs_path.js",
  "dynajs_random.js",
  "dynajs_structures.js",
  "dynajs_http.js",   /* self-contained: starts its own HTTPServer */
];

/** Examples that additionally need a live server on localhost. */
const NEEDS_SERVER = {
  "dynajs_postgres.js": "no PostgreSQL on 127.0.0.1:5432",
  "dynajs_redis.js": "no Redis on 127.0.0.1:6379",
};

const CPU_SEC = 60;      // bounds a spin
const WALL_MS = 120000;  // bounds a block, which burns no CPU

/** Directory containing this script, derived from how it was invoked. */
function scriptDir() {
  const self = scriptArgs[0] ?? "examples/js/run_all.js";
  const slash = self.lastIndexOf("/");
  return slash < 0 ? "." : self.slice(0, slash);
}

function resolveDynajs() {
  if (scriptArgs[1]) return scriptArgs[1];
  const env = std.getenv("DYNAJS");
  if (env) return env;
  const f = std.open("./dynajs", "r");
  if (f) { f.close(); return "./dynajs"; }
  return "dynajs";
}

function log(line = "") {
  print(line);
  std.out.flush();
}

/** True when this binary has the dyna:* native modules compiled in.
 *
 *  The STATIC import has to come first: the engine decides module-vs-script by
 *  looking for one, so a probe that opened with `import("dyna:bytes")` was
 *  parsed as a script and died on a SyntaxError -- which this function then
 *  read as "no native modules" and every dyna:* example was silently skipped.
 *  A broken probe reporting absence is the exact failure the skips exist to
 *  prevent, so it is verified below by a second probe that must FAIL. */
function probe(dynajs, src) {
  const path = "/tmp/dynajs_runall_probe.js";
  const f = std.open(path, "w");
  if (!f) return 127;
  f.puts(src);
  f.close();
  return os.exec([dynajs, path], { block: true });
}

function hasNativeModules(dynajs) {
  const present = probe(dynajs, 'import * as m from "dyna:bytes";\nimport * as std from "std";\nstd.exit(0);\n') === 0;
  // A control that must NOT succeed. If this "passes", the probe is answering
  // yes to everything and its verdict is worthless either way.
  const bogus = probe(dynajs, 'import * as m from "dyna:definitely_not_a_module";\nimport * as std from "std";\nstd.exit(0);\n') === 0;
  if (bogus) {
    log("  ! the native-module probe succeeds on a module that cannot exist;" +
        " treating its verdict as unreliable");
    return false;
  }
  return present;
}

/**
 * Spawn under a CPU rlimit, poll the wall clock, and report which bound hit.
 * @returns {{code:number, ms:number, timeout:""|"cpu"|"wall"}}
 */
function runBounded(dynajs, path) {
  const started = Date.now();
  const pid = os.exec(
    ["/bin/sh", "-c", `ulimit -t ${CPU_SEC}; exec "$0" "$1"`, dynajs, path],
    { block: false },
  );
  if (pid < 0) return { code: 127, ms: 0, timeout: "" };

  let status = 0, reaped = false, killed = false;
  for (;;) {
    const [r, st] = os.waitpid(pid, os.WNOHANG);
    if (r === pid) { status = st; reaped = true; break; }
    if (!killed && Date.now() - started > WALL_MS) {
      os.kill(pid, 9);           // SIGKILL: SIGTERM is catchable, this is not
      killed = true;
    }
    os.sleep(20);
  }
  const ms = Date.now() - started;
  if (!reaped) return { code: -1, ms, timeout: "wall" };

  // POSIX wait status: the low 7 bits are the signal, 0 means a normal exit.
  const sig = status & 0x7f;
  if (sig === 0) return { code: (status >> 8) & 0xff, ms, timeout: "" };
  if (killed) return { code: -sig, ms, timeout: "wall" };
  if (sig === 24) return { code: -sig, ms, timeout: "cpu" };  // SIGXCPU
  return { code: -sig, ms, timeout: "" };
}

function main() {
  const dir = scriptDir();
  const dynajs = resolveDynajs();
  const native = hasNativeModules(dynajs);

  const plan = [];
  for (const f of PORTABLE) plan.push({ file: f, skip: null });
  for (const f of NATIVE) {
    plan.push({
      file: f,
      skip: native ? null : "this binary has no dyna:* modules (build with CONFIG_NATIVE_MODULES=y)",
    });
  }
  for (const [f, why] of Object.entries(NEEDS_SERVER)) {
    plan.push({ file: f, skip: native ? why : "no dyna:* modules, and " + why });
  }

  // The lists above are hand-maintained, and a hand-maintained list of what a
  // harness covers goes stale the moment somebody adds a file -- silently,
  // because nothing compares it to the directory. So compare it to the
  // directory: an unclassified example is a FAILURE, not a quiet omission.
  // This is how the twelve dyna:* examples came to be in no runner at all.
  const [entries, rderr] = os.readdir(dir);
  if (rderr === 0) {
    const known = new Set(plan.map((p) => p.file));
    known.add("harness.js");
    known.add("run_all.js");
    const stray = entries
      .filter((e) => e.endsWith(".js") && !known.has(e))
      .sort();
    if (stray.length) {
      log("════════ unclassified examples ════════");
      for (const s of stray) log(`  ✗ ${s} is in no list in run_all.js, so nothing runs it`);
      log(`\n  ${stray.length} example(s) uncovered`);
      log("\nSOME FAILED");
      std.exit(1);
    }
  } else {
    log(`  ! could not read ${dir} (err ${rderr}); the coverage cross-check did not run`);
  }

  log(`running ${plan.length} examples with: ${dynajs}`);
  log(`native modules: ${native ? "yes" : "NO — the dyna:* examples will be skipped"}`);
  log(`bounds: ${CPU_SEC}s CPU, ${WALL_MS / 1000}s wall, per example\n`);

  const results = [];
  for (const { file, skip } of plan) {
    if (skip) {
      log(`──────── ${file} — SKIP: ${skip}`);
      results.push({ file, skip, code: 0, ms: 0, timeout: "" });
      continue;
    }
    log(`──────── ${file} ────────`);
    const r = runBounded(dynajs, `${dir}/${file}`);
    results.push({ file, skip: null, ...r });
    if (r.timeout) log(`TIMEOUT (${r.timeout}) after ${r.ms}ms`);
    log("");
  }

  log("════════ summary ════════");
  let failed = 0, timedOut = 0, skippedN = 0;
  for (const { file, code, ms, timeout, skip } of results) {
    let status;
    if (skip) { status = "SKIP"; skippedN++; }
    else if (timeout) { status = `TIMEOUT (${timeout})`; timedOut++; }
    else if (code !== 0) { status = `FAIL (exit ${code})`; failed++; }
    else status = "PASS";
    log(`  ${status.padEnd(18)} ${file.padEnd(26)} ${skip ? skip : ms + "ms"}`);
  }

  const passed = results.length - failed - timedOut - skippedN;
  log(`\n  ${passed}/${results.length} suites passed` +
      (skippedN ? `, ${skippedN} skipped` : "") +
      (timedOut ? `, ${timedOut} timed out` : ""));

  // A skip is not a pass, but it is not a failure either — it is reported so a
  // run against the wrong build cannot look complete.
  const ok = failed === 0 && timedOut === 0;
  log(ok ? "\nALL PASS" : "\nSOME FAILED");
  std.exit(ok ? 0 : 1);
}

main();
