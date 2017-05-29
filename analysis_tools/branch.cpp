
#include <stdio.h>
#include <string.h>
#include "pin.H"
#include "instlib.H"
#include "portability.H"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdlib>
#include <sys/time.h>
#include <map>

FILE* Trace_File;
/* ===================================================================== */
static char nibble_to_ascii_hex(UINT8 i) {
    if (i<10) return i+'0';
    if (i<16) return i-10+'A';
    return '?';
}

/* ===================================================================== */
static void print_hex_line(char* buf, const UINT8* array, const int length) {
  int n = length;
  int i=0;
  if (length == 0)
      n = XED_MAX_INSTRUCTION_BYTES;
  for( i=0 ; i< n; i++)     {
      buf[2*i+0] = nibble_to_ascii_hex(array[i]>>4);
      buf[2*i+1] = nibble_to_ascii_hex(array[i]&0xF);
  }
  buf[2*i]=0;
}

/* ===================================================================== */
static string
disassemble(UINT64 start, UINT64 stop) {
    UINT64 pc = start;
    xed_state_t dstate;
    xed_syntax_enum_t syntax = XED_SYNTAX_INTEL;
    xed_error_enum_t xed_error;
    xed_decoded_inst_t xedd;
    ostringstream os;
    if (sizeof(ADDRINT) == 4) 
        xed_state_init(&dstate,     
                       XED_MACHINE_MODE_LEGACY_32,
                       XED_ADDRESS_WIDTH_32b, 
                       XED_ADDRESS_WIDTH_32b);
    else
        xed_state_init(&dstate,
                       XED_MACHINE_MODE_LONG_64,
                       XED_ADDRESS_WIDTH_64b, 
                       XED_ADDRESS_WIDTH_64b);

    xed_decoded_inst_zero_set_mode(&xedd, &dstate);
    UINT32 len = 15;
    if (stop - pc < 15)
        len = stop-pc;

    xed_error = xed_decode(&xedd, reinterpret_cast<const UINT8*>(pc), len);
    bool okay = (xed_error == XED_ERROR_NONE);
    iostream::fmtflags fmt = os.flags();
    if (okay) {
        char buffer[200];
        unsigned int dec_len;

        dec_len = xed_decoded_inst_get_length(&xedd);
        print_hex_line(buffer, reinterpret_cast<UINT8*>(pc), dec_len);
        memset(buffer,0,200);
        int dis_okay = xed_format_context(syntax, &xedd, buffer, 200, pc, 0, 0);
        if (dis_okay) 
            os << buffer;
        else
            os << "Error disasassembling pc 0x" << std::hex << pc << std::dec << endl;
        pc += dec_len;
    }
    else { // print the byte and keep going.
        UINT8 memval = *reinterpret_cast<UINT8*>(pc);
        os << "???? " // no extension
           << std::hex
           << std::setw(2)
           << std::setfill('0')
           << static_cast<UINT32>(memval)
           << std::endl;
        pc += 1;
    }
    os.flags(fmt);
    
    return os.str();
}

/* ===================================================================== */

static VOID AtBranch(ADDRINT ip, ADDRINT target, BOOL taken)
{
	if (taken)
    {
        string s = disassemble ((ip),(ip)+15);
        //prints: assembly
        fprintf (Trace_File, "%s\n", s.c_str());
        //fflush (Trace_File);
    }
}

/* ===================================================================== */
static VOID Trace(TRACE trace, VOID *v)
{
	IMG img = IMG_FindByAddress(TRACE_Address(trace));
	if(!IMG_Valid(img)) return;
	for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
	{
		for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins))
		{
			if (INS_IsBranchOrCall(ins))
			{
				INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AtBranch, IARG_INST_PTR, 
					IARG_BRANCH_TARGET_ADDR, IARG_BRANCH_TAKEN , IARG_END);
			}
		}
	}        
}

/* ===================================================================== */
VOID Fini(INT32 code, VOID *v)
{
	fclose(Trace_File);
}

/* ===================================================================== */
int main(INT32 argc, CHAR **argv)
{
    PIN_Init(argc, argv);
    PIN_InitSymbols();
	Trace_File = fopen("branch.out", "w");	
	PIN_AddFiniFunction(Fini, 0);
    TRACE_AddInstrumentFunction(Trace, 0);
    PIN_StartProgram();
    return 0;
}


