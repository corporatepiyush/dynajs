// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// tp02: dispatch over a const object is identical to the literal-label
// version, exhaustively over all opcodes and misses. 12 checks.
const OP = { HALT: 0, PUSH: 1, ADD: 2, SUB: 3, MUL: 4, DUP: 5, SWAP: 6,
             DROP: 7, JMP: 8, JZ: 9, LD: 10, ST: 11, PRINT: 12, NEG: 13,
             MOD: 14, DIV: 15, LT: 16, GT: 17 };

function step(op, x, y) {
    switch (op) {
        case OP.HALT: return -1;
        case OP.PUSH: return x;
        case OP.ADD: return x + y;
        case OP.SUB: return x - y;
        case OP.MUL: return (x * y) | 0;
        case OP.DUP: return x * 2;
        case OP.SWAP: return y * 100 + x;
        case OP.DROP: return 0;
        case OP.JMP: return x + 1;
        case OP.JZ: return x === 0 ? 1 : 0;
        case OP.LD: return x - 1;
        case OP.ST: return x + 1;
        case OP.PRINT: return x * 3;
        case OP.NEG: return -x;
        case OP.MOD: return y === 0 ? 0 : x % y;
        case OP.DIV: return y === 0 ? 0 : (x / y) | 0;
        case OP.LT: return x < y ? 1 : 0;
        case OP.GT: return x > y ? 1 : 0;
        default: return -2;
    }
}

function stepLit(op, x, y) {
    switch (op) {
        case 0: return -1;
        case 1: return x;
        case 2: return x + y;
        case 3: return x - y;
        case 4: return (x * y) | 0;
        case 5: return x * 2;
        case 6: return y * 100 + x;
        case 7: return 0;
        case 8: return x + 1;
        case 9: return x === 0 ? 1 : 0;
        case 10: return x - 1;
        case 11: return x + 1;
        case 12: return x * 3;
        case 13: return -x;
        case 14: return y === 0 ? 0 : x % y;
        case 15: return y === 0 ? 0 : (x / y) | 0;
        case 16: return x < y ? 1 : 0;
        case 17: return x > y ? 1 : 0;
        default: return -2;
    }
}

let mismatch = 0;
let sum = 0;
for (let op = -2; op <= 20; op++) {
    for (let x = -3; x <= 3; x++) {
        for (let y = -2; y <= 2; y++) {
            const a = step(op, x, y);
            const b = stepLit(op, x, y);
            sum = (sum + a) | 0;
            if (a !== b) mismatch++;
        }
    }
}
__A("tp02_frozen_vs_literal.js:c01 mismatch", function () { assert_eq(mismatch, 0, "c01 mismatch"); });
__A("tp02_frozen_vs_literal.js:c02 sum", function () { assert_eq(sum, -315, "c02 sum"); });
__A("tp02_frozen_vs_literal.js:c03 halt", function () { assert_eq(step(OP.HALT, 0, 0), -1, "c03 halt"); });
__A("tp02_frozen_vs_literal.js:c04 gt", function () { assert_eq(step(OP.GT, 5, 1), 1, "c04 gt"); });
__A("tp02_frozen_vs_literal.js:c05 mod", function () { assert_eq(step(OP.MOD, 17, 5), 2, "c05 mod"); });
__A("tp02_frozen_vs_literal.js:c06 miss-low", function () { assert_eq(step(-100, 0, 0), -2, "c06 miss-low"); });
__A("tp02_frozen_vs_literal.js:c07 miss-high", function () { assert_eq(step(1000, 0, 0), -2, "c07 miss-high"); });
__A("tp02_frozen_vs_literal.js:c08 swap", function () { assert_eq(step(OP.SWAP, 3, 9), 903, "c08 swap"); });
__A("tp02_frozen_vs_literal.js:c09 ld", function () { assert_eq(step(OP.LD, 42, 0), 41, "c09 ld"); });
__A("tp02_frozen_vs_literal.js:c10 st", function () { assert_eq(step(OP.ST, 42, 0), 43, "c10 st"); });
__A("tp02_frozen_vs_literal.js:c11 print", function () { assert_eq(step(OP.PRINT, 7, 0), 21, "c11 print"); });
__A("tp02_frozen_vs_literal.js:c12 jz0", function () { assert_eq(step(OP.JZ, 0, 0), 1, "c12 jz0"); });

summary("constprop");
