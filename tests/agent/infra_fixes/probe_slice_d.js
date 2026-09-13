/* probe_slice_d.js -- -d accounting probe for live sliced strings (INFRA 4).
 *
 * Holds ONE flat 1 MB narrow parent plus N live slices of it, then exits, so
 * the CLI's -d dump reports the strings row for exactly this shape:
 *
 *   dynajs -d probe_slice_d.js <N>
 *
 * Old model: each slice was charged sizeof(JSString)+window as if the window
 * were its own allocation, while the parent's full cost was divided by its
 * refcount but visited only ONCE -- so a 1 MB parent was reported as ~9 KB
 * and the reported total went DOWN as you held MORE slices. New model: a
 * slice is charged its 28-byte prefix block (16 payload + 12 header) and the
 * parent's cost is charged once per holding reference, so the parent is
 * counted exactly once and each extra slice adds exactly 28 bytes.
 */
const n = parseInt(scriptArgs[1] || "100", 10);
const PARENT = "x".repeat(1000000);
const slices = [];
for (let i = 0; i < n; i++)
    slices.push(PARENT.substring(i * 100, i * 100 + 50));
globalThis.hold = slices;          // keep every slice reachable at exit
globalThis.holdParent = PARENT;    // and the parent
print("held: parent=" + PARENT.length + "B window=50B slices=" + n);
