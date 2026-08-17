/* bench_codec_zstd.js -- zstd/brotli/snappy ns/op over reps (plan 3.16).
 * Emits machine-readable #DATA lines: case, size, ns/op, MB/s.
 * Run: dynajs tests/bench_codec_zstd.js
 */
import { zstd, unzstd, brotli, unbrotli, snappy, unsnappy }
    from "dyna:compress";

const TEXT = "the quick brown fox jumps over the lazy dog ".repeat(64);

function bench(name, size, fn) {
    fn(); fn();
    let b = Infinity;
    for (let t = 0; t < 5; t++) {
        const t0 = performance.now();
        fn();
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    const ns = b * 1e6;
    const mbs = size / 1048576 / (b / 1000);
    print(`#DATA\t${name}\t${ns.toFixed(1)}\t${mbs.toFixed(1)}`);
}

const txt = new TextEncoder().encode(TEXT);
const zeros = new Uint8Array(1 << 20);

for (const [label, data] of [["text", txt], ["zeros", zeros]]) {
    const z = zstd(data);
    const br = brotli(data);
    const sn = snappy(data);
    bench("zstd_compress_" + label, data.length, () => zstd(data));
    bench("zstd_decompress_" + label, data.length, () => unzstd(z));
    bench("brotli_compress_" + label, data.length, () => brotli(data));
    bench("brotli_decompress_" + label, data.length, () => unbrotli(br));
    bench("snappy_compress_" + label, data.length, () => snappy(data));
    bench("snappy_decompress_" + label, data.length, () => unsnappy(sn));
}
print("done.");
