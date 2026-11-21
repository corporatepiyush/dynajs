// covers: dyna:yaml.Parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_scalar");
import { Parse } from "dyna:yaml";
let n=0,bad=0; const ok=(c,w)=>{n++;if(!c){bad++;print("FAIL: "+w);}};
const v = (y) => Parse("k: " + y + "\n").k;
const vb = (y) => Parse("k: " + y + "\n", { bigint: true }).k;
const cases = [
  ["~",null],["null",null],["Null",null],["NULL",null],
  ["true",true],["True",true],["TRUE",true],
  ["false",false],["False",false],["FALSE",false],
  ["0",0],["123",123],["-7",-7],["+7",7],["0x1F",31],["0o17",15],
  ["1.5",1.5],["-1.5",-1.5],[".5",0.5],["+.5",0.5],["1e3",1000],
];

__kitS.section("want", () => {
for (const [y,want] of cases) { const g=v(y); ok(Object.is(g,want)||g===want, y+" -> "+JSON.stringify(g)+" want "+JSON.stringify(want)); }
});

__kitS.section(".inf", () => {
ok(v(".inf")===Infinity,".inf");
});

__kitS.section("-.inf", () => {
ok(v("-.inf")===-Infinity,"-.inf");
});

__kitS.section(".nan", () => {
ok(Number.isNaN(v(".nan")),".nan");
});

__kitS.section("is Infinity", () => {
for (const y of [".inf",".Inf",".INF","+.inf","+.Inf","+.INF"]) ok(v(y)===Infinity, y+" is Infinity");
});

__kitS.section("is -Infinity", () => {
for (const y of ["-.inf","-.Inf","-.INF"]) ok(v(y)===-Infinity, y+" is -Infinity");
});

__kitS.section("is NaN", () => {
for (const y of [".nan",".NaN",".NAN"]) ok(Number.isNaN(v(y)), y+" is NaN");
});

__kitS.section("0x7f", () => {
for (const [y,want] of [["0x7f",127],["0X7F",127],["0o777",511],["0O777",511],["0777",777],["-0xff",-255]])
  ok(v(y)===want, y+" -> "+want);
});

__kitS.section("has no core radix: stays a string", () => {
for (const y of ["0b101","1_000","0x","12a"])
  ok(typeof v(y)==="string", y+" has no core radix: stays a string");
  ok(vb("-9223372036854775808")===-9223372036854775808n, "INT64_MIN is the exact BigInt under { bigint: true }, not a string");
  ok(vb("9223372036854775808")===9223372036854775808n && vb("-9223372036854775809")===-9223372036854775809n, "beyond int64 is an exact BigInt under { bigint: true }, not a string");
  ok(v("-9223372036854775809")===-9223372036854775808 && v("9223372036854775808")===9223372036854775808, "by default (D8) beyond int64 is the rounded Number, not a string");
});

__kitS.section("stays a string", () => {
for (const y of ["value","nope","truthy","False_","nullish","North","Tea","Fun","abc","zed"])
  ok(typeof v(y)==="string", y+" stays a string, got "+JSON.stringify(v(y)));
});

__kitS.section("yaml scalar:", () => {
for (const y of ["value123","hello","xyz","_priv","/path"]) ok(typeof v(y)==="string", y);
print("yaml scalar: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error("fail");
});
__kitS.end();
