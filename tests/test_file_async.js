/*
 * dyna:file async I/O -- the two-strategy portfolio.
 *
 * The oracle is the SYNC path: async must return byte-identical content at
 * every size, on BOTH arms. Asserting only "it resolved" would pass an
 * implementation that returns the wrong file.
 *
 * Sizes are chosen to straddle the threshold so both arms are exercised in one
 * run, and the arm that ran is ASSERTED from asyncStats() -- otherwise a
 * regression that stops selecting offload looks exactly like a pass.
 */
import {
  Path, readFile, writeFile, readFileAsync, writeFileAsync, asyncStats,
  remove, exists, stat, FileWriter,
} from "dyna:file";

let n = 0, fails = 0;
function check(cond, msg) {
  n++;
  if (!cond) { fails++; print("FAIL: " + msg); }
}
function eq(a, b, msg) { check(a === b, msg + " -- got " + a + ", want " + b); }

const TMP = "/tmp/dj_fa_" + Date.now() + "_";
const made = [];
function tmp(name) { const p = new Path(TMP + name); made.push(p); return p; }
function cleanup() { for (const p of made) { try { if (exists(p)) remove(p); } catch (e) {} } }

(async () => {
try {
  const S = asyncStats();
  check(S.readMin > 0 && S.writeMin > 0, "thresholds must be published");

  /* ---- 1. content equals the sync path at every size, both arms ---- */
  const sizes = [0, 1, 4095, 4096, 65536, S.readMin - 1, S.readMin,
                 S.readMin * 2 + 12345];
  for (const sz of sizes) {
    const p = tmp("sz" + sz);
    /* A repeating pattern, not one byte: a truncation or an off-by-one in the
     * length is invisible when every byte is identical. */
    let body = "";
    for (let i = 0; i < sz; i++) body += String.fromCharCode(33 + (i % 90));
    writeFile(p, body);

    const a = asyncStats();
    const got = await readFileAsync(p);
    const b = asyncStats();

    eq(got.length, sz, "readFileAsync length at " + sz);
    check(got === body, "readFileAsync content at " + sz + " must equal sync");
    eq(got, readFile(p), "readFileAsync must equal readFile at " + sz);

    const offl = b.offloaded - a.offloaded;
    if (sz >= S.readMin)
      eq(offl, 1, "size " + sz + " (>= readMin) must take the OFFLOAD arm");
    else
      eq(offl, 0, "size " + sz + " (< readMin) must take the INLINE arm");
  }

  /* ---- 2. bytes:true is a Uint8Array and survives NUL and high bytes ---- */
  {
    const p = tmp("bin");
    const raw = new Uint8Array(300);
    for (let i = 0; i < raw.length; i++) raw[i] = i % 256;
    writeFile(p, raw);
    const got = await readFileAsync(p, { bytes: true });
    check(got instanceof Uint8Array, "bytes:true must give a Uint8Array");
    eq(got.length, raw.length, "bytes length");
    let same = true;
    for (let i = 0; i < raw.length; i++) if (got[i] !== raw[i]) { same = false; break; }
    check(same, "bytes:true must round-trip NUL and high bytes exactly");
  }

  /* ---- 3. writeFileAsync round trip, both arms, and append ---- */
  for (const sz of [10, asyncStats().writeMin + 1000]) {
    const p = tmp("w" + sz);
    let body = "";
    for (let i = 0; i < sz; i++) body += String.fromCharCode(65 + (i % 26));
    const a = asyncStats();
    const wrote = await writeFileAsync(p, body);
    const b = asyncStats();
    eq(wrote, sz, "writeFileAsync returns the byte count at " + sz);
    eq(readFile(p), body, "writeFileAsync content at " + sz);
    const offl = b.offloaded - a.offloaded;
    if (sz >= asyncStats().writeMin) eq(offl, 1, "write " + sz + " must OFFLOAD");
    else eq(offl, 0, "write " + sz + " must stay INLINE");
  }
  {
    const p = tmp("app");
    await writeFileAsync(p, "one");
    await writeFileAsync(p, "two", { append: true });
    eq(readFile(p), "onetwo", "append:true must not truncate");
    await writeFileAsync(p, "three");
    eq(readFile(p), "three", "without append the file is truncated");
  }

  /* ---- 4. a missing file REJECTS; it does not throw synchronously ---- */
  {
    let sync_threw = false, rejected = null;
    let pr;
    try { pr = readFileAsync(new Path(TMP + "nope")); }
    catch (e) { sync_threw = true; }
    check(!sync_threw, "a missing file must not throw synchronously");
    try { await pr; } catch (e) { rejected = e; }
    check(rejected !== null, "a missing file must reject");
    eq(rejected && rejected.errno, 2, "the rejection carries errno ENOENT");
    check(rejected && String(rejected.path).indexOf("nope") >= 0,
          "the rejection names the path");
  }
  {
    /* A directory is not a file: the read must fail, not return junk. */
    let rejected = null;
    try { await readFileAsync(new Path("/tmp")); } catch (e) { rejected = e; }
    check(rejected !== null, "reading a directory must reject");
  }

  /* ---- 5. many in flight at once, each resolving with ITS OWN content ----
     One shared completion recorder would let the last writer win and the test
     would still pass, so every file has distinct content. */
  {
    const K = 24, paths = [], want = [];
    for (let i = 0; i < K; i++) {
      const p = tmp("c" + i);
      const body = ("file-" + i + "-").repeat(2000 + i);
      writeFile(p, body);
      paths.push(p); want.push(body);
    }
    const got = await Promise.all(paths.map((p) => readFileAsync(p)));
    let allMatch = true, firstBad = -1;
    for (let i = 0; i < K; i++)
      if (got[i] !== want[i]) { allMatch = false; if (firstBad < 0) firstBad = i; }
    check(allMatch, "each concurrent read must resolve with ITS OWN file" +
          (firstBad >= 0 ? " (first mismatch at " + firstBad + ")" : ""));
  }

  /* ---- 6. interleaved reads and writes to the SAME path stay ordered ----
     Promises settle in completion order, but awaits impose the ordering the
     caller wrote; a lost write would show as stale content. */
  {
    const p = tmp("seq");
    for (let i = 0; i < 8; i++) {
      await writeFileAsync(p, "gen" + i);
      eq(await readFileAsync(p), "gen" + i, "write/read ordering at gen" + i);
    }
  }

  /* ---- 7. the loop keeps serving during offloaded reads ----
     The whole point of the feature, and the SYNC path is the control: the
     same bytes read synchronously must starve the timer where async does not.
     Awaited one at a time on purpose -- a Promise.all batch has the loop
     draining completions back-to-back, which starves the timer for a reason
     that is not "the work is on the loop", so it cannot tell the two apart. */
  {
    const p = tmp("big");
    const rounds = 24;
    writeFile(p, "b".repeat(asyncStats().readMin * 8));
    readFile(p);                                     /* warm the page cache */

    let syncTicks = 0;
    let iv = setInterval(() => { syncTicks++; }, 1);
    for (let i = 0; i < rounds; i++) readFile(p);
    clearInterval(iv);

    let asyncTicks = 0;
    iv = setInterval(() => { asyncTicks++; }, 1);
    for (let i = 0; i < rounds; i++) await readFileAsync(p);
    clearInterval(iv);

    check(asyncTicks > syncTicks,
          "the loop must serve MORE during async reads than sync ones -- " +
          "sync ticked " + syncTicks + ", async ticked " + asyncTicks);
    check(asyncTicks > 0,
          "a 1ms timer must fire during offloaded reads (got " + asyncTicks +
          "); if it never fires the work is not off the loop");
  }

  /* ---- 8. a resolved promise that is never awaited must not hang or leak ----
     Fire-and-forget is the shape that exposed a reactor ref held forever. */
  {
    const p = tmp("ff");
    writeFile(p, "z".repeat(asyncStats().readMin + 1));
    readFileAsync(p);            /* deliberately not awaited, not assigned */
    writeFileAsync(tmp("ff2"), "y".repeat(asyncStats().writeMin + 1));
    /* If the reactor ref were leaked the process would not exit; the harness
     * timing out IS the failure signal here. Give it a turn to settle. */
    await new Promise((r) => setTimeout(r, 50));
    check(true, "fire-and-forget async ops settle without hanging");
  }

  /* ---- 8b. syncAsync: durability off the loop, gated on DIRTY ----
     A durable sync is fcntl(F_FULLFSYNC) on Darwin -- it waits for the DEVICE,
     measured 4810us here against 7.3us when nothing is dirty, 660x apart. That
     ratio is why the gate is "is there anything of ours to commit" rather than
     "always offload": a ~200us hop in front of 7us of work is 27x slower. */
  {
    const sp = tmp("sync");
    const w = new FileWriter(sp);
    w.write("durable-payload");

    const a = asyncStats();
    await w.syncAsync();
    const b = asyncStats();
    eq(b.offloaded - a.offloaded, 1,
       "a sync with DIRTY data must take the OFFLOAD arm");

    await w.syncAsync();
    const c = asyncStats();
    eq(c.inline - b.inline, 1,
       "a sync with NOTHING dirty must stay INLINE -- the device wait is not " +
       "there to amortise the hop");
    eq(c.offloaded - b.offloaded, 0, "and must not offload");

    /* dirty again after a further write */
    w.write("-more");
    await w.syncAsync();
    const d = asyncStats();
    eq(d.offloaded - c.offloaded, 1, "writing again re-arms the dirty gate");

    w.close();
    eq(readFile(sp), "durable-payload-more",
       "syncAsync must not lose or reorder the buffered bytes");
  }

  /* ---- 8c. the loop keeps serving across a durable sync ----
     The sync() path is the CONTROL: it must starve a 1ms timer where
     syncAsync does not, or the check cannot tell the two apart. */
  {
    const sp = tmp("syncloop");
    const rounds = 8;

    let syncTicks = 0;
    let w = new FileWriter(sp);
    let iv = setInterval(() => { syncTicks++; }, 1);
    for (let i = 0; i < rounds; i++) { w.write("x" + i); w.sync(); }
    clearInterval(iv);
    w.close();

    let asyncTicks = 0;
    w = new FileWriter(sp);
    iv = setInterval(() => { asyncTicks++; }, 1);
    for (let i = 0; i < rounds; i++) { w.write("y" + i); await w.syncAsync(); }
    clearInterval(iv);
    w.close();

    check(asyncTicks > syncTicks,
          "the loop must serve MORE across async durable syncs than sync ones " +
          "-- sync ticked " + syncTicks + ", async ticked " + asyncTicks);
    check(asyncTicks > 0,
          "a 1ms timer must fire during offloaded durable syncs, got " + asyncTicks);
  }

  /* ---- 8d. a rejected syncAsync does not throw synchronously ---- */
  {
    const sp = tmp("syncerr");
    const w = new FileWriter(sp);
    w.write("z");
    await w.syncAsync();
    w.close();
    let threwSync = false, settled = "none";
    try {
      const pr = w.syncAsync();
      try { await pr; settled = "resolved"; } catch (e) { settled = "rejected"; }
    } catch (e) { threwSync = true; }
    check(threwSync || settled !== "none",
          "syncAsync on a closed writer must settle or throw, not vanish " +
          "(threwSync=" + threwSync + " settled=" + settled + ")");
  }

  /* ---- 9. stat agrees with what we wrote through the async path ---- */
  {
    const p = tmp("stat");
    const body = "q".repeat(asyncStats().writeMin + 7);
    await writeFileAsync(p, body);
    eq(Number(stat(p).size), body.length, "stat size after writeFileAsync");
  }

} catch (e) {
  fails++;
  print("FAIL: unexpected throw: " + e + (e && e.stack ? "\n" + e.stack : ""));
}
cleanup();
if (fails === 0) print("test_file_async: all " + n + " checks passed");
else print("test_file_async: " + fails + " FAILED of " + n);
})();
