#!/usr/bin/env python3
"""gen_uring.py — dyna:uring probes. io_uring is Linux-only; on macOS the
module is expected to be inert or absent — probe + document, never assert
Linux-only paths. readFileSync/checksum(path,false) use the pread reference.
"""
from probe_lib import emit

IMP = '''import { Path, writeFile, readFile, makeDir, removeAll, exists } from "dyna:file";
import { Exec } from "dyna:sys";
'''

emit("uring", "macos", IMP, r'''
// dyna:uring registers only in CONFIG_IO_URING builds (Linux). Detect by
// dynamic import so the probe is meaningful on both shapes of build.
let M = null, importErr = null;
try { M = await0(); } catch (e) { importErr = e; }
function await0() { throw new Error("placeholder"); }
summary("uring.macos");
''')
# The above static shape is awkward; emit a hand-built probe instead.
import os
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "probes", "uring")
os.makedirs(p, exist_ok=True)
# find h.js content relative
harness = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "h.js")).read()
uring_src = '''import { Path, writeFile, readFile, makeDir, removeAll, exists } from "dyna:file";
import { Exec } from "dyna:sys";
''' + harness + '''
// ---- generated probe uring/macos ----
// dyna:uring registers only in CONFIG_IO_URING builds (Linux). Detect by
// dynamic import so the probe documents the build shape it runs on.
let M = null, importErr = null;
try {
  M = await import("dyna:uring");
  if (M && M.default) M = M.default;
} catch (e) { importErr = e; }

const D = new Path(Path.cwd(), "scratch", "uring-" + ((Math.random() * 1e9) | 0));
makeDir(D, { recursive: true });
const f = new Path(D, "payload.bin");
const payload = "uring-differential-payload\\n".repeat(1000);
writeFile(f, payload);

if (!M) {
  // ABSENT build: document. The module is Linux-only; nothing to assert.
  print("NOTE dyna:uring not registered on this build (" +
        (importErr ? importErr.message : "?") + ") -- Linux-only module");
  summary("uring.macos");
} else {
  // PRESENT build: the pread reference must match dyna:file byte-for-byte.
  assert_eq(typeof M.readFileSync, "function", "readFileSync exported");
  assert_eq(typeof M.checksum, "function", "checksum exported");
  assert_eq(M.readFileSync(f), payload, "readFileSync (pread reference) matches");
  // non-Path argument refused with a TypeError naming the parameter
  assert_throws(() => M.readFileSync(String(f)), "TypeError", "string path refused");
  assert_throws(() => M.checksum({}), "TypeError", "object refused");
  // FNV-1a 32-bit reference computed in JS
  function fnv(str) {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
      h = h ^ (str.charCodeAt(i) & 0xff);
      h = Math.imul(h, 16777619) >>> 0;
      // str is ASCII here; the payload is byte-exact ASCII
    }
    return h >>> 0;
  }
  const ref = M.checksum(f, false);
  assert_eq(ref.bytes, payload.length, "checksum bytes == length");
  assert_eq(ref.sum, fnv(payload), "checksum(pread) == JS FNV-1a reference");
  // the uring backend (default true) must agree with the reference when the
  // kernel supports it; where it cannot (ENOSYS), it refuses, never corrupts.
  try {
    const u = M.checksum(f, true);
    assert_eq(u.sum, ref.sum, "checksum(uring) == checksum(pread)");
    assert_eq(u.bytes, ref.bytes, "checksum(uring) bytes match");
  } catch (e) {
    assert_throws(() => { throw e; }, "InternalError", "uring backend refuses (ENOSYS) as InternalError");
    assert_true(String(e.message).indexOf("read failed") >= 0, "refusal names the failure: " + e.message);
  }
  summary("uring.macos");
}
removeAll(D);
'''
with open(os.path.join(p, "macos.js"), "w") as f:
    f.write(uring_src)
print("gen_uring: 1 probe (hand-built)")
