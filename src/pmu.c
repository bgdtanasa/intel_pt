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

#if 0
#define PRINT_PMU
#endif

#define PMU_PDIST_PERIOD     (128llu)
#define PMU_PDIST_NO_PERIODS (32llu)

#define FIXED_PMU_INST_RETIRED_ANY       ((__u64) (0x00C0llu))

#define GEN_PMU_BR_INST_RETIRED_ALL_BRANCHES ((__u64) (0x00C4llu))
#define GEN_PMU_BR_INST_RETIRED_TAKEN_JCC    ((__u64) (0xFEC4llu))
#define GEN_PMU_BR_INST_RETIRED_CALL         ((__u64) (0xF9C4llu))
#define GEN_PMU_BR_MISP_RETIRED_ALL_BRANCHES ((__u64) (0x00C5llu))
#define GEN_PMU_BR_MISP_RETIRED_RETURN       ((__u64) (0xF7C5llu))

#define GEN_PMU_MEM_LOAD_UOPS_RETIRED_L1_HIT   ((__u64) (0x01D1llu))
#define GEN_PMU_MEM_LOAD_UOPS_RETIRED_L1_MISS  ((__u64) (0x08D1llu))
#define GEN_PMU_MEM_LOAD_UOPS_RETIRED_L2_HIT   ((__u64) (0x02D1llu))
#define GEN_PMU_MEM_LOAD_UOPS_RETIRED_L2_MISS  ((__u64) (0x10D1llu))
#define GEN_PMU_MEM_LOAD_UOPS_RETIRED_L3_HIT   ((__u64) (0x04D1llu))
#define GEN_PMU_MEM_LOAD_UOPS_RETIRED_DRAM_HIT ((__u64) (0x80D1llu))

typedef struct {
  __u64        config;
  unsigned int precise_ip;
  const char*  name;
} pmu_t;

static pmu_t pmus[ 64u ];

static const char* get_pmu_name(const __u64 config) {
  switch (config) {
    case FIXED_PMU_INST_RETIRED_ANY: {
      return "inst_retired_any";
    } break;

    case GEN_PMU_BR_INST_RETIRED_ALL_BRANCHES: {
      return "br_inst_retired_all_branches";
    } break;

    case GEN_PMU_BR_MISP_RETIRED_ALL_BRANCHES: {
      return "br_misp_retired_all_branches";
    } break;

    case GEN_PMU_MEM_LOAD_UOPS_RETIRED_L1_HIT: {
      return "mem_load_uops_retired_l1_hit";
    } break;

    case GEN_PMU_MEM_LOAD_UOPS_RETIRED_L1_MISS: {
      return "mem_load_uops_retired_l1_miss";
    } break;

    case GEN_PMU_MEM_LOAD_UOPS_RETIRED_L2_HIT: {
      return "mem_load_uops_retired_l2_hit";
    } break;

    case GEN_PMU_MEM_LOAD_UOPS_RETIRED_L2_MISS: {
      return "mem_load_uops_retired_l2_miss";
    } break;

    case GEN_PMU_MEM_LOAD_UOPS_RETIRED_L3_HIT: {
      return "mem_load_uops_retired_l3_hit";
    } break;

    case GEN_PMU_MEM_LOAD_UOPS_RETIRED_DRAM_HIT: {
      return "mem_load_uops_retired_dram_hit";
    } break;

    default: {
      return "unknown";
    } break;
  }
}

static void update_pmu(const __u64 config, const unsigned int precise_ip) {
  static unsigned int no_fix = 0u;
  static unsigned int no_gen = 0u;

  if (config == FIXED_PMU_INST_RETIRED_ANY) {
    pmus[ 32u + no_fix ] = (pmu_t) {
      .config     = config,
      .precise_ip = precise_ip,
      .name       = get_pmu_name(config)
    };
    no_fix++;
  } else {
    pmus[  0u + no_gen ] = (pmu_t) {
      .config     = config,
      .precise_ip = precise_ip,
      .name       = get_pmu_name(config)
    };
    no_gen++;
  }
}

static struct perf_event_mmap_page* mmap_pmu(const int pmu_fd) {
  void* pmu_mem = mmap(NULL,
                       4096,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED,
                       pmu_fd,
                       0);
  if (pmu_mem != MAP_FAILED) {
    return ((struct perf_event_mmap_page* ) (pmu_mem));
  } else {
    fprintf(stderr, "mmap failed %s\n", strerror(errno));
  }

  return NULL;
}

static int install_pmu(const __u64        config,
                       const unsigned int precise_ip,
                       const pid_t        perfed_pid,
                       const int          perfed_cpu,
                       const int          intel_pt_fd) {
  int                    pmu_fd;
  struct perf_event_attr perf_attrs;

  memset(&perf_attrs, 0, sizeof(perf_attrs));
  // 10 == /sys/devices/cpu_atom/type
  perf_attrs.type           = 10u; //PERF_TYPE_RAW;
  perf_attrs.size           = sizeof(struct perf_event_attr);
  perf_attrs.config         = config;
  perf_attrs.sample_period  = (precise_ip == 1u) ? (PMU_PDIST_NO_PERIODS * PMU_PDIST_PERIOD) : (2llu);
  perf_attrs.sample_type    = PERF_SAMPLE_IP   |
                              PERF_SAMPLE_TID  |
                              PERF_SAMPLE_TIME |
                              PERF_SAMPLE_CPU;
  perf_attrs.read_format    = PERF_FORMAT_ID;
  perf_attrs.disabled       = 1;
  perf_attrs.exclude_kernel = 1;
  perf_attrs.exclude_idle   = 1;
  perf_attrs.precise_ip     = (precise_ip == 1u) ? (3u) : (2u);
  perf_attrs.sample_id_all  = 1;
  perf_attrs.aux_output     = 1;

  pmu_fd = syscall(SYS_perf_event_open,
                   &perf_attrs,
                   perfed_pid,
                   perfed_cpu,
                   intel_pt_fd,
                   PERF_FLAG_FD_CLOEXEC);
  if (pmu_fd != -1) {
    struct perf_event_mmap_page* pmu_mem = mmap_pmu(pmu_fd);

    if (pmu_mem != NULL) {
      ioctl(pmu_fd, PERF_EVENT_IOC_RESET, 0);
      ioctl(pmu_fd, PERF_EVENT_IOC_ENABLE, 0);

      update_pmu(config, precise_ip);
      fprintf(stdout,
              "\t[%8d %3d] :: %04llX %08x :: %u -> %s\n",
              perfed_pid,
              perfed_cpu,
              config,
              ((unsigned int) (pmu_mem->index)),
              precise_ip,
              get_pmu_name(config));
    }
  } else {
    fprintf(stdout, "perf_event_open[ %04llX ] failed :: %d %s\n", config, errno, strerror(errno));
  }

  return pmu_fd;
}

void perfed_pmu(const pid_t perfed_pid, const int perfed_cpu, const int intel_pt_fd) {
  //return;
  int cnt_fd;

  fprintf(stdout, "====== PMU ======\n");
  cnt_fd = install_pmu(FIXED_PMU_INST_RETIRED_ANY,
                       1u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_BR_INST_RETIRED_ALL_BRANCHES,
                       1u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
#if 0
  cnt_fd = install_pmu(GEN_PMU_BR_MISP_RETIRED_ALL_BRANCHES,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_MEM_LOAD_UOPS_RETIRED_L1_HIT,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_MEM_LOAD_UOPS_RETIRED_L1_MISS,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_MEM_LOAD_UOPS_RETIRED_L2_HIT,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_MEM_LOAD_UOPS_RETIRED_L2_MISS,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_MEM_LOAD_UOPS_RETIRED_L3_HIT,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
  cnt_fd = install_pmu(GEN_PMU_MEM_LOAD_UOPS_RETIRED_DRAM_HIT,
                       0u,
                       perfed_pid,
                       perfed_cpu,
                       intel_pt_fd);
#endif
  fprintf(stdout, "====== PMU ======\n");

  (void) (cnt_fd);
}

unsigned int pmu_info(const unsigned long long int pmu_mask) {
  unsigned int n = 0u;

  for (unsigned long long int i = 0llu; i < 64llu; i++) {
    if (pmu_mask & (1llu << i)) {
      n += pmus[ i ].precise_ip;

#if defined(PRINT_PMU)
      fprintf(stdout, "BIP PMU[ %2llu ] = %20u %64.64s\n", i, pmus[ i ].precise_ip, pmus[ i ].name);
#endif
    }
  }

  return n;
}
