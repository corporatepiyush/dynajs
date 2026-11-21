// covers: dyna:random.Random.choice
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_choice");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("choice returns a member", () => {
    const pop = [7, 8, 9, 10];
    const r = new Random(99);
    for (let i = 0; i < 50; i++)
        assert(pop.includes(r.choice(pop)), "choice returns a member");
    let threw = null;
    try { new Random(1).choice([]); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "choice on empty is a RangeError");
    const hole = [1, , 3];
    const v = new Random(5).choice(hole);
    assert(v === undefined || v === 1 || v === 3, "choice over a hole yields undefined or a value");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
