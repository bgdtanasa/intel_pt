#ifndef _X_UNWIND_
#define _X_UNWIND_

#include "xed.h"

#include <sys/user.h>

#define MAX_NO_UNWIND_INSTS (512u)

typedef struct {
  const inst_t* insts[ MAX_NO_UNWIND_INSTS ];
  unsigned int  no_insts;
} unwind_insts_t;

extern void perfed_unwind(const int perfed_pid);

extern void unwind(const int                            perfed_pid,
                   const int                            perfed_cpu,
                   const double                         tsc,
                   const struct user_regs_struct* const regs,
                   unwind_insts_t* const                unwind_insts);
extern void unwind_close(void);

#endif
