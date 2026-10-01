const n = parseInt(scriptArgs[1] || "100", 10);
const PARENT = "x".repeat(1000000);
const slices = [];
for (let i = 0; i < n; i++)
    slices.push(PARENT.substring(i * 100, i * 100 + 50));
globalThis.hold = slices;
globalThis.holdParent = PARENT;
print("held: parent=" + PARENT.length + "B window=50B slices=" + n);
