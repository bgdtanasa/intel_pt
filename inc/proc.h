#ifndef _PROC_
#define _PROC_

#include <sys/user.h>

extern void perfed_proc(const int perfed_pid, struct user_regs_struct* regs);

#endif
