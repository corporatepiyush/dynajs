const OP = Object.freeze({ HLT:0, PUS:1, POP:2, ADD:3, SUB:4, MUL:5, DIV:6, NEG:7,
DUP:8, SWP:9, JMP:10, JZ:11, JPZ:12, LD:13, ST:14, PRT:15, NOP:16, EXT:17 });
function step(op, x, y) {
    switch (op) {
        case OP.HLT: return -1;
        case OP.PUS: return x;
        case OP.POP: return 0;
        case OP.ADD: return x + y;
        case OP.SUB: return x - y;
        case OP.MUL: return (x * y) | 0;
        case OP.DIV: return y === 0 ? 0 : (x / y) | 0;
        case OP.NEG: return -x;
        case OP.DUP: return x * 2;
        case OP.SWP: return y * 100 + x;
        case OP.JMP: return x + 1;
        case OP.JZ:  return x === 0 ? 1 : 0;
        case OP.JPZ: return x > 0 ? 2 : 0;
        case OP.LD:  return x - 1;
        case OP.ST:  return x + 1;
        case OP.PRT: return x * 3;
        case OP.NOP: return x;
        case OP.EXT: return x ^ y;
        default: return -999;
    }
}
const N = 600000;
let acc = 0;
for (let i = 0; i < N; i++) {
    for (let op = 0; op < 18; op++) acc = (acc + step(op, i & 1023, (i >> 3) & 7)) | 0;
}
console.log("acc", acc);
