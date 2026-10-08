// timeout: 120
import { Spawn } from "dyna:sys";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function sleepMs(ms) { return new Promise((res) => setTimeout(res, ms)); }
function withTimeout(p, ms, what) {
    return Promise.race([
        p,
        sleepMs(ms).then(() => { throw new Error("TIMEOUT: " + what); }),
    ]);
}

async function pendingThenTransfer() {
    const p = new Spawn("sh", ["-c", "sleep 0.2; echo hello"]);
    const buf = new Uint8Array(64);
    const pr = p.stdout.read(buf);
    const moved = buf.buffer.transfer(0);
    assert(moved.byteLength === 0, "transfer(0) detaches the source buffer");
    let rejected = false;
    try {
        await withTimeout(pr, 5000, "detached read");
    } catch (e) {
        rejected = true;
        assert(e instanceof TypeError, "detached pending read rejects with TypeError");
    }
    assert(rejected, "a pending read into a detached buffer rejects");
    const r = await withTimeout(p.wait(), 5000, "wait after detach");
    assert(r.code === 0, "the child still reaps after the aborted read");
}

async function pendingThenTransferToFixed() {
    const p = new Spawn("sh", ["-c", "sleep 0.2; echo hi"]);
    const buf = new Uint8Array(64);
    const pr = p.stdout.read(buf);
    buf.buffer.transferToFixedLength(128);
    let rejected = false;
    try {
        await withTimeout(pr, 5000, "transferred read");
    } catch (e) {
        rejected = true;
    }
    if (!rejected) {
        assert(false, "a pending read into a moved buffer must not silently write the old base");
    } else {
        assert(true, "a pending read into a moved buffer rejects");
    }
    await withTimeout(p.wait(), 5000, "wait after transferToFixedLength");
}

async function resizeKeepsReadAlive() {
    const p = new Spawn("sh", ["-c", "sleep 0.2; printf xyz"]);
    const rab = new ArrayBuffer(64, { maxByteLength: 256 });
    const buf = new Uint8Array(rab);
    const pr = p.stdout.read(buf);
    rab.resize(128);
    const got = await withTimeout(pr, 5000, "resized read");
    assert(got === 3, "a pending read survives a grow-resize");
    assert(String.fromCharCode(buf[0], buf[1], buf[2]) === "xyz",
        "the bytes land in the resized view's current base");
    rab.resize(8);
    assert(buf[0] === 120, "shrinking afterwards keeps the written prefix visible");
    const r = await withTimeout(p.wait(), 5000, "wait");
    assert(r.code === 0, "child exits 0 after resize");
}

async function normalPendingReadStillWorks() {
    const p = new Spawn("sh", ["-c", "sleep 0.1; printf abc"]);
    const buf = new Uint8Array(64);
    const got = await withTimeout(p.stdout.read(buf), 5000, "live read");
    assert(got === 3, "a live pending read receives the bytes");
    assert(String.fromCharCode(buf[0], buf[1], buf[2]) === "abc", "and writes them into the view");
    const r = await withTimeout(p.wait(), 5000, "wait");
    assert(r.code === 0, "child exits 0");
}

await pendingThenTransfer();
await pendingThenTransferToFixed();
await resizeKeepsReadAlive();
await normalPendingReadStillWorks();

print("test_spawn_read_detach: all tests passed (" + n + " assertions)");
