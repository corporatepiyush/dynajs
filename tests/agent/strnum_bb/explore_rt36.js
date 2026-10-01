function o(s){ (typeof print==='function'?print:console.log)(s); }
for (var k=-1074; k<=1023; k+=2){
  var v = Math.pow(2,k);
  var s = v.toString(36);
  var back = parseInt(s, 36);
  o(k + ' ' + (back === v ? 'OK' : 'BROKEN'));
}
