#include "pin.H"
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <string>

using std::cerr;
using std::endl;
using std::string;
using std::vector;

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */

std::ofstream bblOut;
std::ofstream rtnOut;

#define MAX_TARGETS 64

struct TargetInfo {
    ADDRINT addr;
    UINT64 count;
};

struct BblData {
    ADDRINT addr;
    UINT64 count;
    bool is_conditional;
    UINT64 taken_count;
    UINT64 fallthru_count;
    bool is_indirect;
    TargetInfo targets[MAX_TARGETS];
    int num_targets;
};

vector<BblData*> bbl_list;

struct RtnData {
    string name;
    ADDRINT addr;
    UINT64 exec_count;
    ADDRINT rax[20];
    ADDRINT rbx[20];
    ADDRINT rcx[20];
    ADDRINT rdx[20];
    ADDRINT rsi[20];
    ADDRINT rdi[20];
};

vector<RtnData*> rtn_list;

/* ===================================================================== */
/* Analysis routines                                                     */
/* ===================================================================== */

VOID PIN_FAST_ANALYSIS_CALL docount_bbl(BblData* bbl) {
    bbl->count++;
}

VOID PIN_FAST_ANALYSIS_CALL docount_branch(BblData* bbl, BOOL taken) {
    if (taken) {
        bbl->taken_count++;
    } else {
        bbl->fallthru_count++;
    }
}

VOID PIN_FAST_ANALYSIS_CALL docount_indirect(BblData* bbl, ADDRINT target, BOOL taken) {
    if (!taken) return;
    for (int i = 0; i < bbl->num_targets; i++) {
        if (bbl->targets[i].addr == target) {
            bbl->targets[i].count++;
            return;
        }
    }
    if (bbl->num_targets < MAX_TARGETS) {
        bbl->targets[bbl->num_targets].addr = target;
        bbl->targets[bbl->num_targets].count = 1;
        bbl->num_targets++;
    }
}

VOID PIN_FAST_ANALYSIS_CALL analyze_rtn(RtnData* data, ADDRINT rax, ADDRINT rbx, ADDRINT rcx, ADDRINT rdx, ADDRINT rsi, ADDRINT rdi) {
    if (data->exec_count < 20) {
        int idx = (int)data->exec_count;
        data->rax[idx] = rax;
        data->rbx[idx] = rbx;
        data->rcx[idx] = rcx;
        data->rdx[idx] = rdx;
        data->rsi[idx] = rsi;
        data->rdi[idx] = rdi;
    }
    data->exec_count++;
}

/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */

VOID Trace(TRACE trace, VOID* v) {
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        BblData* bdata = new BblData;
        bdata->addr = BBL_Address(bbl);
        bdata->count = 0;
        bdata->is_conditional = false;
        bdata->taken_count = 0;
        bdata->fallthru_count = 0;
        bdata->is_indirect = false;
        bdata->num_targets = 0;
        bbl_list.push_back(bdata);

        BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)docount_bbl, IARG_FAST_ANALYSIS_CALL, IARG_PTR, bdata, IARG_END);

        INS tail = BBL_InsTail(bbl);
        if (INS_IsControlFlow(tail)) {
            if (INS_HasFallThrough(tail) && INS_IsControlFlow(tail)) {
                if (INS_Category(tail) == XED_CATEGORY_COND_BR) {
                    bdata->is_conditional = true;
                    INS_InsertCall(tail, IPOINT_BEFORE, (AFUNPTR)docount_branch, IARG_FAST_ANALYSIS_CALL, IARG_PTR, bdata, IARG_BRANCH_TAKEN, IARG_END);
                }
            }
            if (INS_IsIndirectControlFlow(tail) && !INS_IsRet(tail)) {
                bdata->is_indirect = true;
                INS_InsertCall(tail, IPOINT_BEFORE, (AFUNPTR)docount_indirect, IARG_FAST_ANALYSIS_CALL, IARG_PTR, bdata, IARG_BRANCH_TARGET_ADDR, IARG_BRANCH_TAKEN, IARG_END);
            }
        }
    }
}

VOID Routine(RTN rtn, VOID* v) {
    RtnData* rdata = new RtnData;
    rdata->name = RTN_Name(rtn);
    rdata->addr = RTN_Address(rtn);
    rdata->exec_count = 0;
    rtn_list.push_back(rdata);

    RTN_Open(rtn);
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)analyze_rtn, IARG_FAST_ANALYSIS_CALL, IARG_PTR, rdata, 
                   IARG_REG_VALUE, LEVEL_BASE::REG_RAX, IARG_REG_VALUE, LEVEL_BASE::REG_RBX, IARG_REG_VALUE, LEVEL_BASE::REG_RCX, 
                   IARG_REG_VALUE, LEVEL_BASE::REG_RDX, IARG_REG_VALUE, LEVEL_BASE::REG_RSI, IARG_REG_VALUE, LEVEL_BASE::REG_RDI, IARG_END);
    RTN_Close(rtn);
}

/* ===================================================================== */

bool compareTargets(const TargetInfo& a, const TargetInfo& b) {
    return a.count > b.count;
}

bool compareBbl(const BblData* a, const BblData* b) {
    return a->count > b->count;
}

VOID Fini(INT32 code, VOID* v) {
    bblOut.open("edge-profile.csv");
    std::sort(bbl_list.begin(), bbl_list.end(), compareBbl);
    
    for (size_t i = 0; i < bbl_list.size(); ++i) {
        BblData* bdata = bbl_list[i];
        if (bdata->count == 0) continue;
        
        bblOut << "0x" << std::hex << bdata->addr << std::dec << ", " << bdata->count;
        if (bdata->is_conditional) {
            bblOut << ", " << bdata->taken_count << ", " << bdata->fallthru_count;
        } else {
            // Pad empty columns so the indirect targets stay aligned in their proper columns
            bblOut << ", , ";
        }
        
        int printed_targets = 0;
        if (bdata->is_indirect && bdata->num_targets > 0) {
            std::sort(bdata->targets, bdata->targets + bdata->num_targets, compareTargets);
            
            int limit = std::min(bdata->num_targets, 10);
            for (int j = 0; j < limit; j++) {
                bblOut << ", 0x" << std::hex << bdata->targets[j].addr << std::dec << ", " << bdata->targets[j].count;
                printed_targets++;
            }
        }
        
        // Pad the remaining target columns up to 10 targets (2 columns each) so the CSV is perfectly rectangular
        for (int j = printed_targets; j < 10; j++) {
            bblOut << ", , ";
        }
        bblOut << std::endl;
    }
    bblOut.close();

    rtnOut.open("rtn-output.csv");
    for (size_t i = 0; i < rtn_list.size(); ++i) {
        RtnData* rdata = rtn_list[i];
        if (rdata->exec_count == 0) continue;
        
        rtnOut << rdata->name << ", 0x" << std::hex << rdata->addr << std::dec << ", " << rdata->exec_count;
        
        int n = std::min((int)rdata->exec_count, 20);

        auto print_reg = [&](const char* reg_name, ADDRINT* reg_vals) {
            rtnOut << ", " << reg_name << ":";
            for (int j = 0; j < 20; j++) {
                if (j < n) {
                    rtnOut << ", 0x" << std::hex << reg_vals[j] << std::dec;
                } else {
                    rtnOut << ", ";
                }
            }
            if (n > 1) {
                rtnOut << ", Has an Average delta: Yes";
                INT64 delta = ((INT64)reg_vals[n-1] - (INT64)reg_vals[0]) / (n - 1);
                if (delta < 0) {
                    rtnOut << ", Average delta: -0x" << std::hex << -delta << std::dec;
                } else {
                    rtnOut << ", Average delta: 0x" << std::hex << delta << std::dec;
                }
            } else {
                rtnOut << ", Has an Average delta: No, ";
            }
        };

        print_reg("RAX", rdata->rax);
        print_reg("RBX", rdata->rbx);
        print_reg("RCX", rdata->rcx);
        print_reg("RDX", rdata->rdx);
        print_reg("RSI", rdata->rsi);
        print_reg("RDI", rdata->rdi);

        rtnOut << std::endl;
    }
    rtnOut.close();
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage() {
    cerr << "This tool produces edge-profile.csv and rtn-output.csv" << endl;
    cerr << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char* argv[]) {
    PIN_InitSymbols();

    if (PIN_Init(argc, argv)) {
        return Usage();
    }

    TRACE_AddInstrumentFunction(Trace, 0);
    RTN_AddInstrumentFunction(Routine, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();
    
    return 0;
}
