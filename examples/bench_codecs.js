import { parse } from "dyna:csv";
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";
import { sum } from "dyna:simd";
import { bench, table } from "dyna:bench";

function buildCsv(n) {
  let s = "id,v\n";
  for (let i = 0; i < n; i++) s += i + ",x" + i + "\n";
  return s;
}

{
  const N = 20000;
  const text = buildCsv(N);
  const r1 = bench("csv.parse 20k", () => { parse(text); }, { timeMs: 300, warmupMs: 50 });
  print("csv.parse: ops/sec=" + r1.opsPerSec.toFixed(1) + " rsd=" + r1.rsd.toFixed(3) + " p50=" + r1.p50Ms.toFixed(2) + "ms");
  const p = new Path("/tmp/bench_codecs_csv.csv");
  const f = new CSVFile(p);
  const t0 = performance.now();
  await import("dyna:file").then(async (file) => {
    await file.writeFile(p, text);
  }).catch(() => {});
  print("csv file roundtrip setup ms=" + (performance.now() - t0).toFixed(1));
  f.close();
}

{
  const a = new Float32Array(100000);
  for (let i = 0; i < a.length; i++) a[i] = (i % 7) + 1;
  const rWin = bench("simd.sum window", () => { sum(a, 1000, 50000); }, { timeMs: 300, warmupMs: 50 });
  const rSub = bench("simd.sum subarray", () => { sum(a.subarray(1000, 51000)); }, { timeMs: 300, warmupMs: 50 });
  print("window: " + rWin.opsPerSec.toFixed(1) + "/s  subarray: " + rSub.opsPerSec.toFixed(1) + "/s");
}

print(table());
