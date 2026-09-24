// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[3] = ["c04 true [13,7,28,13]"];
// tp16: a realistic VM interpreter — dispatch over a top-level const opcode
// table, exact output sequence checked. 4 checks (but many lines).
const OP = Object.freeze({
    PUSH: 0, ADD: 1, SUB: 2, MUL: 3, PRINT: 4, JMP: 5, JNZ: 6, HALT: 7
});

// program: pushes 3 values, adds, subs, muls, prints, jumps back once
const prog = [
    OP.PUSH, 10, OP.PUSH, 3, OP.ADD, OP.PRINT,
    OP.PUSH, 6, OP.SUB, OP.PRINT,
    OP.PUSH, 4, OP.MUL, OP.PRINT,
    OP.JMP, 0
];

function run(prog, ops) {
    const out = [];
    let pc = 0;
    let sp = -1;
    let st = [];
    let guard = 0;
    while (guard++ < 1000) {
        const op = prog[pc++];
        switch (op) {
            case ops.PUSH: st[++sp] = prog[pc++]; break;
            case ops.ADD: { const b = st[sp--], a = st[sp--]; st[++sp] = a + b; break; }
            case ops.SUB: { const b = st[sp--], a = st[sp--]; st[++sp] = a - b; break; }
            case ops.MUL: { const b = st[sp--], a = st[sp--]; st[++sp] = a * b; break; }
            case ops.PRINT: out.push(st[sp]); break;
            case ops.JMP: pc = prog[pc]; break;
            case ops.JNZ: { const t = prog[pc]; if (st[sp] !== 0) pc = t; else pc++; break; }
            case ops.HALT: return out;
            default: out.push("bad" + op); return out;
        }
    }
    out.push("guard");
    return out;
}

// single pass with a JNZ terminating run
const prog2 = [OP.PUSH, 2, OP.PUSH, 1, OP.JNZ, 8, OP.HALT, OP.PUSH, 9, OP.HALT];
const prog3 = [OP.PUSH, 0, OP.JNZ, 8, OP.HALT, OP.PUSH, 9, OP.HALT];

__A("tp16_vm_interpreter.js:c01", function () { assert_eq(JSON.stringify(run(prog, OP).slice(0, 4)), "[13,7,28,13]", "c01"); });
__A("tp16_vm_interpreter.js:c02", function () { assert_eq(JSON.stringify(run(prog2, OP)), "[\"bad9\"]", "c02"); });
__A("tp16_vm_interpreter.js:c03", function () { assert_eq(JSON.stringify(run(prog3, OP)), "[]", "c03"); });

// literal version must produce identical output
const L = { PUSH: 0, ADD: 1, SUB: 2, MUL: 3, PRINT: 4, JMP: 5, JNZ: 6, HALT: 7 };
const a1 = JSON.stringify(run(prog, OP).slice(0, 4));
const a2 = JSON.stringify(run(prog, L).slice(0, 4));
__L(3, "c04", a1 === a2, a1);

summary("constprop");
