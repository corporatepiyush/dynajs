// non-frozen const kernel: v2 contract says NO fold, bytecode == pristine
const OP = { HLT:0, PUS:1, ADD:2, SUB:3, MUL:4, NEG:5, DUP:6, END:7 };
function step(op, x, y) {
    switch (op) {
        case OP.HLT: return -1;
        case OP.PUS: return x;
        case OP.ADD: return x + y;
        case OP.SUB: return x - y;
        case OP.MUL: return (x * y) | 0;
        case OP.NEG: return -x;
        case OP.DUP: return x * 2;
        case OP.END: return 0;
        default: return -999;
    }
}
let acc = 0;
for (let i = 0; i < 300; i++) acc = (acc + step(i & 7, i, 2)) | 0;
console.log("acc", acc);
