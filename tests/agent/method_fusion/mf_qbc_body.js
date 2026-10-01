function enc(s, k) { var r = s.charCodeAt(k); return r; }
function encLoc(s) { var j = 1; var r = s.charCodeAt(j); return r; }
function chk(s) { let j = 2; var r = s.startsWith("ab", j); return r; }
function none(o) { var r = o.m(); return r; }
function wide(s) { var r = s.charCodeAt(300); return r; }
function fnoargs() { return none({ m: function () { return "M0"; } }); }
var acc = enc("abc", 0) + encLoc("abc") + (chk("abcd") ? 1 : 0) +
          (fnoargs() === "M0" ? 1 : 0) + (isNaN(wide("abc")) ? 1 : 0);
console.log("qbc-acc", acc);
console.log("qbc-enc", enc("hello", 4));
