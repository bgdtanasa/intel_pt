#ifndef _BRK_
#define _BRK_

#include "xed.h"

extern int brking_cpu;

extern void install_brk(const inst_t* const inst);
extern void perfed_brks(const int perfed_pid);
extern void brk_close(void);

#endif
