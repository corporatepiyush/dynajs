// covers: dyna:mathx.primes
// primes(n) compacts the sieve branchlessly (store i, advance by !sieve[i]) into a buffer sized count + 1, so the
// store past the last prime stays in bounds. The oracle is trial division.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_primes_sieve_oracle");
import { primes } from "dyna:mathx";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; print("FAIL: " + what); } }
function isPrime(x) {
    if (x < 2) return false;
    for (let d = 2; d * d <= x; d++) if (x % d === 0) return false;
    return true;
}

__kitS.section("every limit up to 3000 lists exactly the primes <= n", () => {
    const want = [];
    for (let n = 0; n <= 3000; n++) {
        if (isPrime(n)) want.push(n);
        const got = primes(n);
        let same = got.length === want.length;
        for (let i = 0; same && i < want.length; i++) same = got[i] === want[i];
        ok(same, "primes(" + n + ") has " + got.length + " entries, want " + want.length);
    }
});

__kitS.section("a large limit ending on a prime and one ending on a composite", () => {
    for (const n of [1000003, 1000000]) {
        const got = primes(n);
        let c = 0;
        for (let x = 2; x <= n; x++) if (isPrime(x)) c++;
        ok(got.length === c, "primes(" + n + ").length " + got.length + " want " + c);
        ok(got[got.length - 1] === (n === 1000003 ? 1000003 : 999983), "last prime of primes(" + n + ") is " + got[got.length - 1]);
        let sorted = true;
        for (let i = 1; i < got.length; i++) if (!(got[i] > got[i - 1])) sorted = false;
        ok(sorted, "primes(" + n + ") strictly increasing");
    }
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_mathx_primes_sieve_oracle: " + checks + " checks passed");
__kitS.end();
