// counterfactual: identical switch with literal labels (constprop inert)
function step(op, x, y) {
    switch (op) {
        case 0: return -1;
        case 1: return x;
        case 2: return 0;
        case 3: return x + y;
        case 4: return x - y;
        case 5: return (x * y) | 0;
        case 6: return y === 0 ? 0 : (x / y) | 0;
        case 7: return -x;
        case 8: return x * 2;
        case 9: return y * 100 + x;
        case 10: return x + 1;
        case 11: return x === 0 ? 1 : 0;
        case 12: return x > 0 ? 2 : 0;
        case 13: return x - 1;
        case 14: return x + 1;
        case 15: return x * 3;
        case 16: return x;
        case 17: return x ^ y;
        default: return -999;
    }
}
const N = 600000;
let acc = 0;
for (let i = 0; i < N; i++) {
    for (let op = 0; op < 18; op++) {
        acc = (acc + step(op, i & 1023, (i >> 3) & 7)) | 0;
    }
}
console.log("acc", acc);
