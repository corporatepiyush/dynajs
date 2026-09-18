// bignum_crypto.js — number-theory primitives and a toy RSA built on BigInt:
// modular exponentiation, extended-Euclid modular inverse, deterministic
// Miller-Rabin primality, reproducible prime generation, and RSA
// keygen/encrypt/decrypt/sign/verify.
//
// This is a *teaching* implementation (small key sizes, deterministic PRNG) —
// not for real security. It exercises the engine's BigInt arithmetic hard.
//
// Engine features exercised: BigInt literals and operators (** % / & >>),
// BigInt64-free bit manipulation, and byte<->BigInt message encoding.

import { test, run, assert, assertEqual, assertThrows, rng } from "./harness.js";

/** (base ** exp) mod m, via right-to-left square-and-multiply. */
export function modPow(base, exp, m) {
  if (m === 1n) return 0n;
  let result = 1n;
  base %= m;
  while (exp > 0n) {
    if (exp & 1n) result = (result * base) % m;
    exp >>= 1n;
    base = (base * base) % m;
  }
  return result;
}

/** Extended Euclidean gcd: returns [g, x, y] with a*x + b*y = g. */
export function egcd(a, b) {
  let [oldR, r] = [a, b];
  let [oldS, s] = [1n, 0n];
  let [oldT, t] = [0n, 1n];
  while (r !== 0n) {
    const q = oldR / r;
    [oldR, r] = [r, oldR - q * r];
    [oldS, s] = [s, oldS - q * s];
    [oldT, t] = [t, oldT - q * t];
  }
  return [oldR, oldS, oldT];
}

/** Modular inverse of a mod m, or throws if none exists. */
export function modInverse(a, m) {
  const [g, x] = egcd(((a % m) + m) % m, m);
  if (g !== 1n) throw new Error(`no inverse: gcd(${a}, ${m}) = ${g}`);
  return ((x % m) + m) % m;
}

// Deterministic Miller-Rabin: this witness set is a *proven* primality test for
// all n < 3.3e24 (~2^81), which comfortably covers our <=64-bit primes.
const MR_WITNESSES = [2n, 3n, 5n, 7n, 11n, 13n, 17n, 19n, 23n, 29n, 31n, 37n];

export function isPrime(n) {
  if (n < 2n) return false;
  for (const p of MR_WITNESSES) {
    if (n === p) return true;
    if (n % p === 0n) return false;
  }
  // Write n-1 = d * 2^r with d odd.
  let d = n - 1n;
  let r = 0n;
  while ((d & 1n) === 0n) { d >>= 1n; r += 1n; }

  witnessLoop:
  for (const a of MR_WITNESSES) {
    let x = modPow(a, d, n);
    if (x === 1n || x === n - 1n) continue;
    for (let i = 1n; i < r; i++) {
      x = (x * x) % n;
      if (x === n - 1n) continue witnessLoop;
    }
    return false; // composite
  }
  return true;
}

/** Build a random BigInt with exactly `bits` bits (top bit set) from a PRNG. */
function randomBigInt(bits, next) {
  let n = 1n; // ensure the high bit is set
  for (let i = 1; i < bits; i++) {
    n = (n << 1n) | (next() < 0.5 ? 0n : 1n);
  }
  return n;
}

/** Find a prime near a random `bits`-bit start by scanning odd candidates. */
export function randomPrime(bits, next) {
  let candidate = randomBigInt(bits, next) | 1n; // make odd
  for (let tries = 0; tries < 10000; tries++) {
    if (isPrime(candidate)) return candidate;
    candidate += 2n;
  }
  throw new Error("failed to find a prime (should not happen)");
}

/**
 * Generate an RSA keypair. `bits` is the size of each prime, so the modulus is
 * ~2*bits bits. Public exponent defaults to 65537.
 */
export function generateKeypair(bits, next, e = 65537n) {
  let p, q, n, phi;
  do {
    p = randomPrime(bits, next);
    do { q = randomPrime(bits, next); } while (q === p);
    n = p * q;
    phi = (p - 1n) * (q - 1n);
  } while (egcd(e, phi)[0] !== 1n); // ensure e is invertible mod phi
  const d = modInverse(e, phi);
  return { publicKey: { e, n }, privateKey: { d, n }, p, q };
}

export function encrypt(message, { e, n }) {
  if (message < 0n || message >= n) throw new RangeError("message out of range [0, n)");
  return modPow(message, e, n);
}

export function decrypt(cipher, { d, n }) {
  return modPow(cipher, d, n);
}

/** Textbook signing: sign with the private key, verify with the public key. */
export function sign(message, privateKey) {
  return modPow(message, privateKey.d, privateKey.n);
}

export function verify(message, signature, publicKey) {
  return modPow(signature, publicKey.e, publicKey.n) === message;
}

// --- string <-> BigInt (big-endian byte packing) ----------------------------

export function textToBigInt(text) {
  let n = 0n;
  for (const ch of text) n = (n << 8n) | BigInt(ch.charCodeAt(0) & 0xff);
  return n;
}

export function bigIntToText(n) {
  const bytes = [];
  while (n > 0n) {
    bytes.unshift(Number(n & 0xffn));
    n >>= 8n;
  }
  return String.fromCharCode(...bytes);
}

// --- tests -------------------------------------------------------------------

test("modPow matches known answers", () => {
  assertEqual(modPow(4n, 13n, 497n), 445n);
  assertEqual(modPow(2n, 10n, 1000n), 24n);
  assertEqual(modPow(0n, 0n, 7n), 1n);
  assertEqual(modPow(123456789n, 0n, 7n), 1n);
  // Fermat's little theorem: a^(p-1) ≡ 1 (mod p) for prime p.
  assertEqual(modPow(2n, 100n - 1n + 1n, 101n) % 101n, modPow(2n, 100n, 101n));
  assertEqual(modPow(3n, 100n, 101n), 1n);
});

test("modular inverse round-trips", () => {
  assertEqual((3n * modInverse(3n, 11n)) % 11n, 1n);
  assertEqual((17n * modInverse(17n, 3120n)) % 3120n, 1n);
  assertThrows(() => modInverse(6n, 9n), "no inverse"); // gcd(6,9)=3
});

test("primality test classifies knowns", () => {
  for (const p of [2n, 3n, 5n, 97n, 7919n, 104729n, 2147483647n]) assert(isPrime(p), `${p} prime`);
  for (const c of [1n, 4n, 100n, 7917n, 104730n, 3215031751n]) assert(!isPrime(c), `${c} composite`);
  // 3215031751 is a classic strong pseudoprime to bases 2,3,5,7 — MR with more
  // witnesses must still reject it.
});

test("RSA encrypt/decrypt round-trip (reproducible keygen)", () => {
  const next = rng(0xBADC0DE);
  const { publicKey, privateKey } = generateKeypair(48, next);
  assert(publicKey.n > 0n);
  for (const m of [0n, 1n, 2n, 42n, 1000003n, publicKey.n - 1n]) {
    const c = encrypt(m, publicKey);
    assertEqual(decrypt(c, privateKey), m);
  }
});

test("RSA signatures verify and detect tampering", () => {
  const next = rng(0x5EED);
  const { publicKey, privateKey } = generateKeypair(48, next);
  const message = 123456789n % publicKey.n;
  const signature = sign(message, privateKey);
  assert(verify(message, signature, publicKey));
  assert(!verify(message + 1n, signature, publicKey)); // wrong message
  assert(!verify(message, signature + 1n, publicKey)); // wrong signature
});

test("text encryption round-trip", () => {
  const next = rng(0xF00D);
  const { publicKey, privateKey } = generateKeypair(64, next);
  const secret = "hi!"; // 3 bytes < 128-bit modulus
  const m = textToBigInt(secret);
  assert(m < publicKey.n);
  const c = encrypt(m, publicKey);
  assertEqual(bigIntToText(decrypt(c, privateKey)), secret);
});

test("out-of-range messages are rejected", () => {
  const next = rng(1);
  const { publicKey } = generateKeypair(32, next);
  assertThrows(() => encrypt(publicKey.n, publicKey), "out of range");
  assertThrows(() => encrypt(-1n, publicKey), "out of range");
});

await run("BigInt crypto (toy RSA)");
