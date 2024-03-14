#include <stdio.h>
#include <unistd.h>

static inline unsigned long long read_tsc(void) {
    unsigned int lo;
    unsigned int hi;

    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));

    return (((unsigned long long) (hi)) << 32llu) | 
           (((unsigned long long) (lo)) <<  0llu);
}

int main(int argc, char* argv[]) {
    unsigned long long int value = 6llu;
    for (;;) {
        asm volatile ("mov %0, %%rax" : : "r"(value) : "rax");
        asm volatile ("ptwrite %%rax" : : : "rax");
        asm volatile ("mfence" : : :"memory");
        value++;

#if 1
        unsigned long long start   = read_tsc();
        unsigned long long elapsed = 0llu;
        do {
            elapsed = read_tsc() - start;
        } while (elapsed <= 44000llu);
#else
        sleep(1);
#endif
        //fprintf(stdout, "Test Tick!\n");
    }
    return 0;
}