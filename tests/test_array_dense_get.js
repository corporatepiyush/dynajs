function eq(a,b,m){ var x=JSON.stringify(a),y=JSON.stringify(b); if(x!==y){print("FAIL "+m+": "+x+" want "+y); throw new Error(m);} }
/* dense */
eq([1,2,3].map(function(v){return v*2}), [2,4,6], "map");
eq([1,2,3,4].filter(function(v){return v%2==0}), [2,4], "filter");
eq([1,2,3].every(function(v){return v>0}), true, "every");
eq([1,2,3].some(function(v){return v>2}), true, "some");
eq([1,2,3].reduce(function(a,v){return a+v},0), 6, "reduce");
eq([1,2,3].indexOf(2), 1, "indexOf");
/* HOLES must stay holes: map/filter/forEach skip them, and the result keeps them */
var h = [1,,3];
var seen=[]; h.forEach(function(v,i){seen.push(i)}); eq(seen,[0,2],"holeForEach");
eq(1 in h.map(function(v){return v}), false, "holeMapKeepsHole");
eq(h.filter(function(){return true}), [1,3], "holeFilter");
eq(h.every(function(v){return v!==undefined}), true, "holeEvery");
/* array with an index shadowed on the prototype (fast array must not be assumed) */
var pa=[1,,3]; Object.prototype[1]="proto";
eq(pa.map(function(v){return v}), [1,"proto",3], "protoHole");
eq(pa.indexOf("proto"), 1, "protoIndexOf");
delete Object.prototype[1];
/* slow (dictionary) array: forced out of fast_array mode */
var s=[1,2,3]; Object.defineProperty(s,1,{get:function(){return 42},configurable:true});
eq(s.map(function(v){return v}), [1,42,3], "getterArray");
var s2=[1,2,3]; s2[1000]=9; eq(s2.filter(function(v){return v!==undefined}),[1,2,3,9],"sparseBig");
/* non-array objects and typed arrays */
eq(Array.prototype.map.call({0:"a",1:"b",length:2}, function(v){return v}), ["a","b"], "arraylike");
eq(Array.from(new Int32Array([1,2,3]).map(function(v){return v*2})), [2,4,6], "typedarray");
eq(Array.prototype.map.call("abc", function(v){return v}), ["a","b","c"], "string");
/* mutation during iteration: callback shrinks the array */
var m=[1,2,3,4,5]; var got=[];
m.forEach(function(v,i){ got.push(v); if(i===0) m.length=2; });
eq(got,[1,2],"shrinkDuringForEach");
/* callback converts the array to slow mode mid-iteration */
var c=[1,2,3,4]; var got2=[];
c.forEach(function(v,i){ got2.push(v); if(i===0) Object.defineProperty(c,3,{get:function(){return 99}}); });
eq(got2,[1,2,3,99],"convertDuringForEach");
/* Index beyond INT32_MAX -- JS_TryGetPropertyInt64's fast path must decline it
   (it is not a fast array) and the >JS_ATOM_MAX_INT atom path must still work.
   Probed directly rather than via indexOf: a length of 2^32 would make indexOf
   walk 4.3e9 indices, each interning an atom (minutes plain, hours under ASan). */
var big = {}; big[4294967296] = "x"; big[9007199254740990] = "y";
eq(Array.prototype.lastIndexOf.call({length:4294967297, 4294967296:"x"}, "x", 4294967296),
   4294967296, "bigIndexLast");
eq(big[4294967296], "x", "bigIndexRead");
eq(Object.keys(big).length, 2, "bigIndexKeys");
print("tryget: ok");
