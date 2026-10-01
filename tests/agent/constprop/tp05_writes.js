__EXP = {};
__EXP[0] = ["c01 0 50"];
__EXP[2] = ["c03 77 add"];
__EXP[6] = ["c07 undefined false 2"];
__EXP[7] = ["c08 undefined false"];
__EXP[8] = ["c09 1 11"];
__EXP[9] = ["c10 7 1"];
const OP = { HALT: 0, PUSH: 1, ADD: 2 };

const before = OP.HALT;
OP.HALT = 50;
const after = OP.HALT;
__L(0, "c01", before, after);

let seen = [];
OP.HALT = 0;
for (let i = 0; i < 3; i++) {
    seen.push(dispatch(OP.HALT));
    OP.HALT += 10;
}
function dispatch(pc) {
    switch (pc) {
        case OP.PUSH: return "push";
        case OP.HALT: return "halt";
        case OP.ADD: return "add";
        default: return "d" + pc;
    }
}
__A("tp05_writes.js:c02", function () { assert_eq(seen.join(","), "halt,halt,halt", "c02"); });

OP["ADD"] = 77;
__L(2, "c03", OP.ADD, dispatch(77));

const key = "HALT";
OP[key] = 5;
__A("tp05_writes.js:c04", function () { assert_eq(OP.HALT, 5, "c04"); });

OP.PUSH++;
__A("tp05_writes.js:c05", function () { assert_eq(OP.PUSH, 2, "c05"); });
OP.PUSH += 10;
__A("tp05_writes.js:c06", function () { assert_eq(OP.PUSH, 12, "c06"); });

const OP2 = { X: 1, Y: 2 };
delete OP2.X;
__L(6, "c07", OP2.X, "X" in OP2, OP2.Y);

delete OP2["Y"];
__L(7, "c08", OP2.Y, "Y" in OP2);

const OP3 = { A: 1, B: 2 };
OP3.B = OP3.A + 10;
__L(8, "c09", OP3.A, OP3.B);

const OP4 = { K: 7 };
OP4.NEWPROP = 1;
__L(9, "c10", OP4.K, OP4.NEWPROP);

summary("constprop");
