__EXP = {};
__EXP[0] = [["~work chk", "~work chk"]];
let s = 0;
for (let i = 0; i < 300000; i++) {
    const a = { x: i, y: i + 1 };
    const b = { p: a, q: [i, i + 2] };
    s += b.p.x + b.q[1];
}
const keep = [];
for (let i = 0; i < 1000; i++) keep.push({ i: i });
__L(0, "work chk", s % 1000003, keep.length);

summary("nursery");
