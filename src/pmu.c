#include "pmu.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

#define FIXED_PMU_INST_RETIRED_ANY           ((__u64) (0x00C0llu))

#define GEN_PMU_BR_INST_RETIRED_ALL_BRANCHES ((__u64) (0x00C4llu))
#define GEN_PMU_BR_INST_RETIRED_TAKEN_JCC    ((__u64) (0xFEC4llu))
#define GEN_PMU_BR_INST_RETIRED_CALL         ((__u64) (0xF9C4llu))
#define GEN_PMU_BR_MISP_RETIRED_ALL_BRANCHES ((__u64) (0x00C5llu))
#define GEN_PMU_BR_MISP_RETIRED_RETURN       ((__u64) (0xF7C5llu))

static struct perf_event_mmap_page* mmap_cnt(const int cnt_fd) {
    void* cnt_mem = mmap(NULL,
                         4096,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         cnt_fd,
                         0);
    if (cnt_mem != MAP_FAILED) {
        return ((struct perf_event_mmap_page* ) (cnt_mem));
    } else {
        fprintf(stderr, "mmap failed %s\n", strerror(errno));
    }

    return NULL;
}

static int install_cnt(const __u64        config,
                       const unsigned int precise_ip,
                       const pid_t        perfed_pid,
                       const int          perfed_cpu,
                       const int          intel_pt_fd) {
    int                    cnt_fd;
    struct perf_event_attr perf_attrs;

    memset(&perf_attrs, 0, sizeof(perf_attrs));
    // 10 == /sys/devices/cpu_atom/type
    perf_attrs.type           = 10; //PERF_TYPE_RAW;
    perf_attrs.size           = sizeof(struct perf_event_attr);
    perf_attrs.config         = config;
    perf_attrs.sample_period  = 128;
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
    perf_attrs.precise_ip     = (precise_ip == 1u) ? (3u) : (0u);
    perf_attrs.sample_id_all  = 1;  
    //perf_attrs.exclude_host   = 1;
    //perf_attrs.exclude_guest  = 1;
    //perf_attrs.use_clockid    = 1;
    perf_attrs.aux_output     = (precise_ip == 1u) ? (1u) : (0u);

    cnt_fd = syscall(SYS_perf_event_open,
                     &perf_attrs,
                     perfed_pid,
                     perfed_cpu,
                     intel_pt_fd,
                     PERF_FLAG_FD_CLOEXEC);
    if (cnt_fd != -1) {
        struct perf_event_mmap_page* cnt_mem = mmap_cnt(cnt_fd);

        if (cnt_mem != NULL) {
            ioctl(cnt_fd, PERF_EVENT_IOC_RESET, 0);
            ioctl(cnt_fd, PERF_EVENT_IOC_ENABLE, 0);

            fprintf(stdout,
                    "\t[%8d %3d] :: %04llX %08x :: %u\n",
                    perfed_pid,
                    perfed_cpu,
                    config,
                    ((unsigned int) (cnt_mem->index)),
                    precise_ip);
        }
    } else {
        fprintf(stdout, "perf_event_open[ %04llX ] failed :: %d %s\n", config, errno, strerror(errno));
    }

    return cnt_fd;
}

void perfed_pmu(const pid_t perfed_pid, const int perfed_cpu, const int intel_pt_fd) {
    int cnt_fd;

    fprintf(stdout, "====== PMU ======\n");
    cnt_fd = install_cnt(FIXED_PMU_INST_RETIRED_ANY,
                         1u,
                         perfed_pid,
                         perfed_cpu,
                         intel_pt_fd);
    cnt_fd = install_cnt(GEN_PMU_BR_INST_RETIRED_ALL_BRANCHES,
                         1u,
                         perfed_pid,
                         perfed_cpu,
                         intel_pt_fd);
    fprintf(stdout, "====== PMU ======\n");

    (void) (cnt_fd);
}
