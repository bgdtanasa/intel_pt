#ifndef _X_UNWIND_
#define _X_UNWIND_

#include <sys/user.h>

extern void unwind(struct user_regs_struct* regs);

#endif
