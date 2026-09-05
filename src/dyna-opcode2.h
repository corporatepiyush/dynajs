/* Bank-2 opcodes — the extended opcode space reached through OP_ext.
 *
 * Encoding in the bytecode stream: [OP_ext][op2][operands...], where op2 is a
 * byte selecting the entry below. This lifts the 1-byte opcode ceiling (bank 1
 * = 256) to 512 total. Bank-2 ops are the less-hot / newer superinstructions;
 * the hottest ops stay in bank 1 (single dispatch).
 *
 * DEF2(id, size, n_pop, n_push, fmt):
 *   - `size` is the TOTAL instruction length in bytes INCLUDING the 2-byte
 *     [OP_ext][op2] prefix (so a two-u16-operand op is 2 + 2 + 2 = 6).
 *   - `fmt` must not be an atom/label form in this bank yet: operands are raw
 *     (local indices), so (de)serialization copies them verbatim — no fixup.
 *
 * CATEGORY SHARDING: ops are grouped into contiguous category ranges, and the
 * interpreter lays their handlers out in the SAME order. Two payoffs:
 *   1. locality — a hot loop that stays within one category keeps that
 *      category's handlers hot in L1i and its computed-goto targets clustered,
 *      which the indirect-branch predictor (BTB) handles better.
 *   2. range checks — the optimizer/specializer tests membership with a pair of
 *      compares (OP2_<CAT>_FIRST..LAST) instead of a switch.
 * Keep each category's DEF2 rows contiguous; add new ops at the end of their
 * category. Category bounds are derived in dynajs.c (OP2_<CAT>_FIRST/LAST).
 *
 * Every consumer that walks bytecode by size (interpreter dispatch,
 * compute_stack_size, bc_read/bc_write) special-cases OP_ext and reads the real
 * length/pops/pushes from opcode_info2[op2]. The untrusted reader MUST check
 * op2 < OP2_COUNT before indexing opcode_info2[].
 */

/* ═══ category ARITH — fused binary arithmetic superinstructions ═══
   Read two locals directly and push the result: replaces
   <get_loc|get_loc_check> A; <get_loc|get_loc_check> B; <arith> (7 bytes, 3
   dispatches, 4 stack ops) with one 6-byte op and a single dispatch. Each
   operand is TDZ-checked (JS_IsUninitialized) exactly like OP_get_loc_check
   before the arithmetic, so the op serves lexical (`let`/`const`) and plain
   (`var`) locals and any mix — a `var` slot is never uninitialized, so its
   check never fires. Emitted by resolve_labels (gate CONFIG_FUSED_ARITH);
   fast paths mirror OP_mul/OP_add/OP_sub for byte-identical output. */
DEF2(  mul_loc_loc, 6, 0, 1, loc2)
DEF2(  add_loc_loc, 6, 0, 1, loc2)
DEF2(  sub_loc_loc, 6, 0, 1, loc2)
/* (future ARITH: div_loc_loc, {mul,add,sub}_loc_const, …) */

/* ═══ category BRANCH — fused compare+branch extensions (reserved) ═══
   if_true-polarity relational + strict_eq/neq+branch; label operands, so this
   shard adds a label-aware path to the bank-2 (de)serializer when populated. */

/* ═══ category PROP — property / inline-cache extensions (reserved) ═══ */

/* ═══ category SIMD — autovec kernel dispatch (reserved) ═══ */
