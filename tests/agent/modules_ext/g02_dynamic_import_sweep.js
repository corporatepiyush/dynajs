__EXP = {};
__EXP[1] = ["DONE"];
__EXP[3] = [["bytes: rejected ReferenceError", "bytes: rejected Error"], ["cli: rejected ReferenceError", "cli: rejected Error"], ["compress: rejected ReferenceError", "compress: rejected Error"], ["config: rejected ReferenceError", "config: rejected Error"], ["crypto: rejected ReferenceError", "crypto: rejected Error"], ["csv: rejected ReferenceError", "csv: rejected Error"], ["dataframe: rejected ReferenceError", "dataframe: rejected Error"], ["decimal: rejected ReferenceError", "decimal: rejected Error"], ["encoding: rejected ReferenceError", "encoding: rejected Error"], ["file: rejected ReferenceError", "file: rejected Error"], ["hash: rejected ReferenceError", "hash: rejected Error"], ["html: rejected ReferenceError", "html: rejected Error"], ["http: rejected ReferenceError", "http: rejected Error"], ["json: rejected ReferenceError", "json: rejected Error"], ["log: rejected ReferenceError", "log: rejected Error"], ["matcher: rejected ReferenceError", "matcher: rejected Error"], ["mathx: rejected ReferenceError", "mathx: rejected Error"], ["ml: rejected ReferenceError", "ml: rejected Error"], ["net: rejected ReferenceError", "net: rejected Error"], ["random: rejected ReferenceError", "random: rejected Error"], ["schema: rejected ReferenceError", "schema: rejected Error"], ["scrape: rejected ReferenceError", "scrape: rejected Error"], ["semver: rejected ReferenceError", "semver: rejected Error"], ["serialize: rejected ReferenceError", "serialize: rejected Error"], ["simd: rejected ReferenceError", "simd: rejected Error"], ["structures: rejected ReferenceError", "structures: rejected Error"], ["sys: rejected ReferenceError", "sys: rejected Error"], ["time: rejected ReferenceError", "time: rejected Error"], ["uring: rejected ReferenceError", "uring: rejected Error"], ["url: rejected ReferenceError", "url: rejected Error"], ["uuid: rejected ReferenceError", "uuid: rejected Error"], ["validate: rejected ReferenceError", "validate: rejected Error"], ["vserialize: rejected ReferenceError", "vserialize: rejected Error"], ["xml: rejected ReferenceError", "xml: rejected Error"], ["yaml: rejected ReferenceError", "yaml: rejected Error"]];
__REQ = {"dynajs": {"1": 1, "3": 35}, "node": {"1": 1, "3": 35}};
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
const names = ['bytes', 'cli', 'compress', 'config', 'crypto', 'csv', 'dataframe', 'decimal', 'encoding', 'file', 'hash', 'html', 'http', 'json', 'log', 'matcher', 'mathx', 'ml', 'net', 'random', 'schema', 'scrape', 'semver', 'serialize', 'simd', 'structures', 'sys', 'time', 'uring', 'url', 'uuid', 'validate', 'vserialize', 'xml', 'yaml'];
let i = 0;
function step() {
  if (i >= names.length) { __L(1, 'DONE'); return; }
  const n = names[i++];
  import('dyna:' + n).then(
    m => { __L(2, n + ': resolved typeof=' + (typeof m)); step(); },
    e => { __L(3, n + ': rejected ' + ((e && e.name) || 'thrown')); step(); }
  );
}
step();

__FINISH("modules_ext");
