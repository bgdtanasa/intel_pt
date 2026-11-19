#ifndef _PROC_
#define _PROC_

#include <sys/user.h>

extern void perfed_proc(const int perfed_pid, struct user_regs_struct* regs);
#if defined(EN_VMLINUX)
extern void proc_update_modules(void);
#endif

extern unsigned long long int proc_read_perfed_vm(const int                    perfed_pid,
                                                  const unsigned long long int addr);

#endif
