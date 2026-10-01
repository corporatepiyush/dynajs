__EXP = {};
__EXP[0] = ["c01 object 1"];
__EXP[1] = ["c02 ff 255.0"];
__EXP[2] = ["c03 n 1"];
__EXP[3] = ["c04 undefined undefined"];
__EXP[4] = ["c05 string number object"];
__EXP[6] = ["c07 n 1"];
__EXP[8] = ["c09 true 0"];
__EXP[9] = ["c10 n 0"];
const OP = { INNER: { DEEP: 1 }, NAME: "n", ZERO: 0 };

__L(0, "c01", typeof OP.INNER, OP.INNER.DEEP);

const NUMS = { V: 255 };
__L(1, "c02", NUMS.V.toString(16), NUMS.V.toFixed(1));

__L(2, "c03", OP?.NAME, OP.NAME?.length);

__L(3, "c04", OP?.NOTTHERE, OP.NOTTHERE?.x);

__L(4, "c05", typeof OP.NAME, typeof OP.ZERO, typeof OP.INNER);

let acc = 0;
acc += OP.ZERO === 0 ? 1 : 0;
acc += OP.NAME === "n" ? 10 : 0;
acc += OP.INNER.DEEP === 1 ? 100 : 0;
__A("tp14_chains.js:c06", function () { assert_eq(acc, 111, "c06"); });

__L(6, "c07", (OP.ZERO, OP.NAME), (OP.ZERO + 1));

__A("tp14_chains.js:c08", function () { assert_eq(`v=${OP.ZERO} n=${OP.NAME}`, "v=0 n=n", "c08"); });

const list = [OP.ZERO, 1, 2];
__L(8, "c09", list.indexOf(OP.ZERO) >= 0, list[0]);

const built = { k: OP.NAME, z: OP.ZERO };
__L(9, "c10", built.k, built.z);

summary("constprop");
