Exercise 4 - Modify and optimize the bprofile.cpp pintool
=========================================================

a. Authors
   - Fady Mattar, 212866362
   - Donya Nasir, 324130673

b. How to run the tool
   Build (needs a Pin 4.0 kit; PIN_ROOT points to it):
       make PIN_ROOT=<path-to-pin-kit> TARGET=intel64
   This produces obj-intel64/bprofile.so.

   Run on an input binary, collecting the profile for <N> seconds:
       <PIN_ROOT>/pin -t obj-intel64/bprofile.so -prof_time <N> -- <binary> [args]
   The tool writes edge-profile.csv in the current directory.

   Output format (one line per BBL, sorted hottest -> coldest):
       <bbl addr>, <exec count>, <taken count>, <fallthru count>,
                   <targ1 addr>, <targ1 count>, ... up to 4 indirect targets

c. Problems fixed in the pintool and how
   1. Running on all provided binaries (bzip2, cc1, sgcc_base, sgcc_peak):
      - Disable mechanism corruption: The original `disable_profiling_in_tc` routine caused code corruption because the NOP stub (`NOP4`) was smaller than the 5-byte skip JMP, causing an overspill into the next instruction. Additionally, patching the code byte-by-byte with `memcpy` on a live translation cache caused torn instruction execution (crashing cc1 and corrupting bzip2). We fixed this by using an exact 5-byte NOP and an 8-byte atomic store.
      - Probe replacement failures on tiny routines: `RTN_ReplaceProbed` failed on sub-5-byte routines, as the 5-byte probe JMP would overwrite adjacent code. We added size checks via `RTN_IsSafeForProbedReplacement` and XED byte counting to safely skip these routines.
      - Graceful Native Fallback for untranslatable routines: Conditional branches in `sgcc` with targets outside the translation cache previously aborted the tool. We modified the rejection block in `fix_direct_jmp_or_call_to_orig_addr` to gracefully fall back to native execution.
   2. Avoid saving/restoring dead registers in the profiling stubs:
      - We implemented high-coverage basic-block level liveness analysis for `RAX`. We scan forward from the instrumentation point until the end of the basic block (terminating at jumps, calls, returns, or explicitly identified jump targets via `is_targ_map`). If an instruction writes/kills `RAX` unconditionally before reading it, we omit the push/pop overhead, using the register directly.
   3. Output file changed from bprofile.out to edge-profile.csv in the
      exercise-3 CSV format (up to 4 indirect targets, sorted by count):
      - We accumulated block executions in `dump_profile`, recording execution counts, taken vs fallthrough counters for conditional branches, and tracking up to 4 targets for indirect branches. The output was correctly formatted into a CSV and sorted from hottest to coldest.

d. Large differences vs. the exercise-3 profiling (and why)
   Reference: profiling gathered by the provided (unmodified) bprofile.cpp
   pintool (close to the exercise-2 JIT profile).
   - Shared libraries are excluded: Exercise 2 (JIT) profiles the entire process including shared libraries. Exercise 4 (probe mode) translates `IMG_IsMainExecutable` only.
   - Address spaces differ: Exercise 2 uses runtime/ASLR addresses, while Exercise 4 relies on static ELF addresses.
   - Counting differs: Exercise 2 counts the full run. Exercise 4 counts only during the limited `-prof_time` window.

Performance improvement (Requirement 5)
   Measured with -prof_time <N>. Optimized vs. provided non-optimized version:
   - Our fully functional disable mechanism (e.g. `prof_time=2`) allows profiling to actually terminate safely, resulting in an execution time of ~8.68 seconds on `bzip2`. The provided non-optimized version crashed when `-prof_time` was triggered, forcing it to run completely un-disabled (~10.48 seconds). By fixing the disable feature and adding our dead-register optimization, we achieved a >17% performance improvement while remaining functionally correct.
