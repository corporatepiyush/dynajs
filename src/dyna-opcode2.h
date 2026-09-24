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
 *   - `fmt` raw-operand forms (loc2, label_const8's cpool byte) are copied
 *     verbatim by the (de)serializer; label-bearing forms (label_const8,
 *     label_var_ref_atom) carry position-RELATIVE u32 offsets (no fixup, same
 *     as bank-1 labels) but must be taught to: the read-time label-target
 *     validator, the stack-size simulator's branch exploration,
 *     compute_stack_size, and bc_byte_swap; atom-bearing forms
 *     (label_var_ref_atom) additionally need the atom relocate/fixup/free
 *     paths in JS_WriteFunctionBytecode / JS_ReadFunctionBytecode /
 *     free_bytecode_atoms.
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

/* ═══ category BRANCH — fused switch-probe superinstructions ═══
   The switch compare-chain probe (CONFIG: every switch lowers to
   `dup; <case expr>; strict_eq; if_false` per case; the dense OP_switch table
   only engages when EVERY case is a const-int literal, so computed labels like
   `case OP.HALT:` always run the chain). These ops fold the whole probe into
   ONE dispatch; the label operand is the same relative-offset u32 OP_if_false
   carries, so the jump-shrink pass renumbers/rewrites it through jump_slots
   like any other non-shrinking label-bearing op (with_* / catch / *_if_false
   fused compares): jp->op = OP_ext is not in the shrink switch, so the operand
   stays 4 bytes and only participates in compaction renumbering + the final
   put_u32 fixup.

   Both peek sp[-1] (v, the discriminant) and LEAVE it — the original chain's
   net effect (dup made the copy, strict_eq_if_false consumed both operands).
   Branch is taken when v is NOT strict-equal (the if_false polarity).
   n_pop/n_push are 1/1 (net 0) so compute_stack_size and the read-time
   validator demand depth >= 1 at the op (stronger than OP_switch's peek).

   OP2_streq_const_if_false [label:u32][const:u8]: peeks v, strict-compares
   against cpool[const]; both operands BORROWED (v stays, cpool is immortal),
   so no frees on any path. Folds `dup; push_i32(k)|push_const(k)|
   push_atom_value(a); strict_eq; if_false L` (int/atom materialized into the
   cpool at fold time; fold skipped when the resulting cpool index >= 256 or
   the atom is not a plain string atom).

   OP2_streq_varprop_if_false [label:u32][var_ref:u16][atom:u32]: peeks v,
   reads var_refs[var_ref] with OP_get_var's exact semantics (TDZ
   ReferenceError for lexical closure vars, undeclared-global fallback via
   JS_GetPropertyInternal), loads .atom with OP_get_field's exact semantics
   (inline proto walk, getter/receiver = the object, can throw -> exception
   path), strict-compares against v, frees the loaded value, branches when not
   equal. Folds `dup; get_var(n); get_field(a); strict_eq; if_false L` — the
   `case CONST_OBJ.PROP:` shape that dominates computed-label switches. */
DEF2(streq_const_if_false, 7, 1, 1, label_const8)
DEF2(streq_varprop_if_false, 12, 1, 1, label_var_ref_atom)


/* ═══ category CALL — fused method-call superinstructions ═══
   Fold `get_field2 <atom>; [<hot arg op>]; call_method <argc>` — the
   method-call shape whose ARGUMENTS sit BETWEEN the property load and the
   call (which is why forward matching from get_field2 can never see the
 call_method, audit ). resolve_labels matches BACKWARD at the
   call_method walk position: a memo armed when OP_get_field2 is emitted
   bounds the output window, which must be EXACTLY [get_field2][at most one
   hot arg op] (any other emission between them breaks the exact-fill length
   check -> no fold, generic ops stay). The memo is invalidated on OP_label,
   so a jump target can never end up inside a folded window, and after every
   call_method (folded or not), so stale memos cannot mis-fold.

   Single property load, single dispatch: the op pops the receiver, performs
   the OP_get_field2 load exactly (inline proto walk / getter / exception),
   materializes the one argument with the EXACT semantics of its source op
      (get_loc / get_loc_check / get_arg / push_i8-family / push_const8 /
   push_const), then calls exactly like OP_call_method (typeof-object check
   and TypeError included), frees this/func/arg inline like the generic call
   and pushes the result. argc is fixed per op2, so n_pop/n_push are static
   (every size-walking pass works unchanged). STACK ACCOUNTING: the method
   value and the argument never touch the interpreter stack — the op POPS the
   receiver and PUSHES the result (n_pop 1, n_push 1 for all four). */

/* The interpreter fast path dispatches JS_CLASS_C_FUNCTION callees straight
   through js_call_c_function (exactly what JS_CallInternal's non-bytecode
   routing does); everything else falls back to JS_CallInternal unchanged.

   Formats: atom (plain u32 atom, call_method0), atom_loc (form u8: 0=get_loc,
   1=get_loc_check, 2=get_arg, 3=push_i16 + u16 operand), atom_i8 (form u8:
   0=int imm, covering the source push_minus1/push_0..7/push_i8 forms,
   1=push_const8 + i8 imm), atom_const (u32 cpool index). Atom-bearing, so
   they join the read-time atom fixup (incl. ROM), the writer's atom
   relocation and free_bytecode_atoms; operand ranges (form byte, idx,
   cpool index) are validated at read time like the bank-1 indexed formats. */
DEF2(call_method0, 6, 1, 1, atom)
DEF2(call_method1_loc, 9, 1, 1, atom_loc)
DEF2(call_method1_imm8, 8, 1, 1, atom_i8)
DEF2(call_method1_const, 10, 1, 1, atom_const)


/* ═══ category PROP — property / inline-cache extensions (reserved) ═══ */

/* ═══ category SIMD — autovec kernel dispatch (reserved) ═══ */


/* ═══ category ERM — explicit resource management (`using` declarations) ═══

   The single-u16 operand rides in the raw loc2 form (two raw u16 slots; the
   second is unused/zero and copied verbatim by the serializer).

   OP2_using_add [hint:u16]: stack recs value -> value. Validates the
   initializer per spec AddDisposableResource (null/undefined: no-op for the
   sync hint, await-marker otherwise; non-object or missing/non-callable
   dispose method: TypeError) and registers it on the capability.

   OP2_using_dispose [flags:u16]: stack recs pending_or_dummy -> undefined |
   promise. bit0 = async (drive the reaction chain, push the capability
   promise; the parser follows with OP_await), bit1 = pending (the top value
   is the caught completion seeded into the SuppressedError chain; the sync
   path always throws, the async path rejects). bit1 clear: the top value is
   an OP_undefined dummy. */
DEF2(using_new, 2, 0, 1, none)
DEF2(using_add, 6, 2, 1, loc2)
DEF2(using_dispose, 6, 2, 1, loc2)
