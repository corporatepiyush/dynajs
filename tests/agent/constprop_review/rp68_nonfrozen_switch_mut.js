// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["before a b h"];
__EXP[1] = ["after b h ? a"];
__EXP[2] = ["members 100 1 2"];
// non-frozen const + switch on member values, then mutate the members
const OP = { A: 1, B: 2, C: 3, D: 4, E: 5, F: 6, G: 7, H: 8 };
function f(v) {
    switch (v) {
        case OP.A: return "a";
        case OP.B: return "b";
        case OP.C: return "c";
        case OP.D: return "d";
        case OP.E: return "e";
        case OP.F: return "f";
        case OP.G: return "g";
        case OP.H: return "h";
        default: return "?";
    }
}
__L(0, "before", f(1), f(2), f(8));
OP.A = 100; OP.B = 1; OP.H = 2;
__L(1, "after", f(1), f(2), f(8), f(100));
__L(2, "members", OP.A, OP.B, OP.H);

summary("constprop_review");
