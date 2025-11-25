#ifndef _UTILS_
#define _UTILS_

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
