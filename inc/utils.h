#ifndef _UTILS_
#define _UTILS_

typedef enum {
  PERFED_STATUS_RUNNING = 0,
  PERFED_STATUS_RUNNING_PTRACING,
  PERFED_STATUS_RUNNING_DECODING,
  PERFED_STATUS_FULL_AUX_BUFFER,
  PERFED_STATUS_SIG_INT,
  PERFED_STATUS_SIG_NEW_EXEC_MAPPING,
} perfed_status_t;

extern volatile perfed_status_t perfed_status;

static inline __attribute__((always_inline)) unsigned long long int read_tsc(void) {
    unsigned int lo;
    unsigned int hi;

    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
#if 0
    asm volatile ("mfence" : : : "memory");
#endif

    return (((unsigned long long int) (hi)) << 32llu) | (((unsigned long long int) (lo)) <<  0llu);
}

#endif
