// 10k-hole dictionary: one occupied entry at index 10000.
// Hit counts scale linearly with length: at 1e6 holes the base engine
// performs ~1e6 of the same JS_GetPropertyInt64/JS_TryGetPropertyInt64
// calls per includes/indexOf (see bench P1/P2: 18.7ms / 28.2ms per op).
let a = []; a[10000] = 1;
a.includes(42);
a.indexOf(42);
