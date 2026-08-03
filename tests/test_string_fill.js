function eq(a,b,m){ if(a!==b){ print("FAIL "+m+": got "+JSON.stringify(a)+" want "+JSON.stringify(b)); throw new Error(m);} }
/* repeat */
eq("ab".repeat(0), "", "repeat0");
eq("ab".repeat(1), "ab", "repeat1");
eq("ab".repeat(2), "abab", "repeat2");
eq("ab".repeat(3), "ababab", "repeat3");
eq("abc".repeat(5), "abcabcabcabcabc", "repeat5");
eq("x".repeat(0), "", "repeat1char0");
eq("x".repeat(7), "xxxxxxx", "repeat1char7");
eq("☃☄".repeat(3), "☃☄☃☄☃☄", "repeatWide");
eq("☃".repeat(4), "☃☃☃☃", "repeatWide1");
eq("ab".repeat(1000).length, 2000, "repeatBig");
eq("ab".repeat(1000).slice(1996), "abab", "repeatBigTail");
/* pad */
eq("x".padEnd(5), "x    ", "padEndDefault");
eq("x".padStart(5), "    x", "padStartDefault");
eq("x".padEnd(5,"-"), "x----", "padEndChar");
eq("x".padStart(5,"-"), "----x", "padStartChar");
eq("x".padEnd(5,"ab"), "xabab", "padEndMulti");
eq("x".padEnd(1), "x", "padNoop");
eq("x".padEnd(0), "x", "padZero");
eq("x".padEnd(4,"☃"), "x☃☃☃", "padWideChar");
eq("x".padStart(4,"☃"), "☃☃☃x", "padStartWideChar");
eq("☃".padEnd(4,"-"), "☃---", "padWideBaseNarrowPad");
eq("x".padEnd(4,"é"), "xééé", "padLatin1Char");
eq("x".padEnd(200," ").length, 200, "padLong");
eq("x".padEnd(200,"☃").charCodeAt(199), 0x2603, "padLongWide");
print("fill/repeat: ok");

/* Adversarial: exercise the widen path that the presized-capacity branch of
   string_buffer_fill() must still handle (realloc is skipped, so fill() itself
   is responsible for widening the buffer to hold c). */
eq("x".padEnd(3, "Ā"), "xĀĀ", "padWidenExactCapacity");
eq("abc".padStart(6, "￿"), "￿￿￿abc", "padWidenMax");
/* repeat-by-doubling: odd/even totals, and the non-power-of-two tail */
for (var r = 0; r < 40; r++) {
    eq("xyz".repeat(r).length, 3 * r, "repeatLen" + r);
    eq("xyz".repeat(r), new Array(r + 1).join("xyz"), "repeatVal" + r);
    eq("☃☄".repeat(r).length, 2 * r, "repeatWideLen" + r);
}
print("fill/repeat adversarial: ok");
