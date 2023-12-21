#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>

#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>

//  /sys/bus/event_source/devices/intel_pt/type
#define INTEL_PT_TYPE              ((__u32) (8u))
// /sys/bus/event_source/devices/intel_pt/format/*
#define INTEL_PT_CONFIG_PT         ((__u64) (1llu <<  0llu))
#define INTEL_PT_CONFIG_CYC        ((__u64) (1llu <<  1llu))
#define INTEL_PT_CONFIG_PWR_EVT    ((__u64) (1llu <<  4llu))
#define INTEL_PT_CONFIG_FUP_ON_PTW ((__u64) (1llu <<  5llu))
#define INTEL_PT_CONFIG_TSC        ((__u64) (1llu << 10llu))
#define INTEL_PT_CONFIG_NORETCOMP  ((__u64) (1llu << 11llu))
#define INTEL_PT_CONFIG_PTW        ((__u64) (1llu << 12llu))
#define INTEL_PT_CONFIG            (INTEL_PT_CONFIG_PT | INTEL_PT_CONFIG_CYC | INTEL_PT_CONFIG_FUP_ON_PTW | INTEL_PT_CONFIG_TSC | INTEL_PT_CONFIG_NORETCOMP | INTEL_PT_CONFIG_PTW)

static pid_t perfed_pid;
static int   perfed_cpu;
static int   perfing_cpu;
static int   perfing_fd;

static void* perfing_main(void* args) {
    int       ret;
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);
    CPU_SET(perfing_cpu, &cpu_set);

    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    if (ret == 0) {
        fprintf(stdout,
                "perfing from cpu %2d via fd %2d :: cpu %6d pid %2d\n",
                perfing_cpu,
                perfing_fd,
                perfed_pid,
                perfed_cpu);
        for (;;) {

        }
    } else {
        fprintf(stderr, "pthread_setaffinity_np failed %s\n", strerror(ret));
    }

    pthread_exit(NULL);
}

static void perfing_setup(void) {
    int       ret;
    pthread_t perfing_thread;

    ret = pthread_create(&perfing_thread, NULL, perfing_main, NULL);
    if (ret == 0) {
        pthread_join(perfing_thread, NULL);
    } else {
        fprintf(stderr, "pthread_create failed %s\n", strerror(ret));
    }
}

static void perfed_setup(void) {
    int       ret;
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);
    CPU_SET(perfed_cpu, &cpu_set);

    ret = sched_setaffinity(0, sizeof(cpu_set), &cpu_set);
    if (ret == 0) {
        struct perf_event_attr perf_attrs;

        memset(&perf_attrs, 0, sizeof(perf_attrs));
        perf_attrs.type           = INTEL_PT_TYPE;
        perf_attrs.size           = sizeof(struct perf_event_attr);
        perf_attrs.config         = INTEL_PT_CONFIG;
        perf_attrs.sample_freq    = 12000;
        perf_attrs.read_format    = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CPU;
#if 0
        perf_attrs.pinned         = 1;
        perf_attrs.exclusive      = 1;
        perf_attrs.exclude_kernel = 1;
        perf_attrs.exclude_hv     = 1;
        perf_attrs.exclude_idle   = 1;
        perf_attrs.freq           = 1;
        perf_attrs.precise_ip     = 3;
        perf_attrs.exclude_host   = 1;
#endif

        perfing_fd = syscall(SYS_perf_event_open,
                             &perf_attrs,
                             perfed_pid,
                             perfed_cpu,
                             -1,
                             PERF_FLAG_FD_CLOEXEC | PERF_FLAG_FD_NO_GROUP);
        if (perfing_fd != -1) {
            
            perfing_setup();
        } else {
            fprintf(stderr, "perf_event_open failed %s\n", strerror(errno));
        }
    } else {
        fprintf(stderr, "sched_setaffinity failed %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[ ]) {
    perfed_pid  = atoi(argv[ 1 ]);
    perfed_cpu  = atoi(argv[ 2 ]);
    perfing_cpu = atoi(argv[ 3 ]);

    perfed_setup();

    return 0;
}