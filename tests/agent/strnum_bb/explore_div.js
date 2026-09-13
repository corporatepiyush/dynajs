// explore_div.js — record-only scans (not gated): T2 cap neighborhood, locale,
// radix breadth. Prints one JSON line per row.
function o(s){ (typeof print==='function'?print:console.log)(s); }
function cap(k){
  var r; try { var s='x'.repeat(k); r='OK len='+s.length; } catch(e){ r='THROW '+e.name+' msg='+JSON.stringify(String(e.message)); }
  o(JSON.stringify(['repeat_'+k, r]));
}
cap(1073741823); cap(1073741824); cap(2147483647); cap(536870889); cap(536870888);
var pairs = [['a','b'],['á','b'],['é','e'],['A','a'],['中','日'],['x','X'],['a','a'],['👍','a'],['ß','ss'],['ä','b']];
for (var i=0;i<pairs.length;i++){
  var c;
  try { c = pairs[i][0].localeCompare(pairs[i][1]); } catch(e){ c='THROW '+e.name; }
  var cl;
  try { cl = pairs[i][0].localeCompare(pairs[i][1], 'de'); } catch(e){ cl='THROW '+e.name; }
  o(JSON.stringify(['loc_'+pairs[i][0]+'_'+pairs[i][1], c, cl]));
}
var locs = [['A','en'],['A','tr'],['I','tr'],['i','en-US']];
for (var j=0;j<locs.length;j++){
  var r2;
  try { r2 = locs[j][0].toLocaleLowerCase(locs[j][1]) + '/' + locs[j][0].toLocaleUpperCase(locs[j][1]); }
  catch(e){ r2='THROW '+e.name; }
  o(JSON.stringify(['tolocale_'+locs[j][0]+'_'+locs[j][1], r2]));
}
var bases='';
for (var b=2;b<=36;b++){ var v; try { v=(1234567.5).toString(b); } catch(e){ v='THROW '+e.name; } bases+=b+':'+v+'|'; }
o(JSON.stringify(['radix_1234567.5_all', bases]));
var neg=''; for (var b2=2;b2<=36;b2++){ neg+=b2+':'+(-0.5).toString(b2)+'|'; }
o(JSON.stringify(['radix_-0.5_all', neg]));
o(JSON.stringify(['intl', typeof Intl]));
o(JSON.stringify(['normalize_spaces', JSON.stringify('\u00e9'.normalize('NFD') + '|' + '\ufb01'.normalize('NFC'))]));
