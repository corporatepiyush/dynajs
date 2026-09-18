// Shared portable harness for the method-fusion equivalence probes.
// Each probe file ends with `MF.sum(name)`; the runner byte-compares stdout
// between engines, so only deterministic output is allowed here.
function MFHarness() {
  var n = 0, bad = 0;
  function check(name, cond) {
    n++;
    if (!cond) {
      bad++;
      console.log("FAIL", name);
    }
  }
  function eq(name, got, want) {
    check(name + "=" + got, got === want);
  }
  function near(name, got, want) {
    // NaN-aware scalar equality for number results
    check(name + "=" + got,
          got === want || (typeof got === "number" && typeof want === "number" &&
                           isNaN(got) && isNaN(want)));
  }
  function sum(label) {
    console.log(label, "probes=" + n, "failed=" + bad);
    return bad === 0 ? 0 : 1;
  }
  return { check: check, eq: eq, near: near, sum: sum };
}
