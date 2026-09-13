function o(s){ (typeof print==='function'?print:console.log)(s); }
var dv=new DataView(new ArrayBuffer(8));
function mk(hi,lo){dv.setUint32(0,lo>>>0,true);dv.setUint32(4,hi>>>0,true);return dv.getFloat64(0,true);}
// powers of two across the small-magnitude range, each radix
for (var k=-1074; k<=-900; k+=17){
  var v=mk(1023+k+1>>>0===0?0:((1023+k+1)<<0), 0); // placeholder, compute properly below
}
for (var k=-1074; k<=-950; k+=19){
  var exp=1023+k; var hi; var v;
  if (exp>0){ hi=(exp<<20)>>>0; } else { hi=(1<<(-exp-1))>>>0; } // denormal mantissa bit
  v=mk(hi,0);
  var row=[k];
  [2,8,16,36].forEach(function(r){ row.push(v.toString(r)); });
  o(JSON.stringify(row));
}
for (var k=-940; k>=-1020; k-=20){
  var v=Math.pow(2,k);
  var row=[k]; [2,8,16,36].forEach(function(r){ row.push(v.toString(r)); });
  o(JSON.stringify(row));
}
