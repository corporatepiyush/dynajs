// Fixture for tests/core/test_using_syntax.js: `using` and `await using` at the top level of a module are disposed
// in reverse order when the module body completes, after its last statement and before import() settles.
using a = globalThis.__usingRes(1);
await using b = globalThis.__usingRes(2);
globalThis.__usingLog.push("body");
export const v = 1;
