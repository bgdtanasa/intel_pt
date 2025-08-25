#ifndef _PMU_
#define _PMU_

#include <stdio.h>

#include <sys/types.h>

extern void perfed_pmu(const pid_t perfed_pid,
                       const int   perfed_cpu,
                       const int   intel_pt_fd);

extern void pmu_info(const unsigned long long int pmu_mask, FILE* fp);
extern void pmu_close(void);

#endif
