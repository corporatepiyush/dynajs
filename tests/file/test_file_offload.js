// covers: integration dyna:async.offload dyna:file.readFile dyna:file.readBytes dyna:file.writeFile -- the offload contract (byte-identical result, loop ticks) checked for every marked function of the module
// Owner ask E, wave 4: readFile/readBytes mark the open+read of the file and writeFile the open+write (or the atomic
// write) DYN_OFFLOAD_IO, so under offload() they run as IO jobs on the shared io pool; building the result string or
// Uint8Array stays on the loop thread. writeFile copies a buffer payload first under offload (a string payload is
// held as an immutable C string). Per function: the offloaded result equals the direct call, the file on disk is the
// same, errors reject with the direct call's error, and the promise is still pending when offload() returns (a mechanism, not a timer race: see tests/lib/offload_check.js).
// Mutation: removing the DYN_OFFLOAD_IO mark of the read or of the write makes its "loop ticks" check red (settled at return).
import { suite } from "../lib/kit.js";
import { same, loopTicks, text, equalValue } from "../lib/offload_check.js";
import { offload } from "dyna:async";
import { readFile, readBytes, writeFile, makeTempDir, removeAll, Path } from "dyna:file";

const t = suite("file.offload");
const dir = makeTempDir("dj_offload_");
const BIG = text(64 << 20, 5);
const pathA = new Path(dir, "a.bin"), pathB = new Path(dir, "b.bin"), pathC = new Path(dir, "c.txt");
writeFile(pathA, BIG);

t.test("readBytes / readFile: offloaded result equals the direct call", async (a) => {
    const b = await same(a, "readBytes", readBytes, [pathA]);
    a.ok(equalValue(b, BIG), "readBytes gives the file");
    await same(a, "readFile bytes", readFile, [pathA, { bytes: true }]);
    writeFile(pathC, "héllo wörld\n".repeat(1000));
    await same(a, "readFile text", readFile, [pathC]);
});

t.test("writeFile: offloaded write leaves the same file, for bytes, string, append and atomic", async (a) => {
    a.eq(await offload(writeFile, pathB, BIG), BIG.length, "returns the byte count");
    a.ok(equalValue(readBytes(pathB), BIG), "bytes on disk");
    await offload(writeFile, pathB, "abc");
    a.eq(readFile(pathB), "abc", "string payload truncates and writes");
    await offload(writeFile, pathB, "def", { append: true });
    a.eq(readFile(pathB), "abcdef", "append");
    await offload(writeFile, pathB, BIG, { atomic: true });
    a.ok(equalValue(readBytes(pathB), BIG), "atomic");
});

t.test("writeFile: the payload is copied at the call", async (a) => {
    const u = text(8 << 20, 9);
    const want = u.slice();
    const p = offload(writeFile, pathB, u);
    u.fill(0);
    await p;
    a.ok(equalValue(readBytes(pathB), want), "the file holds the bytes at call time");
});

t.test("errors reject with the direct call's error", async (a) => {
    const missing = new Path(dir, "nope", "x.bin");
    for (const [name, fn, args] of [["readBytes", readBytes, [missing]], ["readFile", readFile, [missing]], ["writeFile", writeFile, [missing, "x"]]]) {
        let direct;
        try { fn(...args); } catch (e) { direct = e; }
        a.ok(direct, `${name}: the direct call throws`);
        const e = await a.rejects(offload(fn, ...args), direct.constructor, `${name}: rejects`);
        a.eq(e && e.message, direct && direct.message, `${name}: same message`);
        a.eq(e && e.code, direct && direct.code, `${name}: same code`);
    }
});

t.test("loop ticks", async (a) => {
    await loopTicks(a, "readBytes", readBytes, [pathA]);
    await loopTicks(a, "readFile", readFile, [pathA, { bytes: true }]);
    await loopTicks(a, "writeFile", writeFile, [pathB, BIG]);
});

t.after(() => removeAll(dir));
await t.run();
