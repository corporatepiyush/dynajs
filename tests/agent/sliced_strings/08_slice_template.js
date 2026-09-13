// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 08_slice_template"];
/* template literals over slices. */
var s = parent(600);
var t = s.slice(10, 300);
eq(`[${t}]`, "[" + t + "]", "template basic");
eq(`${t}${t}`, t + t, "template twice");
eq(`${t.length}:${t.charAt(0)}`, "290:" + s.charAt(10), "template nested reads");
function tag(parts, a) { return parts[0] + "|" + a + "|" + parts[1]; }
eq(tag`pre${t}post`, "pre|" + t + "|post", "tagged template");
/* String.raw */
eq(String.raw`x${t}y`, "x" + t + "y", "String.raw");
/* cooked array identity */
function tag2(p, v) { eq(p.raw.length, 2, "raw length"); return v + p.raw[0]; }
eq(tag2`${t}rest`, t, "tag cooked/raw");
/* template over slice with escapes */
var esc = "a\\nb".repeat(200).slice(2, 500);
eq(`v=${esc}`.length, 2 + esc.length, "template escape len");
__L(0, "PASS 08_slice_template");

summary("sliced_strings");
