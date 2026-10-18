// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the matcher sections.
import { Levenshtein } from "dyna:matcher";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- M1c-23 ------------------------------------------------------------------------
{
    const ref = (a, b) => {
        let prev = new Array(b.length + 1);
        for (let j = 0; j <= b.length; j++) prev[j] = j;
        for (let i = 1; i <= a.length; i++) {
            const cur = [i];
            for (let j = 1; j <= b.length; j++)
                cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
            prev = cur;
        }
        return prev[b.length];
    };
    let bad = 0, n = 0;
    for (const m of [63, 64, 65, 66, 97]) {
        for (const k of [m, m + 1, 2 * m - 1, 2 * m, 2 * m + 1, 2 * m + 3, 3 * m, 5 * m]) {
            const variants = [
                ["x".repeat(m), "y".repeat(k)],
                ["ab".repeat(m).slice(0, m), "ba".repeat(k).slice(0, k)],
                ["x".repeat(m), "x".repeat(k)],
                ["\u0416".repeat(m), "\u0416\u0429".repeat(k).slice(0, k)],
            ];
            for (const [a, b] of variants) {
                n++;
                const w1 = ref(a, b);
                if (Levenshtein(a, b) !== w1 || Levenshtein(b, a) !== w1) {
                    if (bad++ < 4) print("  FAIL: Levenshtein len " + a.length + "/" + b.length + " got " + Levenshtein(a, b) + " want " + w1);
                }
            }
        }
    }
    checks++;
    if (bad) failures++;
    print("  Levenshtein differential: " + n + " pairs, " + bad + " mismatches");
    eq(Levenshtein("x".repeat(70), "y".repeat(200), { max: 10 }), 11, "Levenshtein { max } early-out (control)");
}
if (failures) {
    print("test_matcher_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_matcher_audit_w2_native_1 failed");
}
print("test_matcher_audit_w2_native_1: " + checks + " checks passed");
