#ifndef _X_UNWIND_
#define _X_UNWIND_

#include "xed.h"

#include <sys/user.h>

#define MAX_NO_UNWIND_INSTS (256u)
#define UNWIND_QUEUE_LEN    (2048u)

typedef struct {
  unsigned long long int tsc;
  unsigned int           no_insts;
  const inst_t*          insts[ MAX_NO_UNWIND_INSTS ];

  unsigned long long int attach_tsc;
  unsigned long long int detach_tsc;
} unwind_insts_t;

extern unwind_insts_t unwind_queue[ UNWIND_QUEUE_LEN ];
extern unsigned int   unwind_queue_head;
extern unsigned int   unwind_queue_tail;

extern void perfed_unwind(const int perfed_pid);

extern unwind_insts_t* unwind(const int                            perfed_pid,
                              const struct user_regs_struct* const uregs);
extern void unwind_close(void);

#endif
