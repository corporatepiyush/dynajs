// converted transcript generator: record stream pinned by
// count + FNV-1a digest + first-40 records (node oracle; dynajs divergences are explicit)
__PIN_EXP_N = 40;
__PIN_EXP_LINES = ["E0x0|mul|85070591730234615865843651857942052864\nE0x0|p16|40000000000000000000000000000000\nE0x0|mod|0\nE0x2|mul|170141183460469231722463931679029329920\nE0x2|p16|7fffffffffffffff8000000000000000\nE0x2|mod|9223372036854775808\nE0x4|mul|1569275433846670190958947355801916604025588861116008628224\nE0x4|p16|400000000000000000000000000000000000000000000000\nE0x4|mod|9223372036854775808\nE0x6|mul|85070591730234615856620279821087277056\nE0x6|p16|3fffffffffffffff8000000000000000\nE0x6|mod|1\nE0x8|mul|-1998244521506527032641115761249058568700621588874848868245201441656675708940998367574142056320628869208930647116735140289574637041659786928905123497005976064702779421656001729087700914327020283932623635985058116674762012426586087809557833092382239611748320099926257577152464782846690339543279755965120866873870104706740825912925754742668376770182059892005381117185609765788642463356800007497307818519529050538130173510341041703284841551612510980882095478916117263161948555970525436177307906997913271773289202670357626383593276610046351815744623679181758043491615594666378901125478058883728003323535161416156043480629040032237534716059201133730007398968325329609947831244089443194094247422144587573438142965755398682099611080741865462434933723694944204203560364234439547610757933300921139200\nE0x8|p16|-3fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000000000000000000000\nE0x8|mod|9223372036854775808\nE0x10|mul|101428567446364111116729248231247920765458196314723554392970601893072405845668843030200070986340857307266970592742637242087950462132003173940376485539422670732748434960823712760139664084879307462912495665512557266401017343082601995084357576733560538404105387979940985194421259171266307174515682182485341121835417071972295434277542723285856596420793052758872587923968867957449414524436554480259034234281370611274982027138968348450398738177394555071673795288876304147734942893068449071008885357109461042484032631443835547967334709107397472834520612323022698285972766447285658350049075589888195455499739680779887419365235187203900325727987954061466036195474779471055480277696512\nE0x10|p16|7fffffffffffffff7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8000000000000000\nE0x10|mod|9223372036854775808\nE1x0|mul|-85070591730234615865843651857942052864\nE1x0|p16|-40000000000000000000000000000000\nE1x0|mod|0\nE1x2|mul|-170141183460469231722463931679029329920\nE1x2|p16|-7fffffffffffffff8000000000000000\nE1x2|mod|-9223372036854775808\nE1x4|mul|-1569275433846670190958947355801916604025588861116008628224\nE1x4|p16|-400000000000000000000000000000000000000000000000\nE1x4|mod|-9223372036854775808\nE1x6|mul|-85070591730234615856620279821087277056\nE1x6|p16|-3fffffffffffffff8000000000000000\nE1x6|mod|-1\nE1x8|mul|19982445215065270326411157612490585687006215888748488682452014416566757089409983675741420563206288692089306471167351402895746370416597869289051234970059760647027794216560017290877009143270202839326236359850581166747620124265860878095578330923", "HASH 9e3779b985ebca6b"];
__PIN_DYN_LINES = ["E0x0|mul|85070591730234615865843651857942052864\nE0x0|p16|40000000000000000000000000000000\nE0x0|mod|0\nE0x2|mul|170141183460469231722463931679029329920\nE0x2|p16|7fffffffffffffff8000000000000000\nE0x2|mod|9223372036854775808\nE0x4|mul|1569275433846670190958947355801916604025588861116008628224\nE0x4|p16|400000000000000000000000000000000000000000000000\nE0x4|mod|9223372036854775808\nE0x6|mul|85070591730234615856620279821087277056\nE0x6|p16|3fffffffffffffff8000000000000000\nE0x6|mod|1\nE0x8|mul|-1998244521506527032641115761249058568700621588874848868245201441656675708940998367574142056320628869208930647116735140289574637041659786928905123497005976064702779421656001729087700914327020283932623635985058116674762012426586087809557833092382239611748320099926257577152464782846690339543279755965120866873870104706740825912925754742668376770182059892005381117185609765788642463356800007497307818519529050538130173510341041703284841551612510980882095478916117263161948555970525436177307906997913271773289202670357626383593276610046351815744623679181758043491615594666378901125478058883728003323535161416156043480629040032237534716059201133730007398968325329609947831244089443194094247422144587573438142965755398682099611080741865462434933723694944204203560364234439547610757933300921139200\nE0x8|p16|-3fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000003fffffffffffffffc0000000000000000000000000000000\nE0x8|mod|9223372036854775808\nE0x10|mul|101428567446364111116729248231247920765458196314723554392970601893072405845668843030200070986340857307266970592742637242087950462132003173940376485539422670732748434960823712760139664084879307462912495665512557266401017343082601995084357576733560538404105387979940985194421259171266307174515682182485341121835417071972295434277542723285856596420793052758872587923968867957449414524436554480259034234281370611274982027138968348450398738177394555071673795288876304147734942893068449071008885357109461042484032631443835547967334709107397472834520612323022698285972766447285658350049075589888195455499739680779887419365235187203900325727987954061466036195474779471055480277696512\nE0x10|p16|7fffffffffffffff7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff8000000000000000\nE0x10|mod|9223372036854775808\nE1x0|mul|-85070591730234615865843651857942052864\nE1x0|p16|-40000000000000000000000000000000\nE1x0|mod|0\nE1x2|mul|-170141183460469231722463931679029329920\nE1x2|p16|-7fffffffffffffff8000000000000000\nE1x2|mod|-9223372036854775808\nE1x4|mul|-1569275433846670190958947355801916604025588861116008628224\nE1x4|p16|-400000000000000000000000000000000000000000000000\nE1x4|mod|-9223372036854775808\nE1x6|mul|-85070591730234615856620279821087277056\nE1x6|p16|-3fffffffffffffff8000000000000000\nE1x6|mod|-1\nE1x8|mul|19982445215065270326411157612490585687006215888748488682452014416566757089409983675741420563206288692089306471167351402895746370416597869289051234970059760647027794216560017290877009143270202839326236359850581166747620124265860878095578330923", "HASH 9e3779b985ebca6b"];
// special2.js — adversarial edge battery for Karatsuba mul + D&C toString
// Covers: all-0xFF limbs, 2^63/2^64 limb edges, TH=32 limb-boundary m/nh1 edges,
// exact power-of-2 chunk counts (radix 10 dpl=19, radix 36 dpl=18), max 16384 limbs,
// borrow-ripple patterns (zero low limbs), sign combinations.
// Deterministic; transcript must be byte-identical across engines (control/patched/node).
let rs = 0x0badf00d | 0;
function rnd32() {
  rs = (rs + 0x6D2B79F5) | 0;
  let t = Math.imul(rs ^ (rs >>> 15), 1 | rs);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return (t ^ (t >>> 14)) | 0;
}
function rndU() { return (rnd32() >>> 0); }
let h1 = 0x9e3779b9 | 0, h2 = 0x85ebca6b | 0;
function feed(s) {
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 2654435761) | 0;
    h2 = (Math.imul(h2 ^ c, 1597334677) + i) | 0;
  }
}
function emit(s) { feed(s); (typeof print === 'function' ? print : (x => process.stdout.write(x + "\n")))(s); }
function strHash(s) {
  let a = 0x811c9dc5 | 0, b = 0x1000193 | 0;
  for (let i = 0; i < s.length; i++) {
    a = Math.imul(a ^ s.charCodeAt(i), 16777619) | 0;
    b = (Math.imul(b + s.charCodeAt(i) | 0, 2654435761) + i) | 0;
  }
  return ((a >>> 0).toString(16) + (b >>> 0).toString(16));
}
const FULL_LIMIT = 40000;
function strTag(s) {
  if (s.length <= FULL_LIMIT) return s;
  return s.length + "|" + s.slice(0, 160) + "..." + s.slice(-160) + "|" + strHash(s);
}
const out = [];
function line(s) { out.push(s); if (out.length > 2048) { __PIN(out.join("\n") + "\n"); out.length = 0; } }
let fails = 0;
function chk(ok, tag) { if (!ok) { fails++; line("FAIL " + tag); } }
// ops that may exceed JS_BIGINT_MAX_SIZE throw RangeError by spec; record and continue
function tr(f) { try { return f(); } catch (e) { return (e instanceof RangeError) ? "<RANGE>" : "<ERR:" + e.name + ">"; } }
// Horner parse of signed base-radix string (BigInt() has no radix arg)
function parseBase(s, radix) {
  const neg = s[0] === '-';
  if (neg) s = s.slice(1);
  const R = BigInt(radix);
  let v = 0n;
  for (let i = 0; i < s.length; i++) {
    const c = parseInt(s[i], radix);
    if (!(c >= 0)) throw new SyntaxError("bad digit");
    v = v * R + BigInt(c);
  }
  return neg ? -v : v;
}

// random bigint with exactly `limbs` 64-bit limbs (all limbs random incl. high bit)
function rndLimbs(nlimbs, allowZero) {
  let v = 0n;
  for (let i = 0; i < nlimbs; i++) v = (v << 64n) | (BigInt(rndU()) << 32n) | BigInt(rndU());
  if (!allowZero) v |= (1n << BigInt(64 * nlimbs - 1)); // force full width
  if (rndU() & 1) v = -v;
  return v;
}

// ---- 2. limb edge values (2^63, 2^64-1, alternating high-bit limbs) ----
const edges = [
  1n << 63n, -(1n << 63n), (1n << 64n) - 1n, ((1n << 64n) - 1n) * ((1n << 64n) - 1n),
  (1n << 127n), ((1n << 64n) - 1n) << 64n, ((1n << 63n) - 1n),
];
// alternating 0x8000...0 limbs
let alt = 0n;
for (let i = 0; i < 40; i++) alt = (alt << 64n) | (i & 1 ? 0x8000000000000000n : 0x7fffffffffffffffn);
edges.push(alt, -alt);
// 0xFF..F followed by zero limbs (carry/normalize stress)
edges.push((((1n << 64n) - 1n) << (64n * 33n)), (((1n << 64n) - 1n) << (64n * 33n)) - 1n);
for (let i = 0; i < edges.length; i++) {
  for (let j = 0; j < edges.length; j += 2) {
    const a = edges[i], b = edges[j];
    const p = a * b;
    const tag = "E" + i + "x" + j;
    chk(b !== 0n ? p / b === a : true, tag + "div");
    line(tag + "|mul|" + strTag(p.toString(10)));
    line(tag + "|p16|" + strTag(p.toString(16)));
    if (b !== 0n) line(tag + "|mod|" + strTag((a % b).toString()));
  }
}

// ---- 3. TH boundary: nb exactly 31..35 limbs, both signs, high-bit set ----
for (let nb = 31; nb <= 35; nb++) {
  for (const na of [nb, nb + 1, nb - 1, 2 * nb, 39, 72]) {
    const a = rndLimbs(na, false), b = rndLimbs(nb, false);
    for (const [x, y] of [[a, b], [b, a], [-a, b], [a, -b], [-a, -b]]) {
      const p = x * y;
      chk(y !== 0n ? p / y === x : true, "TH" + na + "." + nb + "div");
      chk(y !== 0n ? p % y === 0n : true, "TH" + na + "." + nb + "mod0");
      line("TH" + na + "." + nb + "|p|" + strTag(p.toString(16)));
      line("TH" + na + "." + nb + "|p10|" + strTag(p.toString(10)));
    }
  }
}
// nh1/m boundary: nb=33 -> m=17, nh1=16; limb 16 = 0 or max
for (const pat of [0n, (1n << 64n) - 1n, 1n << 63n]) {
  for (const pos of [15, 16, 17]) {
    let b = 0n;
    for (let i = 0; i < 33; i++) b = (b << 64n) | (i === pos ? pat : (BigInt(rndU()) << 32n) | BigInt(rndU()));
    const a = rndLimbs(33, false);
    for (const [x, y] of [[a, b], [b, a], [-a, -b]]) {
      const p = x * y;
      chk(p / y === x, "NH1." + pos + "." + pat.toString(16) + "div");
      line("NH1." + pos + "." + pat.toString(16) + "|p|" + strTag(p.toString(10)));
    }
  }
}
// zero-low-limb borrow-ripple patterns
for (const sh of [16, 17, 32, 33, 34, 64]) {
  const hi = rndLimbs(10, false);
  const a = hi << BigInt(64 * sh);
  const b = rndLimbs(12, false) << BigInt(64 * sh);
  const p = a * b;
  chk(p / a === b, "ZL" + sh + "div");
  line("ZL" + sh + "|p|" + strTag(p.toString(10)));
  line("ZL" + sh + "|p3|" + strTag(p.toString(3)));
}

// ---- 4. exact power-of-2 chunk counts ----
// radix 10: dpl=19 -> chunk counts c = 19-digit groups
const pows = [0,1,2,3,4,5,6,7,8,9,10,11,12];
for (const k of pows) {
  for (const delta of [0, 1, -1]) {
    const chunks = (1 << k) + (delta < 0 ? -1 : delta);
    if (chunks < 1) continue;
    const digits = 19 * chunks; // exactly `chunks` chunks when d digits with 10^(d-1) <= v
    const vmax = 10n ** BigInt(digits) - 1n;   // all 9s
    const vmin = 10n ** BigInt(digits - 1);    // exactly `digits` digits
    for (const [v, nm] of [[vmax, "9s"], [vmin, "1e"], [vmin + BigInt(rndU() % 1000000), "rr"]]) {
      for (const s of [1n, -1n]) {
        const x = s * v;
        const t = x.toString(10);
        chk(t.length === digits + (s < 0n ? 1 : 0), "PW" + k + "." + delta + nm + "len");
        chk(BigInt(t) === x, "PW" + k + "." + delta + nm + "rt");
        line("PW" + k + "." + delta + nm + (s < 0n ? "m" : "p") + "|" + strTag(t));
        line("PW" + k + "." + delta + nm + "b36|" + strTag(x.toString(36)));
      }
    }
    // mul whose product lands exactly on a chunk boundary
    const p = vmax * vmin;
    line("PW" + k + "." + delta + "mul|" + strTag(p.toString(10)));
    chk(p / vmin === vmax, "PW" + k + "." + delta + "muldiv");
  }
}
// radix 36: dpl=18
for (const k of [0,1,2,3,4,5,6,7,8,9,10]) {
  for (const delta of [0, 1, -1]) {
    const chunks = (1 << k) + (delta < 0 ? -1 : delta);
    if (chunks < 1) continue;
    const digits = 18 * chunks;
    const base = 36n ** BigInt(digits);
    const v = base - 1n;
    const t = v.toString(36);
    chk(t.length === digits && t[0] === 'z', "Q36." + k + "." + delta + "len");
    chk(parseBase(t, 36) === v, "Q36." + k + "." + delta + "rt");
    line("Q36." + k + "." + delta + "|max|" + strTag(t));
    line("Q36." + k + "." + delta + "|dec|" + strTag(v.toString(10)));
  }
}
// radix 7 (odd, non-pow2, dpl=33 per 64 bits: floor(64/log2(7))=22? keep round-trip only)
for (const k of [0,2,4,6]) {
  const digits = 10n ** 3n * BigInt(1 << k);
  const v = 10n ** digits - 1n;
  line("R7." + k + "|dec2str7|" + strTag(v.toString(7)));
  chk(parseBase(v.toString(7), 7) === v, "R7." + k + "rt");
}

// ---- 6. sign combos across the D&C toString threshold ----
for (const digits of [1216, 1217, 2432, 4864, 9728, 19456]) {
  const v = 10n ** BigInt(digits) - 1n;
  for (const s of [1n, -1n]) {
    const t = (s * v).toString(10);
    chk(BigInt(t) === s * v, "SG" + digits + "rt");
    line("SG" + digits + (s < 0n ? "m" : "p") + "|" + strTag(t));
  }
}

line("SPECIAL2 fails=" + fails);
// ---- 1. all-0xFF limbs ----
const ffs = [1,2,3,4,5,8,16,17,32,33,34,63,64,65,100,127,128,129,200,255,256,511,512,1024,2048,4096,8191,8192,16383,16384];
for (const k of ffs) {
  const v = tr(() => (1n << BigInt(64 * k)) - 1n); // k limbs all 0xFF (16384 throws: needs limb 16384)
  if (typeof v !== 'bigint') { line("FF" + k + "|mk|<RANGE>"); continue; }
  for (const s of [1n, -1n]) {
    const x = tr(() => s * v);
    if (typeof x !== 'bigint') { line("FF" + k + (s < 0n ? "m" : "p") + "|neg|" + x); continue; }
    const tag = "FF" + k + (s < 0n ? "m" : "p");
    line(tag + "|hex|" + strTag(x.toString(16)));
    line(tag + "|dec|" + strTag(x.toString(10)));
    line(tag + "|b36|" + strTag(x.toString(36)));
    chk((x * 3n) / 3n === x, tag + "mul3div");
    const y = rndLimbs(Math.min(k, 40), false);
    const p = tr(() => x * y);
    if (p !== "<RANGE>") chk(p / y === x, tag + "muldiv");
    line(tag + "|mulFFy|" + strTag(String(p) === p ? p : p.toString(10)));
    if (k <= 512) {
      line(tag + "|u64|" + strTag(BigInt.asUintN(64 * k, x).toString()));
      line(tag + "|i64x|" + strTag(BigInt.asIntN(64 * k, x).toString()));
    }
    if (k >= 4) {
      const q = tr(() => x / (1n << BigInt(64 * (k >> 1))));
      line(tag + "|shiftdiv|" + strTag(typeof q === 'bigint' ? q.toString(10) : String(q)));
    }
  }
}

// ---- 5. max-size edges (16384-limb class) ----
// max positive = 2^1048575 - 1 (top limb 0x7FFF...). (1n << 1048575n) alone throws
// (needs a 16385th limb), so build it as a mul + add that never exceed 16384 limbs.
const A = 1n << 524287n;                  // 8193 limbs (sign-extended positive)
const B = (1n << 524287n) - 1n;           // 8192 limbs, top limb 0x7FFF...
const MAXV = B << 524287n;                // 2^1048574 - 2^524287, 16384 limbs via shift
const MAXNEG = -MAXV;                     // top limb 0x8000...
{
  chk(MAXV.toString(16).length === 262144, "MAXhexLen");
  const t10 = MAXV.toString(10);
  chk(tr(() => BigInt(t10) === MAXV) === true, "MAXrt"); // parse of max-size throws by engine limit (pre-existing); must match control
  chk(t10.length >= 315650 && t10.length <= 315655, "MAXdecLen=" + t10.length);
  line("MAX|dec|" + strTag(t10));
  line("MAX|neg|" + strTag(MAXNEG.toString(10)));
  chk(tr(() => BigInt(MAXNEG.toString(10)) === MAXNEG) === true, "MAXNEGrten");
  line("MAX|b36|" + strTag(MAXV.toString(36)));
  line("MAX|x3|" + String(tr(() => MAXV * 3n)));
  const half = MAXV >> (64n * 8192n);
  const p = half * half;
  chk(tr(() => p / half) === half, "MAXsqdiv");
  line("MAX|sq|" + strTag(tr(() => p.toString(10))));
  line("MAX|q|" + String(tr(() => MAXV / (1n << (64n * 8191n)))));
  line("MAX|ffsq|" + String(tr(() => MAXV * MAXV)));
  const half3 = (1n << (64n * 8191n)) - 1n;
  const p2 = half3 * half3; // 16383-limb(ish) operands -> 16365-limb product, inside limit
  chk(tr(() => p2 / half3) === half3, "MAXhalf3sqdiv");
  line("MAX|h3sq|" + strTag(tr(() => p2.toString(10))));
  line("MAX|negdiv|" + String(tr(() => (MAXNEG / half3).toString(10))));
  chk(tr(() => (MAXNEG / half3) * half3 + (MAXNEG % half3) === MAXNEG) === true, "MAXnegdivrt");
}



__PIN(out.join("\n") + "\n");
__PIN("HASH " + ((h1 >>> 0).toString(16)) + ((h2 >>> 0).toString(16)));

__A("transcript-record-count", function () { assert_eq(__PIN_count, 2); });
__A("transcript-digest-fnv1a", function () { assert_diverge((__PIN_h1 >>> 0).toString(16), "d79ea73a", "52547b6b", "fnv1a"); });
summary("ev_bigint");
