/* isolate: huge-cols tiny-nnz CSR probes, one per invocation (argv[2] selects).
 * Each is bounded: the runner wraps this in timeout. */
import { CSR, LinearRegression } from "dyna:ml";
import { getEnv } from "dyna:sys";
function errOf(fn) { try { return { v: fn() }; } catch (e) { return { e }; } }
const which = getEnv("WHICH") || "ctor";
const H = new CSR([1.0], [0], [0, 1], Math.pow(2, 40));
print("constructed cols=" + H.cols + " nnz=" + H.nnz);
if (which === "ctor") print("ctor ok");
else if (which === "row") {
    const r = errOf(() => H.row(0));
    print("row: " + (r.e ? r.e.constructor.name + ": " + r.e.message : "RETURNED len " + r.v.length));
} else if (which === "dense") {
    const r = errOf(() => H.toDense());
    print("toDense: " + (r.e ? r.e.constructor.name + ": " + r.e.message : "RETURNED"));
} else if (which === "fit") {
    const m = new LinearRegression();
    const t0 = Date.now();
    const r = errOf(() => m.fit(H, [1]));
    print("fit: " + (r.e ? r.e.constructor.name + ": " + r.e.message : "ACCEPTED") +
          " in " + (Date.now() - t0) + "ms");
} else if (which === "predict") {
    const m = new LinearRegression();
    m.fit([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [1, 2, 3]);
    const r = errOf(() => m.predict(H));
    print("predict: " + (r.e ? r.e.constructor.name + ": " + r.e.message : "RETURNED"));
}
H.close();
print("done");
