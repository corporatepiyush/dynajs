// covers: dyna:compress.gzip
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gzip_dictionary");
import { Dictionary, gzip, gunzip } from "dyna:compress";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throws(fn, msg) {
    n++;
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (caught === null) throw new Error("assertion failed: " + msg + " (expected throw)");
    return caught;
}

const enc = new TextEncoder();
const dec = new TextDecoder();
const bytes = (s) => enc.encode(s);
const text = (b) => dec.decode(b);

const RPC = ['"jsonrpc":"2.0"', '"method":', '"params":', '"id":',
             '"result":', '"error":', '{"', '"}', '":"', '","'];

__kitS.section("templated record round-trips", () => {
    const d = new Dictionary(RPC);

    const rpc = '{"jsonrpc":"2.0","method":"sum","params":[1,2],"id":7}';
    const packed = d.compress(bytes(rpc));
    assert(text(d.decompress(packed)) === rpc, "templated record round-trips");
    assert(packed.length < rpc.length,
        "a templated record SHRINKS (" + rpc.length + " -> " + packed.length + ")");
    assert(packed.length < gzip(bytes(rpc)).length,
        "and beats gzip on a payload this short (" + packed.length + " vs " +
        gzip(bytes(rpc)).length + ")");

    const noise = "zqx".repeat(30);
    const grown = d.compress(bytes(noise));
    assert(text(d.decompress(grown)) === noise, "unmatched bytes still round-trip");
    assert(grown.length > noise.length,
        "a payload with no phrases EXPANDS (" + noise.length + " -> " +
        grown.length + ") -- the honest losing case");

    const gz = gzip(bytes(rpc.repeat(20)));
    const twice = d.compress(gz);
    assert(text(gunzip(d.decompress(twice))) === rpc.repeat(20),
        "compressing already-compressed bytes still round-trips");
    assert(twice.length >= gz.length,
        "already-compressed input does not shrink further");

    assert(d.compress(bytes("")).length === 8, "the empty input is header-only");
    assert(d.decompress(d.compress(bytes(""))).length === 0, "empty round-trips");
});
__kitS.end();

print("test_dictionary: all " + n + " assertions passed");
