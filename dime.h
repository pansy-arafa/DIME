
/*
	Nov 2016
	Pansy Arafa
	This is DIME header file
	no redundancy suppression, uni-threaded only
	Dime Implementation Type: Trace Version
*/

#include <iostream>
#include <fstream>
#include <sstream>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <math.h>

#define sec_to_nsec 1000000000//from second to nanosecond
#define sec_to_usec 1000000//from second to microsecond
#define usec_to_nsec 1000//from microsecond to nanosecond
#define Freq 3401 //grep 'cpu MHz' /proc/cpuinfo
#define rdtsc(low,high) \
     __asm__ __volatile__("rdtsc" : "=a" (low), "=d" (high))

static struct sigaction Alarm_Reset;//alarm to reset the budget using signal.h
static struct itimerval Interval;//used by setitimer()	
static UINT32 Budget;//in nanoseconds
static INT64 Budget_Dec;//variable to decrement, in nanoseconds
static const INT32 MAX_SIZE = 86400;//enough to write a string of 11 char's every second for a full day
static INT64 Budget_Array[MAX_SIZE];
static INT32 Counter = 0;
UINT32 Low_S, Low_E;//used by rdtsc()
UINT32 High_S, High_E;//used by rdtsc()
static REG Version_Reg;//used by INS_InsertVersionCase()
enum 
{
    VERSION_BASE,
    VERSION_INSTRUMENT
};

// Knobs (command line arguments)
KNOB<float> KnobBudgPercent(KNOB_MODE_WRITEONCE, "pintool", "b", "10", "Budget Percentage");
KNOB<float> KnobPeriod(KNOB_MODE_WRITEONCE, "pintool", "p", "1.0", "Time Period in seconds (as float)");

/* ----------------------------------------------------------------- */
//Alarm handler of alarm_reset
VOID handler_reset(int signum)
{
	//print budget before reset (for testing)
	Budget_Array[Counter++] = Budget_Dec;
	//Reset Budget
    Budget_Dec = Budget;
}
/* ----------------------------------------------------------------- */
// returns 1 if we should switch to heavyweight instrumentation 
static inline int dime_has_budget()
{    
    return (Budget_Dec > 0);//this gets inlined
}

/* ================================================================= */
/* ----------------------- Switching Versions ---------------------- */
/*	From Pin Documentation:
	* INS_InsertVersionCase(): Insert a dynamic test to switch between versions before ins. If the value in reg matches case_value, then continue execution at ins with version version. Switching to a new version will cause execution to continue at a new trace starting with ins. This API can be called multiple times for the same instruction, creating a switch/case construct to select the version.
	* By default, all traces have a version value of 0. A trace with version value N only transfers control to successor traces with version value N. There are some situations where Pin will reset the version value to 0, even if executing a non 0 trace. This may occur after a system call, exception or other unusual control flow. 
*/
/* ----------------------------------------------------------------- */
/* Switches version if required. Uses Pin Trace Version APIs */
static inline void dime_switch_version(ADDRINT version, INS ins)
{
	INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(dime_has_budget),
		                   IARG_RETURN_REGS, Version_Reg,IARG_END);
	if(version == VERSION_BASE) {  //check if you need to switch to VERSION_INSTRUMENT
		INS_InsertVersionCase(ins, Version_Reg, 1, VERSION_INSTRUMENT, IARG_END);      
	}
	else if(version == VERSION_INSTRUMENT){  //check if you need to switch to VERSION_BASE
		INS_InsertVersionCase(ins, Version_Reg, 0, VERSION_BASE, IARG_END);      
	}
}

/* ----------------------------------------------------------------- */
static inline void dime_start_time()
{
	rdtsc(Low_S,High_S);
}
/* ----------------------------------------------------------------- */
static inline void dime_end_time()
{
	rdtsc(Low_E,High_E);
	Budget_Dec -= (Low_E - Low_S)*1000/Freq;
	//Note: we ignore High_E and High_S since the analysis routine can never exceed 3.5 sec
}
/* ----------------------------------------------------------------- */
//for testing
//writes Budget_Array to pintool.log using LOG() 
//(Budget_Array holds the budget values before reset)
static inline void dime_fini()
{
    //write overshoots to pintool.log
	LOG("#begin (BUDGET = " + decstr(Budget) +  "ns)\n");
	LOG("#Interval = " + decstr(Interval.it_value.tv_sec) + " sec + " + decstr(Interval.it_value.tv_usec) + " usec\n" );
    for (int i = 0; i < Counter; i++){
        LOG(decstr(Budget_Array[i]) + "\n");
    }
    LOG("#eof\n");
}
/* ----------------------------------------------------------------- */

// Dime initialization function
// Sets Dime parameters
static inline void dime_init()
{
    // to reset budget every period_t_sec seconds plus period_t_usec microseconds */
	float period_t;//time period in seconds
	float percentage;//budget percentage from 0% to 100% 
	// Get Knob values (command line arguments) 
	// Defaults: budget percentage = 10%, period time = 1 sec */
	percentage = KnobBudgPercent.Value();
	period_t = KnobPeriod.Value();
	/* Set Budget */	
	Budget = ((float)percentage/100) * (period_t * sec_to_nsec); //% budget in nanoseconds
	Budget_Dec = Budget;	
	/* Set Alarm */
	// Alarm handler
	Alarm_Reset.sa_handler = handler_reset;
	int ret = sigaction(SIGVTALRM, &Alarm_Reset, NULL);//sigaction() returns 0 on success
	if(ret != 0) 
	{
		ofstream errfile;//error file
		errfile.open("error_dime.out", std::ofstream::out);
		errfile << "Dime initialization failed! sigaction() failed!\nErrNo = " << errno << endl;
		errfile.close();
		return;
	}
	// Alarm Interval
	//to fire the first time
    Interval.it_value.tv_sec = int(period_t);// seconds
	Interval.it_value.tv_usec = fmod(period_t, 1.0)*sec_to_usec; //micro seconds
    //to repeat the alarm
	Interval.it_interval.tv_sec = int(period_t); //seconds
    Interval.it_interval.tv_usec = fmod(period_t, 1.0)*sec_to_usec; // micro second
    ret = setitimer(ITIMER_VIRTUAL, &Interval, NULL);   
    if(ret != 0) 
	{
		ofstream errfile;//error file
		errfile.open("error_dime.out", std::ofstream::out);
		errfile << "Dime initialization failed! setitimer() failed!\nErrNo = " << errno << endl;
		errfile.close();
		return;
	}
    /* For Trace Versioning */
	PIN_InitSymbols();
	// Scratch register used to select instrumentation version
	Version_Reg = PIN_ClaimToolRegister();
}


