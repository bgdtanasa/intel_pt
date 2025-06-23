#include <emmintrin.h>
#include <immintrin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <cpuid.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/ioctl.h>

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>

#include "intel_pt.h"
#include "xed.h"
#include "proc.h"
#include "pmu.h"
#include "kmod.h"
#include "x_unwind.h"

//  /sys/bus/event_source/devices/intel_pt/type
#define INTEL_PT_TYPE              ((__u32) (12u))
// /sys/bus/event_source/devices/intel_pt/format/*
#define INTEL_PT_CONFIG_PT         ((__u64) (1llu <<  0llu))
#define INTEL_PT_CONFIG_CYC        ((__u64) (1llu <<  1llu))
#define INTEL_PT_CONFIG_PWR_EVT    ((__u64) (1llu <<  4llu))
#define INTEL_PT_CONFIG_FUP_ON_PTW ((__u64) (0llu <<  5llu))
#define INTEL_PT_CONFIG_MTC        ((__u64) (0llu <<  9llu))
#define INTEL_PT_CONFIG_TSC        ((__u64) (1llu << 10llu))
#define INTEL_PT_CONFIG_NORETCOMP  ((__u64) (1llu << 11llu))
#define INTEL_PT_CONFIG_PTW        ((__u64) (0llu << 12llu))
#define INTEL_PT_CONFIG_BRANCH     ((__u64) (1llu << 13llu))
#define INTEL_PT_CONFIG_MTC_PERIOD ((__u64) (0llu << 14llu))
#define INTEL_PT_CONFIG_CYC_THRESH ((__u64) (0llu << 19llu))
#define INTEL_PT_CONFIG_PSB_PERIOD ((__u64) (0llu << 24llu))
#define INTEL_PT_CONFIG            ((INTEL_PT_CONFIG_PT)         | \
                                    (INTEL_PT_CONFIG_CYC)        | \
                                    (INTEL_PT_CONFIG_FUP_ON_PTW) | \
                                    (INTEL_PT_CONFIG_MTC)        | \
                                    (INTEL_PT_CONFIG_TSC)        | \
                                    (INTEL_PT_CONFIG_NORETCOMP)  | \
                                    (INTEL_PT_CONFIG_PTW)        | \
                                    (INTEL_PT_CONFIG_BRANCH)     | \
                                    (INTEL_PT_CONFIG_MTC_PERIOD) | \
                                    (INTEL_PT_CONFIG_CYC_THRESH) | \
                                    (INTEL_PT_CONFIG_PSB_PERIOD))

#define ONE_KB   (1024llu)
#define ONE_MB   ((ONE_KB) * (ONE_KB))
#define N_KB(n)  ((n) * (ONE_KB))
#define N_MB(n)  ((n) * (ONE_MB))
#define ONE_PAGE (N_KB(4llu))

#define MMAP_DATA_NO_PAGES (12llu)
#define MMAP_SIZE          ((1llu + (1llu << (MMAP_DATA_NO_PAGES))) * (ONE_PAGE))
#define MMAP_AUX_NO_PAGES  (18llu)
#define MMAP_AUX_SIZE      ((1llu << (MMAP_AUX_NO_PAGES)) * (ONE_PAGE))

#define DATA_BUFFER_SIZE (N_MB(2llu))
#define AUX_BUFFER_SIZE  (N_MB(8llu))

#define AUX_MEMCPY_SSE  (0u)
#define AUX_MEMCPY_AVX  (1u)
#define AUX_MEMCPY      (AUX_MEMCPY_AVX)
#if (AUX_MEMCPY == AUX_MEMCPY_SSE)
    #define AUX_ALIGNMENT ((unsigned long long int) (sizeof(__m128i)))
#elif (AUX_MEMCPY == AUX_MEMCPY_AVX)
    #define AUX_ALIGNMENT ((unsigned long long int) (sizeof(__m256i)))
#else
#endif

#define IA32_PERFEVTSEL0              (0x0186llu)
#define IA32_PERFEVTSEL1              (0x0187llu)
#define IA32_PERFEVTSEL2              (0x0188llu)
#define IA32_PERFEVTSEL3              (0x0189llu)
#define IA32_FIXED_CTR0               (0x0309llu)
#define IA32_FIXED_CTR1               (0x030Allu)
#define IA32_FIXED_CTR2               (0x030Bllu)
#define IA32_PERF_CAPABILITIES        (0x0345llu)
#define IA32_FIXED_CTR_CTRL           (0x038Dllu)
#define IA32_PERF_GLOBAL_STATUS       (0x038Ellu)
#define IA32_PERF_GLOBAL_CTRL         (0x038Fllu)
#define IA32_PERF_GLOBAL_STATUS_RESET (0x0390llu)
#define IA32_PERF_GLOBAL_STATUS_SET   (0x0391llu)
#define IA32_PERF_GLOBAL_INUSE        (0x0392llu)
#define IA32_PEBS_ENABLE              (0x03F1llu)
#define IA32_RTIT_CTL                 (0x0570llu)
#define IA32_RTIT_STATUS              (0x0571llu)

#define MSR_RELOAD_FIXED_CTR0 (0x1309llu)
#define MSR_RELOAD_FIXED_CTR1 (0x130Allu)
#define MSR_RELOAD_FIXED_CTR2 (0x130Bllu)

#define MSR_RELOAD_PMC0 (0x14C1llu)
#define MSR_RELOAD_PMC1 (0x14C2llu)
#define MSR_RELOAD_PMC2 (0x14C3llu)
#define MSR_RELOAD_PMC3 (0x14C4llu)

typedef struct {
    __u32 pid;
    __u32 tid;
    __u64 time;
    __u32 cpu;
    __u32 res;
} sample_id_t;

typedef struct {
    __u64       id;
    __u64       lost;
    sample_id_t sample_id;
} perf_record_lost_t;

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
    perf_record_lost_t         record_lost;
    perf_record_aux_t          record_aux;
    perf_record_itrace_start_t record_itrace_start;
    perf_record_switch_t       record_switch;

    unsigned char              one_page[ ONE_PAGE ];
} perf_record_t __attribute__((aligned(4096)));

static pid_t perfed_pid;
static int   perfed_cpu;
#if 0
static int   perfed_msr_fd;
#endif
static int   perfing_cpu;
static int   perfing_fd;

static unsigned int          perfed_is_stopped;
static volatile unsigned int perfing_is_running;

static struct perf_event_mmap_page* perf_metadata;
static __u8*                        perf_data_buffer;
static __u8*                        perf_aux_buffer;
static struct perf_event_header     perf_header __attribute__((aligned(64)));
static __u8*                        data_header = ((__u8*) (&perf_header));
static __u8*                        data_buffer;
static __u8*                        aux_buffer;
static perf_record_t*               perf_record;

unsigned long long int tsc_hz;
unsigned long long int tsc_ratio;
unsigned long long int bus_hz;

static __u64 switch_ref;
static __u64 switch_in;
static __u64 switch_out;
static __u64 last_switch_in;
static __u64 last_switch_out;

#if 0
static void perfed_msr(void) {
    char fd_name[ 128u ];

    sprintf(&fd_name[ 0u ], "/dev/cpu/%d/msr", perfed_cpu);
    perfed_msr_fd = open(&fd_name[ 0u ], O_RDWR);
    if (perfed_msr_fd != -1) {
        unsigned long long int msr_val;

        fprintf(stdout, "perfed_msr = %2d %s\n", perfed_msr_fd, &fd_name[ 0u ]);

        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERFEVTSEL0) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERFEVTSEL0 = %016llx\n", msr_val);
            fprintf(stdout, "\tEVENT_SELECT = %16llx\n", (msr_val >>  0llu) & 0xFFllu);
            fprintf(stdout, "\tUMASK        = %16llx\n", (msr_val >>  8llu) & 0xFFllu);
            fprintf(stdout, "\tUSR          = %16llx\n", (msr_val >> 16llu) & 0x01llu);
            fprintf(stdout, "\tOS           = %16llx\n", (msr_val >> 17llu) & 0x01llu);
            fprintf(stdout, "\tEDGE         = %16llx\n", (msr_val >> 18llu) & 0x01llu);
            fprintf(stdout, "\tINT          = %16llx\n", (msr_val >> 20llu) & 0x01llu);
            fprintf(stdout, "\tANYTHREAD    = %16llx\n", (msr_val >> 21llu) & 0x01llu);
            fprintf(stdout, "\tENABLE       = %16llx\n", (msr_val >> 22llu) & 0x01llu);
            fprintf(stdout, "\tINVERT       = %16llx\n", (msr_val >> 23llu) & 0x01llu);
            fprintf(stdout, "\tCMASK        = %16llx\n", (msr_val >> 24llu) & 0xFFllu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERFEVTSEL1) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERFEVTSEL1 = %016llx\n", msr_val);
            fprintf(stdout, "\tEVENT_SELECT = %16llx\n", (msr_val >>  0llu) & 0xFFllu);
            fprintf(stdout, "\tUMASK        = %16llx\n", (msr_val >>  8llu) & 0xFFllu);
            fprintf(stdout, "\tUSR          = %16llx\n", (msr_val >> 16llu) & 0x01llu);
            fprintf(stdout, "\tOS           = %16llx\n", (msr_val >> 17llu) & 0x01llu);
            fprintf(stdout, "\tEDGE         = %16llx\n", (msr_val >> 18llu) & 0x01llu);
            fprintf(stdout, "\tINT          = %16llx\n", (msr_val >> 20llu) & 0x01llu);
            fprintf(stdout, "\tANYTHREAD    = %16llx\n", (msr_val >> 21llu) & 0x01llu);
            fprintf(stdout, "\tENABLE       = %16llx\n", (msr_val >> 22llu) & 0x01llu);
            fprintf(stdout, "\tINVERT       = %16llx\n", (msr_val >> 23llu) & 0x01llu);
            fprintf(stdout, "\tCMASK        = %16llx\n", (msr_val >> 24llu) & 0xFFllu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERFEVTSEL2) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERFEVTSEL2 = %016llx\n", msr_val);
            fprintf(stdout, "\tEVENT_SELECT = %16llx\n", (msr_val >>  0llu) & 0xFFllu);
            fprintf(stdout, "\tUMASK        = %16llx\n", (msr_val >>  8llu) & 0xFFllu);
            fprintf(stdout, "\tUSR          = %16llx\n", (msr_val >> 16llu) & 0x01llu);
            fprintf(stdout, "\tOS           = %16llx\n", (msr_val >> 17llu) & 0x01llu);
            fprintf(stdout, "\tEDGE         = %16llx\n", (msr_val >> 18llu) & 0x01llu);
            fprintf(stdout, "\tINT          = %16llx\n", (msr_val >> 20llu) & 0x01llu);
            fprintf(stdout, "\tANYTHREAD    = %16llx\n", (msr_val >> 21llu) & 0x01llu);
            fprintf(stdout, "\tENABLE       = %16llx\n", (msr_val >> 22llu) & 0x01llu);
            fprintf(stdout, "\tINVERT       = %16llx\n", (msr_val >> 23llu) & 0x01llu);
            fprintf(stdout, "\tCMASK        = %16llx\n", (msr_val >> 24llu) & 0xFFllu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERFEVTSEL3) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERFEVTSEL3 = %016llx\n", msr_val);
            fprintf(stdout, "\tEVENT_SELECT = %16llx\n", (msr_val >>  0llu) & 0xFFllu);
            fprintf(stdout, "\tUMASK        = %16llx\n", (msr_val >>  8llu) & 0xFFllu);
            fprintf(stdout, "\tUSR          = %16llx\n", (msr_val >> 16llu) & 0x01llu);
            fprintf(stdout, "\tOS           = %16llx\n", (msr_val >> 17llu) & 0x01llu);
            fprintf(stdout, "\tEDGE         = %16llx\n", (msr_val >> 18llu) & 0x01llu);
            fprintf(stdout, "\tINT          = %16llx\n", (msr_val >> 20llu) & 0x01llu);
            fprintf(stdout, "\tANYTHREAD    = %16llx\n", (msr_val >> 21llu) & 0x01llu);
            fprintf(stdout, "\tENABLE       = %16llx\n", (msr_val >> 22llu) & 0x01llu);
            fprintf(stdout, "\tINVERT       = %16llx\n", (msr_val >> 23llu) & 0x01llu);
            fprintf(stdout, "\tCMASK        = %16llx\n", (msr_val >> 24llu) & 0xFFllu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERF_CAPABILITIES) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERF_CAPABILITIES = %016llx\n", msr_val);
            fprintf(stdout, "\tLBR_FORMAT          = %16llx\n", (msr_val >>  0llu) & 0x2Fllu);
            fprintf(stdout, "\tPEBS_TRAP           = %16llx\n", (msr_val >>  6llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_SAVE_ARCH_REGS = %16llx\n", (msr_val >>  7llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_RECORD_FORMAT  = %16llx\n", (msr_val >>  8llu) & 0x0Fllu);
            fprintf(stdout, "\tPEBS_BASELINE       = %16llx\n", (msr_val >> 14llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_OUTPUT_PT      = %16llx\n", (msr_val >> 16llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_TIMING_INFO    = %16llx\n", (msr_val >> 17llu) & 0x01llu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_FIXED_CTR_CTRL) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_FIXED_CTR_CTRL     = %016llx\n", msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERF_GLOBAL_STATUS) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERF_GLOBAL_STATUS = %016llx\n", msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERF_GLOBAL_CTRL) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERF_GLOBAL_CTRL   = %016llx\n", msr_val);
        }
        //IA32_PERF_GLOBAL_STATUS_RESET
        //IA32_PERF_GLOBAL_STATUS_SET
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PERF_GLOBAL_INUSE) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PERF_GLOBAL_INUSE  = %016llx\n", msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PEBS_ENABLE) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_PEBS_ENABLE = %016llx\n", msr_val);
            fprintf(stdout, "\tPEBS_EN_PMC0          = %16llx\n", (msr_val >>  0llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_EN_PMC1          = %16llx\n", (msr_val >>  1llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_EN_PMC2          = %16llx\n", (msr_val >>  2llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_EN_PMC3          = %16llx\n", (msr_val >>  3llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_EN_FIXED0        = %16llx\n", (msr_val >> 32llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_EN_FIXED1        = %16llx\n", (msr_val >> 33llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_EN_FIXED2        = %16llx\n", (msr_val >> 34llu) & 0x01llu);
            fprintf(stdout, "\tPMI_AFTER_EACH_RECORD = %16llx\n", (msr_val >> 60llu) & 0x01llu);
            fprintf(stdout, "\tPEBS_OUTPUT           = %16llx\n", (msr_val >> 61llu) & 0x03llu);

#if 0
            msr_val = (0x01llu << 61llu) | // PEBS_OUTPUT
                      (0x01llu << 60llu) | // PMI_AFTER_EACH_RECORD
                      (0x00llu << 34llu) |
                      (0x00llu << 33llu) |
                      (0x01llu << 32llu) | // PEBS_EN_FIXED0
                      (0x00llu <<  3llu) |
                      (0x00llu <<  2llu) |
                      (0x00llu <<  1llu) |
                      (0x01llu <<  0llu);  // PEBS_EN_PMC0
            if (pwrite(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PEBS_ENABLE) == sizeof(msr_val)) {
                if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_PEBS_ENABLE) == sizeof(msr_val)) {
                    fprintf(stdout, "IA32_PEBS_ENABLE = %016llx\n", msr_val);
                    fprintf(stdout, "\tPEBS_EN_PMC0          = %16llx\n", (msr_val >>  0llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_EN_PMC1          = %16llx\n", (msr_val >>  1llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_EN_PMC2          = %16llx\n", (msr_val >>  2llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_EN_PMC3          = %16llx\n", (msr_val >>  3llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_EN_FIXED0        = %16llx\n", (msr_val >> 32llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_EN_FIXED1        = %16llx\n", (msr_val >> 33llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_EN_FIXED2        = %16llx\n", (msr_val >> 34llu) & 0x01llu);
                    fprintf(stdout, "\tPMI_AFTER_EACH_RECORD = %16llx\n", (msr_val >> 60llu) & 0x01llu);
                    fprintf(stdout, "\tPEBS_OUTPUT           = %16llx\n", (msr_val >> 61llu) & 0x03llu);
                }
            }
#endif
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), IA32_RTIT_CTL) == sizeof(msr_val)) {
            fprintf(stdout, "IA32_RTIT_CTL = %016llx\n", msr_val);
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
            fprintf(stdout, "IA32_RTIT_STATUS = %016llx\n", msr_val);
            fprintf(stdout, "\tFilterEn      = %16llx\n", (msr_val >>  0llu) & 0x00001llu);
            fprintf(stdout, "\tContexEn      = %16llx\n", (msr_val >>  1llu) & 0x00001llu);
            fprintf(stdout, "\tTriggerEn     = %16llx\n", (msr_val >>  2llu) & 0x00001llu);
            fprintf(stdout, "\tError         = %16llx\n", (msr_val >>  4llu) & 0x00001llu);
            fprintf(stdout, "\tStopped       = %16llx\n", (msr_val >>  5llu) & 0x00001llu);
            fprintf(stdout, "\tPendPSB       = %16llx\n", (msr_val >>  6llu) & 0x00001llu);
            fprintf(stdout, "\tPacketByteCnt = %16llu\n", (msr_val >> 32llu) & 0x1FFFFllu);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_FIXED_CTR0) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_FIXED_CTR0 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
#if 0
            msr_val = 0x0000FFFFFFFFFFFFllu - 127llu;
            if (pwrite(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_FIXED_CTR0) == sizeof(msr_val)) {
                if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_FIXED_CTR0) == sizeof(msr_val)) {
                    fprintf(stdout, "MSR_RELOAD_FIXED_CTR0 = %016llx\n", msr_val);
                }
            }
#endif
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_FIXED_CTR1) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_FIXED_CTR1 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_FIXED_CTR2) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_FIXED_CTR2 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_PMC0) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_PMC0 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
#if 0
            msr_val = 0x0000FFFFFFFFFFFFllu - 127llu;
            if (pwrite(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_PMC0) == sizeof(msr_val)) {
                if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_PMC0) == sizeof(msr_val)) {
                    fprintf(stdout, "MSR_RELOAD_PMC0 = %016llx\n", msr_val);
                }
            }
#endif
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_PMC1) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_PMC1 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_PMC2) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_PMC2 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
        }
        if (pread(perfed_msr_fd, &msr_val, sizeof(msr_val), MSR_RELOAD_PMC3) == sizeof(msr_val)) {
            fprintf(stdout, "MSR_RELOAD_PMC3 = %016llx :: %12llu\n", msr_val, 0x0000FFFFFFFFFFFFllu - msr_val);
        }
    } else {
        fprintf(stderr, "open failed %s\n", strerror(errno));
    }
}
#endif

static void perfing_flush(void) {
    if ((perfed_is_stopped == 1u) || (ptrace(PTRACE_ATTACH, perfed_pid, NULL, NULL) != -1)) {
        int status = 0;

        if ((perfed_is_stopped == 1u) || (waitpid(perfed_pid, &status, 0) == perfed_pid)) {
            if ((perfed_is_stopped == 1u) || (WIFSTOPPED(status))) {
                unsigned int no_retries = 0u;

                fprintf(stdout, "Flushing Intel PT %u ...\n", perfed_is_stopped);
                ioctl(perfing_fd, PERF_EVENT_IOC_DISABLE, 0);
                for (;;) {
                    const __u64 data_head = perf_metadata->data_head;
                    __u64       data_tail = __atomic_load_n(&perf_metadata->data_tail, __ATOMIC_ACQUIRE);

                    if (data_tail != data_head) {
                        perf_metadata->aux_tail = perf_metadata->aux_head;
                        __atomic_store_n(&perf_metadata->data_tail, data_head, __ATOMIC_RELEASE);

                        no_retries = 0u;
                    } else {
                        if (no_retries == 25u) {
                            break;
                        } else {
                            const struct timespec ts = {
                                .tv_sec  = 0,
                                .tv_nsec = 1000 * 1000
                            };

                            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
                        }
                        no_retries++;
                    }
                }
                intel_pt_reset();
                ioctl(perfing_fd, PERF_EVENT_IOC_ENABLE, 0);
                if (perfed_is_stopped == 1u) {
                    return;
                }

                ptrace(PTRACE_DETACH, perfed_pid, NULL, NULL);
            }
        }
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
        unsigned int no_record_lost   = 0u;
        unsigned int no_record_aux    = 0u;
        unsigned int no_record_switch = 0u;

        double aux_util     = 0.0f;
        double aux_util_avg = 0.0f;
        double switch_util  = 0.0f;

        const __u64  data_size      = perf_metadata->data_size;
        __u64        data_tail      = __atomic_load_n(&perf_metadata->data_tail, __ATOMIC_ACQUIRE);
        __u64        data_head;
        const __u64  aux_size       = perf_metadata->aux_size;
        const __u64  perf_header_sz = ((__u64) (sizeof(struct perf_event_header)));
        __u64        perf_record_sz = 0llu;
        const double tsc_factor     = ((double) (tsc_hz)) / 1e9;

        struct timespec a;
        struct timespec b;
        struct timespec c;
        signed long long int ts_0 = 0ll;
        signed long long int ts_1 = 0ll;
        signed long long int ts_2 = 0ll;

        perfing_is_running = 1u;
        fprintf(stdout,
                "perfing from cpu %2d via fd %2d :: pid %6d cpu %2d\n",
                perfing_cpu,
                perfing_fd,
                perfed_pid,
                perfed_cpu);

        // Stopping the perfed pid to load the kernel module.
        {
            int status;

            if (ptrace(PTRACE_ATTACH, perfed_pid, NULL, NULL) != -1) {
                const pid_t perfed_pid_wait = waitpid(perfed_pid, &status, 0);

                if (perfed_pid_wait == perfed_pid) {
                    if (WIFSTOPPED(status)) {
                        kmod_load(perfed_pid);
                        ptrace(PTRACE_DETACH, perfed_pid, NULL, NULL);

                        unwind_init(perfed_pid);
                    }
                } else {
                    fprintf(stderr, "waitpid failed :: %d vs %d\n", perfed_pid, perfed_pid_wait); for (;;) {}
                }
            }
        }

        (void) posix_memalign(((void**) (&data_buffer)), AUX_ALIGNMENT, DATA_BUFFER_SIZE * sizeof(__u8)); memset(data_buffer, 0x00, DATA_BUFFER_SIZE * sizeof(__u8));
        (void) posix_memalign(((void**) (&aux_buffer)),  AUX_ALIGNMENT, AUX_BUFFER_SIZE * sizeof(__u8));  memset(aux_buffer,  0x00, AUX_BUFFER_SIZE * sizeof(__u8));

        perf_record = ((perf_record_t*) (data_buffer));
        ioctl(perfing_fd, PERF_EVENT_IOC_ENABLE, 0);
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &a);

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
                    __atomic_store_n(&perf_metadata->data_tail, data_tail, __ATOMIC_RELEASE);

                    switch (perf_header.type) {
                        case PERF_RECORD_LOST: {
                            no_record_lost++;
                            fprintf(stdout,
                                    "  RECORD_LOST :: %12llu %12llu :: %6u\n",
                                    perf_record->record_lost.id,
                                    perf_record->record_lost.lost,
                                    no_record_lost);
                        } break;

                        case PERF_RECORD_AUX: {
                            const __u64  aux_head      = __atomic_load_n(&perf_metadata->aux_head, __ATOMIC_ACQUIRE);
                            const __u64  aux_tail      = __atomic_load_n(&perf_metadata->aux_tail, __ATOMIC_ACQUIRE);
                            const __u64  rc_aux_offset = perf_record->record_aux.aux_offset;
                            const __u64  rc_aux_size   = perf_record->record_aux.aux_size;
                            const double aux_ratio     = ((double) (aux_head - (rc_aux_offset + rc_aux_size))) / ((double) (rc_aux_offset + rc_aux_size));

                            // Periodic unwinding by stopping the perfed pid
                            aux_util      = ((double) (aux_head - aux_tail)) / ((double) (aux_size));
                            aux_util_avg += aux_util;
                            if ((1)) {// && (aux_util_avg / ((double) (no_record_aux + 1u)) >= 0.01f) && (perfed_is_stopped == 0u)) {
                                long ret;
                                int  status;

                                errno = 0;
                                ret   = ptrace(PTRACE_ATTACH, perfed_pid, NULL, NULL);
                                if (ret == -1l) {
                                    fprintf(stderr, "PTRACE_ATTACH failed %s\n", strerror(errno));
                                } else {
                                    const pid_t perfed_pid_wait = waitpid(perfed_pid, &status, 0);

                                    if (perfed_pid_wait == perfed_pid) {
                                        if (WIFSTOPPED(status)) {
                                            struct user_regs_struct regs = { 0 };

                                            errno = 0;
                                            ret   = ptrace(PTRACE_GETREGS, perfed_pid, NULL, &regs);
                                            if (ret == -1l) {
                                                fprintf(stderr, "PTRACE_GETREGS failed %s\n", strerror(errno));
                                            } else {
                                                unwind(perfed_pid, perfed_cpu, &regs);
                                            }
                                            errno = 0;
                                            ret   = ptrace(PTRACE_DETACH, perfed_pid, NULL, NULL);
                                            if (ret == -1l) {
                                                fprintf(stderr, "PTRACE_DETACH failed %s\n", strerror(errno));
                                            }
                                            perfed_is_stopped = 1u;
                                            ioctl(perfing_fd, PERF_EVENT_IOC_DISABLE, 0);

                                            clock_gettime(CLOCK_MONOTONIC, &b);
                                            ts_0 = ((signed long long) (b.tv_sec - a.tv_sec)) * 1000000000ll + ((signed long long) (b.tv_nsec - a.tv_nsec));
                                            fprintf(stdout, "attach ts = %12lld ns\n", ts_0);
                                        }
                                    } else {
                                        fprintf(stderr, "waitpid failed :: %d vs %d\n", perfed_pid, perfed_pid_wait); for (;;) {}
                                    }
                                }
                            }

                            no_record_aux++;
                            fprintf(stdout,
                                    "   RECORD_AUX :: %3u %3u \e[0;31m%12llu\e[0m %04llx :: \e[0;31m%12.5lf %12.5lf %12.5lf\e[0m %6u\n",
                                    perf_record->record_aux.id.cpu,
                                    perf_record->record_aux.id.tid,
                                    rc_aux_size,
                                    perf_record->record_aux.flags,
                                    aux_util,
                                    aux_util_avg / ((double) (no_record_aux)),
                                    aux_ratio,
                                    no_record_aux);
                            if (no_record_aux >= 50u) {
                                aux_util_avg  = 0.0f;
                                no_record_aux = 0u;
                            }

                            if (perf_record->record_aux.flags == 0llu) {
                                __u64       j   = 0llu;
                                const __u64 j_a = AUX_ALIGNMENT * (rc_aux_size / AUX_ALIGNMENT);
                                const __u64 j_b = rc_aux_size % AUX_ALIGNMENT;

                                // Perform non-temporal copy
                                for (j = 0llu; j < j_a; j += AUX_ALIGNMENT) {
                                    const __u64 j_c = (j + rc_aux_offset) % aux_size;

                                    if (j_c + AUX_ALIGNMENT <= aux_size) {
#if (AUX_MEMCPY == AUX_MEMCPY_SSE)
                                        __m128i* dst = ((__m128i*) (&aux_buffer[ j ]));
                                        __m128i* src = ((__m128i*) (&perf_aux_buffer[ j_c ]));

                                        _mm_stream_si128(dst, _mm_loadu_si128(src));
#elif (AUX_MEMCPY == AUX_MEMCPY_AVX)
                                        __m256i* dst = ((__m256i*) (&aux_buffer[ j ]));
                                        __m256i* src = ((__m256i*) (&perf_aux_buffer[ j_c ]));

                                        _mm256_stream_si256(dst, _mm256_loadu_si256(src));
#else
#endif
                                    } else {
                                        for (__u64 k = 0llu; k < AUX_ALIGNMENT; k++) {
                                            aux_buffer[ j + k ] = perf_aux_buffer[ (j + k + rc_aux_offset) % aux_size ];
                                        }
                                    }
                                }
                                for (__u64 k = 0llu; k < j_b; k++) {
                                    aux_buffer[ j + k ] = perf_aux_buffer[ (j + k + rc_aux_offset) % aux_size ];
                                }
                                // Commit all non-temporal stores
                                _mm_sfence();

                                (void) intel_pt_decode(((unsigned char*) (&aux_buffer[ 0 ])),
                                                       ((unsigned long long int) (rc_aux_size)),
                                                       ((double) (perf_record->record_aux.id.time)) * tsc_factor);

                                __atomic_store_n(&perf_metadata->aux_tail, rc_aux_offset + rc_aux_size, __ATOMIC_RELEASE);
                            } else {
                                perfing_is_running = 2u;
                            }
                        } break;

                        case PERF_RECORD_ITRACE_START: {
                            fprintf(stdout, " ITRACE_START :: %12u %12u\n", perf_record->record_itrace_start.pid, perf_record->record_itrace_start.tid);
                        } break;

                        case PERF_RECORD_SWITCH: {
                            switch_ref = perf_record->record_switch.id.time;
                            if (no_record_switch == 0u) {
                                switch_in       = 0llu;
                                last_switch_out = switch_ref;
                                switch_out      = 0llu;
                                last_switch_in  = switch_ref;
                                switch_util = 0.0f;
                            } else {
                                if (perf_header.misc & PERF_RECORD_MISC_SWITCH_OUT) {
                                    switch_in       += switch_ref - last_switch_in;
                                    last_switch_out  = switch_ref;
                                } else {
                                    switch_out      += switch_ref - last_switch_out;
                                    last_switch_in   = switch_ref;
                                }
                                switch_util = ((double) (switch_in)) / ((double) (switch_in + switch_out));
                            }

                            no_record_switch++;
                            fprintf(stdout,
                                    "RECORD_SWITCH :: %3u %3u \e[0;31m%12.5lf\e[0m %14s :: %6u\n",
                                    perf_record->record_switch.id.cpu,
                                    perf_record->record_switch.id.tid,
                                    switch_util,
                                    (perf_header.misc & PERF_RECORD_MISC_SWITCH_OUT) ? ("ON  -> OFF") : ("OFF ->  ON"),
                                    no_record_switch);
                            if (no_record_switch >= 50u) {
                                no_record_switch = 0u;
                            }
                        } break;

                        default: {
                            fprintf(stdout, "RECORD_xxxxxx :: %2u\n", perf_header.type);
                        } break;
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &c);
                ts_1 = ((signed long long) (c.tv_sec - a.tv_sec)) * 1000000000ll + ((signed long long) (c.tv_nsec - a.tv_nsec));
                fprintf(stdout, "record ts = %12lld ns\n", ts_1);
            } else {
                if (perfed_is_stopped == 1u) {
                    perfed_is_stopped = 0u;
                    ioctl(perfing_fd, PERF_EVENT_IOC_ENABLE, 0);

                    clock_gettime(CLOCK_MONOTONIC, &c);
                    ts_2 = ((signed long long) (c.tv_sec - b.tv_sec)) * 1000000000ll + ((signed long long) (c.tv_nsec - b.tv_nsec));
                    fprintf(stdout, "detach ts = %12lld ms\n", ts_2 / 1000ll / 1000ll);
                }
            }

            if (perfing_is_running == 0u) {
                fprintf(stdout, "Shutting Intel PT ...\n");
                break;
            } else if (perfing_is_running == 2u) {
                fprintf(stdout, "Reseting Intel PT ...\n");

                perfing_flush();

                fprintf(stdout, "Enabling Intel PT ...\n");
                perfing_is_running = 1u;
            }
        }
    } else {
        fprintf(stderr, "pthread_setaffinity_np failed %s\n", strerror(ret));
    }

    unwind_close();
    kmod_unload();
    free(data_buffer);
    free(aux_buffer);
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
        //perf_attrs.sample_period  = 50000;
        perf_attrs.sample_freq    = 25000;
        perf_attrs.sample_type    = PERF_SAMPLE_IP   |
                                    PERF_SAMPLE_TID  |
                                    PERF_SAMPLE_TIME |
                                    PERF_SAMPLE_CPU;
        perf_attrs.read_format    = PERF_FORMAT_ID;
        perf_attrs.disabled       = 1;
        //perf_attrs.pinned         = 1;
        //perf_attrs.exclusive      = 1;
        perf_attrs.exclude_kernel = 1;
        //perf_attrs.exclude_hv     = 1;
        perf_attrs.exclude_idle   = 1;
        //perf_attrs.mmap           = 1;
        perf_attrs.freq           = 1;
        perf_attrs.precise_ip     = 3;
        perf_attrs.sample_id_all  = 1;
        //perf_attrs.exclude_host   = 1;
        //perf_attrs.exclude_guest   = 1;
        //perf_attrs.use_clockid    = 1;
        perf_attrs.context_switch = 1;
        //perf_attrs.aux_output     = 1;
        //perf_attrs.clockid        = CLOCK_MONOTONIC_RAW;

        perfing_fd = syscall(SYS_perf_event_open,
                             &perf_attrs,
                             perfed_pid,
                             perfed_cpu,
                             -1,
                             PERF_FLAG_FD_CLOEXEC);
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
#if 1
                    perfed_xed(perfed_pid);
                    perfed_proc(perfed_pid, NULL);
                    perfed_pmu(perfed_pid, perfed_cpu, perfing_fd);
                    //perfed_msr();

                    fprintf(stdout, "perf_metadata->data_head   = %10llu\n", perf_metadata->data_head);
                    fprintf(stdout, "perf_metadata->data_tail   = %10llu\n", perf_metadata->data_tail);
                    fprintf(stdout, "perf_metadata->data_offset = %10llu\n", perf_metadata->data_offset);
                    fprintf(stdout, "perf_metadata->data_size   = %10llu MB\n", perf_metadata->data_size / ONE_MB);
                    fprintf(stdout, "perf_metadata->aux_head    = %10llu\n", perf_metadata->aux_head);
                    fprintf(stdout, "perf_metadata->aux_tail    = %10llu\n", perf_metadata->aux_tail);
                    fprintf(stdout, "perf_metadata->aux_offset  = %10llu MB\n", perf_metadata->aux_offset / ONE_MB);
                    fprintf(stdout, "perf_metadata->aux_size    = %10llu MB\n", perf_metadata->aux_size / ONE_MB);
#endif

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

static void sig_action_SIGSEGV(int sig_no, siginfo_t* sig_info, void* u_ctx) {
    (void) sig_no;
    (void) sig_info;
    (void) u_ctx;
}

static void sig_action_SIGINT(int sig_no, siginfo_t* sig_info, void* u_ctx) {
    (void) sig_no;
    (void) sig_info;
    (void) u_ctx;

    perfing_is_running = 0u;
}

int main(int argc, char *argv[ ]) {
    struct sigaction sig_action;

    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_flags     = SA_SIGINFO;
    sig_action.sa_sigaction = &sig_action_SIGSEGV;
    sigaction(SIGSEGV, &sig_action, NULL);

    memset(&sig_action, 0, sizeof(sig_action));
    sig_action.sa_flags     = SA_SIGINFO;
    sig_action.sa_sigaction = &sig_action_SIGINT;
    sigaction(SIGINT, &sig_action, NULL);

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
            // Leaf 0AH
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x0Au, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 0AH :: %08x %08x %08x %08x :: vPMU = %u :: noGPMUs = %u %u :: noFPMUs = %u %u\n",
                    eax, ebx, ecx, edx,
                    (eax >>  0u) & 0xFFu,
                    (eax >>  8u) & 0xFFu,
                    (eax >> 16u) & 0xFFu,
                    (edx >>  0u) & 0x1Fu,
                    (edx >>  5u) & 0xFFu);
            // Leaf 14H :: ECX = 0
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid_count(0x14u, 0x00, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 14H :: %08x %08x %08x %08x\n",
                    eax, ebx, ecx, edx);
            fprintf(stdout, "\tCR3 Filtering Support = %02x\n", (ebx >> 0u) & 0x01u);
            fprintf(stdout, "\tConfigurable PSB      = %02x\n", (ebx >> 1u) & 0x01u);
            fprintf(stdout, "\tIP Filtering Support  = %02x\n", (ebx >> 2u) & 0x01u);
            fprintf(stdout, "\tMTC Support           = %02x\n", (ebx >> 3u) & 0x01u);
            fprintf(stdout, "\tPTWRITE Support       = %02x\n", (ebx >> 4u) & 0x01u);
            // Leaf 14H :: ECX = 1
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid_count(0x14u, 0x01, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 14H :: %08x %08x %08x %08x\n",
                    eax, ebx, ecx, edx);
            fprintf(stdout, "\tMTC Period Bitmap     = %04x\n", (eax >> 16u) & 0xFFFFu);
            fprintf(stdout, "\tCYC Threshold Bitmap  = %04x\n", (ebx >>  0u) & 0xFFFFu);
            fprintf(stdout, "\tPSB Period Bitmap     = %04x\n", (ebx >> 16u) & 0xFFFFu);
            // Leaf 15H
            eax = 0u; ebx = 0u; ecx = 0u; edx = 0u;
            __get_cpuid(0x15u, &eax, &ebx, &ecx, &edx);
            fprintf(stdout,
                    "CPUID 15H :: %08x %08x %08x %08x :: TSC       = %12u Hz :: %12.5lf\n",
                    eax, ebx, ecx, edx,
                    ecx * (ebx / eax),
                    ((double) (ecx * (ebx / eax))) / 1e9);
            tsc_ratio = ((unsigned long long int) (ebx / eax));
            tsc_hz    = ((unsigned long long int) (ecx)) * tsc_ratio;
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

            fprintf(stdout, "TSC = %12llu %12llu\n", tsc_hz, tsc_ratio);
            fprintf(stdout, "BUS = %12llu\n", bus_hz);
        }

        perfed_setup();
    }

    fprintf(stdout, "Done!\n");
    fflush(stdout);
    return 0;
}
