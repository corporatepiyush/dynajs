DEF2(mul_loc_loc, 6, 0, 1, loc2)
DEF2(add_loc_loc, 6, 0, 1, loc2)
DEF2(sub_loc_loc, 6, 0, 1, loc2)
DEF2(streq_const_if_false, 7, 1, 1, label_const8)
DEF2(streq_varprop_if_false, 12, 1, 1, label_var_ref_atom)

DEF2(call_method0, 6, 1, 1, atom)
DEF2(call_method1_loc, 9, 1, 1, atom_loc)
DEF2(call_method1_imm8, 8, 1, 1, atom_i8)
DEF2(call_method1_const, 10, 1, 1, atom_const)

DEF2(using_new, 2, 0, 1, none)
DEF2(using_add, 6, 2, 1, loc2)
DEF2(using_dispose, 6, 2, 1, loc2)
