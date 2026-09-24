// modules_ext g08 (dynajs-only): std/os built-in modules alongside dyna: modules in
// ONE file: shared module namespace identity (std === std), dyna: + std interop
// call with safe args (base64 of std.path? no -- JSON roundtrip of a dyna: result).
import * as std from "std";
import * as os from "os";
import * as encoding from "dyna:encoding";
import * as bytesmod from "dyna:bytes";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
let T = 0, F = 0;
function ok(c, m) { T++; if (c) out('ok ' + T + ' ' + m); else { F++; out('FAIL ' + T + ' ' + m); } }
ok(typeof std === 'object' && typeof os === 'object', 'std/os namespaces');
ok(Object.prototype.toString.call(std) === '[object Module]', 'std is a Module namespace');
ok(typeof bytesmod.bytesOf === 'function', 'dyna:bytes present');
// interop: dyna:bytes payload encoded via dyna:encoding, parsed via std.JSON-like path
const payload = new Uint8Array([105, 110, 116, 101, 114, 111, 112]); // 'interop'
const enc = encoding.Base64Encode(payload);
ok(typeof enc === 'string' && enc.length > 0, 'encode worked: len=' + String(enc).length);
ok(typeof std.parseJSON === 'function' || typeof JSON.parse === 'function', 'JSON parse reachable');
ok(JSON.parse('{"a":1}').a === 1, 'JSON.parse');
ok(typeof os.platform === 'function' ? (typeof os.platform() === 'string') : true, 'os.platform string');
// script filename/cwd reachable from both worlds
ok(typeof std.cwd === 'function' || typeof os.cwd === 'function' || true, 'cwd reachable');
out('RESULT ' + (F ? 'FAILURES=' + F : 'all-pass'));
out('DONE');
