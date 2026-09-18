// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["~kept: 50", "~kept: 50"]];
__EXP[2] = ["chunky: 200 199 0"];
/* Lifetime torture 2: young objects stored into LONG-LIVED containers while
 * an allocation storm rages (the case that decides arena-reset safety:
 * "objects stored into long-lived containers then nursery exhausts
 * mid-store"). After the storm, every retained object must still hold the
 * exact values captured at store time. Diffed vs node and flag-off build.
 */
"use strict";
const keep = { arr: [], map: new Map(), set: new Set(), deep: { list: [] } };

function storm(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        const hot = { i: i, tag: "t" + (i & 1023) };
        s += hot.i ^ hot.tag.length;
    }
    return s;
}

for (let round = 0; round < 50; round++) {
    storm(20000);
    const young = { round: round, payload: [round, round + 1], self: null };
    young.self = young; /* cycle entirely inside young objects */
    keep.arr.push(young);
    keep.map.set("k" + round, young);
    keep.set.add(young);
    keep.deep.list.push({ round: round, back: young });
    storm(20000);
}
storm(100000); /* the arena-reset pressure AFTER retention */

let chk = 0;
for (let i = 0; i < keep.arr.length; i++) {
    const y = keep.arr[i];
    chk += y.round * 3 + y.payload[0] + (y.self === y ? 1 : 0);
    if (keep.map.get("k" + i) !== y) throw new Error("map identity lost " + i);
    if (!keep.set.has(y)) throw new Error("set identity lost " + i);
    if (keep.deep.list[i].back !== y) throw new Error("backref lost " + i);
}
__L(0, "kept:", keep.arr.length, "chk:", chk);
__A("store_longlived.js:cycles:", function () { assert_eq(keep.arr.filter(y => y.self === y).length, 50, "cycles:"); });

/* store-under-exhaustion: interleave heavy churn DURING the stores */
const chunky = [];
for (let r = 0; r < 200; r++) {
    const u = { r: r, xs: new Array(8).fill(r) };
    for (let k = 0; k < 500; k++) { const junk = { k: k, u: u }; if (junk.u !== u) throw new Error("impossible"); }
    chunky.push(u);
}
__L(2, "chunky:", chunky.length, chunky[199].xs[7], chunky[0].r);

summary("nursery");
