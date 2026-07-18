# ex2 Intel Pin Tool

Student 1 : Donya Nasir
ID : 324130673

Student 2: Fady Mattar
ID : 212866362

## Description
This is an Intel Pin tool designed to run in JIT mode and collect:
1. Basic block execution counts, conditional branch taken/fallthrough frequencies, and up to 10 indirect targets per indirect branch (`edge-profile.csv`).
2. Up to 20 register states (RAX, RBX, RCX, RDX, RSI, RDI) at the entry of each routine, including the calculated average delta if executed more than once (`rtn-output.csv`).

The tool uses `IARG_FAST_ANALYSIS_CALL` and memory-optimized structures to remain extremely lightweight and satisfy the < 5 seconds execution requirement on the `bzip2` benchmark.

## Compilation Instructions
Ensure you have the Intel Pin framework installed.

1. Navigate to the `src` directory:
   `cd src`
2. Build the tool by specifying your Pin root directory (if not located in `source/tools/`):
   `make PIN_ROOT=/path/to/pin`
   (If you copied the `src` folder into `pin/source/tools/ex2`, you can simply run `make`).

## Run Instructions
Run the tool against your target binary (e.g. `bzip2`) as follows:

```bash
<pindir>/pin -t obj-intel64/ex2.so -- ./bzip2 -k -f input.txt
```

After execution, two output files will be generated in the current working directory:
- `edge-profile.csv`
- `rtn-output.csv`
