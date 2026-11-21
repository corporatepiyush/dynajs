// flags: --std
// timeout: 60
// covers: c:src/builtins/builtin-typed-array.c
// A buffer js_array_buffer_constructor3 makes from a source pointer (alloc_len == len) must hold exactly
// the caller's bytes. Skipping its zero fill (the memcpy right after writes every byte) was measured and
// NOT shipped: Linux interleaved A/B, 12 reps, identical-binary control row, TextEncoder 16 KiB / 64 KiB
// gained 1.010x / 1.006x over base while the control drifted 1.002x / 1.003x with a 0.97-1.42x range, so
// the gain is inside the control spread; the zero fill stays. This test guards any later retry.
// AUDIT (every path that would reach a non-zeroed allocation, kept for that retry). The constructor
// copies exactly len bytes from the caller's source, so an unwritten byte is impossible; the only risk is
// a caller whose len exceeds the bytes it actually filled, which was already an over-read of its source
// (the zero fill never hid it: it was overwritten by that same memcpy). Each caller's len is the count it filled:
//   libc-std-file readBytes: fread() result `got` (partial reads and EOF pass the short count, not max).
//   net-tcp tcp_deliver / net-tcp-udp: the recv/recvfrom/CQE result (a short read passes its own length;
//     errors take the res < 0 path before any buffer is made).
//   libc-text-codec TextEncoder.encode: len from JS_ToCStringLen, or flen from the lone-surrogate rewrite
//     (fallback when that rewrite returns NULL: the original str/len pair).
//   http-message / http-app-send bodies, postgres, redis, sqlite, protobuf, vserialize, asn1:
//     lengths parsed and bounds-checked against the received buffer before the call.
//   bc_write.inc.c ArrayBuffer read: byte_length checked against buf_end - ptr first.
//   JS_NewUint8ArrayCopy, compress, crypto-hash, dataframe, mathx, ml, graph, simd, structures, random,
//     uuid, ui, sdl, raylib, dyna-nat, dyna-uring, dyna-serialize: n elements the caller just wrote
//     (n * element size), or 0 with a 1-byte stub source.
//   ArrayBuffer transfer (builtin-typed-array.c): alloc_flag FALSE, adopts the old pointer; not this path.
// This test pins that contract where JS can see it: the heap is first churned with 0xA5-filled blocks
// of the same sizes, so a buffer whose copy was skipped or shortened shows 0xA5 garbage, not zeros.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_arraybuffer_copy_init");
import * as std from "std";
import { TCPServer } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
const sizes = [1, 2, 7, 8, 15, 16, 63, 64, 255, 1000, 4096, 4097, 16383, 16384, 16385, 65536, 70001];
const pat = (n, salt) => { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (i * 31 + salt) & 0x7f; return u; };
const churn = () => { for (const n of sizes) for (let k = 0; k < 4; k++) new Uint8Array(n).fill(0xa5); std.gc(); };

__kitS.section("block L36", () => {
  let bad = 0;
  for (const n of sizes) {
    churn();
    const s = Array.from(pat(n, 3), (b) => String.fromCharCode(b || 1)).join("");
    const u = new TextEncoder().encode(s);
    for (let i = 0; i < n; i++) if (u[i] !== (s.charCodeAt(i))) { bad++; break; }
  }
  ok(bad === 0, `TextEncoder.encode: ${bad} of ${sizes.length} sizes had a byte the source did not`);
});
__kitS.section("block L46", () => {
  let bad = 0;
  for (const n of sizes) {
    churn();
    const src = pat(n + 9, 5).buffer;
    const sl = new Uint8Array(src.slice(9));
    for (let i = 0; i < n; i++) if (sl[i] !== (((i + 9) * 31 + 5) & 0x7f)) { bad++; break; }
  }
  ok(bad === 0, `ArrayBuffer.slice: ${bad} of ${sizes.length} sizes differ from the source`);
});
await __kitS.section("block L56", async () => {
  const srv = new TCPServer({ port: 0 });
  srv.start({ data: (c, b) => c.write(b) });
  const total = sizes.reduce((a, n) => a + n, 0);
  const want = new Uint8Array(total);
  let off = 0;
  for (const n of sizes) { want.set(pat(n, 11), off); off += n; }
  const got = new Uint8Array(total);
  let at = 0, chunks = 0;
  await new Promise((resolve) => {
    TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
      connect: (c) => { if (!c) return resolve(); churn(); for (const n of sizes) c.write(pat(n, 11)); },
      data: (c, b) => { chunks++; got.set(b.subarray(0, Math.min(b.length, total - at)), at); at += b.length; if (at >= total) { c.close(); resolve(); } },
      close: () => resolve(),
    });
  });
  srv.close();
  let firstBad = -1;
  for (let i = 0; i < total; i++) if (got[i] !== want[i]) { firstBad = i; break; }
  ok(at === total && firstBad < 0, `TCP echo of ${total} bytes in ${chunks} data chunks: ${at} arrived, first wrong byte at ${firstBad}`);
});
__kitS.end();
print(`test_arraybuffer_copy_init: ${pass} passed, ${fail} failed`);
if (fail) std.exit(1);
