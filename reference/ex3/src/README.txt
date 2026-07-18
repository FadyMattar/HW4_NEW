================================================================================
Exercise 3 - btranslate Pintool Fix
README.txt
================================================================================

Names + ID Numbers:
-------------------
[Fady Mattar] - [212866362]
[Donya Nasir] - [324130673]

--------------------------------------------------------------------------------
How to Run the Tool:
--------------------------------------------------------------------------------

Build (produces obj-intel64/btranslate.so; the submitted ex3.so is this same
binary, renamed):
   $ make PIN_ROOT=<pindir>

Run (with the submitted binary):
   $ <pindir>/pin -t ex3.so -- ./sgcc_base.mytest-m64 200.i -o 200.s

Or after building from src:
   $ <pindir>/pin -t obj-intel64/btranslate.so -- ./sgcc_base.mytest-m64 200.i -o 200.s

Tested with the alternative binary sgcc_base.mytest-m64 (provided in the
course announcement): the generated 200.s is byte-identical (verified with
cmp/md5sum) to the output of a native (non-Pin) run, and also byte-identical
to the native output of the original cpugcc_r_base.Oz-m64.

Note: running the pintool on the original cpugcc_r_base.Oz-m64 segfaults
during execution of the translated code, even on an AVX512-capable machine
(the binary runs fine natively there). This is consistent with the course
announcement regarding new AVX512 instructions, so per that announcement we
tested with sgcc_base.mytest-m64.

--------------------------------------------------------------------------------
Bugs Found and Fixed:
--------------------------------------------------------------------------------

Bug 1: Off-by-one in jump_to_orig_addr_map indexing
       (fix_direct_jmp_or_call_to_orig_addr, ~line 607)
-------------------------------------------------------
PROBLEM:
  jump_to_orig_addr_num was incremented BEFORE being used as the new slot
  index. This caused slot 0 to never be written, and at full capacity wrote
  one slot past the end of the buffer (heap corruption -> segfault). The
  RIP-relative displacement also pointed to the wrong uninitialized slot,
  crashing translated code at runtime.

FIX:
  Assign jump_to_orig_addr_map_entry = jump_to_orig_addr_num FIRST,
  then increment jump_to_orig_addr_num.


Bug 2: Missing RTN_IsSafeForProbedReplacement() check in commit loop
       (commit_translated_rtns_to_tc, ~line 974)
-------------------------------------------------------
PROBLEM:
  RTN_ReplaceProbed() was called without first checking if the routine is
  safe to probe. In probe mode, Pin writes a 5-byte JMP at the routine
  header. Short routines (thunks, stubs) don't have 5 bytes of space, so
  Pin overwrites adjacent code or read-only memory -> segfault.

FIX:
  Added RTN_IsSafeForProbedReplacement(rtn) guard before every call to
  RTN_ReplaceProbed(). Routines that fail the check are skipped.


Bug 3: Wrong comparison operator in chain_all_direct_jmp_and_call_target_entries()
       (~line 447)
-------------------------------------------------------
PROBLEM:
  The condition was `targ_map_entry > 0` instead of `>= 0`. Since
  targ_map_entry is initialized to -1 (unresolved) and set to a
  non-negative index after chaining, the check > 0 incorrectly re-processed
  entries already resolved to index 0, corrupting branch displacements for
  any branch targeting the first instruction in the map.

FIX:
  Changed condition to `targ_map_entry >= 0`.


Bug 4: RTN_Size() is unreliable for checking probe size requirement
       (commit_translated_rtns_to_tc, ~line 980)
-------------------------------------------------------
PROBLEM:
  Even after Bug 2's fix, some routines still caused RTN_ReplaceProbed()
  to fail (e.g. "push 0x1", "mov rcx, rdi", "push rax"). RTN_Size() is
  derived from the symbol table, not actual instruction bytes, so it
  over-reports the size of tiny stub routines and they slip through the
  size guard.

FIX:
  Replaced RTN_Size() < 5 with actual XED instruction decoding: walk
  instructions from RTN_Address(rtn) accumulating real decoded byte counts
  until >= 5. Only routines with at least 5 real bytes at their entry are
  probed. This is the same XED decoder already used elsewhere in the pintool.

================================================================================
