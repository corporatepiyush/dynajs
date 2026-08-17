import { Parse } from "dyna:yaml";
let n=0,bad=0; const ok=(c,w)=>{n++;if(!c){bad++;print("FAIL: "+w);}};
const v = (y) => Parse("k: " + y + "\n").k;
/* every value the bypass could wrongly divert to "string" */
const cases = [
  ["~",null],["null",null],["Null",null],["NULL",null],
  ["true",true],["True",true],["TRUE",true],
  ["false",false],["False",false],["FALSE",false],
  ["0",0],["123",123],["-7",-7],["+7",7],["0x1F",31],["0o17",15],
  ["1.5",1.5],["-1.5",-1.5],[".5",0.5],["+.5",0.5],["1e3",1000],
];
for (const [y,want] of cases) { const g=v(y); ok(Object.is(g,want)||g===want, y+" -> "+JSON.stringify(g)+" want "+JSON.stringify(want)); }
ok(v(".inf")===Infinity,".inf"); ok(v("-.inf")===-Infinity,"-.inf"); ok(Number.isNaN(v(".nan")),".nan");
/* things that MUST stay strings, incl. near-misses on the bypass set */
for (const y of ["value","nope","truthy","False_","nullish","North","Tea","Fun","abc","zed"])
  ok(typeof v(y)==="string", y+" stays a string, got "+JSON.stringify(v(y)));
/* the diverted class: first byte outside the set */
for (const y of ["value123","hello","xyz","_priv","/path"]) ok(typeof v(y)==="string", y);
print("yaml scalar: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error("fail");
