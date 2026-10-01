__EXP = {};
__EXP[0] = ["100 101 102"];
const OP = { A: 1 };
const fs = [];
for (let i = 0; i < 3; i++) { fs.push(function() { return OP.A + i; }); }
OP.A = 100;
__L(0, fs[0](), fs[1](), fs[2]());

summary("constprop_review");
