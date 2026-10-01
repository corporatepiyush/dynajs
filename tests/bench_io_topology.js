import * as std from "std";
import { Path, writeFile, readFile, readFileAsync, asyncStats, remove }
  from "dyna:file";

const MB = 1024 * 1024;
const sink = [];
const made = [];
function tmp(n) { const p = new Path(`${std.getenv("TMPDIR") || "/tmp"}/dj_topo_${Date.now() % 10000000}_${n}`); made.push(p); return p; }

function fill(p, sz) {
  let s = "";
  for (let i = 0; i < sz; i++) s += String.fromCharCode(33 + (i % 90));
  writeFile(p, s);
  readFile(p);
  return s.length;
}

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
