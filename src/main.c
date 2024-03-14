#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <cpuid.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>

#include "intel_pt.h"

//  /sys/bus/event_source/devices/intel_pt/type
#define INTEL_PT_TYPE              ((__u32) (9u))
// /sys/bus/event_source/devices/intel_pt/format/*
#define INTEL_PT_CONFIG_PT         ((__u64) (1llu <<  0llu))
#define INTEL_PT_CONFIG_CYC        ((__u64) (1llu <<  1llu))
#define INTEL_PT_CONFIG_PWR_EVT    ((__u64) (1llu <<  4llu))
#define INTEL_PT_CONFIG_FUP_ON_PTW ((__u64) (1llu <<  5llu))
#define INTEL_PT_CONFIG_TSC        ((__u64) (1llu << 10llu))
#define INTEL_PT_CONFIG_NORETCOMP  ((__u64) (1llu << 11llu))
#define INTEL_PT_CONFIG_PTW        ((__u64) (1llu << 12llu))
#define INTEL_PT_CONFIG_BRANCH     ((__u64) (0llu << 13llu))
#define INTEL_PT_CONFIG_PSB_PERIOD ((__u64) (0llu << 24llu))
#define INTEL_PT_CONFIG            ((INTEL_PT_CONFIG_PT)         | \
                                    (INTEL_PT_CONFIG_CYC)        | \
                                    (INTEL_PT_CONFIG_FUP_ON_PTW) | \
                                    (INTEL_PT_CONFIG_TSC)        | \
                                    (INTEL_PT_CONFIG_NORETCOMP)  | \
                                    (INTEL_PT_CONFIG_PTW)        | \
                                    (INTEL_PT_CONFIG_BRANCH)     | \
                                    (INTEL_PT_CONFIG_PSB_PERIOD))

#define ONE_KB   (1024llu)
#define ONE_MB   ((ONE_KB) * (ONE_KB))
#define N_KB(n)  ((n) * (ONE_KB))
#define N_MB(n)  ((n) * (ONE_MB))
#define ONE_PAGE (N_KB(4llu))

#define MMAP_DATA_NO_PAGES (4llu)
#define MMAP_SIZE          ((1llu + (1llu << (MMAP_DATA_NO_PAGES))) * (ONE_PAGE))
#define MMAP_AUX_NO_PAGES  (12llu)
#define MMAP_AUX_SIZE      ((1llu << (MMAP_AUX_NO_PAGES)) * (ONE_PAGE))

#define DATA_BUFFER_SIZE (N_MB(2llu))
#define AUX_BUFFER_SIZE  (N_MB(8llu))

#define IA32_PERF_CAPABILITIES (0x0345llu)
#define IA32_PEBS_ENABLE       (0x03F1llu)
#define IA32_RTIT_CTL          (0x0570llu)
#define IA32_RTIT_STATUS       (0x0571llu)

typedef struct {
    __u32 pid;
    __u32 tid;
    __u64 time;
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

typedef struct {
    sample_id_t id;
} perf_record_switch_t;

typedef union {
    perf_record_aux_t          record_aux;
    perf_record_itrace_start_t record_itrace_start;
    perf_record_switch_t       record_switch;
} perf_record_t;

static pid_t perfed_pid;
static int   perfed_cpu;
static int   perfed_msr_fd;
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

unsigned long long int tsc_hz;
unsigned long long int bus_hz;

static __u64 last_switch_out;
static __u64 last_switch_in;

static void perfed_msr(void) {
    char fd_name[ 128u ];

    sprintf(&fd_name[ 0u ], "/dev/cpu/%d/msr", perfed_cpu);
    perfed_msr_fd = open(&fd_name[ 0u ], O_RDONLY);
    if (perfed_msr_fd != -1) {
        unsigned long long int msr_val;

        fprintf(stdout, "perfed_msr_fd %d for %s\n", perfed_msr_fd, &fd_name[ 0u ]);
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERF_CAPABILITIES) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERF_CAPABILITIES = %016llx\n", msr_val);
            fprintf(stdout, "\tLBR format     = %16llx\n", (msr_val >>  0llu) & 0x1Fllu);
            fprintf(stdout, "\tPEBS to PT     = %16llx\n", (msr_val >> 16llu) & 0x01llu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PEBS_ENABLE) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PEBS_ENABLE       = %016llx\n", msr_val);
            fprintf(stdout, "\tPEBS output    = %16llx\n", (msr_val >> 61llu) & 0x03llu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_RTIT_CTL) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_RTIT_CTL          = %016llx\n", msr_val);
            fprintf(stdout, "\tTraceEn        = %16llx\n", (msr_val >>  0llu) & 0x01llu);
            fprintf(stdout, "\tCYCEn          = %16llx\n", (msr_val >>  1llu) & 0x01llu);
            fprintf(stdout, "\tOS             = %16llx\n", (msr_val >>  2llu) & 0x01llu);
            fprintf(stdout, "\tUser           = %16llx\n", (msr_val >>  3llu) & 0x01llu);
            fprintf(stdout, "\tCR3Filter      = %16llx\n", (msr_val >>  7llu) & 0x01llu);
            fprintf(stdout, "\tToPA           = %16llx\n", (msr_val >>  8llu) & 0x01llu);
            fprintf(stdout, "\tMTCEn          = %16llx\n", (msr_val >>  9llu) & 0x01llu);
            fprintf(stdout, "\tTSCEn          = %16llx\n", (msr_val >> 10llu) & 0x01llu);
            fprintf(stdout, "\tDisRETC        = %16llx\n", (msr_val >> 11llu) & 0x01llu);
            fprintf(stdout, "\tBranchEn       = %16llx\n", (msr_val >> 13llu) & 0x01llu);
            fprintf(stdout, "\tMTCFreq        = %16llx\n", (msr_val >> 14llu) & 0x0Fllu);
            fprintf(stdout, "\tCycThresh      = %16llx\n", (msr_val >> 19llu) & 0x0Fllu);
            fprintf(stdout, "\tPSBFreq        = %16llx\n", (msr_val >> 24llu) & 0x0Fllu);
            fprintf(stdout, "\tInjectPsb      = %16llx\n", (msr_val >> 56llu) & 0x01llu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_RTIT_STATUS) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_RTIT_STATUS       = %016llx\n", msr_val);
            fprintf(stdout, "\tFilterEn       = %16llx\n", (msr_val >>  0llu) & 0x00001llu);
            fprintf(stdout, "\tContexEn       = %16llx\n", (msr_val >>  1llu) & 0x00001llu);
            fprintf(stdout, "\tTriggerEn      = %16llx\n", (msr_val >>  2llu) & 0x00001llu);
            fprintf(stdout, "\tError          = %16llx\n", (msr_val >>  4llu) & 0x00001llu);
            fprintf(stdout, "\tStopped        = %16llx\n", (msr_val >>  5llu) & 0x00001llu);
            fprintf(stdout, "\tPendPSB        = %16llx\n", (msr_val >>  6llu) & 0x00001llu);
            fprintf(stdout, "\tPacketByteCnt  = %16llu\n", (msr_val >> 32llu) & 0x1FFFFllu);
        }
    } else {
        fprintf(stderr, "open failed %s\n", strerror(errno));
    }
}

static void* perfing_main(void* args) {
    (void) (args);

    int       ret;
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);
    CPU_SET(perfing_cpu, &cpu_set);

    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    if (ret == 0) {
        const __u64  data_size      = perf_metadata->data_size;
        __u64        data_tail      = __atomic_load_n(&perf_metadata->data_tail, __ATOMIC_ACQUIRE);
        __u64        data_head;
        const __u64  aux_size       = perf_metadata->aux_size;
        const __u64  perf_header_sz = ((__u64) (sizeof(struct perf_event_header)));
        __u64        perf_record_sz = 0llu;
        const double tsc_factor     = ((double) (tsc_hz)) / 1e9;

        fprintf(stdout,
                "perfing from cpu %2d via fd %2d :: pid %6d cpu %2d\n",
                perfing_cpu,
                perfing_fd,
                perfed_pid,
                perfed_cpu);

        data_buffer = (__u8*) malloc(DATA_BUFFER_SIZE * sizeof(__u8));
        aux_buffer  = (__u8*) malloc(AUX_BUFFER_SIZE * sizeof(__u8));
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

#if 1
                    {
                        unsigned long long int msr_val = 0llu;

#if 1
                        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_RTIT_CTL) == sizeof(msr_val)) {
                            fprintf(stdout, "\tTraceEn       = %16llx\n", (msr_val >>  0llu) & 0x01llu);
                        }
#endif
                        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_RTIT_STATUS) == sizeof(msr_val)) {
                            fprintf(stdout, "\tPacketByteCnt = %16llu\n", (msr_val >> 32llu) & 0x1FFFFllu);
                        }
                    }
#endif
                    switch (perf_header.type) {
                        case PERF_RECORD_AUX:
                            fprintf(stdout,
                                    "   RECORD_AUX :: %3u %3u %24llu %12.5lf :: %24llu\n",
                                    perf_record->record_aux.id.cpu,
                                    perf_record->record_aux.id.tid,
                                    perf_record->record_aux.id.time,
                                    ((double) (perf_record->record_aux.id.time)) * tsc_factor,
                                    aux_buffer_offset + perf_record->record_aux.aux_size);

                            if (aux_buffer_offset + perf_record->record_aux.aux_size > AUX_BUFFER_SIZE) {
                                fprintf(stderr, "aux_buffer is too small!\n");
                                for (;;) {}
                            }
                            // Replace this loop with non-temporal stores
                            for (__u64 j = 0llu; j < perf_record->record_aux.aux_size; j++) {
                                aux_buffer[ j + aux_buffer_offset ] = perf_aux_buffer[ (j + perf_record->record_aux.aux_offset) % aux_size ];
                            }
                            __atomic_store_n(&perf_metadata->aux_tail,
                                             perf_record->record_aux.aux_offset + perf_record->record_aux.aux_size,
                                             __ATOMIC_RELEASE);

                            aux_buffer_offset = intel_pt_decode(((unsigned char*) (&aux_buffer[ 0 ])),
                                                                ((unsigned long long int) (perf_record->record_aux.aux_size + aux_buffer_offset)),
                                                                ((double) (perf_record->record_aux.id.time)) * tsc_factor);
    
                            if (perf_record->record_aux.flags & PERF_AUX_FLAG_TRUNCATED) {
                                fprintf(stdout, "PERF_AUX_FLAG_TRUNCATED\n");
                            }
                        break;

                        case PERF_RECORD_ITRACE_START:
                            fprintf(stdout, " ITRACE_START\n");
                        break;

                        case PERF_RECORD_SWITCH:
                            fprintf(stdout,
                                    "RECORD_SWITCH :: %3u %3u %24llu %12.5lf :: %24llu %12s\n",
                                    perf_record->record_switch.id.cpu,
                                    perf_record->record_switch.id.tid,
                                    perf_record->record_switch.id.time,
                                    ((double) (perf_record->record_switch.id.time)) * tsc_factor,
                                    (perf_header.misc & PERF_RECORD_MISC_SWITCH_OUT) ? 
                                        (last_switch_in  != 0llu) ? (perf_record->record_switch.id.time - last_switch_in)  : (0llu) :
                                        (last_switch_out != 0llu) ? (perf_record->record_switch.id.time - last_switch_out) : (0llu),
                                    (perf_header.misc & PERF_RECORD_MISC_SWITCH_OUT) ? " on cpu :: OFF" : "off cpu ::  ON");
                            if (perf_header.misc & PERF_RECORD_MISC_SWITCH_OUT) {
                                last_switch_out = perf_record->record_switch.id.time;
                            } else {
                                last_switch_in  = perf_record->record_switch.id.time;
                            }
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
        perf_attrs.sample_freq    = 25000;
        perf_attrs.sample_type    = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU;
        perf_attrs.read_format    = PERF_FORMAT_ID;
        perf_attrs.pinned         = 1;
        perf_attrs.exclusive      = 1;
        //perf_attrs.exclude_kernel = 1;
        //perf_attrs.exclude_hv     = 1;
        //perf_attrs.exclude_idle   = 1;
        perf_attrs.mmap           = 1;
        perf_attrs.freq           = 1;
        //perf_attrs.precise_ip     = 3;
        perf_attrs.sample_id_all  = 1;  
        //perf_attrs.exclude_host   = 1;
        //perf_attrs.exclude_guest   = 1;
        perf_attrs.use_clockid    = 1;
        perf_attrs.context_switch = 1;
        perf_attrs.clockid        = CLOCK_MONOTONIC_RAW;

        perfing_fd = syscall(SYS_perf_event_open,
                             &perf_attrs,
                             perfed_pid,
                             perfed_cpu,
                             -1,
                             PERF_FLAG_FD_CLOEXEC | PERF_FLAG_FD_NO_GROUP);
        if (perfing_fd != -1) {
            void* p = mmap(NULL,
                           MMAP_SIZE,
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
                    perfed_msr();

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

        {
            unsigned int eax;
            unsigned int ebx;
            unsigned int ecx;
            unsigned int edx;

            // Leaf 00H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x00u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 00H :: %08x %.4s%.4s%.4s\n",
                    eax, ((char*) (&ebx)), ((char*) (&edx)), ((char*) (&ecx)));
            // Leaf 01H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x01u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 01H :: %08x %08x %08x %08x :: %4u %4u %4u\n",
                    eax, ebx, ecx, edx,
                    (eax >> 0u) & 0x0Fu,                                    // Stepping
                    (((eax >> 16u) & 0x0Fu) << 4u) + ((eax >> 4u) & 0x0Fu), // Model
                    ((eax >> 20u) & 0xFFu) + ((eax >> 8u) & 0x0Fu));        // Family
            // Leaf 80000002H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x80000002u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 0x80000002u :: %.4s %.4s%.4s%.4s\n",
                    ((char*) (&eax)), ((char*) (&ebx)), ((char*) (&ecx)), ((char*) (&edx)));
            // Leaf 80000003H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x80000003u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 0x80000003u :: %.4s %.4s%.4s%.4s\n",
                    ((char*) (&eax)), ((char*) (&ebx)), ((char*) (&ecx)), ((char*) (&edx)));
            // Leaf 80000004H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x80000004u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 0x80000004u :: %.4s %.4s%.4s%.4s\n",
                    ((char*) (&eax)), ((char*) (&ebx)), ((char*) (&ecx)), ((char*) (&edx)));
            // Leaf 07H :: ECX = 0
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid_count(0x07u, 0x00, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 07H :: %08x %08x %08x %08x :: %s\n",
                    eax, ebx, ecx, edx,
                    (ebx & (1u << 25u)) ? "Intel_PT" : "");
            // Leaf 14H :: ECX = 0
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid_count(0x14u, 0x00, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 14H :: %08x %08x %08x %08x\n",
                    eax, ebx, ecx, edx);
            // Leaf 14H :: ECX = 1
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid_count(0x14u, 0x01, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 14H :: %08x %08x %08x %08x\n",
                    eax, ebx, ecx, edx);
            // Leaf 15H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x15u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 15H :: %08x %08x %08x %08x :: TSC       = %12u Hz :: %12.5lf\n",
                    eax, ebx, ecx, edx,
                    ecx * (ebx / eax),
                    ((double) (ecx * (ebx / eax))) / 1e9);
            tsc_hz = ((unsigned long long int) (ecx * (ebx / eax)));
            // Leaf 16H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x16u, &eax, &ebx, &ecx, &edx);
                        fprintf(stdout,
                    "CPUID 16H :: %08x %08x %08x %08x :: Base Freq = %12u MHz Max Freq = %12u MHz Bus Freq = %12u MHz\n",
                    eax, ebx, ecx, edx,
                    eax & 0xFFFFu,
                    ebx & 0xFFFFu,
                    ecx & 0xFFFFu);
            bus_hz = ((unsigned long long int) (ecx & 0xFFFFu)) * 1000000llu;

            fprintf(stdout, "TSC = %12llu\n", tsc_hz);
            fprintf(stdout, "BUS = %12llu\n", bus_hz);
        }

        perfed_setup();
    }

    return 0;
}
