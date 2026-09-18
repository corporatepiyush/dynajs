// Second qbc body with a higher cpool index (push_const form) so the const
// operand validation is exercised on the warm read too.
function big(s) { var r = s.startsWith("zzzzzzzzzzzzzzzzzzzzzzzz"); return r; }
function mix(s, k) { var j = k; var r = s.charCodeAt(j) + (big(s) ? 1 : 0); return r; }
console.log("qbc2", mix("abc", 0), big("abc"), big("zzzzzzzzzzzzzzzzzzzzzzzz"));
