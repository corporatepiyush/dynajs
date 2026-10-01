__EXP = null;
function run() {
  var log = [];
  var n = 0;
  outer: while (n < 5) {
    n++;
    let cap = () => n;
    switch (n % 3) {
      case 0: log.push("s" + cap()); continue outer;
      case 1: log.push("a" + n); break;
      default: log.push("d" + n);
    }
    log.push("b" + n);
  }
  return log.join(",") + "|" + n;
}
__A("t11_tdz_head_extra_ref.js:a", function () { assert_eq(run(), "a1,b1,d2,b2,s3,a4,b4,d5,b5|5", "a"); });
function run2() {
  var log = [];
  var i = 0;
  do {
    i++;
    let loc = i * 2;
    {
      if (i === 2) continue;
      log.push(loc);
    }
  } while (i < 5);
  return log.join(",");
}
__A("t11_tdz_head_extra_ref.js:b", function () { assert_eq(run2(), "2,6,8,10", "b"); });
function run3() {
  var fns = [];
  for (let t = 0; t < 6; t++) {
    if (t % 2 === 1) continue;
    fns.push(() => t);
  }
  return fns.map(f => f()).join(",");
}
__A("t11_tdz_head_extra_ref.js:c", function () { assert_eq(run3(), "0,2,4", "c"); });

summary("parser_core_ext");
