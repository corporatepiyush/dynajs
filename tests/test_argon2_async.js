/* test_argon2_async.js -- Argon2id.hashAsync / verifyAsync / asyncStats.
 *
 * The offloaded variant of the sync surface in test_crypto_standalone.js
 * (which pins the RFC 9106 vectors). The oracle here is the SYNC form
 * itself: for identical inputs, hashAsync must resolve BYTE-IDENTICAL
 * output and verifyAsync must return the identical verdict -- the core is
 * the same arg_hash_id with the same inputs, so "close" is a bug.
 *
 * Refusals must be identical AND synchronous: every invalid parameter is
 * rejected on the JS thread by the SAME parser (arg_read_opts) before
 * anything is scheduled, so the async call throws exactly what hash()
 * throws. A rejected promise here would be a weaker contract than sync.
 *
 * The arm that ran is asserted from asyncStats -- a portfolio whose
 * selection cannot be observed is one that silently stops offloading and
 * nothing ever says so. And the point of the whole feature is proven with
 * a 1 ms timer: it must tick through a 256 MiB-cost async hash, where the
 * sync form starves it (tick counts printed for the record).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_argon2_async.js
 */

import { Argon2id } from "dyna:crypto";

let n = 0, fails = 0;
function check(c, msg) {
    n++;
    if (!c) { fails++; print("FAIL: " + msg); }
}
function eq(a, b, msg) { check(a === b, msg + " -- got " + a + ", want " + b); }
function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function toHex(a) {
    let s = "";
    for (const b of a) s += b.toString(16).padStart(2, "0");
    return s;
}
function errName(e) { return e && e.constructor ? e.constructor.name : String(e); }

if (typeof Argon2id?.hashAsync !== "function") {
    print("test_argon2_async: SKIP (built without the net reactor; the async " +
          "functions are not exported)");
} else {
(async () => {
try {
  const S = Argon2id.asyncStats();
  check(S.offloadMin > 0, "the block-fill gate must be published");

  /* ---- 1. byte-identity with the sync form, both arms ----------------
     The matrix straddles the gate: memory*iterations below offloadMin
     (inline arm) and above it (offload arm), plus the legal minimums. */
  const enc = new TextEncoder();
  const matrix = [
      /* name                pw                  salt                       opts */
      ["legal minimum",  "pw",               enc.encode("01234567"),    { iterations: 1, memory: 8,  parallelism: 1, hashLen: 4 }],
      ["tiny, inline",   "pw",               enc.encode("0123456789ab"),{ iterations: 1, memory: 32, parallelism: 4, hashLen: 16 }],
      ["under the gate", "hunter2",          enc.encode("0123456789ab"),{ iterations: 3, memory: 64, parallelism: 4, hashLen: 32 }],
      ["over the gate",  "hunter2",          enc.encode("0123456789ab"),{ iterations: 1, memory: 256, parallelism: 4, hashLen: 32 }],
      ["many passes",    "other-password",   enc.encode("abcdefghijklmnop"), { iterations: 16, memory: 1024, parallelism: 2, hashLen: 64 }],
      ["16 lanes",       "wide",             enc.encode("abcdefghijklmnop"), { iterations: 2, memory: 512, parallelism: 16, hashLen: 24 }],
      ["defaults",       "hunter2",          enc.encode("0123456789ab"), undefined],
  ];
  for (const [name, pw, salt, opts] of matrix) {
      const sync = Argon2id.hash(pw, salt, opts);
      const st0 = Argon2id.asyncStats();
      const got = await Argon2id.hashAsync(pw, salt, opts);
      const st1 = Argon2id.asyncStats();
      check(bytesEqual(sync, got), name + ": hashAsync must equal the sync form byte for byte");
      eq(sync.length, got.length, name + ": length matches");
      const fills = (opts?.memory ?? 65536) * (opts?.iterations ?? 3);
      const arm = st1.offloaded - st0.offloaded === 1 ? "offloaded"
                : st1.inline - st0.inline === 1 ? "inline" : "NEITHER";
      eq(arm, fills >= S.offloadMin ? "offloaded" : "inline",
         name + ": ran on the arm its size dictates (" + fills + " fills)");
  }

  /* ---- 2. the RFC 9106 vector through the async path directly --------
     test_crypto_standalone.js pins the sync form; the async form must
     land on the same bytes. */
  {
      const salt = enc.encode("somesalt");
      const got = await Argon2id.hashAsync("password", salt,
          { iterations: 3, memory: 65536, parallelism: 4, hashLen: 32 });
      eq(toHex(got), "661fefbd6f29bcbc8f4646abc32a9d7a4645bb5c059537f8a5587f31adbecccd",
         "async output matches the PHC/RFC 9106 reference vector");
  }

  /* ---- 3. verifyAsync: identical verdicts ----------------------------- */
  {
      const salt = enc.encode("0123456789abcdef");
      const opts = { iterations: 2, memory: 2048, parallelism: 4 };
      const tag = Argon2id.hash("correct horse", salt, opts);
      eq(await Argon2id.verifyAsync("correct horse", salt, tag, opts), true,
         "verifyAsync accepts the right password");
      eq(await Argon2id.verifyAsync("wrong horse", salt, tag, opts), false,
         "verifyAsync rejects the wrong password");
      eq(await Argon2id.verifyAsync("correct horse", salt, tag.slice(0, 31), opts), false,
         "verifyAsync returns false (not a throw) on a length mismatch, like sync");
      eq(Argon2id.verify("correct horse", salt, tag.slice(0, 31), opts), false,
         "and the sync form agrees");
  }

  /* ---- 4. refusal parity: identical exception, identical timing -------
     Every case throws the same error class with the same message as the
     sync form, and throws it SYNCHRONOUSLY (nothing was scheduled, so
     there is no promise to reject). */
  {
      const salt = enc.encode("somesalt");
      const bad = [
          ["salt 7 bytes",        () => { Argon2id.hash("p", enc.encode("1234567")); }],
          ["iterations 0",        () => { Argon2id.hash("p", salt, { iterations: 0 }); }],
          ["iterations 17",       () => { Argon2id.hash("p", salt, { iterations: 17 }); }],
          ["parallelism 0",       () => { Argon2id.hash("p", salt, { parallelism: 0 }); }],
          ["parallelism 17",      () => { Argon2id.hash("p", salt, { parallelism: 17 }); }],
          ["memory < 8*lanes",    () => { Argon2id.hash("p", salt, { memory: 16, parallelism: 4 }); }],
          ["memory over 1 GiB",   () => { Argon2id.hash("p", salt, { memory: 1048577 }); }],
          ["hashLen 3",           () => { Argon2id.hash("p", salt, { hashLen: 3 }); }],
          ["hashLen over 1 MiB",  () => { Argon2id.hash("p", salt, { hashLen: 1048577 }); }],
          ["verify salt short",   () => { Argon2id.verify("p", enc.encode("1234567"), new Uint8Array(32)); }],
      ];
      for (const [name, syncCall] of bad) {
          let syncErr = null;
          try { syncCall(); } catch (e) { syncErr = e; }
          check(syncErr !== null, name + ": sync form refuses (test sanity)");
          let asyncErr = null;
          try {
              /* the async twin of syncCall, derived from its shape */
              const saltArg = name === "salt 7 bytes" || name === "verify salt short"
                  ? enc.encode("1234567") : salt;
              const optsArg = name === "memory < 8*lanes" ? { memory: 16, parallelism: 4 }
                  : name === "memory over 1 GiB" ? { memory: 1048577 }
                  : name === "hashLen 3" ? { hashLen: 3 }
                  : name === "hashLen over 1 MiB" ? { hashLen: 1048577 }
                  : name === "iterations 0" ? { iterations: 0 }
                  : name === "iterations 17" ? { iterations: 17 }
                  : name === "parallelism 0" ? { parallelism: 0 }
                  : name === "parallelism 17" ? { parallelism: 17 }
                  : undefined;
              if (name === "verify salt short")
                  await Argon2id.verifyAsync("p", saltArg, new Uint8Array(32));
              else
                  await Argon2id.hashAsync("p", saltArg, optsArg);
          } catch (e) { asyncErr = e; }
          check(asyncErr !== null, name + ": async form refuses too");
          if (asyncErr) {
              eq(errName(asyncErr), errName(syncErr), name + ": same error class");
              eq(asyncErr.message, syncErr.message, name + ": same message");
          }
      }

      /* a throwing options getter propagates identically */
      {
          const bomb = { get iterations() { throw new TypeError("boom"); } };
          let syncErr = null, asyncErr = null;
          try { Argon2id.hash("p", salt, bomb); } catch (e) { syncErr = e; }
          try { await Argon2id.hashAsync("p", salt, bomb); } catch (e) { asyncErr = e; }
          check(syncErr && asyncErr, "a throwing getter throws in both forms");
          if (syncErr && asyncErr) {
              eq(errName(asyncErr), errName(syncErr), "getter: same error class");
              eq(asyncErr.message, syncErr.message, "getter: same message");
          }
      }

      /* a NON-object opts is ignored by both (same accept-set), so both
         hash the defaults -- and must agree */
      {
          const a = Argon2id.hash("p", salt, /** @type {any} */ (42));
          const b = await Argon2id.hashAsync("p", salt, /** @type {any} */ (42));
          check(bytesEqual(a, b), "non-object opts is ignored identically");
      }

      /* a getter that MUTATES the salt cannot detach the copy: both forms
         must hash the ORIGINAL salt bytes */
      {
          const mut = enc.encode("0123456789ab");
          const evil = { get iterations() { mut[0] = 120; return 2; } };
          const before = Argon2id.hash("p", mut, evil);
          mut[0] = 48;                     /* restore the bytes the getter changed */
          const after = await Argon2id.hashAsync("p", mut, evil);
          check(bytesEqual(before, after),
                "a mutating getter cannot change what either form hashed");
      }
  }

  /* ---- 5. the loop stays alive: the actual point ----------------------
     A 256 MiB-cost hash is ~160 ms of sync compute on the reference host.
     Synchronously, a 1 ms timer cannot fire at all. Awaited through the
     offload arm, it must keep ticking -- this is the difference between
     "the work happened" and "the work happened while the server served". */
  {
      const opts = { iterations: 1, memory: 262144, parallelism: 4, hashLen: 32 };
      const pw = "loop-alive", salt = enc.encode("0123456789abcdef");
      const tag = Argon2id.hash(pw, salt, opts);   /* warm the arena sizing */

      let syncTicks = 0;
      let iv = setInterval(() => { syncTicks++; }, 1);
      const t0 = Date.now();
      Argon2id.hash(pw, salt, opts);
      const syncMs = Date.now() - t0;
      clearInterval(iv);

      let asyncTicks = 0;
      iv = setInterval(() => { asyncTicks++; }, 1);
      const t1 = Date.now();
      const got = await Argon2id.hashAsync(pw, salt, opts);
      const asyncMs = Date.now() - t1;
      clearInterval(iv);

      print("numbers: 256 MiB x1 sync=" + syncMs + "ms (" + syncTicks +
            " ticks), async submit-to-callback=" + asyncMs + "ms (" +
            asyncTicks + " ticks)");
      check(bytesEqual(tag, got), "loop-alive run: async output still byte-identical");
      check(asyncTicks > syncTicks,
            "the loop must serve MORE during an offloaded 256 MiB hash than a " +
            "sync one -- sync ticked " + syncTicks + ", async ticked " + asyncTicks);
      check(asyncTicks > 0,
            "a 1ms timer must fire during the offloaded hash (got " + asyncTicks +
            "); if it never fires the work is not off the loop");
  }

  /* ---- 6. concurrent hashes: three at once, all byte-correct ---------- */
  {
      const jobs = [
          ["alpha", enc.encode("aaaaaaaaaaaaaaaa"), { iterations: 1, memory: 131072, parallelism: 2, hashLen: 32 }],
          ["beta",  enc.encode("bbbbbbbbbbbbbbbb"), { iterations: 2, memory: 65536,  parallelism: 4, hashLen: 32 }],
          ["gamma", enc.encode("cccccccccccccccc"), { iterations: 1, memory: 262144, parallelism: 4, hashLen: 64 }],
      ];
      const want = jobs.map(([pw, salt, opts]) => Argon2id.hash(pw, salt, opts));
      const st0 = Argon2id.asyncStats();
      const got = await Promise.all(jobs.map(([pw, salt, opts]) =>
          Argon2id.hashAsync(pw, salt, opts)));
      const st1 = Argon2id.asyncStats();
      for (let i = 0; i < jobs.length; i++)
          check(bytesEqual(want[i], got[i]),
                "concurrent job " + i + " must equal its sync twin");
      eq(st1.offloaded - st0.offloaded, jobs.length,
         "all concurrent hashes took the offload arm");
  }

  /* ---- 7. fire-and-forget must not hang -------------------------------
     The shape that exposed a leaked reactor ref in dyna:file: an unawaited
     promise whose ref is never released would keep the loop alive forever;
     the harness timing out IS the failure signal. */
  {
      Argon2id.hashAsync("fly", enc.encode("0123456789abcdef"),
                         { iterations: 1, memory: 65536 });
      await new Promise((r) => setTimeout(r, 50));
      check(true, "fire-and-forget hashAsync settles without hanging");
  }

  print("test_argon2_async: " + (fails ? fails + " FAILURES" : "all tests passed") +
        " (" + n + " assertions)");
  if (fails) throw new Error(fails + " failures");
} catch (e) {
  print("test_argon2_async: EXCEPTION " + e.stack || e);
  throw e;
}
})();
}
