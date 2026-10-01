function o(s){ (typeof print==='function'?print:console.log)(s); }
for (var k=-1074; k<=1023; k+=2){
  var v=Math.pow(2,k);
  o(k + ' ' + v.toString(36));
}
