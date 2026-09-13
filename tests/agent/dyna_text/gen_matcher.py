#!/usr/bin/env python3
"""gen_matcher.py — dyna:matcher black-box matrix generator.

Oracles: pinned python implementations of the DOCUMENTED semantics —
substring search via str.find loops (code-unit offsets via UTF-16 encoding),
Levenshtein as the classic DP (code points), DiceCoefficient per the API's
stated formula (bigram MULTISET intersection, ASCII whitespace stripped,
short-side rule), Myers-diff reconstruction properties. Aho-Corasick rows use
the documented example corpus plus overlap invariants.
"""
import json
from collections import Counter
from gen_common import jslit, norm_json, norm_num, norm_any, write_probe

M_IMPORTS = [("M", "dyna:matcher")]

def u16(s):
    return s.encode("utf-16-le")

def u16pos(s, cp_pos):
    return len(s[:cp_pos].encode("utf-16-le")) // 2

def find_all_cp(text, pat):
    """all overlapping match positions in CODE POINTS, then mapped to code units."""
    out, start = [], 0
    if not pat:
        return out
    while True:
        i = text.find(pat, start)
        if i < 0:
            return out
        out.append(u16pos(text, i))
        start = i + 1

def levenshtein(a, b, maxd=None):
    if maxd is not None:
        d = levenshtein(a, b)
        return d if d <= maxd else maxd + 1
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]

def dice(a, b):
    """documented: bigram multiset intersection; ASCII whitespace stripped
    first; a side under two characters scores 0 unless the sides are equal;
    byte-identical input is 1."""
    if a == b:
        return 1.0
    a = "".join(a.split())
    b = "".join(b.split())
    if len(a) < 2 or len(b) < 2:
        return 0.0
    ga, gb = Counter(a[i:i + 2] for i in range(len(a) - 1)), Counter(b[i:i + 2] for i in range(len(b) - 1))
    inter = sum((ga & gb).values())
    return 2.0 * inter / (len(a) - 1 + len(b) - 1)

def norm_dice(v):
    # __norm number: shortest double repr; keep 4 decimals like the docs show
    return v


PAIRS = [
    ("kitten", "sitting"), ("", ""), ("", "abc"), ("abc", ""),
    ("flaw", "lawn"), ("gumbo", "gambol"), ("café", "cafe"),
    ("日本語", "本語"), ("a", "b"), ("same", "same"),
    ("abcdefghijklmnop", "abcdef"), ("🌍Earth", "Earth"),
    ("1234567", "12345678"),
]
DICE_PAIRS = [
    ("night", "nacht"), ("hello", "hello"), ("a", "b"), ("", ""),
    ("aa", "aaaa"), ("AB C", "AB C"), ("ab cd", "abcd"),
    ("telefon", "telephone"), ("🌍🌍", "🌍"), ("hello world", "hello"),
]


def gen_matcher():
    c = []
    texts = ["xxabcyyabc", "abcabcabc", "", "abc", "aaa", "🌍abc🌍abc",
             "héllo héllo", "a" * 300 + "b" + "a" * 300]
    pats = ["abc", "aa", "a", "🌍a", "héllo", "zzz", "ab"]
    m = 0
    for pat in pats:
        for text in texts:
            alls = find_all_cp(text, pat)
            first = alls[0] if alls else -1
            count = len(alls)
            # test / firstIn
            c.append('case_eq("m%d_first", %s, function(){'
                     ' return new M.Matcher(%s).firstIn(%s); });'
                     % (m, json.dumps(norm_num(first)), jslit(pat), jslit(text)))
            c.append('case_eq("m%d_test", %s, function(){'
                     ' return new M.Matcher(%s).test(%s); });'
                     % (m, json.dumps(norm_any(pat in text)), jslit(pat), jslit(text)))
            c.append('case_eq("m%d_count", %s, function(){'
                     ' return new M.Matcher(%s).countIn(%s); });'
                     % (m, json.dumps(norm_num(count)), jslit(pat), jslit(text)))
            c.append('case_eq("m%d_all", %s, function(){'
                     ' return new M.Matcher(%s).allIn(%s); });'
                     % (m, json.dumps(norm_json(alls)), jslit(pat), jslit(text)))
            # replaceAll: non-overlapping left-to-right
            nrep = 0
            start = 0
            while True:
                i = text.find(pat, start)
                if i < 0:
                    break
                nrep += 1
                start = i + len(pat)
            replaced = text.replace(pat, "Z") if pat else text
            c.append('case_eq("m%d_rep", %s, function(){'
                     ' return new M.Matcher(%s).replaceAllIn(%s, "Z"); });'
                     % (m, json.dumps(norm_any(replaced)), jslit(pat), jslit(text)))
            m += 1
    # empty pattern: the documented special case (occurs at 0; count/all EMPTY)
    c.append('case_eq("m_empty_first", {t:"n",v:"0"}, function(){'
             ' return new M.Matcher("").firstIn("abc"); });')
    c.append('case_eq("m_empty_test", {t:"b",v:"true"}, function(){'
             ' return new M.Matcher("").test("abc"); });')
    c.append('case_eq("m_empty_count", {t:"n",v:"0"}, function(){'
             ' return new M.Matcher("").countIn("abc"); });')
    c.append('case_eq("m_empty_all", {t:"j",v:"[]"}, function(){'
             ' return new M.Matcher("").allIn("abc"); });')
    c.append('case_eq("m_empty_replace", {t:"s",v:"abc"}, function(){'
             ' return new M.Matcher("").replaceAllIn("abc", "Z"); });')
    # algo getter + validation
    c.append('case_eq("m_algo_kmp", {t:"s",v:"kmp"}, function(){'
             ' return new M.Matcher("x", {algo:"kmp"}).algo; });')
    c.append('case_eq("m_algo_bmh", {t:"s",v:"bmh"}, function(){'
             ' return new M.Matcher("x", {algo:"bmh"}).algo; });')
    c.append('case_eq("m_algo_boyer", {t:"s",v:"bmh"}, function(){'
             ' return new M.Matcher("x", {algo:"boyer-moore"}).algo; });')
    c.append('case_err("m_algo_bad", "RangeError", function(){'
             ' return new M.Matcher("x", {algo:" psychic"}); });')
    c.append('case_eq("m_length", {t:"n",v:"3"}, function(){'
             ' return new M.Matcher("abc").length; });')
    return c


def gen_multimatcher():
    c = []
    # the documented example corpus
    c.append('var MM = new M.MultiMatcher(["he", "she", "hers"]);')
    c.append('case_eq("mm_size", {t:"n",v:"3"}, function(){ return MM.size; });')
    c.append('case_eq("mm_states", {t:"n",v:"8"}, function(){ return MM.states; });')
    c.append('case_eq("mm_test", {t:"b",v:"true"}, function(){ return MM.test("ushers"); });')
    c.append('case_eq("mm_first", {t:"j",v:"{\\"index\\":1,\\"at\\":1}"}, function(){ return MM.firstIn("ushers"); });')
    c.append('case_eq("mm_count", {t:"n",v:"3"}, function(){ return MM.countIn("ushers"); });')
    c.append('case_eq("mm_all", {t:"j",v:"[{\\"index\\":1,\\"at\\":1},{\\"index\\":0,\\"at\\":2},{\\"index\\":2,\\"at\\":2}]"},'
             ' function(){ return MM.allIn("ushers"); });')
    # overlapping + same-position matrix, oracle = brute force over the corpus
    corpora = [
        (["ab", "bc"], "abc"),
        (["a", "aa", "aaa"], "aaaa"),
        (["key", "word", "ord"], "keyword keyword"),
        (["🌍"], "x🌍y🌍"),
        (["pattern"], "no match here"),
    ]
    for i, (pats, text) in enumerate(corpora):
        # brute force: every (index, at) with at as a code-unit offset
        hits = []
        for idx, p in enumerate(pats):
            for at in find_all_cp(text, p):
                hits.append({"index": idx, "at": at})
        hits.sort(key=lambda h: (h["at"], h["index"]))
        c.append('var MM%d = new M.MultiMatcher(%s);' % (i, json.dumps(pats, ensure_ascii=True)))
        # same-position emission order follows the automaton's output chain
        # (impl-defined beyond the documented example) -> compare as multisets
        hits_sorted = sorted(hits, key=lambda h: (h["at"], h["index"]))
        c.append('case_eq("mm%d_all", %s, function(){'
                 ' return MM%d.allIn(%s).slice().sort(function(a, b){'
                 ' return (a.at - b.at) || (a.index - b.index); }); });'
                 % (i, json.dumps(norm_json(hits_sorted)), i, jslit(text)))
        c.append('case_eq("mm%d_count", %s, function(){ return MM%d.countIn(%s); });'
                 % (i, json.dumps(norm_num(len(hits))), i, jslit(text)))
        if hits:
            first = min(hits, key=lambda h: (h["at"],))
            c.append('case_eq("mm%d_first", %s, function(){ return MM%d.firstIn(%s); });'
                     % (i, json.dumps(norm_json(first)), i, jslit(text)))
    c.append('case_err("mm_empty_pat", "RangeError", function(){'
             ' return new M.MultiMatcher(["a", ""]); });')
    return c


def gen_levenshtein_dice():
    c = []
    for i, (a, b) in enumerate(PAIRS):
        want = levenshtein(a, b)
        c.append('case_eq("lev%d", %s, function(){ return M.Levenshtein(%s, %s); });'
                 % (i, json.dumps(norm_num(want)), jslit(a), jslit(b)))
        # max cutoff: exact while <= max, max+1 beyond
        for mx in (max(0, want - 1), want, want + 3):
            c.append('case_eq("lev%d_mx%d", %s, function(){'
                     ' return M.Levenshtein(%s, %s, {max: %d}); });'
                     % (i, mx, json.dumps(norm_num(levenshtein(a, b, mx))),
                        jslit(a), jslit(b), mx))
    for i, (a, b) in enumerate(DICE_PAIRS):
        want = dice(a, b)
        c.append('case_eq("dice%d", %s, function(){ return M.DiceCoefficient(%s, %s); });'
                 % (i, json.dumps(norm_num(want)), jslit(a), jslit(b)))
    return c


def gen_diff():
    c = []
    pairs = [
        ("ab", "acb"), ("a\nb", "a\nc"), ("quick brown", "quick red"),
        ("", "abc"), ("abc", ""), ("same", "same"),
        ("the quick brown fox", "the lazy dog"),
        ("line1\nline2\nline3", "line1\nlineX\nline3"),
        ("🌍 abc", "abc 🌍"),
        ("one two three four", "two three five"),
    ]
    for i, (a, b) in enumerate(pairs):
        for kind, tok in (("Chars", None), ("Words", " "), ("Lines", "\n")):
            # reconstruction property = the documented oracle
            c.append('case_true("diff%d_%s_a", function(){'
                     ' var hs = M.Diff%s(%s, %s); var out = "";'
                     ' for (var k = 0; k < hs.length; k++) if (hs[k].op !== 1) out += hs[k].text;'
                     ' return out === %s; });'
                     % (i, kind, kind, jslit(a), jslit(b), jslit(a)))
            c.append('case_true("diff%d_%s_b", function(){'
                     ' var hs = M.Diff%s(%s, %s); var out = "";'
                     ' for (var k = 0; k < hs.length; k++) if (hs[k].op !== -1) out += hs[k].text;'
                     ' return out === %s; });'
                     % (i, kind, kind, jslit(a), jslit(b), jslit(b)))
            c.append('case_true("diff%d_%s_ops", function(){'
                     ' var hs = M.Diff%s(%s, %s);'
                     ' for (var k = 0; k < hs.length; k++)'
                     '  if (hs[k].op !== -1 && hs[k].op !== 0 && hs[k].op !== 1) return false;'
                     ' return hs.length > 0; });'
                     % (i, kind, kind, jslit(a), jslit(b)))
    # identical input: a single common hunk (documented shortcut)
    c.append('case_eq("diff_same", {t:"j",v:"[{\\"op\\":0,\\"text\\":\\"identical\\"}]"},'
             ' function(){ return M.DiffChars("identical", "identical"); });')
    c.append('case_eq("diff_lines_doc", {t:"j",v:'
             '"[{\\"op\\":0,\\"text\\":\\"a\\\\n\\"},{\\"op\\":-1,\\"text\\":\\"b\\"},{\\"op\\":1,\\"text\\":\\"c\\"}]"},'
             ' function(){ return M.DiffLines("a\\nb", "a\\nc"); });')
    return c


def main():
    a = gen_matcher()
    b = gen_multimatcher()
    d = gen_levenshtein_dice()
    e = gen_diff()
    write_probe("matcher/m01_matcher.js", M_IMPORTS, "\n".join(a),
                "Matcher vs pinned python find loops (code-unit offsets)")
    write_probe("matcher/m02_multimatcher.js", M_IMPORTS, "\n".join(b),
                "MultiMatcher vs brute-force corpus (Aho-Corasick overlap)")
    write_probe("matcher/m03_levenshtein_dice.js", M_IMPORTS, "\n".join(d),
                "Levenshtein DP + documented DiceCoefficient formula")
    write_probe("matcher/m04_diff.js", M_IMPORTS, "\n".join(e),
                "DiffChars/Words/Lines reconstruction properties")
    n = sum(len([x for x in c if x.strip().startswith("case_")]) for c in (a, b, d, e))
    print("gen_matcher: %d cases emitted" % n)


if __name__ == "__main__":
    main()
