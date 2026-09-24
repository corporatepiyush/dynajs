// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["e1 len=3 0in=false join=,1,2 keys=1|2 json=[null,1,2]", "e2 len=4 0in=false join=,,1,2 keys=2|3 json=[null,null,1,2]", "e3 len=4 0in=false join=,5,1,2 keys=1|2|3 json=[null,5,1,2]", "e4 len=1 0in=false join= keys= json=[null]", "e5 len=3 0in=false join=,7,8 keys=1|2 json=[null,7,8]", "e6 len=4 0in=false join=,1,,3 keys=1|2|3 json=[null,1,null,3]", "e7 len=3 0in=true join=9,,1 keys=0|2 json=[9,null,1]", "e8 len=5 0in=false join=,1,,3,99 keys=1|2|3|4 json=[null,1,null,3,99]", "e9 len=102 0in=false join=,1,2,3,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,101 keys=1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|51|52|53|54|55|56|57|58|59|60|61|62|63|64|65|66|67|68|69|70|71|72|73|74|75|76|77|78|79|80|81|82|83|84|85|86|87|88|89|90|91|92|93|94|95|96|97|98|99|100|101 json=[null,1,2,3,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,101]", "e10 len=3 0in=false join=,4,5 keys=1|2 json=[null,4,5]"];
// H: array-literal elisions before a spread element (the count-over-write regression class)
// [, ...x] leaves pos > u.array.count — bulk append must NOT store over holes
function probe(label, arr) {
  const desc = [
    "len=" + arr.length,
    "0in=" + (0 in arr),
    "join=" + arr.join(","),
    "keys=" + Object.keys(arr).join("|"),
    "json=" + JSON.stringify(arr),
  ];
  __L(0, label, desc.join(" "));
}
probe("e1", [, ...[1, 2]]);
probe("e2", [, , ...[1, 2]]);
probe("e3", [, 5, ...[1, 2]]);
probe("e4", [, ...[]]);
probe("e5", [, ...new Set([7, 8])]);
const holey = [1, , 3];
probe("e6", [, ...holey]);
probe("e7", [9, , ...[1]]);
probe("e8", [, ...holey, 99]);
const sparse = [1, 2, 3];
sparse[100] = 101;
probe("e9", [, ...sparse]);
probe("e10", [, ...new Uint8Array([4, 5])]);
// stress: 200 iterations checking GC-visible garbage does not resurface
let bad = 0;
for (let i = 0; i < 200; i++) {
  const a = [, ...[1, 2]];
  if (a.length !== 3 || (0 in a) || a.join(",") !== ",1,2") bad++;
}
__A("h05_spread_elision.js:stress", function () { assert_eq(bad, 0, "stress"); });

summary("bbreview");
