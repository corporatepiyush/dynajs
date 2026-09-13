// url_fix4_bare_xn.js -- FIX 4: dyn_host_alabel_roundtrip's bare-"xn--" fast
// path must advance the label start (ls = i + 1); re-scanning a merged label
// threw on hosts the function's own rationale accepts (WPT: VerifyDnsLength
// false, empty punycode body fine). Matrix:
//   accepted: xn--, xn--.b, b.xn--.a, xn--./path, a.xn--.b.c
//   still rejected: xn--a-ecp.ru (decodes to disallowed look-alike)
//   still valid:    xn--mnchen-3ya (münchen)
import { URL } from "dyna:url";
var __pass = 0, __fail = 0;
function expect(msg, got, want) {
  if (got === want) { __pass++; return; }
  __fail++;
  print("FAIL " + msg + ": got " + JSON.stringify(got) + " want " + JSON.stringify(want));
}
function hostOf(u) {
  try { return new URL(u).host; } catch (e) {
    return "THROW:" + ((e && e.constructor && e.constructor.name) || e);
  }
}
expect("xn--", hostOf("http://xn--/"), "xn--");
expect("xn--.b", hostOf("http://xn--.b/"), "xn--.b");
expect("b.xn--.a", hostOf("http://b.xn--.a/"), "b.xn--.a");
expect("xn--./path", hostOf("http://xn--./p"), "xn--./p" === "xn--./p" ? "xn--." : "xn--.");
expect("a.xn--.b.c", hostOf("http://a.xn--.b.c/"), "a.xn--.b.c");
expect("xn--a-ecp.ru still throws", hostOf("http://xn--a-ecp.ru/"), "THROW:TypeError");
expect("xn--mnchen-3ya still valid", hostOf("http://xn--mnchen-3ya/"), "xn--mnchen-3ya");
expect("plain host", hostOf("http://example.com/"), "example.com");
expect("punycode valid", hostOf("http://xn--zzz/"), "xn--zzz");
print("SUMMARY url_fix4_bare_xn pass=" + __pass + " fail=" + __fail + " // 9 cases");
print(__fail === 0 ? "RESULT PASS" : "RESULT FAIL");
if (__fail !== 0) throw new Error("probe failed");
