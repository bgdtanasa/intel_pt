#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>

#include "intel_pt.h"

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
#define INTEL_PT_CONFIG_BRANCH     ((__u64) (1llu << 13llu))
#define INTEL_PT_CONFIG            ((INTEL_PT_CONFIG_PT)         | \
                                    (INTEL_PT_CONFIG_CYC)        | \
                                    (INTEL_PT_CONFIG_FUP_ON_PTW) | \
                                    (INTEL_PT_CONFIG_TSC)        | \
                                    (INTEL_PT_CONFIG_NORETCOMP)  | \
                                    (INTEL_PT_CONFIG_PTW)        | \
                                    (INTEL_PT_CONFIG_BRANCH))

#define ONE_KB   (1024llu)
#define ONE_MB   ((ONE_KB) * (ONE_KB))
#define N_KB(n)  ((n) * (ONE_KB))
#define N_MB(n)  ((n) * (ONE_MB))
#define ONE_PAGE (N_KB(4llu))

#define MMAP_BUFFER_NO_PAGES (4llu)
#define MMAP_BUFFER_SIZE     ((1llu + (1llu << (MMAP_BUFFER_NO_PAGES))) * (ONE_PAGE))
#define MMAP_AUX_NO_PAGES    (12llu)
#define MMAP_AUX_SIZE        ((1llu << (MMAP_AUX_NO_PAGES)) * (ONE_PAGE))

typedef struct {
    __u32 pid;
    __u32 tid;
    __u32 cpu;
    __u32 res;
} sample_id_t;

typedef struct {
    __u64       aux_offset;
    __u64       aux_size;
    __u64       flags;
    sample_id_t id;
} perf_record_aux_t;

typedef struct {
    __u32 pid;
    __u32 tid;
} perf_record_itrace_start_t;

typedef union {
    perf_record_aux_t          aux;
    perf_record_itrace_start_t itrace_start;
} perf_record_t;

static pid_t perfed_pid;
static int   perfed_cpu;
static int   perfing_cpu;
static int   perfing_fd;

static struct perf_event_mmap_page* perf_metadata;
static __u8*                        perf_data_buffer;
static __u8*                        perf_aux_buffer;
static struct perf_event_header     perf_header;
static __u8*                        data_header       = ((__u8*) (&perf_header));
static __u8*                        data_buffer       = NULL;
static __u8*                        aux_buffer        = NULL;
static __u64                        aux_buffer_offset = 0llu;
static perf_record_t*               perf_record;

static void* perfing_main(void* args) {
    (void) (args);

    int       ret;
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);
    CPU_SET(perfing_cpu, &cpu_set);

    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    if (ret == 0) {
        const __u64 data_size      = perf_metadata->data_size;
        __u64       data_tail      = __atomic_load_n(&perf_metadata->data_tail, __ATOMIC_ACQUIRE);
        __u64       data_head;
        const __u64 aux_size       = perf_metadata->aux_size;
        const __u64 perf_header_sz = ((__u64) (sizeof(struct perf_event_header)));
        __u64       perf_record_sz = 0llu;
        __u64       no_aux_records = 0llu;

        fprintf(stdout,
                "perfing from cpu %2d via fd %2d :: pid %6d cpu %2d\n",
                perfing_cpu,
                perfing_fd,
                perfed_pid,
                perfed_cpu);

        data_buffer = (__u8*) malloc(N_MB(2llu) * sizeof(__u8));
        aux_buffer  = (__u8*) malloc(N_MB(8llu) * sizeof(__u8));
        perf_record = ((perf_record_t*) (data_buffer));
        for (;;) {
            data_head = __atomic_load_n(&perf_metadata->data_head, __ATOMIC_ACQUIRE);
            if (data_tail + perf_header_sz <= data_head) {
                // Replace this loop with non-temporal stores
                for (__u64 i = 0llu; i < perf_header_sz; i++) {
                    data_header[ i ] = perf_data_buffer[ (i + data_tail) % data_size ];
                }
                data_tail      += perf_header_sz;
                perf_record_sz  = ((__u64) (perf_header.size)) - perf_header_sz;
                if (data_tail + perf_record_sz <= data_head) {
                    // Replace this loop with non-temporal stores
                    for (__u64 i = 0llu; i < perf_record_sz; i++) {
                        data_buffer[ i ] = perf_data_buffer[ (i + data_tail) % data_size ];
                    }
                    data_tail += perf_record_sz;

                    switch (perf_header.type) {
                        case PERF_RECORD_AUX:
                            // Replace this loop with non-temporal stores
                            for (__u64 j = 0llu; j < perf_record->aux.aux_size; j++) {
                                aux_buffer[ j + aux_buffer_offset ] = perf_aux_buffer[ (j + perf_record->aux.aux_offset) % aux_size ];
                            }
                            __atomic_store_n(&perf_metadata->aux_tail,
                                             perf_record->aux.aux_offset + perf_record->aux.aux_size,
                                             __ATOMIC_RELEASE);

                            aux_buffer_offset = intel_pt_decode(((unsigned char*) (&aux_buffer[ 0 ])), ((unsigned long long int) (perf_record->aux.aux_size + aux_buffer_offset)));
                            no_aux_records++;
    
                            if (perf_record->aux.flags & PERF_AUX_FLAG_TRUNCATED) {
                                fprintf(stdout, "PERF_AUX_FLAG_TRUNCATED\n");
                            }
                            if ((no_aux_records % 10llu) == 0llu) {
                                fprintf(stdout, "no_aux_records = %12llu\n", no_aux_records);
                            }
                            //fflush(stdout);
                        break;

                        case PERF_RECORD_ITRACE_START:
                        break;

                        case PERF_RECORD_SWITCH:
                        break;

                        default:
                            fprintf(stdout, "%2u\n", perf_header.type);
                        break;
                    }
                }
            }
            __atomic_store_n(&perf_metadata->data_tail, data_tail, __ATOMIC_RELEASE);
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
        perf_attrs.sample_type    = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CPU;
        perf_attrs.read_format    = PERF_FORMAT_ID;
        perf_attrs.pinned         = 1;
        perf_attrs.exclusive      = 1;
        perf_attrs.exclude_kernel = 1;
        perf_attrs.exclude_hv     = 1;
        perf_attrs.exclude_idle   = 1;
        perf_attrs.mmap           = 1;
        perf_attrs.freq           = 1;
        perf_attrs.precise_ip     = 3;
        perf_attrs.sample_id_all  = 1;  
        //perf_attrs.exclude_host   = 1;
        //perf_attrs.exclude_guest   = 1;
        perf_attrs.context_switch = 1;

        perfing_fd = syscall(SYS_perf_event_open,
                             &perf_attrs,
                             perfed_pid,
                             perfed_cpu,
                             -1,
                             PERF_FLAG_FD_CLOEXEC | PERF_FLAG_FD_NO_GROUP);
        if (perfing_fd != -1) {
            void* p = mmap(NULL,
                           MMAP_BUFFER_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           perfing_fd,
                           0);
            if (p != MAP_FAILED) {
                perf_metadata = ((struct perf_event_mmap_page* ) (p));

                perf_metadata->aux_offset = perf_metadata->data_offset + perf_metadata->data_size;
                perf_metadata->aux_size   = MMAP_AUX_SIZE;
                p = mmap(NULL,
                         perf_metadata->aux_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         perfing_fd,
                         perf_metadata->aux_offset);
                if (p != MAP_FAILED) {
                    fprintf(stdout, "perf_metadata->data_head   = %10llu\n", perf_metadata->data_head);
                    fprintf(stdout, "perf_metadata->data_tail   = %10llu\n", perf_metadata->data_tail);
                    fprintf(stdout, "perf_metadata->data_offset = %10llu\n", perf_metadata->data_offset);
                    fprintf(stdout, "perf_metadata->data_size   = %10llu\n", perf_metadata->data_size);
                    fprintf(stdout, "perf_metadata->aux_head    = %10llu\n", perf_metadata->aux_head);
                    fprintf(stdout, "perf_metadata->aux_tail    = %10llu\n", perf_metadata->aux_tail);
                    fprintf(stdout, "perf_metadata->aux_offset  = %10llu\n", perf_metadata->aux_offset);
                    fprintf(stdout, "perf_metadata->aux_size    = %10llu\n", perf_metadata->aux_size);

                    perf_data_buffer = &((__u8*) (perf_metadata))[ perf_metadata->data_offset ];
                    perf_aux_buffer  = ((__u8*) (p));
                    perfing_setup();
                } else {
                    fprintf(stderr, "mmap failed %s\n", strerror(errno));
                }
            } else {
                fprintf(stderr, "mmap failed %s\n", strerror(errno));
            }
        } else {
            fprintf(stderr, "perf_event_open failed %s\n", strerror(errno));
        }
    } else {
        fprintf(stderr, "sched_setaffinity failed %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[ ]) {
    if (argc >= 4) {
        perfed_pid  = atoi(argv[ 1 ]);
        perfed_cpu  = atoi(argv[ 2 ]);
        perfing_cpu = atoi(argv[ 3 ]);

        perfed_setup();
    }

    return 0;
}