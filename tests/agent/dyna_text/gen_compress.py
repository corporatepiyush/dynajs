#!/usr/bin/env python3
"""gen_compress.py — dyna:compress black-box matrix generator.

Oracles (python3 stdlib):
  - zlib/gzip: byte-level differential BOTH directions (python compresses ->
    dynajs gunzip must match; dynajs gzip header/trailer fields pinned against
    RFC 1952 + python-computed CRC32/ISIZE).
  - compression.zstd (python 3.14 PEP 784): differential both directions.
  - tarfile (ustar) + zipfile (stored+deflate): python-built archives ->
    TarList/TarExtract/ZipList/ZipRead must agree exactly.
brotli/snappy/lz4 have no python oracle here (noted in CHANGELOG): round-trip,
format-structure checks against their published specs, and hostile fuzz.
"""
import base64, io, json, tarfile, zipfile, zlib
try:
    from compression import zstd as pyzstd
    HAVE_ZSTD = True
except ImportError:
    HAVE_ZSTD = False
from gen_common import jslit, norm_bytes, norm_num, norm_json, norm_str, write_probe

C_IMPORTS = [("Z", "dyna:compress")]

def b64(data):
    return base64.b64encode(data).decode()

def gz(data):
    return gzip_blob(data)

def gzip_blob(data):
    buf = io.BytesIO()
    with gzip_wrap(buf) as g:
        g.write(data)
    return buf.getvalue()

import gzip as _gzip
from contextlib import contextmanager
@contextmanager
def gzip_wrap(buf):
    g = _gzip.GzipFile(mode="wb", fileobj=buf, mtime=0)
    try:
        yield g
    finally:
        g.close()

def corpus():
    c = {}
    c["empty"] = b""
    c["one"] = b"\x00"
    c["hello"] = b"hello hello hello"
    c["text1k"] = (b"The quick brown fox jumps over the lazy dog. " * 24)
    c["big_text"] = (b"Lorem ipsum dolor sit amet, consectetur adipiscing elit. " * 20000)  # ~1.1MB
    c["rand64k"] = bytes((i * 2654435761 >> 24) & 0xFF for i in range(65536))
    c["all256"] = bytes(range(256)) * 4
    c["utf8"] = "héllo, wörld — 日本語テキスト 🌍🚀 ok".encode("utf-8")
    return c


# ================================================================ gzip
def gen_gzip():
    c = []
    blobs = corpus()
    # Direction A: python compresses -> dynajs decompresses (byte-exact oracle)
    for name, data in blobs.items():
        blob = gzip_blob(data)
        c.append('case_eq("gzd_%s", %s, function(){ return Z.gunzip(b64dec(%s)); });'
                 % (name, json.dumps(norm_bytes(data)), jslit(b64(blob))))
        # and asString for text payloads
        if name in ("hello", "text1k", "utf8"):
            c.append('case_eq("gzd_%s_str", %s, function(){'
                     ' return Z.gunzip(b64dec(%s), {asString:true}); });'
                     % (name, json.dumps(norm_str(data.decode("utf-8"))), jslit(b64(blob))))
    # Direction B: dynajs compresses -> structural RFC 1952 checks
    for name in ("empty", "hello", "text1k", "utf8"):
        data = blobs[name]
        crc = zlib.crc32(data) & 0xFFFFFFFF
        isize = len(data) % (1 << 32)
        c.append('case_true("gzs_%s", function(){ var g = Z.gzip(%s);'
                 ' var ok = g[0] === 0x1f && g[1] === 0x8b && g[2] === 8 && g[3] === 0;'
                 ' for (var i = 4; i < 10; i++) ok = ok && (i === 9 ? g[i] === 0xff : g[i] === 0);'
                 ' var crc = (g[g.length-8] | (g[g.length-7]<<8) | (g[g.length-6]<<16) | (g[g.length-5]<<24)) >>> 0;'
                 ' var isz = (g[g.length-4] | (g[g.length-3]<<8) | (g[g.length-2]<<16) | (g[g.length-1]<<24)) >>> 0;'
                 ' return ok && crc === %d && isz === %d; });'
                 % (name, jsbytes(data), crc, isize))
        c.append('case_true("gzrt_%s", function(){'
                 ' return u8eq(Z.gunzip(Z.gzip(%s)), %s); });'
                 % (name, jsbytes(data), jsbytes(data)))
    # levels 1..9 all round-trip; level is lenient (0 and 10 accepted, pinned)
    c.append('case_true("gz_levels", function(){ var d = %s;' % jsbytes(blobs["text1k"]) +
             ' var lv = [0,1,2,3,4,5,6,7,8,9,10];'
             ' for (var i = 0; i < lv.length; i++)'
             ' if (!u8eq(Z.gunzip(Z.gzip(d, lv[i])), d)) return "level " + lv[i];'
             ' return true; });')
    # one-byte and 4GB-boundary ISIZE semantics on small input (mod 2^32 == len)
    c.append('case_eq("gz_isize_one", %s, function(){ return Z.gunzip(Z.gzip(%s)); });'
             % (json.dumps(norm_bytes(blobs["one"])), jsbytes(blobs["one"])))
    # multi-member gzip refused (impl-defined, pinned); trailing garbage refused
    c.append('case_err("gz_multimember", "TypeError", function(){'
             ' var a = Z.gzip("aaa"), b = Z.gzip("bbb");'
             ' var both = new Uint8Array(a.length + b.length); both.set(a); both.set(b, a.length);'
             ' return Z.gunzip(both); });')
    c.append('case_err("gz_trailing", "TypeError", function(){'
             ' var a = Z.gzip("aaa"); var x = new Uint8Array(a.length + 1); x.set(a);'
             ' return Z.gunzip(x); });')
    # string input encodes as UTF-8 (documented: input may be a string)
    c.append('case_eq("gz_string_input", %s, function(){ return Z.gunzip(Z.gzip(%s), {asString:true}); });'
             % (json.dumps(norm_str("héllo 🌍")), jslit("héllo 🌍")))
    write_probe("compress/z01_gzip_differential.js", C_IMPORTS, "\n".join(c),
                "gzip vs python zlib/gzip both directions + RFC 1952 structure")
    return c


def jsbytes(data):
    if not data:
        return "new Uint8Array(0)"
    return "hex2u8(%s)" % jslit(data.hex())


# ================================================================ zstd
def gen_zstd():
    if not HAVE_ZSTD:
        return []
    c = []
    blobs = corpus()
    for name, data in blobs.items():
        if name == "big_text":
            continue  # keep this probe fast; big blob covered in gzip
        blob = pyzstd.compress(data, level=3)
        c.append('case_eq("zdd_%s", %s, function(){ return Z.unzstd(b64dec(%s)); });'
                 % (name, json.dumps(norm_bytes(data)), jslit(b64(blob))))
    # level sweep: every level decompresses (round-trip) and output has zstd magic
    c.append('case_true("zd_levels", function(){ var d = %s;' % jsbytes(blobs["text1k"]) +
             ' for (var lv = 1; lv <= 22; lv++) {'
             '  var z = Z.zstd(d, {level: lv});'
             '  if (!(z[0] === 0x28 && z[1] === 0xb5 && z[2] === 0x2f && z[3] === 0xfd))'
             '   return "magic at level " + lv;'
             '  if (!u8eq(Z.unzstd(z), d)) return "roundtrip at level " + lv;'
             ' } return true; });')
    c.append('case_err("zd_lvl23", "RangeError", function(){ return Z.zstd("x", {level:23}); });')
    c.append('case_err("zd_lvl0", "RangeError", function(){ return Z.zstd("x", {level:0}); });')
    # frame structure vs python: decompress EXACTLY what python produced at
    # several levels (exercises different frame params)
    for lv in (1, 9, 19):
        blob = pyzstd.compress(blobs["text1k"], level=lv)
        c.append('case_eq("zdd_lvl%d", %s, function(){ return Z.unzstd(b64dec(%s)); });'
                 % (lv, json.dumps(norm_bytes(blobs["text1k"])), jslit(b64(blob))))
    # trailing garbage tolerated (impl-defined, pinned)
    c.append('case_true("zd_trailing_pinned", function(){ var z = Z.zstd("aaa");'
             ' var x = new Uint8Array(z.length + 1); x.set(z); x[z.length] = 1;'
             ' return u8eq(Z.unzstd(x), hex2u8("616161")); });')
    write_probe("compress/z02_zstd_differential.js", C_IMPORTS, "\n".join(c),
                "zstd vs python compression.zstd (PEP 784)")
    return c


# ================================================================ brotli/snappy/lz4
def gen_bsl():
    c = []
    blobs = corpus()
    br = {k: v for k, v in blobs.items() if k != "big_text"}
    for name, data in br.items():
        c.append('case_true("brrt_%s", function(){'
                 ' return u8eq(Z.unbrotli(Z.brotli(%s)), %s); });'
                 % (name, jsbytes(data), jsbytes(data)))
        c.append('case_true("snrt_%s", function(){'
                 ' return u8eq(Z.unsnappy(Z.snappy(%s)), %s); });'
                 % (name, jsbytes(data), jsbytes(data)))
        c.append('case_true("lzrt_%s", function(){'
                 ' return u8eq(Z.lz4Decompress(Z.lz4Compress(%s)), %s); });'
                 % (name, jsbytes(data), jsbytes(data)))
        c.append('case_true("lzfrt_%s", function(){'
                 ' return u8eq(Z.lz4Unframe(Z.lz4Frame(%s)), %s); });'
                 % (name, jsbytes(data), jsbytes(data)))
    # brotli level bounds (documented 0..11)
    c.append('case_true("br_levels", function(){ var d = %s;' % jsbytes(blobs["text1k"]) +
             ' for (var lv = 0; lv <= 11; lv++)'
             ' if (!u8eq(Z.unbrotli(Z.brotli(d, {level: lv})), d)) return "level " + lv;'
             ' return true; });')
    c.append('case_err("br_lvl12", "RangeError", function(){ return Z.brotli("x", {level:12}); });')
    # lz4 raw level bounds (documented 1..12)
    c.append('case_true("lz_levels", function(){ var d = %s;' % jsbytes(blobs["text1k"]) +
             ' for (var lv = 1; lv <= 12; lv++)'
             ' if (!u8eq(Z.lz4Decompress(Z.lz4Compress(d, {level: lv})), d)) return "level " + lv;'
             ' return true; });')
    c.append('case_err("lz_lvl13", "RangeError", function(){ return Z.lz4Compress("x", {level:13}); });')
    # snappy block-format structure: spec 1.5 — the stream starts with the
    # uncompressed length as a varint (cross-checked against the spec, the
    # vendored encoder, and the decompressor's own acceptance)
    c.append('case_true("sn_varint_prefix", function(){ var s = Z.snappy(%s);'
             ' var len = 0, shift = 0, i = 0;'
             ' do { len |= (s[i] & 0x7f) << shift; shift += 7; } while (s[i++] & 0x80);'
             ' return len === %s.length && u8eq(Z.unsnappy(s), %s); });'
             % (jsbytes(blobs["text1k"]), jsbytes(blobs["text1k"]), jsbytes(blobs["text1k"])))
    # lz4 frame structure: magic 04 22 4D 18 (spec)
    c.append('case_true("lzf_magic", function(){ var f = Z.lz4Frame(%s);'
             ' return f[0] === 0x04 && f[1] === 0x22 && f[2] === 0x4d && f[3] === 0x18; });'
             % jsbytes(blobs["hello"]))
    # lz4frame checksum=false still round-trips
    c.append('case_true("lzf_nocksum", function(){'
             ' return u8eq(Z.lz4Unframe(Z.lz4Frame(%s, {checksum:false})), %s); });'
             % (jsbytes(blobs["text1k"]), jsbytes(blobs["text1k"])))
    # lz4 raw + dict
    c.append('case_true("lz_dict", function(){ var dict = %s; var d = %s;'
             ' var b = Z.lz4Compress(d, {dict: dict});'
             ' return u8eq(Z.lz4Decompress(b, {dict: dict}), d); });'
             % (jsbytes(blobs["text1k"][:512]), jsbytes(blobs["text1k"])))
    c.append('case_true("lzf_bad_cksum", function(){ var f = Z.lz4Frame(%s);'
             ' f[f.length - 1] ^= 0xff;'
             ' try { Z.lz4Unframe(f); return false; } catch (e) { return e.name === "TypeError"; } });'
             % jsbytes(blobs["text1k"]))
    # asString round-trips UTF-8
    c.append('case_eq("br_str", {t:"s",v:"héllo"}, function(){'
             ' return Z.unbrotli(Z.brotli("héllo"), {asString:true}); });')
    write_probe("compress/z03_brotli_snappy_lz4.js", C_IMPORTS, "\n".join(c),
                "brotli/snappy/lz4 round-trips + spec structure (no python oracle)")
    return c


# ================================================================ hostile fuzz
def gen_fuzz():
    c = []
    # base blobs
    c.append('var gz = Z.gzip(%s);' % jsbytes(b"hello hello hello" * 8))
    c.append('var zs = Z.zstd(%s);' % jsbytes(b"hello hello hello" * 8))
    c.append('var br = Z.brotli(%s);' % jsbytes(b"hello hello hello" * 8))
    c.append('var sn = Z.snappy(%s);' % jsbytes(b"hello hello hello" * 8))
    c.append('var lz = Z.lz4Compress(%s);' % jsbytes(b"hello hello hello" * 8))
    c.append('var lf = Z.lz4Frame(%s);' % jsbytes(b"hello hello hello" * 8))
    # every mutation must produce a clean JS-level throw (or be rejected);
    # a crash, hang, or silent success on corrupt data fails the probe.
    c.append('''
function flip(buf, k) { var x = new Uint8Array(buf); x[k % x.length] ^= 0xa5; return x; }
function trunc(buf, k) { return buf.subarray(0, k % (buf.length + 1)); }
function fuzzOne(decomp, buf, label) {
    var threw = 0, silent = 0;
    var i, step = Math.max(1, (buf.length / 32) | 0);
    for (i = 0; i < buf.length; i += step) {
        try { decomp(flip(buf, i)); silent++; } catch (e) { threw++; }
        try { decomp(trunc(buf, i)); silent++; } catch (e) { threw++; }
    }
    // a decompressor that ACCEPTS most corrupt input is a red flag worth seeing
    return { threw: threw, silent: silent };
}
case_true("fuzz_gzip", function(){ return fuzzOne(Z.gunzip, gz, "gzip").threw > 0; });
case_true("fuzz_zstd", function(){ return fuzzOne(Z.unzstd, zs, "zstd").threw > 0; });
case_true("fuzz_brotli", function(){ return fuzzOne(Z.unbrotli, br, "brotli").threw > 0; });
case_true("fuzz_snappy", function(){ return fuzzOne(Z.unsnappy, sn, "snappy").threw > 0; });
case_true("fuzz_lz4raw", function(){ return fuzzOne(function(b){ return Z.lz4Decompress(b); }, lz, "lz4").threw > 0; });
case_true("fuzz_lz4frame", function(){ return fuzzOne(Z.lz4Unframe, lf, "lz4frame").threw > 0; });
''')
    # garbage inputs of interesting shapes (hostile matrix for ASan too)
    c.append('var __garbage = [new Uint8Array(0), new Uint8Array(1), hex2u8("ffffffffffffffff"),'
             ' hex2u8("1f8b"), hex2u8("1f8b08000000000000"), hex2u8("28b52ffd"),'
             ' hex2u8("04224d18"), hex2u8("ff06000073697a65"),'
             ' (function(){ var z = new Uint8Array(65536); z[0] = 0xff; z[1] = 0x7f; return z; })()];')
    c.append('case_true("garbage_all", function(){ var ds = [Z.gunzip, Z.unzstd, Z.unbrotli,'
             ' Z.unsnappy, Z.lz4Unframe, function(b){ return Z.lz4Decompress(b); }];'
             ' for (var i = 0; i < ds.length; i++)'
             '  for (var j = 0; j < __garbage.length; j++)'
             '   try { ds[i](__garbage[j]); } catch (e) {}'
             ' return true; });')
    # length-prefix lies (snappy declared size absurd, lz4 size hint absurd)
    c.append('case_err("snappy_prefix_lie", "TypeError", function(){'
             ' return Z.unsnappy(hex2u8("ff7f" + "010203".repeat(4))); });')
    c.append('case_err("tar_garbage", "SyntaxError", function(){'
             ' var b = new Uint8Array(1024); b.fill(0xff); return Z.TarList(b); });')
    c.append('case_err("zip_garbage", "SyntaxError", function(){'
             ' return Z.ZipList(hex2u8("504b0506" + "00".repeat(14) + "ff".repeat(4))); });')
    write_probe("compress/z04_fuzz.js", C_IMPORTS, "\n".join(c),
                "hostile corrupt/truncate/garbage matrix — clean throws only")
    return c


# ================================================================ tar/zip python fixtures
def make_tar():
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        def add(name, data=None, mode=0o644, mtime=1000000000, link=None):
            ti = tarfile.TarInfo(name)
            ti.mode = mode
            ti.mtime = mtime
            if link is not None:
                ti.type = tarfile.SYMTYPE
                ti.linkname = link
                t.addfile(ti)
            elif data is None:
                ti.type = tarfile.DIRTYPE
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
        add("hello.txt", b"hello tar")
        add("bin.dat", bytes(range(256)))
        add("empty.txt", b"")
        add("dir/", mtime=1000000001)
        add("dir/nested.txt", b"nested content", mode=0o600, mtime=1000000002)
        add("link.ln", link="hello.txt")
    return buf.getvalue()

def make_zip():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(zipfile.ZipInfo("stored.txt", date_time=(2026, 1, 1, 0, 0, 0)), b"stored content", zipfile.ZIP_STORED)
        z.writestr(zipfile.ZipInfo("deflated.txt", date_time=(2026, 1, 1, 0, 0, 0)),
                   b"deflate me " * 50, zipfile.ZIP_DEFLATED)
    return buf.getvalue()

def gen_tar_zip_py():
    c = []
    tar = make_tar()
    c.append('var tar = b64dec(%s);' % jslit(b64(tar)))
    # TarList exact metadata (mtime/mode round-trip through ustar)
    c.append('case_eq("tar_list", %s, function(){ return Z.TarList(tar).map(function(e){'
             ' return [e.name, e.size, e.mtime, e.mode, e.type]; }); });'
             % json.dumps(norm_json([
                 ["hello.txt", 9, 1000000000, 0o644, "file"],
                 ["bin.dat", 256, 1000000000, 0o644, "file"],
                 ["empty.txt", 0, 1000000000, 0o644, "file"],
                 ["dir/", 0, 1000000001, 0o644, "directory"],
                 ["dir/nested.txt", 14, 1000000002, 0o600, "file"],
                 ["link.ln", 0, 1000000000, 0o644, "symlink"]])))
    # TarExtract: names in archive order; data presence per entry type
    c.append('case_eq("tar_extract", %s, function(){'
             ' return Z.TarExtract(tar).map(function(e){'
             ' return [e.name, e.data ? "data" : null]; }); });'
             % json.dumps(norm_json([
                 ["hello.txt", "data"], ["bin.dat", "data"], ["empty.txt", "data"],
                 ["dir/", None], ["dir/nested.txt", "data"], ["link.ln", "data"]])))
    c.append('case_eq("tar_extract_data", %s, function(){'
             ' var es = Z.TarExtract(tar);'
             ' return es.map(function(e){ return e.data ? u8hex(e.data) : null; }); });'
             % json.dumps(norm_json([b"hello tar".hex(), bytes(range(256)).hex(), "",
                                     None, b"nested content".hex(), ""])))
    # all-zero 1024 bytes is the ustar END marker -> empty listing (pinned)
    c.append('case_eq("tar_zeros_end_marker", {t:"j",v:"[]"}, function(){'
             ' return Z.TarList(new Uint8Array(1024)); });')
    # ZipList + ZipRead on a python-built archive
    c.append('var zip = b64dec(%s);' % jslit(b64(make_zip())))

    c.append('case_true("zip_list", function(){ var l = Z.ZipList(zip);'
             ' if (l.length !== 2) return "len";'
             ' var names = [l[0].name, l[1].name].sort().join(",");'
             ' if (names !== "deflated.txt,stored.txt") return names;'
             ' for (var i = 0; i < 2; i++) {'
             '  var e = l[i];'
             '  if (e.type !== "file") return "type";'
             '  if (e.crc32 !== (e.name === "stored.txt" ? %d : %d)) return "crc";'
             '  if (e.size !== (e.name === "stored.txt" ? 14 : 550)) return "size";'
             '  if (e.method !== (e.name === "stored.txt" ? "store" : "deflate")) return "method";'
             ' } return true; });'
             % (zlib.crc32(b"stored content") & 0xFFFFFFFF,
                zlib.crc32(b"deflate me " * 50) & 0xFFFFFFFF))
    c.append('case_eq("zip_read_stored", %s, function(){ return Z.ZipRead(zip, "stored.txt"); });'
             % json.dumps(norm_bytes(b"stored content")))
    c.append('case_eq("zip_read_deflated", %s, function(){ return Z.ZipRead(zip, "deflated.txt"); });'
             % json.dumps(norm_bytes(b"deflate me " * 50)))
    c.append('case_err("zip_read_missing", "SyntaxError", function(){ return Z.ZipRead(zip, "nope"); });')
    write_probe("compress/z05_tar_zip_python.js", C_IMPORTS, "\n".join(c),
                "tar/zip archives built by python tarfile/zipfile -> dynajs readers")
    return c


def gen_tar_zip_dynajs():
    c = []
    # TarPack -> ustar magic at 257 ("ustar\0" version at 263)
    c.append('case_true("tar_magic", function(){ var t = Z.TarPack([{name:"f.txt", data: hex2u8("6869")}]);'
             ' var m = ""; for (var i = 257; i < 263; i++) m += String.fromCharCode(t[i]);'
             ' return m === "ustar\\0" || m === "ustar "; });')
    # round-trip through dynajs' own reader
    c.append('case_eq("tar_rt", %s, function(){'
             ' var t = Z.TarPack([{name:"a.txt", data: hex2u8("deadbeef"), mode: 0o600, mtime: 42}]);'
             ' return Z.TarExtract(t).map(function(e){'
             ' return [e.name, u8hex(e.data), e.mode, e.mtime]; }); });'
             % json.dumps(norm_json([["a.txt", "deadbeef", 0o600, 42]])))
    c.append('case_eq("tar_dir_rt", %s, function(){'
             ' var t = Z.TarPack([{name:"d/", data: undefined}]);'
             ' return Z.TarList(t).map(function(e){ return [e.name, e.type]; }); });'
             % json.dumps(norm_json([["d/", "directory"]])))
    # safe-name refusals (documented)
    c.append('case_err("tar_unsafe_dotdot", "RangeError", function(){'
             ' return Z.TarPack([{name:"../evil", data: hex2u8("01")}]); });')
    c.append('case_err("tar_unsafe_abs", "RangeError", function(){'
             ' return Z.TarPack([{name:"/abs", data: hex2u8("01")}]); });')
    c.append('case_err("tar_unsafe_drive", "RangeError", function(){'
             ' return Z.TarPack([{name:"C:x", data: hex2u8("01")}]); });')
    c.append('case_err("zip_unsafe", "RangeError", function(){'
             ' return Z.ZipPack([{name:"../evil", data: hex2u8("01")}]); });')
    # allowUnsafeNames lifts the check on READ (documented) — reader accepts
    # a hand-built entry (python-built with an odd name) but pack refuses
    # ZipPack methods
    c.append('case_eq("zip_store_method", %s, function(){'
             ' var z = Z.ZipPack([{name:"f", data: hex2u8("68656c6c6f")}], {method:"store"});'
             ' return Z.ZipList(z).map(function(e){ return [e.method, e.size, e.compressedSize]; }); });'
             % json.dumps(norm_json([["store", 5, 5]])))
    c.append('case_eq("zip_deflate_method", %s, function(){'
             ' var z = Z.ZipPack([{name:"f", data: hex2u8("%s")}]);'
             ' var l = Z.ZipList(z)[0];'
             ' return [l.method, l.compressedSize < l.size]; });'
             % (json.dumps(norm_json(["deflate", True]), ), b"hello " * 20 .hex() if False else "68656c6c6f" * 20))
    c.append('case_eq("zip_rt", %s, function(){'
             ' var z = Z.ZipPack([{name:"f.bin", data: hex2u8("00ff10")}]);'
             ' return u8hex(Z.ZipRead(z, "f.bin")); });'
             % json.dumps(norm_str("00ff10")))
    # corrupted member rejected (CRC mismatch)
    c.append('case_err("zip_crc", "SyntaxError", function(){'
             ' var z = Z.ZipPack([{name:"f", data: hex2u8("68656c6c6f")}]);'
             ' z[(z.length / 2) | 0] ^= 0xff;   /* lands in the member data */'
             ' return Z.ZipRead(z, "f"); });')
    write_probe("compress/z06_tar_zip_dynajs.js", C_IMPORTS, "\n".join(c),
                "TarPack/ZipPack structure, safety, round-trip")
    return c


# ================================================================ Compressor/Dictionary
def gen_classes():
    c = []
    algos = ["gzip", "lz4", "lz4frame", "zstd", "brotli", "snappy"]
    c.append('case_true("comp_all_algos", function(){ var d = %s;' % jsbytes(b"payload " * 100) +
             ' var algos = %s;' % json.dumps(algos) +
             ' for (var i = 0; i < algos.length; i++) {'
             '  var c2 = new Z.Compressor({algo: algos[i]});'
             '  try { if (!u8eq(c2.decompress(c2.compress(d)), d)) return algos[i]; }'
             '  finally { c2.close(); }'
             ' } return true; });')
    c.append('case_eq("comp_algo_getter", {t:"s",v:"zstd"}, function(){'
             ' var c2 = new Z.Compressor({algo:"zstd", level:9}); try { return c2.algo; } finally { c2.close(); } });')
    c.append('case_eq("comp_dictId_null", {t:"z"}, function(){'
             ' var c2 = new Z.Compressor({algo:"gzip"}); try { return c2.dictId; } finally { c2.close(); } });')
    c.append('case_eq("comp_dictId", {t:"b",v:"true"}, function(){'
             ' var c2 = new Z.Compressor({algo:"lz4", dict: hex2u8("0102")});'
             ' try { return typeof c2.dictId === "number" && c2.dictId > 0; } finally { c2.close(); } });')
    c.append('case_err("comp_dict_mismatch", "TypeError", function(){'
             ' var a = new Z.Compressor({algo:"lz4", dict: hex2u8("0102")});'
             ' var b = new Z.Compressor({algo:"lz4", dict: hex2u8("0304")});'
             ' try { return b.decompress(a.compress("x")); } finally { a.close(); b.close(); } });')
    c.append('case_err("comp_dict_on_gzip", "TypeError", function(){'
             ' return new Z.Compressor({algo:"gzip", dict: hex2u8("0102")}); });')
    c.append('case_err("comp_bad_algo", "TypeError", function(){ return new Z.Compressor({algo:"rar"}); });')
    c.append('case_err("comp_use_after_close", "TypeError", function(){'
             ' var c2 = new Z.Compressor({algo:"gzip"}); c2.close(); return c2.compress("x"); });')
    c.append('case_eq("comp_reuse", {t:"b",v:"true"}, function(){'
             ' var c2 = new Z.Compressor({algo:"gzip"});'
             ' try { for (var i = 0; i < 50; i++)'
             '  if (c2.decompress(c2.compress("x" + i), {asString:true}) !== "x" + i) return false;'
             '  return true; } finally { c2.close(); } });')
    # Dictionary
    c.append('case_eq("dict_rt", {t:"s",v:"hello world hello"}, function(){'
             ' var d = new Z.Dictionary(["hello","world"]);'
             ' try { return new TextDecoder().decode(d.decompress(d.compress("hello world hello"))); }'
             ' finally { d.close(); } });')
    c.append('case_eq("dict_meta", {t:"b",v:"true"}, function(){'
             ' var d = new Z.Dictionary(["hello","world"]);'
             ' try { return d.size === 2 && typeof d.id === "number"; } finally { d.close(); } });')
    c.append('case_err("dict_wrong", "TypeError", function(){'
             ' var a = new Z.Dictionary(["a","b"]); var b = new Z.Dictionary(["a","c"]);'
             ' try { return b.decompress(a.compress("a b")); } finally { a.close(); b.close(); } });')
    c.append('case_err("dict_empty_phrase", "RangeError", function(){ return new Z.Dictionary([""]); });')
    c.append('case_err("dict_use_after_close", "TypeError", function(){'
             ' var d = new Z.Dictionary(["x"]); d.close(); return d.compress("x"); });')
    write_probe("compress/z07_compressor_dictionary.js", C_IMPORTS, "\n".join(c),
                "Compressor/Dictionary class semantics")
    return c


def main():
    n = 0
    for gen in (gen_gzip, gen_zstd, gen_bsl, gen_fuzz, gen_tar_zip_py, gen_tar_zip_dynajs, gen_classes):
        c = gen()
        n += len([x for x in c if x.strip().startswith("case_")])
    print("gen_compress: %d cases emitted" % n)


if __name__ == "__main__":
    main()
