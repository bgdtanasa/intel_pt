#ifndef _PROC_
#define _PROC_

#include <sys/user.h>

#define MAX_NO_AMAPS (512u)

typedef struct {
  unsigned long long int a;
  unsigned long long int b;
} amap_t;

typedef amap_t amaps_t[ MAX_NO_AMAPS ];

extern amaps_t      amaps;
extern unsigned int no_amaps;

extern void perfed_proc(const int perfed_pid, struct user_regs_struct* regs);

extern unsigned long long int proc_read_perfed_vm(const int                    perfed_pid,
                                                  const unsigned long long int addr);

#endif
