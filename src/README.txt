Exercise 4 - Modify and optimize the bprofile.cpp pintool
=========================================================

a. Authors
   - <name 1>, <id 1>
   - <name 2>, <id 2>

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
      - <describe the fix>
   2. Avoid saving/restoring dead registers in the profiling stubs:
      - <describe how you detected liveness (e.g. INS_RegWContain / liveness
        of RAX/RBX/RCX/RFLAGS at the instrumentation point) and skipped the
        save/restore when the register is dead>
   3. Output file changed from bprofile.out to edge-profile.csv in the
      exercise-3 CSV format (up to 4 indirect targets, sorted by count):
      - <describe>

d. Large differences vs. the exercise-3 profiling (and why)
   Reference: profiling gathered by the provided (unmodified) bprofile.cpp
   pintool (close to the exercise-2 JIT profile).
   - <difference 1 + reason, e.g. probe-mode + limited -prof_time window
     captures fewer iterations than the full JIT run>
   - <difference 2 + reason>

Performance improvement (Requirement 5)
   Measured with -prof_time <N>. Optimized vs. provided non-optimized version:
   - <measure, e.g. user time>: <baseline> -> <optimized>  (>= 5% improvement)
