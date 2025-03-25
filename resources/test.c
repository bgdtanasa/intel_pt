#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

#define BUSY_WAIT (4400llu)

static unsigned int a;
static unsigned int b;
static unsigned int c;
static unsigned int d;

static void f_a(void) {
    a++;
}

static void f_b(void) {
    b++;
}

static void f_c(void) {
    c++;
}

static void f_d(void) {
    d++;
}

static inline __attribute__((always_inline)) unsigned long long read_tsc(void) {
    unsigned int lo;
    unsigned int hi;

    fprintf(stdout, "x\n");
    free(malloc(250));
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));

    return (((unsigned long long) (hi)) << 32llu) |
           (((unsigned long long) (lo)) <<  0llu);
}

#define STACK_SIZE (16llu * 1024llu)

static pthread_t      th_id;
static pthread_attr_t th_attrs;
static void*          th_stack;

static void* th_main(void* args) {
    void (*my_f)(void);

    pthread_setname_np(pthread_self(), "bgd_test");
    for (;;) {
        unsigned long long start   = read_tsc();
        unsigned long long elapsed;

#if 1
        do {
            elapsed = read_tsc() - start;
        } while (elapsed <= BUSY_WAIT);
#endif

        asm volatile ("mov %0, %%rax" : : "r"(elapsed) : "rax");
        asm volatile ("ptwrite %%rax" : : : "rax");
        asm volatile ("mfence" : : : "memory");

        elapsed -= BUSY_WAIT;
        if (elapsed > 500llu) {
            my_f = f_a;
        } else if (elapsed > 250llu) {
            my_f = f_b;
        } else if (elapsed > 100llu) {
            my_f = f_c;
        } else {
            my_f = f_d;
        }
        my_f();
        //fprintf(stdout, "X\n");
        {
            unsigned long long int ret     = 0llu;
            const char             buf[]   = "Bogdan\n";
            const unsigned int     buf_len = ((unsigned int) (sizeof(buf)));

            // write
            //asm volatile("syscall" : "=a"(ret) : "a"(1), "D"(1), "S"(buf), "d"(buf_len) : "memory", "cc", "r11", "cx");
        }
        {
            struct timespec ts;

            clock_gettime(CLOCK_MONOTONIC, &ts);
        }
    }
    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
    //mlockall(MCL_FUTURE | MCL_CURRENT);

    th_stack = malloc(STACK_SIZE);
    memset(th_stack, 0xAA, STACK_SIZE);
    fprintf(stdout, "th_stack = %016llx\n", ((unsigned long long int) (th_stack)));

    pthread_attr_init(&th_attrs);
    pthread_attr_setstack(&th_attrs, th_stack, STACK_SIZE);
    pthread_create(&th_id, &th_attrs, th_main, NULL);

    pthread_join(th_id, NULL);
    return 0;
}

