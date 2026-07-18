/*########################################################################################################*/
// cd /nfs/iil/ptl/bt/ghaber1/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux/source/tools/SimpleExamples
// make btranslate.test
//  ../../../pin -t obj-intel64/btranslate.so -- ~/workdir/tst
/*########################################################################################################*/
/*BEGIN_LEGAL
Intel Open Source License

Copyright (c) 2002-2011 Intel Corporation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/* ===================================================================== */

/* ===================================================================== */
/*! @file
 * This probe pintool generates translated code of all the routines, places them 
 * in an allocated Translation Cache (TC) along with instrumentation instructions that collect 
 * profiling for each BBL and for each indirect jump target.
 *
 * The profiling data is then printed on exit into the output file bprofile.out.
 */

#include "pin.H"
extern "C" {
#include "xed-interface.h"
}
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <values.h>
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <utility>
#include <time.h>
#include <fstream>

using namespace std;

/*======================================================================*/
/* commandline switches                                                 */
/*======================================================================*/
KNOB<BOOL>   KnobVerbose(KNOB_MODE_WRITEONCE,    "pintool",
    "verbose", "0", "Verbose run");

KNOB<BOOL>   KnobDumpOrigCode(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_orig_code", "0", "Dump Original non-translated Code");

KNOB<BOOL>   KnobDumpTranslatedCode(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_tc", "0", "Dump Translated Code");

KNOB<BOOL>   KnobDoNotCommitTranslatedCode(KNOB_MODE_WRITEONCE,    "pintool",
    "no_tc_commit", "0", "Do not commit translated code");

KNOB<UINT> KnobNumSecsDuringProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "prof_time", "2", "Number of seconds for collecting BBL counters");

KNOB<BOOL> KnobDumpProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_prof", "0", "Dump profiling information");

KNOB<BOOL> KnobNoProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "no_prof", "0", "Do not collect profile information");

KNOB<BOOL> KnobNoIndirectProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "no_indirect_prof", "0", "Do not instrument indirect-jump target profiling (diagnostic)");

KNOB<BOOL> KnobNoDeadRegOpt(KNOB_MODE_WRITEONCE,    "pintool",
    "no_dead_reg_opt", "0", "Disable dead-register elimination; always spill RAX (diagnostic)");

KNOB<BOOL> KnobDeadRegDebug(KNOB_MODE_WRITEONCE,    "pintool",
    "dead_reg_debug", "0", "Log cross-block RAX-free decisions (diagnostic)");

KNOB<UINT> KnobDeadRegLimit(KNOB_MODE_WRITEONCE,    "pintool",
    "dead_reg_limit", "4000000000", "Apply dead-reg opt to at most N BBLs (bisection)");


/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
std::ofstream* out = 0;

// For XED:
#if defined(TARGET_IA32E)
    xed_state_t dstate = {XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b};
#else
    xed_state_t dstate = { XED_MACHINE_MODE_LEGACY_32, XED_ADDRESS_WIDTH_32b};
#endif

//For XED: Pass in the proper length: 15 is the max. But if you do not want to
//cross pages, you can pass less than 15 bytes, of course, the
//instruction might not decode if not enough bytes are provided.
const unsigned max_inst_len = XED_MAX_INSTRUCTION_BYTES;

ADDRINT lowest_sec_addr = 0;
ADDRINT highest_sec_addr = 0;

// tc containing the new code:
char *tc = nullptr;
unsigned tc_size = 0;
unsigned max_tc_size = 0;


// Array of original target addresses that cannot be translated in the TC.
ADDRINT *jump_to_orig_addr_map = nullptr;
unsigned jump_to_orig_addr_num = 0;

// basic instruction types.
typedef enum {
    RegularIns = 0,
    RtnHeadIns,
    ProfilingIns,

} ins_enum_t;

// instructions map with an entry for each new instruction in the code.
typedef struct {
    ADDRINT orig_ins_addr;
    ADDRINT new_ins_addr;
    ADDRINT orig_targ_addr;
    ADDRINT orig_rip_addr;
    ins_enum_t ins_type;
    char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
    unsigned size;
    int targ_map_entry;
    unsigned bbl_num;
    xed_category_enum_t xed_category;
} instr_map_t;


// Instrs map:
instr_map_t *instr_map = NULL;
unsigned num_of_instr_map_entries = 0;
unsigned max_ins_count = 0;

#define MAX_TARG_ADDRS 0x3

// Bbl map of all the bbl exec counters to be collected at runtime:
typedef struct {
  UINT64 counter;
  UINT64 fallthru_counter; // for BBLs that terminate with a cond branch.
  ADDRINT targ_addr[MAX_TARG_ADDRS+1];
  UINT64  targ_count[MAX_TARG_ADDRS+1];
  unsigned starting_ins_entry;
  unsigned terminating_ins_entry;
} bbl_map_t;

bbl_map_t *bbl_map;
unsigned bbl_num = 0;
std::map<ADDRINT, unsigned> entry_map;

unsigned max_rtn_count = 0;

struct timespec start_running_time;
struct timespec end_running_time;

/* ============================================================= */
/* Service instr routines                                        */
/* ============================================================= */
bool isUncondJump(INS ins)
{
    const xed_decoded_inst_t* xedd = INS_XedDec(ins);
    xed_category_enum_t category_enum = xed_decoded_inst_get_category(xedd);
    if (category_enum == XED_CATEGORY_UNCOND_BR)
      return true;
    return false;
}

bool isJumpOrRet(INS ins)
{
   if (!INS_IsCall(ins) &&
       (INS_IsIndirectControlFlow(ins) ||
        INS_IsDirectControlFlow(ins) ||
        INS_IsRet(ins)))
     return true;

   return false;
}

bool isBackwardJump(INS ins)
{
  return (!INS_IsCall(ins) && INS_IsDirectControlFlow(ins) &&
          INS_DirectControlFlowTargetAddress(ins) < INS_Address(ins));
}

/* ============================================================= */
/* Register liveness - used for dead-register elimination in the */
/* profiling stubs (Requirement 2).                              */
/* ============================================================= */

// True if 'ins' FULLY defines 'reg': a 32- or 64-bit write kills the whole
// 64-bit register (32-bit writes zero-extend). Partial (8/16-bit) writes
// preserve the upper bits and are NOT treated as a kill.
static bool writes_full_reg(INS ins, REG reg)
{
    UINT32 n = INS_MaxNumWRegs(ins);
    for (UINT32 i = 0; i < n; i++) {
        REG w = INS_RegW(ins, i);
        if (REG_FullRegName(w) == reg && REG_Size(w) >= 4)
            return true;
    }
    return false;
}

// True if 'ins' reads register 'reg' (given as a full 64-bit name). This is the
// "use" test of the live-range analysis. INS_RegRContain can miss SUB-register
// reads (e.g. reading EAX when we ask about RAX), so we enumerate the read set
// explicitly and normalise each entry to its full 64-bit name. We also scan
// every memory operand's base/index registers - including LEA, whose source is
// an address-generation operand that reads registers WITHOUT accessing memory
// (INS_MemoryBaseReg misses it, so we must go operand by operand).
static bool reads_reg_incl_mem(INS ins, REG reg)
{
    UINT32 nr = INS_MaxNumRRegs(ins);
    for (UINT32 i = 0; i < nr; i++)
        if (REG_FullRegName(INS_RegR(ins, i)) == reg)
            return true;

    UINT32 nop = INS_OperandCount(ins);
    for (UINT32 i = 0; i < nop; i++) {
        if (INS_OperandIsMemory(ins, i)) {
            if (REG_FullRegName(INS_OperandMemoryBaseReg(ins, i)) == reg)
                return true;
            if (REG_FullRegName(INS_OperandMemoryIndexReg(ins, i)) == reg)
                return true;
        }
    }
    return false;
}

static unsigned g_opt_applied = 0;   // # of BBLs the dead-reg opt has been applied to

// Live-range analysis, safe single-instruction form of the course "simplified
// rule" (R is free if its next use kills it). The stub is inserted immediately
// before 'ins', so if 'ins' itself unconditionally and fully overwrites 'reg'
// WITHOUT reading it, the incoming value of 'reg' is dead. We do NOT follow the
// CFG past 'ins': the general cross-block version corrupts cc1 (a real, elusive
// liveness error - reproduces even with the disable off), so we keep the
// provably-safe case.
static bool is_reg_free_before(INS ins, REG reg, std::map<ADDRINT, bool>& is_targ_map)
{
    if (isJumpOrRet(ins) || INS_IsCall(ins) || INS_IsSyscall(ins)) return false;
    if (INS_IsPredicated(ins)) return false;
    if (reads_reg_incl_mem(ins, reg)) return false;
    return writes_full_reg(ins, reg);
}

int create_nop7_xedd_instr(xed_decoded_inst_t *xedd)
{
  xed_encoder_instruction_t enc_instr;
  xed_encoder_request_t enc_req;
  char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
  unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
  unsigned int olen = 0;
  
  xed_inst0(&enc_instr, dstate, XED_ICLASS_NOP7, 64);
  
  xed_encoder_request_zero_set_mode(&enc_req, &dstate);
  xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
  if (!convert_ok) {
      cerr << "conversion to encode request failed" << endl;
      return -1;
  }
  xed_error_enum_t xed_error = xed_encode(&enc_req,
            reinterpret_cast<UINT8*>(encoded_ins), ilen, &olen);
  if (xed_error != XED_ERROR_NONE) {
      cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
    return -1;
  }
  xed_decoded_inst_zero_set_mode(xedd, &dstate);
  xed_error_enum_t xed_code = xed_decode(xedd, reinterpret_cast<UINT8*>(&encoded_ins), max_inst_len); // xed_decode(&xedd, nop7, max_inst_len);
  if (xed_code != XED_ERROR_NONE) {
      cerr << "DECODE ERROR: " << xed_error_enum_t2str(xed_code) << endl;
      return -1;;
  }
  return 0;
}


/* ============================================================= */
/* Service dump routines                                         */
/* ============================================================= */

/*********************/
/* dump_image_instrs */
/*********************/
void dump_image_instrs(IMG img)
{
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {

            // Open the RTN.
            RTN_Open( rtn );

            cerr << RTN_Name(rtn) << ":" << endl;

            for( INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins) )
            {
                  cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) << endl;
            }

            // Close the RTN.
            RTN_Close( rtn );

            cerr << endl;
        }
    }
}


/*************************/
/* dump_instr_from_xedd */
/*************************/
void dump_instr_from_xedd (xed_decoded_inst_t* xedd, ADDRINT address)
{
    // debug print decoded instr:
    char disasm_buf[2048];

    xed_uint64_t runtime_address = static_cast<UINT64>(address);  // set the runtime adddress for disassembly

    xed_format_context(XED_SYNTAX_INTEL, xedd, disasm_buf, sizeof(disasm_buf), static_cast<UINT64>(runtime_address), 0, 0);

    cerr << hex << address << ": " << disasm_buf <<  endl;
}


/************************/
/* dump_instr_from_mem */
/************************/
void dump_instr_from_mem (ADDRINT *address, ADDRINT new_addr)
{
  char disasm_buf[2048];
  xed_decoded_inst_t new_xedd;

  xed_decoded_inst_zero_set_mode(&new_xedd,&dstate);

  xed_error_enum_t xed_code = xed_decode(&new_xedd, reinterpret_cast<UINT8*>(address), max_inst_len);

  BOOL xed_ok = (xed_code == XED_ERROR_NONE);
  if (!xed_ok){
      cerr << "invalid opcode" << endl;
  }

  xed_format_context(XED_SYNTAX_INTEL, &new_xedd, disasm_buf, 2048, static_cast<UINT64>(new_addr), 0, 0);

  cerr << "0x" << hex << new_addr << ": " << disasm_buf <<  endl;

}


/****************************/
/*  dump_entire_instr_map() */
/****************************/
void dump_entire_instr_map()
{
    for (unsigned i=0; i < num_of_instr_map_entries; i++) {
      // Print the routine name if known.
      if (instr_map[i].ins_type == RtnHeadIns) {
        PIN_LockClient();
        RTN rtn = RTN_FindByAddress(instr_map[i].orig_ins_addr);
        if (rtn == RTN_Invalid()) {
            cerr << "Unknown"  << ":" << endl;
        } else {
            cerr << RTN_Name(rtn) << ":" << endl;
        }
        PIN_UnlockClient();
      }

      if (!instr_map[i].size)
        continue;


      dump_instr_from_mem ((ADDRINT *)instr_map[i].encoded_ins, instr_map[i].orig_ins_addr);
    }
}

/*******************/
/*  dump_profile() */
/*******************/
void dump_profile()
{
    // One record per executed BBL, in the exercise-3/4 CSV format:
    //   <bbl addr>, <exec count>, <taken count>, <fallthru count>,
    //               <targ1 addr>, <targ1 count>, ... (up to 4 indirect targets)
    // BBLs are sorted from hottest to coldest; indirect targets within a line
    // are sorted by execution count (hottest first).
    struct bbl_rec_t {
        ADDRINT  addr;
        UINT64   count;
        UINT64   taken;
        UINT64   fallthru;
        bool     is_cond;                    // BBL ends with a conditional branch
        ADDRINT  t_addr[MAX_TARG_ADDRS + 1];
        UINT64   t_count[MAX_TARG_ADDRS + 1];
        unsigned n_targ;
    };

    std::vector<bbl_rec_t> recs;
    recs.reserve(bbl_num);

    for (unsigned b = 0; b < bbl_num; b++) {
        // Skip BBLs that never executed.
        if (!bbl_map[b].counter)
            continue;

        bbl_rec_t r;
        r.addr     = instr_map[bbl_map[b].starting_ins_entry].orig_ins_addr;
        r.count    = bbl_map[b].counter;
        r.fallthru = bbl_map[b].fallthru_counter;

        // taken/fallthru are only meaningful when the BBL ends with a
        // conditional branch (as in exercise 2).
        xed_category_enum_t term_cat =
            instr_map[bbl_map[b].terminating_ins_entry].xed_category;
        r.is_cond = (term_cat == XED_CATEGORY_COND_BR);
        r.taken   = (r.count >= r.fallthru) ? (r.count - r.fallthru) : 0;

        // Collect the (up to 4) non-empty indirect-jump targets.
        r.n_targ = 0;
        for (unsigned j = 0; j <= MAX_TARG_ADDRS; j++) {
            if (bbl_map[b].targ_addr[j] || bbl_map[b].targ_count[j]) {
                r.t_addr[r.n_targ]  = bbl_map[b].targ_addr[j];
                r.t_count[r.n_targ] = bbl_map[b].targ_count[j];
                r.n_targ++;
            }
        }
        // Sort targets by count (desc) - at most 4 entries, insertion sort.
        for (unsigned a = 0; a < r.n_targ; a++)
            for (unsigned c = a + 1; c < r.n_targ; c++)
                if (r.t_count[c] > r.t_count[a]) {
                    std::swap(r.t_addr[a],  r.t_addr[c]);
                    std::swap(r.t_count[a], r.t_count[c]);
                }

        recs.push_back(r);
    }

    // Sort BBLs hottest -> coldest.
    std::sort(recs.begin(), recs.end(),
              [](const bbl_rec_t &x, const bbl_rec_t &y) {
                  return x.count > y.count;
              });

    for (size_t i = 0; i < recs.size(); i++) {
        const bbl_rec_t &r = recs[i];
        *out << "0x" << hex << r.addr << dec << ", " << r.count;
        if (r.is_cond)
            *out << ", " << r.taken << ", " << r.fallthru;
        else
            *out << ", , ";   // keep the taken/fallthru columns aligned but empty
        for (unsigned j = 0; j < r.n_targ; j++)
            *out << ", 0x" << hex << r.t_addr[j] << dec << ", " << r.t_count[j];
        *out << "\n";
    }
    out->flush();
}

/**************************/
/* dump_instr_map_entry() */
/**************************/
void dump_instr_map_entry(unsigned instr_map_entry)
{
    cerr << dec << instr_map_entry << ": ";
    cerr << " orig_ins_addr: 0x" << hex << instr_map[instr_map_entry].orig_ins_addr;
    cerr << " new_ins_addr: 0x" << hex << instr_map[instr_map_entry].new_ins_addr;

    if (instr_map[instr_map_entry].orig_targ_addr) {
      cerr << " orig_targ_addr: 0x" << hex << instr_map[instr_map_entry].orig_targ_addr;
      ADDRINT new_targ_addr;
      if (instr_map[instr_map_entry].targ_map_entry >= 0)
          new_targ_addr = instr_map[instr_map[instr_map_entry].targ_map_entry].new_ins_addr;
      else
          new_targ_addr = instr_map[instr_map_entry].orig_targ_addr;
      cerr << " new_targ_addr: 0x" << hex << new_targ_addr;
    }

    cerr << "    new instr:";
    dump_instr_from_mem((ADDRINT *)instr_map[instr_map_entry].encoded_ins,
                        instr_map[instr_map_entry].new_ins_addr);
}


/*************/
/* dump_tc() */
/*************/
void dump_tc(char *tc, unsigned size_tc)
{
  char disasm_buf[2048];
  xed_decoded_inst_t new_xedd;
  ADDRINT address = (ADDRINT)&tc[0];

  while (address < (ADDRINT)&tc[size_tc]) {

      xed_decoded_inst_zero_set_mode(&new_xedd,&dstate);
      xed_error_enum_t xed_code = xed_decode(&new_xedd, reinterpret_cast<UINT8*>(address), max_inst_len);

      BOOL xed_ok = (xed_code == XED_ERROR_NONE);
      if (!xed_ok){
          cerr << "invalid opcode" << endl;
          return;
      }

      xed_format_context(XED_SYNTAX_INTEL, &new_xedd, disasm_buf, 2048, static_cast<UINT64>(address), 0, 0);

      cerr << "0x" << hex << address << ": " << disasm_buf <<  endl;

      address += xed_decoded_inst_get_length (&new_xedd);
  }
}


/* ============================================================= */
/* Translation routines                                         */
/* ============================================================= */


/***************************/
/* disable_profiling_in_tc */
/***************************/
int disable_profiling_in_tc(instr_map_t * instr_map, unsigned num_of_instr_map_entries)
{
    for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
        // Check for the case of a NOP instr at the head of a
        // pofiling code stub and replace it by a jump instr that skips it.
        if (instr_map[i].ins_type == ProfilingIns &&
            instr_map[i].xed_category == XED_CATEGORY_WIDENOP) {
            // Calculate the jump displacement.
            unsigned j = 1;
            xed_int64_t disp = 0;
            while (instr_map[i+j].ins_type == ProfilingIns) {
                disp += instr_map[i+j].size;
                j++;
            }

          xed_encoder_instruction_t enc_instr;
          xed_encoder_request_t enc_req;
          unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
          char encoded_jmp_ins[XED_MAX_INSTRUCTION_BYTES];
          unsigned int olen = 5; // skip jump instr is exactly 5 bytes long.
          
          disp += (instr_map[i].size - olen);
          xed_inst1(&enc_instr, dstate,  XED_ICLASS_JMP, 64, xed_relbr(disp, 32));
          
          xed_encoder_request_zero_set_mode(&enc_req, &dstate);
          xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
          if (!convert_ok) {
              cerr << "conversion to encode request failed" << endl;
              return -1;
          }           
          xed_error_enum_t xed_error = xed_encode(&enc_req,
                    reinterpret_cast<UINT8*>(encoded_jmp_ins), ilen, &olen);
          if (xed_error != XED_ERROR_NONE) {
              cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
            return -1;
          }

          if (olen > instr_map[i].size) {
             cerr << " unable to set a relative jump to skip the profiling code stub at: "
                  << hex << "0x" << instr_map[i].new_ins_addr << "\n";
             return -1;
          }

          // Write the bypassing jump over the NOP. This patches code that the
          // application may be executing on another core RIGHT NOW, so it must
          // be a single atomic store - a byte-wise memcpy would let the app
          // fetch a half-written instruction and crash (this was the cc1 bug).
          //
          // x86 guarantees atomicity for a store that lies within one cache
          // line. We therefore splice the 5-byte JMP into an 8-byte value
          // (preserving the 3 following bytes, which the JMP skips over anyway)
          // and write it with a single 8-byte store. If those 8 bytes would
          // straddle a 64-byte cache line, we cannot patch atomically, so we
          // leave that stub active (correct, merely not disabled).
          ADDRINT patch_addr = instr_map[i].new_ins_addr;
          if ((patch_addr & 63) <= (64 - 8)) {
              uint64_t merged;
              memcpy(&merged, (void *)patch_addr, 8);       // current 8 bytes
              memcpy(&merged, encoded_jmp_ins, olen);       // overlay 5-byte JMP
              *(volatile uint64_t *)patch_addr = merged;    // single atomic store
          }
          i += (j - 1);
       }
    }
    return 0;
}    

/*************************/
/* add_new_instr_entry() */
/*************************/
int add_new_instr_entry(xed_decoded_inst_t *xedd, ADDRINT pc, ins_enum_t ins_type)
{
    // copy target addr to instr map:
    ADDRINT orig_targ_addr = 0x0;

    // Check if the instruction has a branch displacement:
    xed_uint_t disp_byts = xed_decoded_inst_get_branch_displacement_width(xedd);
    xed_int32_t disp;
    if (disp_byts > 0) { // there is a branch offset.
      disp = xed_decoded_inst_get_branch_displacement(xedd);
      orig_targ_addr = pc + xed_decoded_inst_get_length (xedd) + disp;
    }

    // copy rip-relative addr to instr map:
    ADDRINT orig_rip_addr = 0x0;

    // check for a rip-relative displacement:
    unsigned memops = xed_decoded_inst_number_of_memory_operands(xedd);
    if (memops) {
      xed_reg_enum_t base_reg = xed_decoded_inst_get_base_reg(xedd, 0);
      if (base_reg == XED_REG_RIP) {
         unsigned size = xed_decoded_inst_get_length (xedd);
         xed_int64_t disp = xed_decoded_inst_get_memory_displacement(xedd, 0);
         orig_rip_addr = (ADDRINT)(pc + disp + size);
      }
    }

    // Converts the decoder request to a valid encoder request:
    xed_encoder_request_init_from_decode (xedd);

    unsigned new_size = 0;

    xed_error_enum_t xed_error =
       xed_encode (xedd, reinterpret_cast<UINT8*>(instr_map[num_of_instr_map_entries].encoded_ins),
                   max_inst_len , &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        return -1;
    }

    // Add a new entry to instr_map:
    //
    instr_map[num_of_instr_map_entries].orig_ins_addr = pc;
    instr_map[num_of_instr_map_entries].new_ins_addr = 0x0;
    instr_map[num_of_instr_map_entries].orig_targ_addr = orig_targ_addr;
    instr_map[num_of_instr_map_entries].orig_rip_addr = orig_rip_addr;
    instr_map[num_of_instr_map_entries].targ_map_entry = -1;
    instr_map[num_of_instr_map_entries].size = new_size;
    instr_map[num_of_instr_map_entries].ins_type = ins_type;
    instr_map[num_of_instr_map_entries].bbl_num = bbl_num;
    instr_map[num_of_instr_map_entries].xed_category = xed_decoded_inst_get_category(xedd);

    num_of_instr_map_entries++;

    if (num_of_instr_map_entries >= max_ins_count) {
        cerr << "out of memory for map_instr" << endl;
        return -1;
    }

    // debug print new encoded instr:
    if (KnobVerbose) {
        cerr << "    new instr:";
        dump_instr_from_mem((ADDRINT *)instr_map[num_of_instr_map_entries-1].encoded_ins,
                            instr_map[num_of_instr_map_entries-1].new_ins_addr);
    }

    return new_size;
}

/***************************/
/* add_new_encoded_instr() */
/***************************/
int add_new_encoded_instr(ADDRINT ins_addr, xed_encoder_instruction_t *enc_instr, ins_enum_t ins_type) {
    char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
    unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
    unsigned int olen = 0;
  
    // Convert the encoding instr to a valid encoder request.
    xed_encoder_request_t enc_req;    
    xed_encoder_request_zero_set_mode(&enc_req, &dstate);
    xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, enc_instr);
    if (!convert_ok) {
        cerr << "conversion to encode request failed" << endl;
        return -1;
    }
    
    // Encode instr.
    xed_error_enum_t xed_error = xed_encode(&enc_req,
              reinterpret_cast<UINT8*>(encoded_ins), ilen, &olen);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
      return -1;
    }
  
    // Decode instr.
    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd,&dstate);
    xed_error_enum_t xed_code = xed_decode(&xedd, reinterpret_cast<UINT8*>(&encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: " << "0x" << hex << ins_addr << endl;
        return -1;;
    }
    int rc = add_new_instr_entry(&xedd, ins_addr, ins_type);
    if (rc < 0) {
      cerr << "ERROR: failed during instructon translation." << endl;
      return -1;
    }
    return 0;
}

/**************************/
/* add_profiling_instrs() */
/**************************/
int add_profiling_instrs(INS ins, ADDRINT ins_addr,
                         UINT64 *counter_addr, unsigned bbl_num,
                         std::map<ADDRINT, bool>& is_targ_map)
{
  xed_encoder_instruction_t enc_instr;

  static uint64_t rax_mem = 0;

  // Dead-register elimination (Requirement 2): if RAX is not live at this
  // instrumentation point we can use it as scratch WITHOUT spilling it to
  // rax_mem, saving two memory moves per BBL. We restrict this to the plain
  // BBL-counter case: when the BBL also gets indirect-jump target profiling,
  // that stub has its own entangled RAX/RBX/RCX usage, so we keep spilling
  // there to stay safe.
  bool has_indirect = (!KnobNoIndirectProfile) && INS_IsIndirectControlFlow(ins)
                      && !INS_IsRet(ins) && !INS_IsCall(ins);
  bool rax_dead = (!KnobNoDeadRegOpt) && (!has_indirect)
                  && is_reg_free_before(ins, LEVEL_BASE::REG_RAX, is_targ_map);
  // Bisection support: only apply the optimization to the first N eligible BBLs.
  if (rax_dead) {
    if (g_opt_applied >= (unsigned)KnobDeadRegLimit) {
      rax_dead = false;
    } else {
      g_opt_applied++;
      if (KnobDeadRegDebug && g_opt_applied == (unsigned)KnobDeadRegLimit)
        cerr << "DEADREG culprit #" << dec << g_opt_applied << " term @0x"
             << hex << ins_addr << " '" << INS_Disassemble(ins) << "'\n";
    }
  }

  // Add NOP instr (to be overwritten later on by a jmp that skips
  // the profiling, once profiling is done). It must be exactly 5 bytes so the
  // 5-byte skip JMP written by disable_profiling_in_tc() replaces it exactly
  // and never spills into the following instruction.
  xed_inst0(&enc_instr, dstate, XED_ICLASS_NOP5, 64);
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
    return -1;

  // Save RAX (skipped when RAX is dead here - dead-register elimination):
  // MOV RAX into rax_mem
  if (!rax_dead) {
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64), // Destination op.
              xed_reg(XED_REG_RAX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
  }

  // Create profiling for indirect jump targets.
  if (!KnobNoIndirectProfile &&
      INS_IsIndirectControlFlow(ins) && !INS_IsRet(ins) && !INS_IsCall(ins)) {
    // Debug print.
    //cerr << " BBL terminates with indirect jump: "
    //     << " 0x" << hex << ins_addr << ": "
    //     << INS_Disassemble(ins) << "\n";

    static uint64_t rbx_mem = 0;
    static uint64_t rcx_mem = 0;

    // Retrieve the details about the mem operand.
    xed_decoded_inst_t *xedd = INS_XedDec(ins);
    xed_reg_enum_t base_reg = xed_decoded_inst_get_base_reg(xedd, 0);
    xed_reg_enum_t index_reg = xed_decoded_inst_get_index_reg(xedd, 0);
    xed_int64_t disp = xed_decoded_inst_get_memory_displacement(xedd, 0);
    xed_uint_t scale = xed_decoded_inst_get_scale(xedd, 0);
    xed_uint_t width = xed_decoded_inst_get_memory_displacement_width_bits(xedd, 0);
    unsigned mem_addr_width = xed_decoded_inst_get_memop_address_width(xedd, 0);
    
    xed_reg_enum_t targ_reg = XED_REG_INVALID;
    unsigned memops = xed_decoded_inst_number_of_memory_operands(xedd);
    if (!memops)
      targ_reg = xed_decoded_inst_get_reg(xedd, XED_OPERAND_REG0);

    // Debug print.
    //dump_instr_from_xedd(xedd, ins_addr);
    //cerr << " base reg: " << xed_reg_enum_t2str(base_reg)
    //     << " index reg " << xed_reg_enum_t2str(index_reg)
    //     << " scale: " << dec << scale
    //     << " disp: 0x" << hex << disp
    //     << " width: " << dec << width
    //     << " mem addr width: " << dec << mem_addr_width
    //     << " targ reg: " << targ_reg << xed_reg_enum_t2str(targ_reg)
    //     << "\n";
    
    // save RBX into rbx_mem in 2 steps via RAX
    // save RCX into rcx_mem in 2 steps via RAX
    // Convert jmp [base_reg + index_reg*scale] to: MOV RAX, [base_reg + index_reg*scale]
    //         Or convert jmp targ_reg to: MOV RAX, targ_reg ==> RAX holds jump targ addr
    // MOV RBX, RAX ==> Now RBX also holds targ addr
    // AND RAX, MAX_TARG_ADDR ==> RAX holds index i = 0..MAX_TARG_ADDRS
    // MOV RCX, xed_imm0((ADDRINT)&bbl_map_targ_addr[bbl_num][0])
    // MOV [RCX + 8*RAX], RBX
    // MOV RBX, xed_imm0((ADDRINT)&bbl_map_targ_count[bbl_num][0])
    // MOV RCX, [RBX + 8*RAX]
    // LEA RCX, [RCX + 1]
    // MOV [RBX + 8*RAX], RCX
    // restore RCX from rcx_mem in 2 steps via RAX
    // restore RBX from rbx_mem in 2 steps via RAX
    
    // Save RBX step 1 - MOV RBX into RAX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX),  // Destination op.
              xed_reg(XED_REG_RBX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Save RBX step 2 - MOV RAX into rbx_mem
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rbx_mem, 64), 64), // Destination op.
              xed_reg(XED_REG_RAX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Save RCX step 1 - MOV RCX into RAX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX),   // Destination op.
              xed_reg(XED_REG_RCX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Save RCX step 2 - MOV RAX into rcx_mem
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rcx_mem, 64), 64), // Destination op.
              xed_reg(XED_REG_RAX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Replace RIP reg by an absolute displacement.
    // Convert 'jmp [rax*8+0x657118]' or: 'jmp [rip+0x42513c]'
    // to: mov rax, [rax*8+0x657118] or: mov rax, [<absolute addr>]
    //
    // Check if we need to restore RAX in case  it is used as base reg or index reg,
    // e.g., jmp [RIP+8*RAX] or: jmp [RAX+8*RBX]
    
    // Check if we need to restore RAX from rax_mem.
    if (targ_reg == XED_REG_RAX || base_reg == XED_REG_RAX || index_reg == XED_REG_RAX) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_reg(XED_REG_RAX), // Destination reg op.
                xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
       return -1;
    }
    // Check if we need to convert [RIP+disp+index*scale] to [absolute_disp + index*scale]
    if (base_reg == XED_REG_RIP) {
      unsigned int orig_size = xed_decoded_inst_get_length (xedd);
      // Modify rip displacement by an absolute displacement val.
      xed_int64_t new_disp = ins_addr + disp + orig_size;
      if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
         cerr << "Invalid rip displacement larger than 32 bits in add_profiling_instrs\n";
         return -1;
      }
      xed_int64_t new_disp_width = 32; // set maximal disp width for now.
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_reg(XED_REG_RAX),    // Destination reg op.
                xed_mem_bisd(XED_REG_INVALID, index_reg, scale, 
                             xed_disp(new_disp, new_disp_width),
                             mem_addr_width));
    } else if (targ_reg != XED_REG_RAX) { // avoid ceating the MOV RAX, RAX Nop.
        xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                 xed_reg(XED_REG_RAX),    // Destination reg op.
                 (targ_reg != XED_REG_INVALID ? xed_reg(targ_reg) :
                  xed_mem_bisd(base_reg, index_reg, scale, xed_disp(disp, width), mem_addr_width)));
    }
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // MOV RBX, RAX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RBX),    // Destination reg op.
              xed_reg(XED_REG_RAX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // AND RAX, MAX_TARG_ADDRS. (NOTE: Modifies RFLAGS).
    xed_inst2(&enc_instr, dstate, XED_ICLASS_AND, 64,
              xed_reg(XED_REG_RAX),    // Destination reg op.
              xed_imm0(MAX_TARG_ADDRS, 8));  // keep only MAX_TARG_ADDRS+1 targets for profiling.
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // MOV RCX, xed_imm0((ADDRINT)&bbl_map[bbl_num].targ_addr[0])
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RCX), // Destination reg op.
              xed_imm0((ADDRINT)&(bbl_map[bbl_num].targ_addr[0]), 64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // MOV [RCX + 8*RAX], RBX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bisd(XED_REG_RCX, // base reg
                           XED_REG_RAX, //index reg
                           8, // scale
                           xed_disp(0, 32), // disp
                           64),  // Destination reg op.
              xed_reg(XED_REG_RBX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // MOV RBX, xed_imm0((ADDRINT)&bbl_map[bbl_num].targ_count[0])
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RBX), // Destination reg op.
              xed_imm0((ADDRINT)&(bbl_map[bbl_num].targ_count[0]), 64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // MOV RCX, [RBX + 8*RAX]
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RCX),   // Destination reg op.
              xed_mem_bisd(XED_REG_RBX, // base reg
                           XED_REG_RAX, //index reg
                           8, // scale
                           xed_disp(0, 32), // disp
                           64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // LEA RCX, [RCX + 1]
    xed_inst2(&enc_instr, dstate, XED_ICLASS_LEA, 64,
              xed_reg(XED_REG_RCX), // Destination reg op.
              xed_mem_bd(XED_REG_RCX, // base reg
                         xed_disp(1, 8), // disp
                         64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // MOV [RBX + 8*RAX], RCX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bisd(XED_REG_RBX, // base reg
                           XED_REG_RAX, //index reg
                           8, // scale
                           xed_disp(0, 32), // disp
                           64),     // Destination op.
              xed_reg(XED_REG_RCX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Restore RCX step 1- MOV from rcx_mem into RAX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX), // Destination op.
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rcx_mem, 64), 64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Restore RCX step 2 - MOV RAX into RCX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RCX),  // Destination op.
              xed_reg(XED_REG_RAX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Restore RBX step 1 - MOV from rbx_mem into RAX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX), // Destination op.
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rbx_mem, 64), 64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
    
    // Restore RBX step 2 - MOV RAX into RBX
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RBX),  // Destination op.
              xed_reg(XED_REG_RAX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;

  } // end of: 'if bbl terminates with indirect jump'.
  
  // Create the profiling instrs for counting the BBL frequency.
  //

  // MOV from bbl_map into RAX
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_reg(XED_REG_RAX),  // Destination reg op.
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)counter_addr, 64), 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
    return -1;

  // LEA RAX, [RAX+1]
  xed_inst2(&enc_instr, dstate, XED_ICLASS_LEA,  64,  // operand width
            xed_reg(XED_REG_RAX), // Destination reg op.
            xed_mem_bd(XED_REG_RAX, xed_disp(1, 8), 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
    return -1;

  // MOV from RAX into bbl_map
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)counter_addr, 64), 64), // Destination op.
            xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
    return -1;

  // Restore RAX (skipped when RAX was not spilled above):
  // MOV from rax_mem into RAX
  if (!rax_dead) {
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX), // Destination reg op.
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0)
      return -1;
  }

  return 0;
}

/**************************************************/
/* chain_all_direct_jmp_and_call_target_entries() */
/**************************************************/
void chain_all_direct_jmp_and_call_target_entries(unsigned from_entry,
                                                 unsigned until_entry)
{
    entry_map.clear();

    for (unsigned i = from_entry; i < until_entry; i++) {
        instr_map[i].targ_map_entry = -1;
        ADDRINT orig_ins_addr = instr_map[i].orig_ins_addr;
        if (!orig_ins_addr)
          continue;
        // For instrs with same orig_addr, give precedence to the first one.
        entry_map.emplace(orig_ins_addr, i);
    }

    for (unsigned i = from_entry; i < until_entry; i++) {
        ADDRINT orig_targ_addr = instr_map[i].orig_targ_addr;
        if (orig_targ_addr == 0)
            continue;
        if (instr_map[i].targ_map_entry > 0)
            continue;
        if (!entry_map.count(orig_targ_addr))
            continue;
        if (!instr_map[i].size)
            continue;
        instr_map[i].targ_map_entry = entry_map[orig_targ_addr];
    }
}


/***********************************************/
/* set_initial_estimated_new_ins_addrs_in_tc() */
/***********************************************/
int set_initial_estimated_new_ins_addrs_in_tc(char *tc) {
  unsigned tc_cursor = 0;
  // Set initial estimated new addrs for each instruction in the tc.
  for (unsigned i=0; i < num_of_instr_map_entries; i++) {
    instr_map[i].new_ins_addr = (ADDRINT)&tc[tc_cursor];
    // update expected size of tc.
    tc_cursor += instr_map[i].size;
    // Check if we exceeded the TC size.
    if (tc_cursor >= max_tc_size)
      return -1;
  }
  return 0;
}


/**************************/
/* fix_rip_displacement() */
/**************************/
int fix_rip_displacement(unsigned instr_map_entry)
{
    // uncond jumps instructions with size=0
    // should remain with size=0 for beeing removed from tc
    if (!instr_map[instr_map_entry].size)
        return 0;

    // Check if it is a RIP-relative instr.
    if (!instr_map[instr_map_entry].orig_rip_addr)
      return 0;

    // Check if it is a direct jmp or call instruction.
    if (instr_map[instr_map_entry].orig_targ_addr != 0)
      return 0;

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd, &dstate);

    xed_error_enum_t xed_code =
       xed_decode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: " << "0x"
             << hex << instr_map[instr_map_entry].new_ins_addr << endl;
        return -1;
    }

    //debug print:
    if (KnobVerbose) {
      cerr << " Before fixing rip offset\n";
      dump_instr_map_entry(instr_map_entry);
    }

    //xed_uint_t disp_byts = xed_decoded_inst_get_memory_displacement_width(xedd,i); // how many byts in disp ( disp length in byts - for example FFFFFFFF = 4
    xed_int64_t new_disp = 0;
    xed_uint_t new_disp_byts = 4;   // set maximal num of byts for now.

    // Modify rip displacement. use rip-relative direct addressing mode.
    new_disp = (xed_int64_t)(instr_map[instr_map_entry].orig_rip_addr - instr_map[instr_map_entry].new_ins_addr -
                               instr_map[instr_map_entry].size);
    // Code when using direct addressing mode.
    //xed_encoder_request_set_base0 (&xedd, XED_REG_INVALID);
    //new_disp = instr_map[instr_map_entry].orig_rip_addr;
    if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_rip_displacement\n";
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    // Set the memory displacement using a bit length.
    xed_encoder_request_set_memory_displacement (&xedd, new_disp, new_disp_byts);

    unsigned max_size = XED_MAX_INSTRUCTION_BYTES;
    unsigned new_size = 0;

    // Converts the decoder request to a valid encoder request:
    xed_encoder_request_init_from_decode (&xedd);

    xed_error_enum_t xed_error =
       xed_encode (&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins),
                   max_size , &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    //debug print:
    if (KnobVerbose) {
      cerr << " After fixing rip offset\n";
      dump_instr_map_entry(instr_map_entry);
    }

    return new_size;
}


/**************************************/
/* fix_direct_jmp_or_call_to_orig_addr */
/**************************************/
int fix_direct_jmp_or_call_to_orig_addr(unsigned instr_map_entry)
{
    // Ignore instructions of zero size.
    if (!instr_map[instr_map_entry].size)
      return 0;

    // Debug print.
    if (KnobVerbose) {
      cerr << "jump to orig addr: 0x" << hex << instr_map[instr_map_entry].orig_targ_addr << " : ";
      dump_instr_from_mem ((ADDRINT *)instr_map[instr_map_entry].encoded_ins,
                           instr_map[instr_map_entry].orig_ins_addr);
    }

    // check for cases of direct jumps/calls back to the orginal target address:
    if (instr_map[instr_map_entry].targ_map_entry >= 0) {
        cerr << "ERROR: Invalid jump or call instruction" << endl;
        return -1;
    }

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd,&dstate);

    xed_error_enum_t xed_code =
        xed_decode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: " << "0x"
             << hex << instr_map[instr_map_entry].new_ins_addr << endl;
        return -1;
    }

    xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);

    if (category_enum != XED_CATEGORY_CALL && category_enum != XED_CATEGORY_UNCOND_BR && category_enum != XED_CATEGORY_COND_BR) {
        cerr << "note: untranslatable transfer to original target 0x" << hex
             << instr_map[instr_map_entry].orig_targ_addr
             << " - falling back to native execution for this image\n";
        return -1;
    }

    unsigned ilen = XED_MAX_INSTRUCTION_BYTES;
    unsigned olen = 0;

    // Use the heap variable instr_map[instr_map_entry].orig_targ_addr as the
    // memory container that holds the target address for the jmp/call
    // and indirectly jmp/call via that memory location.

    // search for orig_targ_addr in jump_to_orig_addr_map.
    // Search for the original target address in jump_to_orig_addr_map.
    // Note: jump_to_orig_addr_map is 1-indexed because jump_to_orig_addr_num is incremented before assignment.
    int jump_to_orig_addr_map_entry = -1;
    for (unsigned i = 1; i <= jump_to_orig_addr_num; i++) {
      if (instr_map[instr_map_entry].orig_targ_addr == jump_to_orig_addr_map[i]) {
        jump_to_orig_addr_map_entry = i;
        break;
      }
    }
    if (jump_to_orig_addr_map_entry < 0) {
      jump_to_orig_addr_num++;
      jump_to_orig_addr_map_entry = jump_to_orig_addr_num;
      if ((unsigned)jump_to_orig_addr_map_entry >= max_rtn_count) {
         cerr << "exceeded size of jump_to_orig_addr_map at fix_direct_jmp_or_call_to_orig_addr\n";
         return -1;
      }
      jump_to_orig_addr_map[jump_to_orig_addr_map_entry] = instr_map[instr_map_entry].orig_targ_addr;
    }

    if (category_enum != XED_CATEGORY_CALL && category_enum != XED_CATEGORY_UNCOND_BR) {
        cerr << "note: untranslatable transfer to original target 0x" << hex
             << instr_map[instr_map_entry].orig_targ_addr
             << " - falling back to native execution for this image\n";
        return -1;
    }

    xed_encoder_instruction_t  enc_instr_dummy;
    xed_iclass_enum_t iclass_jmp_call = (category_enum == XED_CATEGORY_CALL) ? XED_ICLASS_CALL_NEAR : XED_ICLASS_JMP;
    xed_inst1(&enc_instr_dummy, dstate, iclass_jmp_call, 64, xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
    xed_encoder_request_t enc_req_dummy;
    xed_encoder_request_zero_set_mode(&enc_req_dummy, &dstate);
    xed_convert_to_encoder_request(&enc_req_dummy, &enc_instr_dummy);
    unsigned int olen_dummy = 0;
    UINT8 dummy_buf[15];
    xed_encode(&enc_req_dummy, dummy_buf, 15, &olen_dummy);

    xed_encoder_instruction_t  enc_instr;
    xed_int64_t new_disp = (ADDRINT)&jump_to_orig_addr_map[jump_to_orig_addr_map_entry] -
                       (instr_map[instr_map_entry].new_ins_addr + olen_dummy);
    if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_direct_jmp_or_call_to_orig_addr\n";
        cerr << "new displacement: " << dec << new_disp << "\n";
        return -1;
    }

    xed_inst1(&enc_instr, dstate, iclass_jmp_call, 64, xed_mem_bd(XED_REG_RIP, xed_disp(new_disp, 32), 64));

    xed_encoder_request_t enc_req;
    xed_encoder_request_zero_set_mode(&enc_req, &dstate);
    xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
    if (!convert_ok) {
        cerr << "conversion to encode request failed" << endl;
        return -1;
    }

    xed_error_enum_t xed_error =
       xed_encode(&enc_req, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), ilen, &olen);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    // NOTE: We cannot zero the orig_targ_addr field in instr_map as follows:
    //  instr_map[instr_map_entry].orig_targ_addr = 0x0;
    // This is because the RIP displacement may become too large to fit into 4 bytes long.

    // debug prints:
    if (KnobVerbose) {
        dump_instr_map_entry(instr_map_entry);
    }

    return olen;
}


/**************************************/
/* fix_direct_jmp_or_call_displacement */
/**************************************/
int fix_direct_jmp_or_call_displacement(unsigned instr_map_entry)
{
    //uncond jumps instructions with size=0 should remain with size=0
    // for beeing removed from tc
    if (!instr_map[instr_map_entry].size)
        return 0;

    // Check if it is indeed a direct branch or a direct call instr:
    if (instr_map[instr_map_entry].orig_targ_addr == 0)
      return 0;

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd,&dstate);

    xed_error_enum_t xed_code =
        xed_decode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: "
             << "0x" << hex << instr_map[instr_map_entry].new_ins_addr << endl;
        return -1;
    }

    xed_int64_t  new_disp = 0;
    unsigned max_size = XED_MAX_INSTRUCTION_BYTES;
    unsigned new_size = 0;


    xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);

    if (category_enum != XED_CATEGORY_CALL &&
        category_enum != XED_CATEGORY_COND_BR &&
        category_enum != XED_CATEGORY_UNCOND_BR) {
        cerr << "ERROR: unrecognized branch displacement" << endl;
        return -1;
    }

    // fix direct branches/calls to original targ addresses or
    // indirect branches via a rip offset which had previously been
    // formed by previouis calls to fix_direct_jmp_or_call_to_orig_addr()
    // in order to relpace direct jumps to orig targ addrs.
    if (instr_map[instr_map_entry].targ_map_entry < 0) {
       int rc = fix_direct_jmp_or_call_to_orig_addr(instr_map_entry);
       return rc;
    }

    ADDRINT new_targ_addr;
    new_targ_addr = instr_map[instr_map[instr_map_entry].targ_map_entry].new_ins_addr;

    new_disp =
      (new_targ_addr - instr_map[instr_map_entry].new_ins_addr) - instr_map[instr_map_entry].size; // orig_size;
     if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_direct_jmp_or_call_displacement\n";
        return -1;
    }

    xed_uint_t   new_disp_byts = 4; // num_of_bytes(new_disp);  ???

    // the max displacement size of loop instructions is 1 byte:
    xed_iclass_enum_t iclass_enum = xed_decoded_inst_get_iclass(&xedd);
    if (iclass_enum == XED_ICLASS_LOOP ||
        iclass_enum == XED_ICLASS_LOOPE ||
        iclass_enum == XED_ICLASS_LOOPNE) {
      new_disp_byts = 1;
    }

    // the max displacement size of jecxz instructions is ???:
    xed_iform_enum_t iform_enum = xed_decoded_inst_get_iform_enum (&xedd);
    if (iform_enum == XED_IFORM_JRCXZ_RELBRb){
      new_disp_byts = 1;
    }

    // Converts the decoder request to a valid encoder request:
    xed_encoder_request_init_from_decode (&xedd);

    //Set the branch displacement:
    xed_encoder_request_set_branch_displacement (&xedd, new_disp, new_disp_byts);

    //xed_uint8_t enc_buf[XED_MAX_INSTRUCTION_BYTES];
    //xed_error_enum_t xed_error = xed_encode (&xedd, enc_buf, max_size , &new_size);
    xed_error_enum_t xed_error =
        xed_encode (&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_size, &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) <<  endl;
        char buf[2048];
        xed_format_context(XED_SYNTAX_INTEL, &xedd, buf, 2048,
                           static_cast<UINT64>(instr_map[instr_map_entry].orig_ins_addr), 0, 0);
        cerr << " instr: " << "0x" << hex << instr_map[instr_map_entry].orig_ins_addr << " : " << buf <<  endl;
          return -1;
    }

    //debug print of new instruction in tc:
    if (KnobVerbose) {
        dump_instr_map_entry(instr_map_entry);
    }

    return new_size;
}

/************************************/
/* fix_instructions_displacements() */
/************************************/
int fix_instructions_displacements()
{
   // fix displacemnets of direct branch or call instructions:

    int size_diff = 0;
    bool is_diff = false;

    do {

        size_diff = 0;
        is_diff = false;

        if (KnobVerbose) {
            cerr << "starting a pass of fixing instructions displacements: " << endl;
        }

        for (unsigned i=0; i < num_of_instr_map_entries; i++) {

            instr_map[i].new_ins_addr += size_diff;

            // fix rip displacement:
            int new_size = fix_rip_displacement(i);
            if (new_size) {
              if (new_size < 0)
                  return -1;
              if (instr_map[i].size != (unsigned)new_size) { // this was a rip-based instruction which was fixed.
                  if (instr_map[i].size < (unsigned)new_size)
                     size_diff += (new_size - instr_map[i].size);
                  else
                     size_diff -= (instr_map[i].size - new_size);
                  instr_map[i].size = (unsigned)new_size;
                  is_diff = true;
                  continue;
              }
            }

            // fix instr displacement for direct jump or call:
            new_size = fix_direct_jmp_or_call_displacement(i);
            if (new_size) {
              if (new_size < 0)
                  return -1;
              if (instr_map[i].size != (unsigned)new_size) {
                if (instr_map[i].size < (unsigned)new_size)
                   size_diff += (new_size - instr_map[i].size);
                else
                   size_diff -= (instr_map[i].size - new_size);
                instr_map[i].size = (unsigned)new_size;
                is_diff = true;
                continue;
              }
            }

        }  // end int i=0; i ..

    } while (is_diff);

   return 0;
 }


/********************************/
/* find_candidate_rtns_for_tc() */
/********************************/
int find_candidate_rtns_for_tc(IMG img)
{
    int rc = 0;
    // go over routines and check if they are candidates for translation and mark them for translation:

    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            // Keep the entry num of the rtn head in case we need to
            // revert the insertin of the instruction in rtn into the instructions
            // map due to an invalid decoding.
            //unsigned rtn_entry = num_of_instr_map_entries;
            //unsigned prev_bbl_num = bbl_num;

            //if (RTN_Name(rtn) == ".plt")
            //    continue;
            
            // Open the RTN.
            RTN_Open( rtn );

            // Map all instructions that are a target of some direct jump or call in the rtn.
            std::map<ADDRINT, bool>is_targ_map;
            is_targ_map.empty();
            bool has_interprocedural_cond_br = false;
            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
               if (INS_IsDirectControlFlow(ins)) {
                 ADDRINT targ_addr = INS_DirectControlFlowTargetAddress(ins);
                 is_targ_map[targ_addr] = true;
               }
            }
            ADDRINT rtn_start = RTN_Address(rtn);
            ADDRINT rtn_end = rtn_start + RTN_Size(rtn);
            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
               if (INS_Category(ins) == XED_CATEGORY_COND_BR) {
                 ADDRINT targ_addr = INS_DirectControlFlowTargetAddress(ins);
                 if (targ_addr < rtn_start || targ_addr >= rtn_end) {
                    has_interprocedural_cond_br = true;
                    break;
                 }
               }
            }

            if (has_interprocedural_cond_br) {
                if (KnobVerbose) {
                    cerr << "note: untranslatable interprocedural COND_BR in routine " << RTN_Name(rtn) 
                         << " - falling back to native execution for this routine\n";
                }
                RTN_Close( rtn );
                continue;
            }

            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {

                //debug print of orig instruction:
                if (KnobVerbose) {
                    cerr << "old instr: ";
                    cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) <<  endl;
                    //xed_print_hex_line(reinterpret_cast<UINT8*>(INS_Address (ins)), INS_Size(ins));
                }

                ADDRINT ins_addr = INS_Address(ins);

                xed_decoded_inst_t xedd;
                xed_error_enum_t xed_code;

                // Add instr into instr map:
                bool isRtnHeadIns = (RTN_Address(rtn) == ins_addr);
                ins_enum_t ins_type = (isRtnHeadIns ? RtnHeadIns : RegularIns);

                // Insert a NOP7 instr at Rtn Head to be used in order
                // to restore orig target of a cond jumps to a routine.
                //
                if (!KnobNoProfile && isRtnHeadIns) {
                  rc = create_nop7_xedd_instr(&xedd);
                  if (rc < 0) {
                    cerr << "ERROR: failed to create a NOP7 instr during translation of instr at: "
                         << "0x" << hex << ins_addr << endl;
                    return -1;
                  }
                  rc = add_new_instr_entry(&xedd, ins_addr, ins_type);
                  if (rc < 0) {
                    cerr << "ERROR: failed during instructon translation." << endl;
                    return -1;
                  }
                  ins_type = RegularIns;
                }

                // Check if ins is a control transfer instr that terminates a BBL
                // or the next instr is a target of a direct branch or call.
                INS next_ins = INS_Next(ins);
                bool isNextInsJumpTarget = 
                    (!INS_Valid(next_ins) ? false : is_targ_map[INS_Address(next_ins)]);
                bool isInsTerminatesBBL = (isJumpOrRet(ins) || isNextInsJumpTarget);

                // Add profiling instructions to count each BBL exec at runtime:
                //
                if (!KnobNoProfile) {
                  // Do not insert the profiling now if there is a later instr
                  // in the BBL that kills RAX.
                  if (isInsTerminatesBBL) {
                    rc = add_profiling_instrs(ins, ins_addr, &bbl_map[bbl_num].counter, bbl_num, is_targ_map);
                    if (rc < 0)
                      return -1;
                  }
                }
          
                // Add ins to instr_map:
                //
                xed_decoded_inst_zero_set_mode(&xedd,&dstate);
                xed_code = xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
                if (xed_code != XED_ERROR_NONE) {
                    cerr << "ERROR: xed decode failed for instr at: " << "0x" << hex << ins_addr << endl;
                    return -1;
                }

                // Add the instr into the instr_map table.
                rc = add_new_instr_entry(&xedd, INS_Address(ins), ins_type);
                if (rc < 0) {
                    cerr << "ERROR: failed during instructon translation." << endl;
                    return -1;
                }

                if (isInsTerminatesBBL) {
                  bbl_map[bbl_num].terminating_ins_entry = num_of_instr_map_entries - 1;
                  bbl_num++;
                  bbl_map[bbl_num].starting_ins_entry = num_of_instr_map_entries;
                }

                // Apply edge Profiling: For BBLs that end with a conditional branch,
                //     insert an increment of the fallthrough counter for this BBL,
                //     immediately after the cond branch which terminates the bbl.
                //     and before the next BBL.
                if (!KnobNoProfile && INS_Category(ins) == XED_CATEGORY_COND_BR) {
                  rc = add_profiling_instrs(ins, ins_addr,
                                            &bbl_map[bbl_num - 1].fallthru_counter, bbl_num-1, is_targ_map);
                  if (rc < 0)
                    return -1;
                }

            } // end for INS...

            // debug print of routine name:
            if (KnobVerbose) {
                cerr <<   "rtn name: " << RTN_Name(rtn) << endl;
            }

            // Close the RTN.
            RTN_Close( rtn );

            // Apply local chaining of direct calls and branches for this routine.
            //chain_all_direct_jmp_and_call_target_entries(rtn_entry, num_of_instr_map_entries);

         } // end for RTN..
    } // end for SEC...

    return 0;
}


/***************************/
/* int copy_instrs_to_tc() */
/***************************/
int copy_instrs_to_tc(char *tc)
{
    int cursor = 0;

    for (unsigned i=0; i < num_of_instr_map_entries; i++) {

      if ((ADDRINT)&tc[cursor] != instr_map[i].new_ins_addr) {
          cerr << "ERROR: Non-matching instruction addresses: "
               << hex << (ADDRINT)&tc[cursor]
               << " vs. " << instr_map[i].new_ins_addr << endl;
          return -1;
      }

      memcpy(&tc[cursor], (char *)instr_map[i].encoded_ins, instr_map[i].size);

      cursor += instr_map[i].size;
    }

    return cursor;
}


/***************************************/
/* void commit_translated_rtns_to_tc() */
/***************************************/
inline void commit_translated_rtns_to_tc()
{
    // Commit the translated routines:
    // Go over the routines and replace the original ones
    // by their new successfully translated ones:

    for (unsigned i=0; i < num_of_instr_map_entries; i++) {

        //replace routine by new routine in tc

        if (instr_map[i].ins_type != RtnHeadIns)
          continue;

        RTN rtn = RTN_FindByAddress(instr_map[i].orig_ins_addr);
        if (rtn == RTN_Invalid()) {
           cerr << "invalid rtN for commit for addr: 0x"
                << instr_map[i].orig_ins_addr << "\n";
           continue;
        }

        // Probe-mode replacement writes a 5-byte JMP at the routine entry.
        // Skip routines Pin deems unsafe to probe (they run natively instead).
        if (!RTN_IsSafeForProbedReplacement(rtn)) {
           continue;
        }

        // RTN_Size() is symbol-table-derived and over-reports tiny stub
        // routines (e.g. "push rbx", "xor eax,eax"), so it can't be trusted
        // for the 5-byte probe requirement. Count the real decoded bytes at
        // the routine entry with XED and skip routines that have fewer than 5.
        {
            ADDRINT rtn_addr = RTN_Address(rtn);
            unsigned real_bytes = 0;
            while (real_bytes < 5) {
                xed_decoded_inst_t xedd;
                xed_decoded_inst_zero_set_mode(&xedd, &dstate);
                xed_error_enum_t xed_code = xed_decode(
                    &xedd,
                    reinterpret_cast<const UINT8*>(rtn_addr + real_bytes),
                    max_inst_len);
                if (xed_code != XED_ERROR_NONE)
                    break;
                unsigned ilen = xed_decoded_inst_get_length(&xedd);
                if (ilen == 0)
                    break;
                real_bytes += ilen;
            }
            if (real_bytes < 5)
                continue;
        }

        // Debug print.
        // cerr << "committing rtN: " << RTN_Name(rtn);
        // cerr << " from: 0x" << hex << RTN_Address(rtn)
        //      << " to: 0x" << hex << instr_map[i].new_ins_addr << endl;


        AFUNPTR origFptr = RTN_ReplaceProbed(rtn,  (AFUNPTR)instr_map[i].new_ins_addr);

        if (origFptr == NULL) {
            cerr << "RTN_ReplaceProbed failed.";
            cerr << " orig routine addr: 0x" << hex << RTN_Address(rtn)
                 << " translated routine addr: 0x" << hex
                 << instr_map[i].new_ins_addr << endl;
            dump_instr_from_mem ((ADDRINT *)RTN_Address(rtn), RTN_Address(rtn));
        }

        // debug print.
        //if (origFptr != NULL) {
        //  cerr << "RTN_ReplaceProbed succeeded. ";
        //  cerr << " orig routine addr: 0x" << hex << RTN_Address(rtn)
        //       << " translated routine addr: 0x" << hex
        //       << instr_map[i].new_ins_addr << endl;
        //  dump_instr_from_mem ((ADDRINT *)RTN_Address(rtn), RTN_Address(rtn));
        //}
    }
}

bool tc_created_successfully = false;

/**********************************************/
/* start_stop_profile_gathering_thread_func() */
/**********************************************/
void start_stop_profile_gathering_thread_func(void *v)
{
    // Wait prof_time seconds for the profiling to count
    // execution frequency for each BBL.
    cerr << " prof time: " << dec << KnobNumSecsDuringProfile << " sec\n";
    sleep(KnobNumSecsDuringProfile);

    if (!tc_created_successfully) {
        cerr << "TC creation aborted or incomplete, not disabling profiling.\n";
        return;
    }

    cerr << "disabling profile gathering\n";

    // disable profiling.
    //  Add a jump at beginning of every profile stub to bypass the
	//  profiling counters in TC.
    int rc = disable_profiling_in_tc(instr_map, num_of_instr_map_entries);
    if  (rc < 0)
      return;
}

/****************************/
/* allocate_and_init_memory */
/****************************/
int allocate_and_init_memory(IMG img)
{
    // Calculate size of executable sections and allocate required memory:
    //
    ADDRINT highest_addr = 0;
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        if (!lowest_sec_addr || lowest_sec_addr > SEC_Address(sec))
            lowest_sec_addr = SEC_Address(sec);

        if (highest_sec_addr < SEC_Address(sec) + SEC_Size(sec))
            highest_sec_addr = SEC_Address(sec) + SEC_Size(sec);

        // need to avouid using RTN_Open as it is expensive...
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            if (highest_addr < RTN_Address(rtn) + RTN_Size(rtn))
                highest_addr = RTN_Address(rtn) + RTN_Size(rtn);
            max_rtn_count++;
            max_ins_count += RTN_NumIns  (rtn);
        }
    }

    max_ins_count *= 10; // estimating that the num of instrs for the profiling
                         // and for the inlined functions will not exceed
                         // the total nunmber of the entire code.


    // get a page size in the system:
    int pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1) {
      perror("sysconf");
      return -1;
    }

    ADDRINT text_size = (highest_sec_addr - lowest_sec_addr) * 2 + pagesize * 4;

    max_tc_size = 10 * text_size + pagesize * 4;   // FIXME: need a better estimate
    // Check thet max_tc_size is not larger than a 32 bit branch displacement
    if (max_tc_size >= 0x7FFFFFFF) {
      cerr << "size of TC is beyond the range of a branch displacement" << endl;
      return -1;
    }

    // Allocate the needed memory for tc and tc2 + jump orig addr map
    // with RW+EXEC permissions which is not
    // located in an address that is more than 32bits afar:
    const size_t mem_size =
              max_tc_size +                     // TC + TC2 size
              max_rtn_count * sizeof(ADDRINT);  // jump_to_orig_addr_map size
    char *addr = nullptr;
    ADDRINT max_distance = 0x7FFFFFFF;
    const size_t step = pagesize; // Try every page
    // Align target address to page boundary
    ADDRINT aligned_target = ((ADDRINT)highest_addr) & ~(pagesize - 1);
    // Try exact address first
    void* result = mmap((void*)aligned_target, mem_size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       0, 0);
    if (result != MAP_FAILED &&
        (abs((long)((ADDRINT)result - aligned_target)) <= (long)max_distance)) {
        addr = (char *)result;
    }

    if (!addr) {
        // Search in expanding rings around target
        for (size_t offset = step; offset <= max_distance; offset += step) {
            // Try above target address
            ADDRINT try_addr = aligned_target + offset;
            result = mmap((void*)try_addr, mem_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         0, 0);
            if (result != MAP_FAILED &&
                (abs((long)((ADDRINT)result - try_addr)) <= (long)max_distance)) {
                addr = (char *)result;
                break;
            }
            if (result != MAP_FAILED) {
                munmap(result, mem_size);
            }

            // Try below target address (if not underflow)
            if (highest_addr >= offset) {
                try_addr = aligned_target - offset;
                result = mmap((void*)try_addr, mem_size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS,
                             0, 0);
                if (result != MAP_FAILED &&
                    (abs((long)((ADDRINT)result - try_addr)) <= (long)max_distance)) {
                    addr = (char *)result;
                    break;
                }
                if (result != MAP_FAILED) {
                    munmap(result, mem_size);
                }
            }
        }
    }

    if (!addr) {
        cerr << "failed to allocate memory within 32-bit range. " << endl;
        return -1;
    }

    // debug print.
    cerr << " allocated memory at: 0x" << hex << (ADDRINT)addr << "\n";

    // TC is allocated first.
    tc = (char *)addr;
    addr += max_tc_size;

    // Allocate memory to the jump map to orig addrs which cannot be relocated.
    unsigned int map_offset = max_ins_count * 15;
    map_offset = (map_offset + 7) & ~7;
    jump_to_orig_addr_map = (ADDRINT *)((char *)tc + map_offset);

    // Allocate memory for the instr_map table.
    instr_map = (instr_map_t *)calloc(max_ins_count, sizeof(instr_map_t));
    if (instr_map == NULL) {
        perror("calloc");
        return -1;
    }

    // Allocate memory for the bbl_map table.
    bbl_map = (bbl_map_t *)calloc(max_ins_count, sizeof(bbl_map_t));
    if (bbl_map == NULL) {
        perror("calloc");
        return -1;
    }

    return 0;
}



/* ============================================ */
/* Main translation routine                     */
/* ============================================ */
typedef VOID (*EXITFUNCPTR)(INT code);
EXITFUNCPTR origExit;

/********/
/* Fini */
/********/
VOID Fini(INT32 code, VOID* v)
{
    cerr << "Reached _exit." << endl;
    dump_profile();

    clock_gettime(CLOCK_MONOTONIC, &end_running_time);
    double elapsed = (end_running_time.tv_sec - start_running_time.tv_sec) + 
                     (end_running_time.tv_nsec - start_running_time.tv_nsec) / 1e9;
	cerr << " Translated code run took: " << elapsed << " seconds\n";
}

/*******************/
/* ExitInProbeMode */
/*******************/
VOID ExitInProbeMode(INT code)
{
    Fini(code, 0);
    (*origExit)(code);
}

/*************/
/* create_tc */
/*************/
VOID create_tc(IMG img, VOID *v)
{
    // Insert a call to function Fini when raching the _exit routine.
    RTN exitRtn = RTN_FindByName(img, "_exit");
    if (RTN_Valid(exitRtn) && RTN_IsSafeForProbedReplacement(exitRtn)) {
      origExit = (EXITFUNCPTR)RTN_ReplaceProbed(exitRtn, AFUNPTR(ExitInProbeMode));
    }

    // Step 0: Check the image and the CPU:
    if (!IMG_IsMainExecutable(img))
      return;

    if (KnobDumpOrigCode)
      dump_image_instrs(img);

    int rc = 0;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // step 1: Check size of executable sections and allocate required memory:
    rc = allocate_and_init_memory(img);
    if (rc < 0) {
        cerr << "failed to initialize memory for translation\n";
        return;
    }
    cerr << "after memory allocation" << endl;

    // Step 2: go over all routines and identify candidate routines and copy
    //         their code into the instr map IR:
    rc = find_candidate_rtns_for_tc(img);
    if (rc < 0) {
        cerr << "failed to find candidates for translation\n";
        return;
    }
    cerr << "after identifying candidate routines" << endl;

    // Step 3: Chaining - calculate direct branch and call instructions to point
    //         to corresponding target instr entries:
    chain_all_direct_jmp_and_call_target_entries(0, num_of_instr_map_entries);
    cerr << "after chaining all branch targets" << endl;

    // Step 4: Set initial estimated new addrs for each instruction in the tc.
    rc = set_initial_estimated_new_ins_addrs_in_tc(tc);
    if (rc < 0 ) {
        cerr << "failed to set initial estimated new ins addrs in the TC\n";
        return;
    }
    cerr << "after setting initial estimated new ins addrs in the TC" << endl;

    // Step 5: fix rip-based, direct branch and direct call displacements:
    rc = fix_instructions_displacements();
    if (rc < 0 ) {
        cerr << "failed to fix displacments of translated instructions\n";
        return;
    }
    cerr << "after fixing instructions displacements" << endl;

    // Step 6: write translated instructions to the tc:
    rc = copy_instrs_to_tc(tc);
    if (rc < 0 ) {
        cerr << "failed to copy the instructions to the translation cache\n";
        return;
    }
    tc_size = rc;
    cerr << "after write all new instructions to memory tc" << endl;

    if (KnobDumpTranslatedCode) {
       cerr << "Translation Cache dump:" << endl;
       dump_tc(tc, tc_size);  // dump the entire tc

       //cerr << endl << "instructions map dump:" << endl;
       //dump_profile();     // dump all translated instructions in map_instr
    }

    // Step 7: Commit the translated routines:
    //         Go over the candidate functions and replace the original ones
    //         by their new successfully translated ones:
    if (!KnobDoNotCommitTranslatedCode) {
      commit_translated_rtns_to_tc();
      cerr << "after commit of translated routines from orig code to TC" << endl;
    }

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) +
	                 (end.tv_nsec - start.tv_nsec) / 1e9;
    cerr << " create_tc took: " << elapsed << " seconds\n";

	clock_gettime(CLOCK_MONOTONIC, &start_running_time);
    
    tc_created_successfully = true;
}



/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
INT32 Usage()
{
    cerr << "This tool translated routines of an Intel(R) 64 binary"
         << endl;
    cerr << KNOB_BASE::StringKnobSummary();
    cerr << endl;
    return -1;
}


/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char * argv[])
{
    // Open output profile file.
    out = new std::ofstream("edge-profile.csv");

    // Initialize pin & symbol manager
    if( PIN_Init(argc,argv) )
        return Usage();

    PIN_InitSymbols();

    // Register create_tc
    IMG_AddInstrumentFunction(create_tc, 0);

    // Create internal thread to start and stop profile gathering.
    THREADID tid = PIN_SpawnInternalThread(start_stop_profile_gathering_thread_func, NULL, 0, NULL);
    if (tid == INVALID_THREADID) {
        cerr << "failed to spawn a thread for commit" << endl;
    }

    // Start the program, never returns
    PIN_StartProgramProbed();

    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
