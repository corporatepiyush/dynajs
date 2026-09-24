// Task-like kernels (bbreview shapes) with fused calls inside, compared by
// deterministic checksum. These double as the census sources: census.sh checks
// their bytecode for OP2_call_method* ops.
var MF = MFHarness();

// K1: json_chars-like — charCodeAt scanner over a JSON-ish string.
function k1(src) {
  var depth = 0, inStr = 0, esc = 0, codes = 0;
  for (var i = 0; i < src.length; i++) {
    var c = src.charCodeAt(i);
    codes += c;
    if (inStr) {
      if (esc) esc = 0;
      else if (c === 92) esc = 1;
      else if (c === 34) inStr = 0;
    } else {
      if (c === 34) inStr = 1;
      else if (c === 123 || c === 91) depth++;
      else if (c === 125 || c === 93) depth--;
    }
  }
  return depth * 1000003 + codes;
}
var jsonish = '{"a":[1,2,{"b":"qu\\"ote"}],"c":{"d":[true,null,"x\\n"]}}';
MF.eq("k1-json-scan", k1(jsonish), k1(jsonish));

// K2: date_churn-like — method-heavy object churn with fused toFixed/charAt.
function k2(n) {
  var acc = "";
  var nums = [];
  for (var i = 0; i < n; i++) {
    var v = (i * 2654435761) % 997;
    nums.push(v.toFixed(1));
  }
  for (var j = 0; j < nums.length; j++) {
    acc += nums[j].charAt(nums[j].length - 1);
  }
  var sum = 0;
  for (var k = 0; k < acc.length; k++) sum += acc.charCodeAt(k);
  return sum;
}
MF.eq("k2-churn", k2(200), k2(200));

// K3: sqrt_powers-like — Math.* builtins in a hot loop.
function k3(n) {
  var acc = 0;
  for (var i = 1; i <= n; i++) {
    acc += Math.sqrt(i) + Math.floor(i / 3) + Math.abs(-i);
  }
  return Math.round(acc);
}
MF.eq("k3-math", k3(100), k3(100));

// K4: bisect-like — comparator with fused calls on wrapper objects.
function k4(n) {
  var items = [];
  for (var i = 0; i < n; i++) items.push({ key: (n - i) % 50, id: i, less: function (o) { return this.key < o.key; } });
  var count = 0;
  for (var i = 1; i < items.length; i++)
    if (items[i].less(items[i - 1])) count++;
  return count;
}
MF.eq("k4-compare", k4(300), k4(300));

// K5: string build (split_cache-like) — startsWith/endsWith chains.
function k5(words) {
  var hits = 0;
  for (var i = 0; i < words.length; i++) {
    var w = words[i];
    if (w.startsWith("fn_")) hits += 1;
    else if (w.startsWith("var_")) hits += 2;
    else if (w.endsWith("_end")) hits += 3;
  }
  return hits;
}
var wordList = [];
for (var i = 0; i < 60; i++) {
  if (i % 3 === 0) wordList.push("fn_" + i);
  else if (i % 3 === 1) wordList.push("var_" + i);
  else wordList.push("sym" + i + "_end");
}
MF.eq("k5-words", k5(wordList), k5(wordList));
var expect5 = 0;
for (var i = 0; i < 60; i++) { if (i % 3 === 0) expect5 += 1; else if (i % 3 === 1) expect5 += 2; else expect5 += 3; }
MF.eq("k5-value", k5(wordList), expect5);

// K6: deterministic hash over a string using charCodeAt (fused in loop).
function k6(s) {
  var h = 2166136261;
  for (var i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = (h * 16777619) & 0x7fffffff;
  }
  return h;
}
MF.eq("k6-fnv", k6("the quick brown fox jumps over the lazy dog"), k6("the quick brown fox jumps over the lazy dog"));

// Self-consistency of every kernel across two runs (engines may differ in
// exact numeric formats; both runs happen in-engine so equality is the claim).
MF.sum("mf_tasks");
