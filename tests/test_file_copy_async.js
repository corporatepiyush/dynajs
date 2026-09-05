/*
 * copyFileAsync -- the sync copy's refusal semantics, off the loop.
 *
 * The oracle is the SYNC path, twice over: the bytes must be identical (a copy
 * that resolves is worthless if the destination is wrong) and the REFUSALS
 * must say the same thing (a rejection whose message differs from the throw
 * the sync call makes is a second dialect nobody asked for). The arm that ran
 * is asserted from asyncStats(), because a regression that stops offloading
 * looks exactly like a pass otherwise -- and the loop staying LIVE during a
 * big copy is the point of the feature, so the sync call is the control.
 */
import {
  Path, File, writeFile, readFile, copyFile, copyFileAsync, asyncStats,
  makeTempDir, removeAll, stat, chmod, exists,
} from "dyna:file";

let n = 0, fails = 0;
function check(cond, msg) {
  n++;
  if (!cond) { fails++; print("FAIL: " + msg); }
}
function eq(a, b, msg) { check(a === b, msg + " -- got " + a + ", want " + b); }
function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}
/* The sync call as ORACLE: run it, keep whatever error it makes. */
function syncErr(fn) { try { fn(); return null; } catch (e) { return e; } }
async function rejectOf(pr) { try { await pr; return null; } catch (e) { return e; } }

const dir = makeTempDir("dynfca");
const p = (f) => new Path(String(dir) + "/" + f);

(async () => {
try {
  const S = asyncStats();
  check(S.copyMin > 0, "asyncStats must publish copyMin");

  /* ---- 1. a 3 MiB copy is byte-exact, every byte value present ----
     A 3 MiB source offloads, so the KERNEL-path arm is the one under test;
     an every-byte-identical repeating pattern makes a truncation, an
     off-by-one or a dropped page visible anywhere in the file. */
  {
    const SZ = 3 * 1024 * 1024;
    const src = new Uint8Array(SZ);
    for (let i = 0; i < SZ; i++) src[i] = i % 256;   /* all 256 values */
    writeFile(p("bin.src"), src);

    const a = asyncStats();
    const got = await copyFileAsync(p("bin.src"), p("bin.dst"));
    const b = asyncStats();
    eq(got, SZ, "resolves the source byte count for the big copy");
    eq(b.offloaded - a.offloaded, 1, "3 MiB must take the OFFLOAD arm");
    eq(b.inline - a.inline, 0, "and not the inline arm");

    const out = new File(p("bin.dst")).readBytes();
    check(bytesEqual(out, src), "the copy is byte-exact at 3 MiB");
    const seen = new Uint8Array(256);
    for (let i = 0; i < out.length; i++) seen[out[i]] = 1;
    let all = true;
    for (let v = 0; v < 256; v++) if (!seen[v]) { all = false; break; }
    check(all, "all 256 byte values survive the copy");
    check(bytesEqual(new File(p("bin.src")).readBytes(), src),
          "the source is untouched by the copy");
  }

  /* ---- 2. the inline arm is byte-exact too, and matches sync ---- */
  {
    writeFile(p("small.src"), "small payload");
    const a = asyncStats();
    const got = await copyFileAsync(p("small.src"), p("small.dst"));
    const b = asyncStats();
    eq(got, 13, "resolves the byte count for the small copy");
    eq(b.offloaded - a.offloaded, 0, "a small copy must stay INLINE");
    eq(readFile(p("small.dst")), "small payload", "inline arm content");
    eq(readFile(p("small.dst")), readFile(p("small.src")),
       "async copy equals the sync copy's output");
  }

  /* ---- 3. an empty file: resolves 0, the destination exists ---- */
  {
    writeFile(p("empty.src"), "");
    eq(await copyFileAsync(p("empty.src"), p("empty.dst")), 0, "empty copy resolves 0");
    check(exists(p("empty.dst")), "the empty destination exists");
    eq(readFile(p("empty.dst")), "", "and is empty");
  }

  /* ---- 4. refusals: the rejection says what the sync throw says ----
     Same message text, same error type, run against the SAME paths so a
     divergence in wording or in which path is named cannot hide. */
  {
    const se = syncErr(() => copyFile(p("nope"), p("x1")));
    const re = await rejectOf(copyFileAsync(p("nope"), p("x1")));
    check(se !== null && re !== null, "a missing source refuses on both paths");
    check(se && re && se.message === re.message,
          "missing source: async message must equal the sync one -- sync: '" +
          (se && se.message) + "' async: '" + (re && re.message) + "'");
    check(re && re.errno === se.errno && re.code === se.code,
          "missing source: errno and code carry over");
    eq(re && re.op, "copyFile", "the rejection names the op");
    check(re && String(re.path).indexOf("nope") >= 0, "the rejection names the path");
  }
  {
    /* A destination under a REGULAR file is ENOTDIR, not a missing directory
       to create: the refusal must name the destination, as sync does. */
    writeFile(p("reg"), "i am a file");
    const se = syncErr(() => copyFile(p("small.src"), p("reg/x")));
    const re = await rejectOf(copyFileAsync(p("small.src"), p("reg/x")));
    check(se !== null && re !== null, "dest inside a non-directory refuses");
    check(se && re && se.message === re.message,
          "non-dir dest: async message must equal the sync one");
  }
  {
    const se = syncErr(() => copyFile(new Path(String(dir)), p("x2")));
    const re = await rejectOf(copyFileAsync(new Path(String(dir)), p("x2")));
    check(se !== null && re !== null, "a directory source refuses on both paths");
    check(se && re && se.message === re.message,
          "directory source: async message must equal the sync one");
    check(se && re && se.constructor.name === re.constructor.name,
          "directory source: the refusal keeps the sync error TYPE (" +
          (re && re.constructor.name) + ")");
  }
  {
    /* Self-copy without overwrite is refused by O_EXCL -- the sync path has
       no special case, so neither does this one. */
    const se = syncErr(() => copyFile(p("small.src"), p("small.src")));
    const re = await rejectOf(copyFileAsync(p("small.src"), p("small.src")));
    check(se !== null && re !== null, "self-copy refuses on both paths");
    check(se && re && se.message === re.message,
          "self-copy: async message must equal the sync one");
    eq(readFile(p("small.src")), "small payload",
       "the refused self-copy left the file alone");
  }
  {
    /* overwrite:true onto ITSELF is the sync path's own quirk: fstat reads
       the size, the O_TRUNC then empties the shared inode, the copy writes
       nothing, and the call still resolves with the ORIGINAL size. Matched
       quirk-for-quirk, sync on one file and async on its twin. */
    writeFile(p("sa"), "truncate-me-please");
    writeFile(p("sb"), "truncate-me-please");
    const s1 = copyFile(p("sa"), p("sa"), { overwrite: true });
    const s2 = await copyFileAsync(p("sb"), p("sb"), { overwrite: true });
    eq(s2, s1, "self-overwrite resolves exactly what sync resolves");
    eq(stat(p("sb")).size, stat(p("sa")).size, "and leaves the same size behind");
  }

  /* ---- 5. a missing source REJECTS; it does not throw synchronously ---- */
  {
    let syncThrew = false, re = null, pr;
    try { pr = copyFileAsync(p("also.nope"), p("x3")); }
    catch (e) { syncThrew = true; }
    check(!syncThrew, "an I/O refusal must not throw synchronously");
    re = await rejectOf(pr);
    check(re !== null, "the promise still rejects");
  }

  /* ---- 6. overwrite replaces content; the source mode carries over ---- */
  {
    writeFile(p("ow1"), "first");
    await copyFileAsync(p("ow1"), p("ow2"));
    writeFile(p("ow1"), "second");
    await copyFileAsync(p("ow1"), p("ow2"), { overwrite: true });
    eq(readFile(p("ow2")), "second", "overwrite:true replaces the content");
    writeFile(p("secret"), "k");
    chmod(p("secret"), 0o600);
    await copyFileAsync(p("secret"), p("secret.copy"));
    eq(stat(p("secret.copy")).mode & 0o777, 0o600,
       "the created file keeps the source's mode bits");
  }

  /* ---- 7. four copies in flight at once, each landing ITS OWN bytes ---- */
  {
    const K = 4, srcs = [], dsts = [], srcData = [];
    for (let i = 0; i < K; i++) {
      const sz = S.copyMin + 100000 + i * 7919;   /* all four OFFLOAD */
      const u = new Uint8Array(sz);
      for (let j = 0; j < sz; j++) u[j] = (j * (i + 3)) % 256;
      writeFile(p("cc.src" + i), u);
      srcs.push(p("cc.src" + i)); dsts.push(p("cc.dst" + i)); srcData.push(u);
    }
    const a = asyncStats();
    const got = await Promise.all(dsts.map((d, i) => copyFileAsync(srcs[i], d)));
    const b = asyncStats();
    eq(b.offloaded - a.offloaded, K, "all four concurrent copies offload");
    for (let i = 0; i < K; i++) {
      eq(got[i], srcData[i].length, "copy " + i + " resolves its own count");
      check(bytesEqual(new File(dsts[i]).readBytes(), srcData[i]),
            "copy " + i + " lands byte-exact");
    }
  }

  /* ---- 8. the loop stays LIVE while a big copy runs ----
     The whole point of the feature, with the SYNC copy as the control: the
     same file copied synchronously must starve a 1 ms timer where the
     offloaded copy does not. Awaited one at a time on purpose, so a batch
     drain cannot fake the result. */
  {
    const SZ = 10 * 1024 * 1024;
    const u = new Uint8Array(SZ);
    for (let i = 0; i < SZ; i++) u[i] = i % 256;
    writeFile(p("big.src"), u);
    copyFile(p("big.src"), p("warm.dst"), { overwrite: true });  /* warm cache */
    const rounds = 6;

    let syncTicks = 0;
    let iv = setInterval(() => { syncTicks++; }, 1);
    const t0 = Date.now();
    for (let i = 0; i < rounds; i++)
      copyFile(p("big.src"), p("warm.dst"), { overwrite: true });
    const syncMs = Date.now() - t0;
    clearInterval(iv);

    let asyncTicks = 0;
    iv = setInterval(() => { asyncTicks++; }, 1);
    const t1 = Date.now();
    for (let i = 0; i < rounds; i++)
      await copyFileAsync(p("big.src"), p("warm2.dst"), { overwrite: true });
    const asyncMs = Date.now() - t1;
    clearInterval(iv);

    check(bytesEqual(new File(p("warm2.dst")).readBytes(), u),
          "the offloaded big copy is still byte-exact");
    check(asyncTicks > syncTicks,
          "the loop must serve MORE during async copies than sync ones -- " +
          "sync ticked " + syncTicks + " (" + syncMs + " ms), async ticked " +
          asyncTicks + " (" + asyncMs + " ms)");
    check(asyncTicks > 0,
          "a 1ms timer must fire during offloaded copies (got " + asyncTicks +
          "); if it never fires the copy is on the loop");
    print("# loop-alive: sync " + rounds + "x10MiB " + syncMs +
          " ms/" + syncTicks + " ticks; async " + asyncMs + " ms/" +
          asyncTicks + " ticks");
  }

  /* ---- 9. fire-and-forget settles without hanging ----
     The shape that exposed a leaked reactor ref in the read/write arms; the
     harness timing out IS the failure signal. */
  {
    writeFile(p("ff.src"), "f".repeat(S.copyMin + 1));
    copyFileAsync(p("ff.src"), p("ff.dst"));     /* deliberately unawaited */
    await new Promise((r) => setTimeout(r, 50));
    check(true, "fire-and-forget copy settles without hanging");
  }

} catch (e) {
  fails++;
  print("FAIL: unexpected throw: " + e + (e && e.stack ? "\n" + e.stack : ""));
}
removeAll(dir);
if (fails === 0) print("test_file_copy_async: all " + n + " checks passed");
else print("test_file_copy_async: " + fails + " FAILED of " + n);
if (fails) throw new Error("test_file_copy_async failed");
})();
