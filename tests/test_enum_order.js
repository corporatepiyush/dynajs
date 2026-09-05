/* Enumeration ORDER and CONTENT must be identical: integer keys ascending
   first, then string keys in insertion order, non-enumerables and symbols
   excluded. The fast path refuses every one of those cases; this proves it. */
let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + "\n  got  " + a + "\n  want " + b); } }
eq(Object.keys({b:1, a:2, c:3}).join(), "b,a,c", "insertion order for string keys");
eq(Object.keys({2:1, 0:2, b:3, 1:4}).join(), "0,1,2,b", "integer keys sort first, ascending");
eq(Object.keys({"-1":1, "1":2, "01":3}).join(), "1,-1,01", "only true array indices sort");
{
  const o = Object.defineProperty({v:1}, "h", {value:2, enumerable:false});
  eq(Object.keys(o).join(), "v", "non-enumerable excluded");
  eq(JSON.stringify(o), '{"v":1}', "and excluded from JSON too");
}
{
  const o = {a:1}; o[Symbol("s")] = 2;
  eq(Object.keys(o).join(), "a", "symbol excluded");
  eq(JSON.stringify(o), '{"a":1}', "symbol excluded from JSON");
}
eq(JSON.stringify({2:"a", 0:"b", z:"c"}), '{"0":"b","2":"a","z":"c"}', "JSON key order");
eq(JSON.stringify(new Proxy({p:1},{})), '{"p":1}', "a proxy still stringifies");
{
  let seen = []; for (const k in {x:1, 3:2, y:3}) seen.push(k);
  eq(seen.join(), "3,x,y", "for-in order");
}
eq(Object.entries({a:1,b:2}).map(e=>e.join(":")).join(), "a:1,b:2", "entries");
eq(JSON.stringify(Object.assign({}, {a:1}, {b:2})), '{"a":1,"b":2}', "assign order");
eq(JSON.stringify({}), "{}", "empty object");
eq(Object.keys({}).length, 0, "empty keys");
if (bad) throw new Error("enumeration semantics changed");
print("enum semantics: " + n + " checks, 0 failures");
