# DIME
A time-aware dynamic binary instrumentation framework that respects the input timing constraint during execution.

# References
- **"DIME: Time-aware Dynamic Binary Instrumentation Using Rate-based Resource Allocation"**
https://uwaterloo.ca/embedded-software-group/sites/ca.embedded-software-group/files/uploads/files/dynamic_0.pdf
- **"Redundancy Suppression In Time-Aware Dynamic Binary Instrumentation"**  
https://arxiv.org/pdf/1703.02873.pdf
- **The Time-aware Instrumentation Project**   
https://uwaterloo.ca/embedded-software-group/projects/time-aware-instrumentation

# Installation
Download Pin instrumentation framework (https://software.intel.com/en-us/articles/pintool-downloads)


# How to use
- Get familiar with Pin instrumentation framework (https://software.intel.com/en-us/articles/pintool/)
- Your working folder should include your pintool.cpp file and dime.h
- Copy Makefile and Makefile.rules from any pintool folder to your working folder
- `#include dime.h` in your pintool.cpp
- In `main()`, call `dime_init()`
- In `Fini()`, call `dime_fini()`
-  In the analysis routines call `dime_start_time()` at the beginning and `dime_end_time()` at the end
- In the instrumentation routine call `dime_switch_version(version, ins)` followed by the switch case as in the example
And If you are using the redundancy supression feature:
- Call `dime_thread_start()` in the ThreadStart callback function
- Before the loops in the instrumentation routine: `if(dime_compare_to_log())`
- After the loop, but inside the condition in the previous bullet call `dime_modify_log()`
- Check call_dime.cpp or branch_dime for examples (in the analysis-tol folder).

# Commands
### To compile  
`make obj-intel64/pintool.so`	  
or `make obj-ia32/pintool.so`  
### To run pintool with DIME  
  `pin -t obj-intel64/pintool.so -r <R> -p <P> -b <B> -- app`  
  Such that:
  - `P` is the time Period in seconds. The default value is 1.0
  - `B` is the budget percentage [0 - 100]. The default value is 10%
  - `R` is the run number for the redundancy suppression feature. Default is 0, i.e., feature disabled.
