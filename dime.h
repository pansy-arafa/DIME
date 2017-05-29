/*
	May 2017
	Pansy Arafa
	This is DIME header file
	    - With the redundancy suppression feature (optional)
	    - Thread-safe
	Dime Implementation Type: Trace Version
	Redundancy suppression log type: hashtable
*/

/*	To use DIME in your Pin tool:
	1- In main() call dime_init()
	2- In Fini() call dime_fini()
	3- In the analysis routines call dime_start_time() at the beginning and dime_end_time() at the end
	4- In the instrumentation routine:
		- call dime_switch_version(version, ins); followed by the switch case
		
	And to enable redundancy supression:
	5- Call dime_thread_start() in the ThreadStart callback function
	6- Before the loops in the instrumentation routine:
		if(dime_compare_to_log())
	7- After the loop, but inside the condition in bullet 6:
		dime_modify_log()
		
	And include DIME's header file:
	8- #include "dime.h"
*/


#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <signal.h>
#include <assert.h>
#include <sys/time.h>
#include <unordered_map>

#define sec_to_nsec 1000000000//from second to nanosecond
#define usec_to_nsec 1000//from microsecond to nanosecond
#define Freq 3401 //grep 'cpu MHz' /proc/cpuinfo
#define rdtsc(low,high) \
     __asm__ __volatile__("rdtsc" : "=a" (low), "=d" (high))
     
// Knobs (command line arguments)
KNOB<float> KnobBudgPercent(KNOB_MODE_WRITEONCE, "pintool", "b", "10", "Budget Percentage");
KNOB<float> KnobPeriod(KNOB_MODE_WRITEONCE, "pintool", "p", "1.0", "Time Period in seconds (as float)");     
KNOB<int> KnobRunNum(KNOB_MODE_WRITEONCE, "pintool", "r", "0", "Run Number (for redundancy suppression)");// 0: to disable (default)

struct sigaction Alarm_Reset;//alarm to reset the budget using signal.h
struct itimerval Interval;//used by setitimer()
static FILE* Trace_File;//for the tool output
static ofstream Trace_File2;//for the overshoots
static UINT32 Budget;//in nanoseconds
static INT64 Budget_Dec;//variable to decrement, in nanoseconds
static const INT32 MAX_SIZE = 86400;//enough to write a string of 11 char's every second for a full day
static INT64 Budget_Array[MAX_SIZE];
static INT32 Counter = 0;
UINT32 Low_S, Low_E;//used by rdtsc()
UINT32 High_S, High_E;//used by rdtsc()
static REG Version_Reg;//used by INS_InsertVersionCase()
static BOOL Alarm_Fired = true;//for TVC
enum 
{
    VERSION_BASE,
    VERSION_INSTRUMENT
};

//the log is a hashtable (unordered_map)
int Run_Num = 1;//DIME run
BOOL Redun_Suppress = false;//flag if redundancy suppression feature is used
static TLS_KEY Tls_Key;
PIN_LOCK Lock;
INT32 Num_Threads = 0;

class LogData
{
  public:
    LogData() : Previous_Trace(0), Previous_Size(0), Total_Test(0), Errors("") {}
    std::unordered_map<UINT64,USIZE> Log;//trace relative address, trace size: only for the instrumented traces
    UINT64 Previous_Trace;//relative address of previous trace whose version = 1
    USIZE Previous_Size;//size of previous trace whose version = 1
    int Total_Test;//total number of traces compared to Log
    string Errors;
    FILE* Trace_File;//use it if you need a Trace_File for each thread to avoid racing
};

class ThreadData
{
    public:
	ThreadData(void) {}
	std::unordered_map<char*,LogData> Img_Logs; //<img_name, LogData>: each image in the thread has its own log data
};
/* ----------------------------------------------------------------- */
// function to access Log data of specific image in a specific thread
LogData* get_logdata(THREADID thread_id, char* img_name)
{
    ThreadData* tdata = 
          static_cast<ThreadData*>(PIN_GetThreadData(Tls_Key, thread_id));
    if(tdata->Img_Logs.find(img_name) == tdata->Img_Logs.end())//if img_name not found
    { 
	tdata->Img_Logs.insert(std::pair<char*, LogData>((char*)img_name,  LogData()));
    }
    return &(tdata->Img_Logs[img_name]);// returns log data of image 
}

// function to access thread-specific data
ThreadData* get_tls(THREADID thread_id)
{
    ThreadData* tdata = 
          static_cast<ThreadData*>(PIN_GetThreadData(Tls_Key, thread_id));
    return tdata;
}
/* ----------------------------------------------------------------- */
int read_log(string file, THREADID thread_id, char* img_name)
{
	string line;
	ifstream myfile(file);
	int ret = 0;
	UINT64 tr;
	USIZE sz;
	LogData* ldata = get_logdata(thread_id, img_name);
	if (myfile.is_open() && myfile.good())
	{
		while(getline (myfile,line,'\n'))
		{
			istringstream iss;
			iss.str(line);
			if(Run_Num > 1)//log
			{
				iss >> tr >> sz;
				ldata->Log[tr] = sz;
			}
		}
		ret = 1;
	 }
	 myfile.close();	
	 return ret;
}
/* ----------------------------------------------------------------- */
//Alarm handler of alarm_reset
VOID handler_reset(int signum)
{
	Alarm_Fired = true;
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
/* TV: Switches version if required */
static inline void dime_switch_version(ADDRINT version, INS ins)
{
    INS_InsertCall(ins, IPOINT_BEFORE, AFUNPTR(dime_has_budget), IARG_RETURN_REGS, Version_Reg,IARG_END);	
	if(version == VERSION_BASE) {  //check if you need to switch to VERSION_INSTRUMENT
		INS_InsertVersionCase(ins, Version_Reg, 1, VERSION_INSTRUMENT, IARG_END);      
	}
	else if(version == VERSION_INSTRUMENT){  //check if you need to switch to VERSION_BASE
		INS_InsertVersionCase(ins, Version_Reg, 0, VERSION_BASE, IARG_END);      
	}
}

/* ----------------------------------------------------------------- */
/* ================================================================= */

static inline void dime_start_time()
{
	rdtsc(Low_S,High_S);
}
/* ----------------------------------------------------------------- */
static inline void dime_end_time()
{
	rdtsc(Low_E,High_E);
	Budget_Dec -= (Low_E - Low_S)*1000/Freq;
	//Note: we ignore High_E and High_S since Analysis routine can never exceed 3.5 sec
}
/* ----------------------------------------------------------------- */
static inline bool dime_compare_to_log(THREADID thread_id, UINT64 trace_rel_addr, USIZE trace_size, char* img_name)
{
	bool ret_val = 0;
	if(Redun_Suppress)
	{
	    std::unordered_map<UINT64,USIZE>::iterator Iterator;
	    LogData* ldata = get_logdata(thread_id, img_name);
	    if(ldata->Log.empty())
	    {
		    ret_val = 1;
	    }
	    else
	    {
		    ldata->Total_Test++;
		    Iterator = ldata->Log.find(trace_rel_addr);
		    //avg. case: constant, worst case: linear
		    //check keys only (range checking is not possible)
		    if(Iterator == ldata->Log.end())//not found
		    {
			    ret_val = 1;
		    }
		    else
		    {
			    //found as key
			    ret_val = 0;
		    }
	    }
	}
	return ret_val;
}
/* ----------------------------------------------------------------- */
static inline void dime_modify_log(ADDRINT version, THREADID thread_id, UINT64 trace_rel_addr, USIZE trace_size, char* img_name)
{
    if(Redun_Suppress)
	{
	    std::unordered_map<UINT64,USIZE>::iterator Iterator;
	    LogData* ldata = get_logdata(thread_id, img_name);
	    USIZE new_size;
	    if(version == VERSION_BASE && trace_rel_addr == ldata->Previous_Trace)
	    {
		    //handle the case in which: the trace initialy has version 1, 
		    //then Pin checks budget, accordingly the trace switches to version 0
		    //therefore, remove this trace from the log
		    if(ldata->Log.erase(trace_rel_addr) != 1)
		    {
			    ostringstream ss;
			    ss <<  "Error " << trace_rel_addr;
			    ldata->Errors += ss.str();
			    ldata->Errors += " not erased\n";
		    }
		    ldata->Previous_Trace = 0;
		    ldata->Previous_Size = 0;
	    }
	    else if(version == VERSION_INSTRUMENT)//record instrumented trace
	    {
		    ldata->Log[trace_rel_addr] = trace_size;
		    //avg. case: constant, worst case: linear
		    ldata->Previous_Trace = trace_rel_addr;
		    ldata->Previous_Size = trace_size;
		
		    ldata->Positives[ldata->Indx_Pos][0] = trace_rel_addr;
		    ldata->Positives[ldata->Indx_Pos][1] = trace_size;
		    ldata->Indx_Pos++;
	    }
	}
}
/* ----------------------------------------------------------------- */
//helper function
// split img name by '/', return last token
// e.g. input= /lib/x86_64-linux-gnu/libc.so.6 output= libc.so.6
char* get_simple_img_name(char* str)
{
	std::istringstream ss(str); //convert string to stream
	std::string tok; //temp
	std::string ret_str; //return string
	while(std::getline(ss, tok, '/')) { }
	return (char*)tok.c_str(); //return last token
}

//1. for testing: writes Budget_Array to pintool.log using LOG() 
//   (Budget_Array holds the budget values before reset)
//2. writes redundancy-suppression Log to logfile
static inline void dime_fini()
{
	//write overshoots to pintool.log
	LOG("#begin (BUDGET = " + decstr(Budget) +  "ns)\n");
	LOG("#Interval = " + decstr(Interval.it_value.tv_sec) + " sec + " + decstr(Interval.it_value.tv_usec) + " usec\n" );
    for (int i = 0; i < Counter; i++){
        LOG(decstr(Budget_Array[i]) + "\n");
    }
    LOG("#eof\n");
    
    if(Redun_Suppress)
    {
        char* img_name;
        char* simple_img_name;
        LogData log_data;    
        string file;
        ofstream logfile;
        ofstream Rfile;
        //for each thread, for each img : write to a log file & write to a run file
        for(int t = 0; t < Num_Threads; t++)
	    {
	       ThreadData* tdata = get_tls(t);
	       for ( auto it0 = tdata->Img_Logs.begin(); it0 != tdata->Img_Logs.end(); ++it0 )
	       {
	            //it0->first is the img name, it0->second is the log data
	            img_name = it0->first;
	            log_data = it0->second;
	            simple_img_name = get_simple_img_name(img_name) ; // get rid of '/' in img_name
	            // *** Log File ***
	            //log file name: log _ threadid _ simpleImgName .out
	            ostringstream ss;
	            ss << "_" << (t + 1) << "_" << simple_img_name; //threadid_imgname
             	file = "log";
             	file += ss.str();
             	file += ".out";
	            //write to log file
	            logfile.open(file, std::ofstream::out);
	            for ( auto it = log_data.Log.begin(); it != log_data.Log.end(); ++it )
	            {
		            logfile << it->first << " " << it->second << endl;
	            }
	            logfile.close();
	       }
	    }
	}

}
/* ----------------------------------------------------------------- */
static inline void dime_thread_start(THREADID thread_id)
{
    if(thread_id > 0) //if not first thread
    {
    	GetLock(&Lock, thread_id+1);
    	Num_Threads++;
    	ReleaseLock(&Lock);
    	ThreadData* tdata = new ThreadData;
    	PIN_SetThreadData(Tls_Key, tdata, thread_id);
    }
    LOG("Thread " + decstr(thread_id) + "\n");
}

VOID ImageLoad(IMG img, VOID *v)
{
   THREADID thread_id = PIN_ThreadId();
   char* img_name = (char*)(IMG_Name(img)).c_str();
   int img_id = IMG_Id(img);
	//read log file
	if(Run_Num > 1 && Redun_Suppress)
	{
		//read log data of current image & current thread from file
		//log file name: log _ threadid _ simpleImgName .out
		ostringstream os;
		string file = "log";
		os << "_" << (thread_id+1) << "_" << get_simple_img_name(img_name);
		file += os.str();//threadid_imgname
		file += ".out";
		read_log(file, thread_id, img_name);
	}
	LOG("loading img " + decstr(img_id) + " " + img_name + " " + decstr(thread_id) + "\n");
}

/* ----------------------------------------------------------------- */
// Dime initialization function
// Sets Dime parameters
static inline void dime_init(char *filename1, char *filename2, int percent, int t_sec, int t_usec)
{
	// to reset budget every period_t_sec seconds plus period_t_usec microseconds */
	float period_t;//time period in seconds
	float percentage;//budget percentage from 0% to 100% 
	// Get Knob values (command line arguments) 
	// Defaults: budget percentage = 10%, period time = 1 sec */
	percentage = KnobBudgPercent.Value();
	period_t = KnobPeriod.Value();
	// open file
	Trace_File = fopen (filename1, "w");
	/* Set Budget */	
	Budget = ((float)percentage/100) * (period_t_sec * sec_to_nsec + period_t_usec * usec_to_nsec); //% budget in nanoseconds
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
	/* Redundancy Suppression */
	if(KnobRunNum.Value() > 0)
	{	
		Redun_Suppress = true;
	}
    // Run number for redundancy suppression
    Run_Num = KnobRunNum.Value();
    // Register ImageLoad to be called when an image is loaded
    IMG_AddInstrumentFunction(ImageLoad, 0);
    // For trace versioning
	PIN_InitSymbols();
	Version_Reg = PIN_ClaimToolRegister();// Scratch register used to select instrumentation version
	InitLock(&Lock);
    // Obtain  a key for Thread local storage.
    Tls_Key = PIN_CreateThreadDataKey(0);
    //set thread data for the first thread
    //so that when image load happens, the thread data is already created
	Num_Threads++;
    //ReleaseLock(&Lock);
	ThreadData* tdata = new ThreadData;
	PIN_SetThreadData(Tls_Key, tdata, 0);

}

