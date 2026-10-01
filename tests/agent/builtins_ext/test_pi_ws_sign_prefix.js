__EXP = {};
__EXP[0] = ["0 [  42] r=undefined => 42 nz=false", "1 [\t\n 42] r=undefined => 42 nz=false", "2 [\u00a042] r=undefined => 42 nz=false", "3 [+42] r=undefined => 42 nz=false", "4 [-42] r=undefined => -42 nz=false", "5 [--42] r=undefined => NaN nz=false", "6 [+-42] r=undefined => NaN nz=false", "7 [0x10] r=undefined => 16 nz=false", "8 [0x10] r=16 => 16 nz=false", "9 [-0x10] r=16 => -16 nz=false", "10 [+0x10] r=16 => 16 nz=false", "11 [-0xg] r=16 => NaN nz=false", "12 [0b101] r=2 => 0 nz=false", "13 [0o17] r=8 => 0 nz=false", "14 [017] r=8 => 15 nz=false", "15 [017] r=10 => 17 nz=false", "16 [017] r=0 => 17 nz=false", "17 [] r=undefined => NaN nz=false", "18 [ ] r=10 => NaN nz=false", "19 [-] r=10 => NaN nz=false", "20 [+] r=16 => NaN nz=false", "21 [0x] r=16 => NaN nz=false", "22 [Infinity] r=undefined => NaN nz=false", "23 [-Infinity] r=undefined => NaN nz=false", "24 [NaN] r=undefined => NaN nz=false", "25 [42abc] r=undefined => 42 nz=false", "26 [abc42] r=undefined => NaN nz=false", "27 [1_000] r=undefined => 1 nz=false", "28 [42] r=undefined => 42 nz=false", "29 [-42] r=undefined => -42 nz=false", "30 [1e3] r=undefined => 1 nz=false", "31 [1e3] r=10 => 1 nz=false", "32 [\u30009] r=undefined => 9 nz=false"];
__EXP[1] = ["hash=9dc8fff7"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var cases = [
  ["  42", undefined], ["\t\n 42", undefined], ["\u00a042", undefined],
  ["+42", undefined], ["-42", undefined], ["--42", undefined], ["+-42", undefined],
  ["0x10", undefined], ["0x10", 16], ["-0x10", 16], ["+0x10", 16], ["-0xg", 16],
  ["0b101", 2], ["0o17", 8], ["017", 8], ["017", 10], ["017", 0],
  ["", undefined], [" ", 10], ["-", 10], ["+", 16], ["0x", 16],
  ["Infinity", undefined], ["-Infinity", undefined], ["NaN", undefined],
  ["42abc", undefined], ["abc42", undefined], ["1_000", undefined],
  [42, undefined], [-42, undefined], ["1e3", undefined], ["1e3", 10],
  ["\u3000" + "9", undefined]
];
for (var i = 0; i < cases.length; i++) {
  var v = parseInt(cases[i][0], cases[i][1]);
  var line = i + " [" + String(cases[i][0]) + "] r=" + cases[i][1] + " => " + String(v) +
    " nz=" + Object.is(v, -0);
  feed += line + "\n";
  __L(0, line);
}
__L(1, "hash=" + fnv(feed));

summary("builtins_ext");
