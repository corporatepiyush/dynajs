// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// D2: shadowing towers — deep nested blocks re-declaring one name, closures
// at every level, plus a 100-level generated tower.
function tower() {
  let x = 0;
  const fns = [];
  {
    let x = 1;
    {
      let x = 2;
      {
        let x = 3;
        {
          let x = 4;
          {
            let x = 5;
            fns.push(() => "L5:" + x);
          }
          fns.push(() => "L4:" + x);
        }
        fns.push(() => "L3:" + x);
      }
      fns.push(() => "L2:" + x);
    }
    fns.push(() => "L1:" + x);
  }
  fns.push(() => "L0:" + x);
  return fns.map(f => f()).join(",");
}
__A("d02_shadow_tower.js:a", function () { assert_eq(tower(), "L5:5,L4:4,L3:3,L2:2,L1:1,L0:0", "a"); });
// generated 100-level tower, capture at every 10th level, read through closures
var levels = 100;
var body = ["(function(){ var fns=[]; let x=0;"];
for (var i = 1; i <= levels; i++) {
  body.push("{ let x=" + i + ";");
  if (i % 10 === 0) body.push("fns.push(function(){return 'd'+x;});");
}
for (var i = 0; i < levels; i++) body.push("}");
body.push("return fns.map(function(f){return f();}).join(',');})()");
console.log("b", eval(body.join("")));
// mixed var/let/function shadow tower in one block chain
function tower2() {
  var v = "var-outer";
  function fn() { return "fn-outer"; }
  {
    let v = "let-inner";
    function fn() { return "fn-inner"; }
    return [typeof fn, fn(), v, typeof v].join("|");
  }
}
__A("d02_shadow_tower.js:c", function () { assert_eq(tower2(), "function|fn-inner|let-inner|string", "c"); });

summary("parser_core_ext");
