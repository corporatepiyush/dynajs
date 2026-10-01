__EXP = {};
__EXP[0] = ["get=2 direct=2"];
const OP={A:1}; globalThis.get=()=>OP.A;
OP.A=2;
__L(0, "get="+get()+" direct="+OP.A);

summary("constprop_review");
