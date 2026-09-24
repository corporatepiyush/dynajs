// flags: --std
/* review probe: select() menu behavior + refusals. One select per mode;
 * select_driver.py owns the input bytes. */
import { select } from "dyna:cli";
const mode = scriptArgs[1] || "three";
if (mode === "three") {
    const r = select("Pick:", ["one", "two", "three"]);
    print("A=" + JSON.stringify(r));
} else if (mode === "nonstring-num") {
    try { select("P:", ["ok", 42]); print("A=NO-THROW"); }
    catch (e) { print("A=" + e.constructor.name + ":" + e.message); }
} else if (mode === "nonstring-null") {
    try { select("P:", [null]); print("A=NO-THROW"); }
    catch (e) { print("A=" + e.constructor.name + ":" + e.message); }
} else if (mode === "nonstring-obj") {
    try { select("P:", [{}]); print("A=NO-THROW"); }
    catch (e) { print("A=" + e.constructor.name + ":" + e.message); }
} else if (mode === "empty") {
    try { select("P:", []); print("A=NO-THROW"); }
    catch (e) { print("A=" + e.constructor.name + ":" + e.message); }
} else if (mode === "toobig") {
    try { select("P:", new Array(65537).fill("x")); print("A=NO-THROW"); }
    catch (e) { print("A=" + e.constructor.name + ":" + e.message); }
} else if (mode === "max") {
    const r = select("P:", new Array(65536).fill("q"));
    print("A=" + JSON.stringify(r));
} else if (mode === "getter-snapshot") {
    // a getter mutates the array while select snapshots it: the answer must
    // be the DECLARED string, not the mutated one
    const arr = ["first", "second"];
    let reads = 0;
    Object.defineProperty(arr, 1, { get() { reads++; return reads > 1 ? "HACKED" : "second"; },
                                    enumerable: true, configurable: true });
    const r = select("P:", arr);
    print("A=" + JSON.stringify(r) + " reads=" + reads);
}
