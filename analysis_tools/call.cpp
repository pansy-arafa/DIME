
#include "pin.H"
#include "portability.H"
#include <vector>
#include <iomanip>
#include <fstream>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include <unistd.h> 
#include <time.h>
#include <unordered_set>
#include <set>

// This file is based on debugtrace.cpp in Pin kit
//Note: no thread handling

string File_Name = "call.out";//output file name
FILE* Trace_File;


/* ===================================================================== */
VOID EmitDirectCall(THREADID threadid, string * str, INT32 tailCall)
{
    fprintf(Trace_File, "%s\n", (*str).c_str() );    
}

string FormatAddress(ADDRINT address, RTN rtn)
{
    string s = "";
    
    if (RTN_Valid(rtn))
    {
        s += " ";
        s += RTN_Name(rtn);
    }
	else
	{
		s += " ";
		s += "invalid";
	}
    return s;
}

VOID EmitIndirectCall(THREADID threadid, string * str, ADDRINT target)
{
    PIN_LockClient();
    string s = FormatAddress(target, RTN_FindByAddress(target));
    PIN_UnlockClient();
	fprintf(Trace_File, "%s%s\n", (*str).c_str(), s.c_str() );
}

VOID EmitReturn(THREADID threadid, string * str)
{
    fprintf(Trace_File, "%s\n", (*str).c_str() );
}

        
VOID CallTrace(TRACE trace, INS ins)
{

    if (INS_IsCall(ins) && !INS_IsDirectBranchOrCall(ins))
    {
        // Indirect call
        string s = "C" + FormatAddress(INS_Address(ins), TRACE_Rtn(trace));
        s += " ";
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(EmitIndirectCall), IARG_THREAD_ID,
                       IARG_PTR, new string(s), IARG_BRANCH_TARGET_ADDR, IARG_END);
    }
    else if (INS_IsDirectBranchOrCall(ins))
    {
        // Is this a tail call?
        RTN sourceRtn = TRACE_Rtn(trace);
        RTN destRtn = RTN_FindByAddress(INS_DirectBranchOrCallTargetAddress(ins));

        if (INS_IsCall(ins)         // conventional call
            || sourceRtn != destRtn // tail call
        )
        {
            BOOL tailcall = !INS_IsCall(ins);
            
            string s = "";
            if (tailcall)
            {
                s += "T";
            }
            else
            {
                if( INS_IsProcedureCall(ins) )
                    s += "C";
                else
                {
                    s += "PcMaterialization";
                    tailcall=1;
                }
                
            }
            s += FormatAddress(INS_Address(ins), TRACE_Rtn(trace));
            ADDRINT target = INS_DirectBranchOrCallTargetAddress(ins);
            s += FormatAddress(target, RTN_FindByAddress(target));
            INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(EmitDirectCall),
                           IARG_THREAD_ID, IARG_PTR, new string(s), IARG_BOOL, tailcall, IARG_END);
        }
    }
    else if (INS_IsRet(ins))
    {
        RTN rtn =  TRACE_Rtn(trace);
#if defined(TARGET_LINUX) && defined(TARGET_IA32)
        if( RTN_Valid(rtn) && RTN_Name(rtn) ==  "_dl_runtime_resolve") return;
#endif
        string tracestring = "R" + FormatAddress(INS_Address(ins), rtn);
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, AFUNPTR(EmitReturn),
                       IARG_THREAD_ID, IARG_PTR, new string(tracestring), IARG_END);
    } 
}
     
/* ===================================================================== */

VOID Trace(TRACE trace, VOID *v)
{
	UINT64 trace_addr = TRACE_Address(trace);
	IMG img = IMG_FindByAddress(trace_addr);
	if(!IMG_Valid(img)) return;
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
    {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
        {
        	if(INS_IsCall(ins) || INS_IsDirectBranchOrCall(ins) || INS_IsRet(ins))
        	{
            	CallTrace(trace, ins);
            }
        }
    }
}

/* ===================================================================== */

VOID Fini(int, VOID * v)
{
	fprintf(Trace_File, "# eof");
	fclose(Trace_File);  	
}
/* ===================================================================== */

int main(int argc, CHAR *argv[])
{
    PIN_Init(argc,argv);
    PIN_InitSymbols();    
    
    Trace_File = fopen(File_Name.c_str(), "w");
    
    TRACE_AddInstrumentFunction(Trace, 0);
    PIN_AddFiniFunction(Fini, 0);
    
    // Never returns
    PIN_StartProgram();
    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
