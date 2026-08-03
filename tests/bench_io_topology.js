/*
 * T2 (pool) vs T1 (inline) -- NET_PLAN.md section 2's central design axis,
 * measured in ONE binary via --io-threads.
 *
 *   ./dynajs --io-threads 0 tests/bench_io_topology.js    # T1, no pool
 *   ./dynajs               tests/bench_io_topology.js    # T2, pool
 *
 * Section 7's risk row is "T2 is slower than T1 for cheap events -- measure
 * both, publish the losing row", so this reports BOTH throughput and loop
 * responsiveness. Throughput alone would hide the only property T2 buys.
 *
 * asyncStats() is the control: in T1 `offloaded` must stay 0. A run that
 * cannot say which topology it measured is not a measurement.
 */
import { Path, writeFile, readFile, readFileAsync, asyncStats, remove }
  from "dyna:file";

const MB = 1024 * 1024;
const sink = [];
const made = [];
function tmp(n) { const p = new Path("/tmp/dj_topo_" + n); made.push(p); return p; }

function fill(p, sz) {
  let s = "";
  for (let i = 0; i < sz; i++) s += String.fromCharCode(33 + (i % 90));
  writeFile(p, s);
  readFile(p);                                   /* warm the page cache */
  return s.length;
}

/* Await one at a time: a Promise.all batch has the loop draining completions
 * back to back, which starves the timer for a reason that is not "the work is
 * on the loop" -- so it cannot separate the two topologies. */
async function measure(label, p, rounds) {
  let ticks = 0;
  const iv = setInterval(() => { ticks++; }, 1);
  const a = asyncStats();
  const t0 = Date.now();
  for (let i = 0; i < rounds; i++) sink.push((await readFileAsync(p)).length);
  const ms = Date.now() - t0;
  clearInterval(iv);
  const b = asyncStats();
  print("  " + label.padEnd(14) +
        String(ms).padStart(5) + " ms" +
        ("  " + (ms * 1000 / rounds).toFixed(0) + " us/op").padStart(14) +
        "   loop served " + String(ticks).padStart(4) + "x" +
        "   offloaded " + (b.offloaded - a.offloaded));
  return { ms, ticks };
}

(async () => {
  const st = asyncStats();
  const topology = st.offloaded === 0 ? "?" : "?";
  print("io topology bench   readMin=" + (st.readMin / 1024) + " KiB");

  /* Below the threshold NOTHING offloads in either topology: this row is the
   * control that must not move between T1 and T2. */
  print("");
  print("SMALL (64 KiB, below readMin -- inline in BOTH topologies):");
  const small = tmp("small"); fill(small, 64 * 1024);
  await measure("64KiB x400", small, 400);

  print("");
  print("LARGE (4 MiB, above readMin -- offloads in T2, inline in T1):");
  const large = tmp("large"); fill(large, 4 * MB);
  await measure("4MiB x60", large, 60);

  print("");
  print("HUGE (16 MiB):");
  const huge = tmp("huge"); fill(huge, 16 * MB);
  await measure("16MiB x20", huge, 20);

  const f = asyncStats();
  print("");
  print("TOPOLOGY: " + (f.offloaded === 0 ? "T1 (inline, --io-threads 0)"
                                          : "T2 (pool)") +
        "   totals: inline=" + f.inline + " offloaded=" + f.offloaded);
  for (const p of made) { try { remove(p); } catch (e) {} }
  print("checksum " + sink.length);
})().catch((e) => print("ERR " + e));
