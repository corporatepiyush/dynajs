// flags: --std
import { Env } from "dyna:config";
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}

const tmp = std.getenv("TMPDIR") || "/tmp";
const base = tmp + "/a4_env_" + os.getpid();

function writeFile(path, bytes) {
    const fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600);
    if (fd < 0)
        throw new Error("open failed: " + path);
    const u8 = new Uint8Array(bytes.length);
    for (let i = 0; i < bytes.length; i++)
        u8[i] = bytes.charCodeAt(i) & 0xff;
    let off = 0;
    while (off < u8.length) {
        const w = os.write(fd, u8.buffer, off, u8.length - off);
        if (w <= 0)
            break;
        off += w;
    }
    os.close(fd);
    if (off !== u8.length)
        throw new Error("short write: " + path);
}

{
    const p = base + "_nul.env";
    writeFile(p, 'A4_NUL="x\u0000y"\nA4_OK=plain\n');
    let threw = null;
    try { Env.load(p, { assign: false }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "NUL-containing value is refused (got " + threw + ")");
    os.remove(p);
}

{
    const p = base + "_exp.env";
    const home = std.getenv("PATH") || "";
    assert(home.length > 0, "PATH is available for the expansion test");
    const refs = Math.ceil((2 * 1024 * 1024) / Math.max(1, home.length));
    writeFile(p, "A4_EXPAND=" + "$PATH".repeat(refs) + "\n");
    let threw = null;
    try { Env.load(p, { expand: true, assign: false }); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "expansion over 1 MiB is refused (got " + threw + ")");
    os.remove(p);
}

{
    const p = base + "_small.env";
    writeFile(p, "A4_SMALL=ok\nA4_TWO=$PATH/x\n");
    const o = Env.load(p, { expand: true, assign: false });
    assert(o.A4_SMALL === "ok", "small value loads");
    assert(o.A4_TWO === (std.getenv("PATH") || "") + "/x", "small expansion works");
    os.remove(p);
}

print("test_a4_env_bounds: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_env_bounds: " + fails + " failures");
