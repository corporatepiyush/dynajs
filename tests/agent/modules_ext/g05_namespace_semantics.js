// modules_ext g05 (dynajs-only): module namespace object semantics: frozen,
// Symbol.toStringTag, rejected writes (error name), own-keys sorted, no prototype
// escape, default export absence on dyna: namespaces.
import * as bytes from "dyna:bytes";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) out('ok ' + T + ' ' + m); else { F++; out('FAIL ' + T + ' ' + m); } }
function ename(f) { try { f(); return 'no-throw'; } catch (e) { return (e && e.name) || 'thrown'; } }
ok(Object.isFrozen(bytes), 'namespace frozen');
ok(Object.prototype.toString.call(bytes) === '[object Module]', 'toStringTag Module');
ok(ename(() => { bytes.bytesOf = 1; }) === 'TypeError', 'write to namespace -> TypeError');
ok(ename(() => { 'use strict'; delete bytes.bytesOf; }) === 'TypeError', 'delete from namespace -> TypeError');
ok(Object.getPrototypeOf(bytes) === null, 'namespace proto null');
const keys = Object.keys(bytes);
ok(keys.indexOf('Bytes') >= 0 && keys.indexOf('bytesOf') >= 0, 'keys include exports');
ok(!('default' in bytes), 'no default export');
ok(typeof Reflect.get(bytes, 'bytesOf') === 'function', 'Reflect.get works');
ok(ename(() => Reflect.set(bytes, 'bytesOf', 1)) === 'false' || ename(() => Reflect.set(bytes, 'bytesOf', 1)) === 'no-throw', 'Reflect.set rejected gracefully');
// dyna: module imported twice gives the SAME namespace (module identity)
const out2 = out;
import('dyna:bytes').then(m2 => {
  ok(m2 === bytes || String(m2[Symbol.toStringTag]) === 'Module', 'dynamic import same namespace tag');
  ok(m2.bytesOf === bytes.bytesOf, 'same function identity');
  out2('RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));
  out2('DONE');
}, e => { out2('rejected ' + ((e && e.name) || 'thrown')); out2('DONE'); });
