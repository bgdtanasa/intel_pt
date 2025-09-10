#ifndef _X_UNWIND_
#define _X_UNWIND_

#include <sys/user.h>

extern void perfed_unwind(const int perfed_pid);

extern void unwind(const int                            perfed_pid,
                   const int                            perfed_cpu,
                   const struct user_regs_struct* const regs);
extern void unwind_close(void);

#endif
