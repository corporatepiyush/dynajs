// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["Float64Array 3.5,NaN,-1,0,2", "Float64Array -1,0,2,3.5,NaN"], ["Float32Array 3.5,NaN,-1,0,2", "Float32Array -1,0,2,3.5,NaN"]];
__EXP[1] = ["Float64Array-def -1,0,2,3.5,NaN", "Float32Array-def -1,0,2,3.5,NaN"];
__EXP[2] = ["Float64Array-pos 1,NaN,NaN", "Float32Array-pos 1,NaN,NaN"];
__EXP[3] = [["Float64Array-zero 1,NaN,0,0", "Float64Array-zero 0,0,1,NaN"], ["Float32Array-zero 1,NaN,0,0", "Float32Array-zero 0,0,1,NaN"]];
__EXP[4] = ["Float64Array-neg NaN,5", "Float32Array-neg NaN,5"];
__REQ = {"dynajs": {"0": 2, "1": 2, "2": 2, "3": 2, "4": 2}, "node": {"0": 2, "1": 2, "2": 2, "3": 2, "4": 2}};
// commit 5: typed-array sort NaN ordering — must now match node exactly
const mk = (Ctor, arr) => { const t = new Ctor(arr.length); arr.forEach((v, i) => t[i] = v); return t; };
for (const Ctor of [Float64Array, Float32Array]) {
  const t = mk(Ctor, [3.5, NaN, -1, 2, 0]);
  t.sort((x, y) => x - y);
  __L(0, Ctor.name, Array.from(t).join(","));
  const t2 = mk(Ctor, [3.5, NaN, -1, 2, 0]);
  t2.sort();
  __L(1, Ctor.name + "-def", Array.from(t2).join(","));
  const t3 = mk(Ctor, [NaN, NaN, 1]);
  t3.sort(() => 1);
  __L(2, Ctor.name + "-pos", Array.from(t3).join(","));
  const t4 = mk(Ctor, [1, NaN, -0, 0]);
  t4.sort((x, y) => x - y);
  __L(3, Ctor.name + "-zero", Array.from(t4).join(","));
  const t5 = mk(Ctor, [NaN, 5]);
  t5.sort((x, y) => -1);
  __L(4, Ctor.name + "-neg", Array.from(t5).join(","));
}
__A("t01_tasort_nan.js:arr", function () { assert_eq([3.5, NaN, -1, 2, 0].sort((x, y) => x - y).map(String).join(","), "3.5,NaN,-1,0,2", "arr"); });
__A("t01_tasort_nan.js:arr-def", function () { assert_eq([3.5, NaN, -1, 2, 0].sort().map(String).join(","), "-1,0,2,3.5,NaN", "arr-def"); });
__A("t01_tasort_nan.js:i32", function () { assert_eq(Array.from((() => { const t = new Int32Array(4); t[0] = 3; t[1] = -1; t[2] = 2; t[3] = 0; t.sort((a, b) => a - b); return t; })()).join(","), "-1,0,2,3", "i32"); });

summary("bbreview");
