#include "intel_pt.h"
#include "pmu.h"
#include "xed.h"
#include "proc.h"
#if defined(EN_PTRACE_UNWIND)
#include "x_unwind.h"
#endif

#include <stdio.h>
#include <string.h>

#include <sys/ioctl.h>

#include <linux/perf_event.h>

#define PSB       (0x82028202u) // 1000_0010_0000_0010_1000_0010_0000_0010
#define MNT       (0x88C302u)   //           1000_1000_1100_0011_0000_0010
#define OVF       (0xF302u)     //                     1111_0011_0000_0010
#define VMCS      (0xC802u)     //                     1100_1000_0000_0010
#define MWAIT     (0xC202u)     //                     1100_0010_0000_0010
#define LONG_TNT  (0xA302u)     //                     1010_0011_0000_0010
#define PWRX      (0xA202u)     //                     1010_0010_0000_0010
#define TRACESTOP (0x8302u)     //                     1000_0011_0000_0010
#define TSC_MTC   (0x7302u)     //                     0111_0011_0000_0010
#define BBP       (0x6302u)     //                     0110_0011_0000_0010
#define EVD       (0x5302u)     //                     0101_0011_0000_0010
#define PIP       (0x4302u)     //                     0100_0011_0000_0010
#define PSBEND    (0x2302u)     //                     0010_0011_0000_0010
#define PWRE      (0x2202u)     //                     0010_0010_0000_0010
#define CFE       (0x1302u)     //                     0001_0011_0000_0010
#define CBR       (0x0302u)     //                     0000_0011_0000_0010
#define EXSTOP    (0x6202u)     //                      110_0010_0000_0010
#define BEP       (0x3302u)     //                      011_0011_0000_0010
#define PTW       (0x1202u)     //                        1_0010_0000_0010
#define MODE      (0x99u)       //                               1001_1001
#define MTC       (0x59u)       //                               0101_1001
#define TSC       (0x19u)       //                               0001_1001
#define PAD       (0x00u)       //                               0000_0000
#define FUP       (0x1Du)       //                                  1_1101
#define TIP_PGE   (0x11u)       //                                  1_0001
#define TIP       (0x0Du)       //                                  0_1101
#define TIP_PGD   (0x01u)       //                                  0_0001
#define BIP       (0x04u)       //                                     100
#define CYC       (0x03u)       //                                      11
#define SHORT_TNT (0x00u)       //                               1xxx_xxx0

#define MNT_MASK       (0xFFFFFFu)
#define EXSTOP_MASK    (0x7FFFu)
#define BEP_MASK       (0x7FFFu)
#define PTW_MASK       (0x1FFFu)
#define FUP_MASK       (0x1Fu)
#define TIP_PGE_MASK   (0x1Fu)
#define TIP_MASK       (0x1Fu)
#define TIP_PGD_MASK   (0x1Fu)
#define BIP_MASK       (0x07u)
#define CYC_MASK       (0x03u)
#define SHORT_TNT_MASK (0x01u)

#define PRINT_FORMAT "%10s :: "
#if 0
#define PRINT_PT
#endif

#define INTEL_PT_PKT_HISTORY (64u)

typedef enum {
  INTEL_PT_PKT_NONE,
  INTEL_PT_PKT_PSB,
  INTEL_PT_PKT_MNT,
  INTEL_PT_PKT_OVF,
  INTEL_PT_PKT_VMCS,
  INTEL_PT_PKT_MWAIT,
  INTEL_PT_PKT_LONG_TNT,
  INTEL_PT_PKT_PWRX,
  INTEL_PT_PKT_TRACESTOP,
  INTEL_PT_PKT_TSC_MTC,
  INTEL_PT_PKT_BBP,
  INTEL_PT_PKT_EVD,
  INTEL_PT_PKT_PIP,
  INTEL_PT_PKT_PSBEND,
  INTEL_PT_PKT_PWRE,
  INTEL_PT_PKT_CFE,
  INTEL_PT_PKT_CBR,
  INTEL_PT_PKT_EXSTOP,
  INTEL_PT_PKT_BEP,
  INTEL_PT_PKT_PTW,
  INTEL_PT_PKT_MODE_EXEC,
  INTEL_PT_PKT_MODE_TSX,
  INTEL_PT_PKT_MTC,
  INTEL_PT_PKT_TSC,
  INTEL_PT_PKT_PAD,
  INTEL_PT_PKT_FUP,
  INTEL_PT_PKT_TIP_PGE,
  INTEL_PT_PKT_TIP,
  INTEL_PT_PKT_TIP_PGD,
  INTEL_PT_PKT_BIP,
  INTEL_PT_PKT_CYC,
  INTEL_PT_PKT_SHORT_TNT,

  INTEL_PT_PKT_FUP_OVF,
  INTEL_PT_PKT_FUP_BEP,
  INTEL_PT_PKT_FUP_PTW,
  INTEL_PT_PKT_FUP_TIP,
} intel_pt_pkt_type_t;

typedef struct {
  intel_pt_pkt_type_t      type;
  union {
    unsigned long long int val;
    unsigned long long int ptw; // INTEL_PT_PKT_PTW
    unsigned long long int ctc; // INTEL_PT_PKT_TSC_MTC, INTEL_PT_PKT_MTC
    unsigned long long int tsc; // INTEL_PT_PKT_TSC
    unsigned long long int ip;  // INTEL_PT_PKT_FUP, INTEL_PT_PKT_TIP_PGE, INTEL_PT_PKT_TIP, INTEL_PT_PKT_TIP_PGD
  } v;
  unsigned long long int   cyc_cnt;
  double                   tsc_approx;

#if defined(AUX_DBG)
  const volatile unsigned char* x;
  unsigned char                 y;
  unsigned long long int        n;
#endif
} intel_pt_pkt_t;

static const char*           intel_pt_pkt_names[ ] = {
  [ INTEL_PT_PKT_PSB ]       = "PSB",
  [ INTEL_PT_PKT_MNT ]       = "MNT",
  [ INTEL_PT_PKT_OVF ]       = "OVF",
  [ INTEL_PT_PKT_VMCS ]      = "VMCS",
  [ INTEL_PT_PKT_MWAIT ]     = "MWAIT",
  [ INTEL_PT_PKT_LONG_TNT ]  = "LONG_TNT",
  [ INTEL_PT_PKT_PWRX ]      = "PWRX",
  [ INTEL_PT_PKT_TRACESTOP ] = "TRACESTOP",
  [ INTEL_PT_PKT_TSC_MTC ]   = "TSC_MTC",
  [ INTEL_PT_PKT_BBP ]       = "BBP",
  [ INTEL_PT_PKT_EVD ]       = "EVD",
  [ INTEL_PT_PKT_PIP ]       = "PIP",
  [ INTEL_PT_PKT_PSBEND ]    = "PSBEND",
  [ INTEL_PT_PKT_PWRE ]      = "PWRE",
  [ INTEL_PT_PKT_CFE ]       = "CFE",
  [ INTEL_PT_PKT_CBR ]       = "CBR",
  [ INTEL_PT_PKT_EXSTOP ]    = "EXSTOP",
  [ INTEL_PT_PKT_BEP ]       = "BEP",
  [ INTEL_PT_PKT_PTW ]       = "PTW",
  [ INTEL_PT_PKT_MODE_EXEC ] = "MODE_EXEC",
  [ INTEL_PT_PKT_MODE_TSX ]  = "MODE_TSX",
  [ INTEL_PT_PKT_MTC ]       = "MTC",
  [ INTEL_PT_PKT_TSC ]       = "TSC",
  [ INTEL_PT_PKT_PAD ]       = "PAD",
  [ INTEL_PT_PKT_FUP ]       = "FUP",
  [ INTEL_PT_PKT_TIP_PGE ]   = "TIP_PGE",
  [ INTEL_PT_PKT_TIP ]       = "TIP",
  [ INTEL_PT_PKT_TIP_PGD ]   = "TIP_PGD",
  [ INTEL_PT_PKT_BIP ]       = "BIP",
  [ INTEL_PT_PKT_CYC ]       = "CYC",
  [ INTEL_PT_PKT_SHORT_TNT ] = "SHORT_TNT",

  [ INTEL_PT_PKT_FUP_OVF ]   = "FUP_OVF",
  [ INTEL_PT_PKT_FUP_BEP ]   = "FUP_BEP",
  [ INTEL_PT_PKT_FUP_PTW ]   = "FUP_PTW",
  [ INTEL_PT_PKT_FUP_TIP ]   = "FUP_TIP"
};
static intel_pt_pkt_t        intel_pt_pkt_hist[ INTEL_PT_PKT_HISTORY ];
static unsigned int          intel_pt_pkt_hist_idx;
static intel_pt_pkt_t*       last_intel_pt_pkt;
static const intel_pt_pkt_t* last_intel_pt_pkt_ovf;
static const intel_pt_pkt_t* last_intel_pt_pkt_ptw;

static unsigned long long int last_ip;

static unsigned int           last_psb;
static unsigned int           last_bbp;
static unsigned int           last_bbp_type;
static unsigned int           last_bbp_sz;
static unsigned long long int bip_ip;
static unsigned long long int bip_pmu_mask;
static unsigned long long int bip_tsc;
static unsigned long long int bip_mem_access_addr;
static unsigned long long int bip_mem_aux_info;
static unsigned long long int bip_mem_access_lat;
static unsigned long long int bip_tsx_aux_info;
static unsigned int           last_bep_fup;

static unsigned int           last_cfe_fup;
static unsigned int           last_cfe_type;
static unsigned int           last_cfe_vct;

static unsigned int           last_ptw_fup;

static unsigned int           tsc_approx_en;
static double                 tsc_approx_ctc;
static unsigned long long int tsc_approx_cyc;
static unsigned int           tsc_approx_ovf;
static double                 tsc_approx_ref;
static double                 tsc_approx_err;
static double                 tsc_approx_ref_prev;
static double                 tsc_ref;
static unsigned long long int tsc_ref_ctc_ovf;
static unsigned long long int tsc_ref_ctc;
static unsigned long long int tsc_ref_fc;
static unsigned long long int tsc_ref_cyc_cnt;
extern unsigned long long int tsc_adj;
extern unsigned long long int tsc_hz;
extern unsigned long long int tsc_ratio;
extern unsigned long long int base_hz;
extern unsigned long long int bus_hz;
static unsigned long long int cbr;

static unsigned long long int cyc_cnt_ref;

double tsc_factor;
double base_factor;
double cbr_factor;

intel_pt_status_t intel_pt_status = INTEL_PT_STATUS_DISABLE;

static inline __attribute__((always_inline)) unsigned int is_cyc_eligible(const intel_pt_pkt_type_t type) {
  return ((type == INTEL_PT_PKT_OVF)       ||
          (type == INTEL_PT_PKT_VMCS)      ||
          (type == INTEL_PT_PKT_LONG_TNT)  ||
          (type == INTEL_PT_PKT_PIP)       ||
          (type == INTEL_PT_PKT_EXSTOP)    ||
          (type == INTEL_PT_PKT_PTW)       ||
          (type == INTEL_PT_PKT_MODE_EXEC) ||
          (type == INTEL_PT_PKT_MODE_TSX)  ||
          (type == INTEL_PT_PKT_MTC)       ||
          (type == INTEL_PT_PKT_TSC)       ||
          (type == INTEL_PT_PKT_TIP_PGE)   ||
          (type == INTEL_PT_PKT_TIP)       ||
          (type == INTEL_PT_PKT_TIP_PGD)   ||
          (type == INTEL_PT_PKT_SHORT_TNT)) ? (1u) : (0u);
}

static inline __attribute__((always_inline)) unsigned int is_timing_event(const intel_pt_pkt_type_t type) {
  return ((type == INTEL_PT_PKT_CBR) ||
          (type == INTEL_PT_PKT_MTC) ||
          (type == INTEL_PT_PKT_TSC) ||
          (type == INTEL_PT_PKT_CYC)) ? (1u) : (0u);
}

static unsigned int is_ovf_event(const unsigned long long int ip) {
  (void) (ip);

  last_intel_pt_pkt_ovf = NULL;
  for (unsigned int i = 1u; i < INTEL_PT_PKT_HISTORY; i++) {
    const unsigned int j = (INTEL_PT_PKT_HISTORY + intel_pt_pkt_hist_idx - i) % INTEL_PT_PKT_HISTORY;

    if ((i == 1u) && (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_FUP)) {
      continue;
    }

    if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_NONE) {
      return 0u;
    } else if ((is_timing_event(intel_pt_pkt_hist[ j ].type) == 1u) ||
               (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_PSB)    ||
               (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_PSBEND)) {
      continue;
    } else if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_OVF) {
      last_intel_pt_pkt_ovf = &intel_pt_pkt_hist[ j ];
      return 1u;
    } else {
      return 0u;
    }
  }
  return 0u;
}

static unsigned int is_async_event(const unsigned long long int ip) {
  (void) (ip);

  for (unsigned int i = 1u; i < INTEL_PT_PKT_HISTORY; i++) {
    const unsigned int j = (INTEL_PT_PKT_HISTORY + intel_pt_pkt_hist_idx - i) % INTEL_PT_PKT_HISTORY;

    if ((i == 1u) && (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_TIP)) {
      continue;
    }

    if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_NONE) {
      return 0u;
    } else if ((is_timing_event(intel_pt_pkt_hist[ j ].type) == 1u) ||
               (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_VMCS)   ||
               (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_PIP)    ||
               (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_MODE_EXEC)) {
      continue;
    } else if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_FUP) {
      intel_pt_pkt_hist[ j ].type = INTEL_PT_PKT_FUP_TIP;
      return 1u;
    } else {
      return 0u;
    }
  }
  return 0u;
}

static void print_intel_pt_bytes(const volatile unsigned char* const x,
                                 const unsigned long long int        n,
                                 const unsigned long long int        n_orig) {
  fprintf(stdout, "n = %16llu %16llu\n", n_orig, n);
  for (unsigned long long int i = (((n_orig - n) >= 48llu) ? (48llu) : (n_orig - n)); i >= 1llu; i--) {
    const signed long long int j = ((signed long long int) (i));

    fprintf(stdout, "%02x ", ((unsigned int) (x[ 0ll - j ])));
  }
  fprintf(stdout, "\n");
  for (unsigned long long int i = 0llu; i < ((n >= 32llu) ? (32llu) : (n)); i++) {
    fprintf(stdout, "%02x ", ((unsigned int) (x[ 0llu + i ])));
  }
  fprintf(stdout, "\n");
}

static void print_intel_pt_pkts(void) {
   for (unsigned int i = 1u; i < INTEL_PT_PKT_HISTORY; i++) {
    const unsigned int j = (INTEL_PT_PKT_HISTORY + intel_pt_pkt_hist_idx - i) % INTEL_PT_PKT_HISTORY;

    if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_NONE) {
      return;
    } else {
#if defined(AUX_DBG)
      fprintf(stdout,
              "%4u " PRINT_FORMAT "%16llx %20llu %20.2lf :: %16llx %02x %02x %12llu\n",
              j,
              intel_pt_pkt_names[ intel_pt_pkt_hist[ j ].type ],
              intel_pt_pkt_hist[ j ].v.val,
              intel_pt_pkt_hist[ j ].cyc_cnt,
              intel_pt_pkt_hist[ j ].tsc_approx,
              ((unsigned long long int) (intel_pt_pkt_hist[ j ].x)),
              ((unsigned int) (intel_pt_pkt_hist[ j ].y)),
              ((unsigned int) (*intel_pt_pkt_hist[ j ].x)),
              intel_pt_pkt_hist[ j ].n);
#else
      fprintf(stdout,
              "%4u " PRINT_FORMAT "%16llx %20llu %20.2lf\n",
              j,
              intel_pt_pkt_names[ intel_pt_pkt_hist[ j ].type ],
              intel_pt_pkt_hist[ j ].v.val,
              intel_pt_pkt_hist[ j ].cyc_cnt,
              intel_pt_pkt_hist[ j ].tsc_approx);
#endif
    }
  }
}

static void reset_intel_pt_pkt(void) {
  memset(&intel_pt_pkt_hist[ 0u ], 0, sizeof(intel_pt_pkt_hist));
  intel_pt_pkt_hist_idx = 0u;
}

static const intel_pt_pkt_t* prev_intel_pt_pkt(const intel_pt_pkt_type_t type) {
  for (unsigned int i = 1u; i < INTEL_PT_PKT_HISTORY; i++) {
    const unsigned int j = (INTEL_PT_PKT_HISTORY + intel_pt_pkt_hist_idx - i) % INTEL_PT_PKT_HISTORY;

    if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_NONE) {
      return NULL;
    } else if (intel_pt_pkt_hist[ j ].type == type) {
      return &intel_pt_pkt_hist[ j ];
    }
  }
  return NULL;
}

static void record_intel_pt_pkt(intel_pt_pkt_type_t           type,
                                unsigned long long int        val,
                                const unsigned long long int  cyc_cnt,
#if defined(AUX_DBG)
                                const volatile unsigned char* x,
                                unsigned long long int        n) {
#else
                                const volatile unsigned char* x __attribute__((unused)),
                                unsigned long long int        n __attribute__((unused))) {
#endif
  const unsigned int a = ((is_cyc_eligible(type) == 1u) || (type == INTEL_PT_PKT_CYC)) ? (1u) : (0u);

  tsc_approx_ovf += (type == INTEL_PT_PKT_OVF) ? (1u) : (0u);

  if (type == INTEL_PT_PKT_TSC_MTC) {
    for (unsigned int i = 1u; i < INTEL_PT_PKT_HISTORY; i++) {
      const unsigned int j = (INTEL_PT_PKT_HISTORY + intel_pt_pkt_hist_idx - i) % INTEL_PT_PKT_HISTORY;

      if (intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_NONE) {
        break;
      } else if ((intel_pt_pkt_hist[ j ].type == INTEL_PT_PKT_MTC) && (intel_pt_pkt_hist[ j ].v.ctc == val)) {
        if (intel_pt_pkt_hist[ j ].tsc_approx > tsc_ref) {
#if defined(PRINT_PT)
          fprintf(stdout, "\e[0;31mTSC_REAL_ERR  = %20.2lf\e[0m\n", tsc_ref - intel_pt_pkt_hist[ j ].tsc_approx);
#endif

          xed_tsc_err(tsc_ref - intel_pt_pkt_hist[ j ].tsc_approx);
        }
        break;
      }
    }

    tsc_approx_en  = 1u;
    tsc_approx_ctc = 0llu;
    tsc_approx_cyc = 0llu;
    tsc_approx_ovf = 0u;
  } if ((type == INTEL_PT_PKT_MTC) && (tsc_approx_en == 1u)) {
    static unsigned long long int ctc_prev = 0llu;
    const unsigned long long int  ctc      = val;

    if (tsc_approx_ovf >= 1u) {
      if (ctc_prev < tsc_ref_ctc) {
        if ((ctc_prev >= ctc) || (ctc >= tsc_ref_ctc)) {
          tsc_ref_ctc_ovf += 1llu;
        }
      } else if (ctc_prev == tsc_ref_ctc) {
      } else if (ctc_prev > tsc_ref_ctc) {
        if ((tsc_ref_ctc <= ctc) && (ctc <= ctc_prev)) {
          tsc_ref_ctc_ovf += 1llu;
        }
      } else {
      }
    } else {
      tsc_ref_ctc_ovf += (ctc == tsc_ref_ctc) ? (1llu) : (0llu);
    }

    tsc_approx_ctc  = ((double) ((((ctc - tsc_ref_ctc) & 0xFFllu) + (tsc_ref_ctc_ovf << 8llu)) * tsc_ratio));
    tsc_approx_cyc  = 0llu;
    tsc_approx_ovf  = 0u;

    tsc_ref_cyc_cnt = cyc_cnt;
    ctc_prev        = ctc;
  } else if (type == INTEL_PT_PKT_TSC) {
#if defined(EN_MTC)
    tsc_approx_en  = 0u;
#else
    tsc_approx_en  = 1u;
#endif
    tsc_approx_ctc = 0llu;
    tsc_approx_cyc = 0llu;
    tsc_approx_ovf = 0u;
  } else if ((a == 1u) && (tsc_approx_en == 1u)) {
    tsc_approx_cyc  += cyc_cnt - tsc_ref_cyc_cnt;
    tsc_ref_cyc_cnt  = cyc_cnt;
  }

  if (tsc_approx_en == 1u) {
    tsc_approx_ref      = (cbr_factor != 0.0f) ? (tsc_ref + tsc_approx_ctc + ((double) (tsc_approx_cyc * base_factor)) / ((double) (cbr_factor))) : (0.0f);
    tsc_approx_err      = (tsc_approx_ref < tsc_approx_ref_prev) ? (tsc_approx_ref - tsc_approx_ref_prev) : (0.0f);
    tsc_approx_ref_prev = tsc_approx_ref;

    if ((type == INTEL_PT_PKT_TSC) && (tsc_approx_err < 0.0f)) {
#if defined(PRINT_PT)
      fprintf(stdout, "\e[0;31mTSC_REAL_ERR  = %20.2lf\e[0m\n", tsc_approx_err);
#endif

      xed_tsc_err(tsc_approx_err);
    }
  } else {
    tsc_approx_ref      = 0.0f;
    tsc_approx_err      = 0.0f;
    tsc_approx_ref_prev = 0.0f;
  }

#if defined(PRINT_PT)
  fprintf(stdout,
          PRINT_FORMAT "%5u %16.2lf %16llu %20.2lf :: %16llu :: %16llx \e[0;31m%8.2lf\e[0m\n",
          intel_pt_pkt_names[ type ],
          intel_pt_pkt_hist_idx,
          ((a == 1u) && (tsc_approx_en == 1u)) ? (tsc_approx_ctc) : (0.0f),
          ((a == 1u) && (tsc_approx_en == 1u)) ? (tsc_approx_cyc) : (0llu),
          ((a == 1u) && (tsc_approx_en == 1u)) ? (tsc_approx_ref) : (0.0f),
          cyc_cnt_ref,
          val,
          tsc_approx_err);
#endif

  intel_pt_pkt_hist[ intel_pt_pkt_hist_idx ] = (intel_pt_pkt_t) {
    .type       = type,
    .v          = {
      .val      = val
    },
    .cyc_cnt    = (a == 1u) ? (cyc_cnt)        : (0llu),
    .tsc_approx = (a == 1u) ? (tsc_approx_ref) : (0llu),
#if defined(AUX_DBG)
    .x          = x,
    .y          = *x,
    .n          = n
#endif
  };
  last_intel_pt_pkt     = &intel_pt_pkt_hist[ intel_pt_pkt_hist_idx ];
  intel_pt_pkt_hist_idx = (intel_pt_pkt_hist_idx + 1u) % INTEL_PT_PKT_HISTORY;

  if (type == INTEL_PT_PKT_TIP_PGD) {
    reset_intel_pt_pkt();

    tsc_approx_en  = 0u;
    tsc_approx_ctc = 0llu;
    tsc_approx_cyc = 0llu;
    tsc_approx_ovf = 0u;
  }
}

static unsigned long long int ip_decode(const volatile unsigned char** x,
                                        unsigned long long int*        n,
#if defined(AUX_DBG)
                                        const unsigned long long int   n_orig,
#else
                                        const unsigned long long int   n_orig __attribute__((unused)),
#endif
                                        const intel_pt_pkt_type_t      pkt_type) {
  const volatile unsigned char* x_p = *x;
  unsigned long long int        n_p = *n;

  unsigned long long int ip             = 0xFFFFFFFFFFFFFFFFllu;
  const unsigned char    ip_bytes       = (((unsigned int) (x_p[ 0u ])) >> 5u) & 0x07u;
  const unsigned char    ip_compressed  = ((ip_bytes == 3u) || ((ip_bytes == 6u))) ? (0u) : (1u);
  unsigned char          last_ip_update = 1u;

  (void) (ip_compressed);

  record_intel_pt_pkt(pkt_type, ip, cyc_cnt_ref, x_p, n_p);
  x_p++;
  n_p--;
  switch (ip_bytes) {
    case 0u:
      ip = 0llu;
      last_ip_update = 0u;
    break;

    case 1u:
      if (n_p >= 2llu) {
        ip = (last_ip & 0xFFFFFFFFFFFF0000llu)                |
             (((unsigned long long int) (x_p[ 1u ])) << 8llu) |
             (((unsigned long long int) (x_p[ 0u ])) << 0llu);
        x_p += 2llu;
        n_p -= 2llu;
      } else {
        last_ip_update = 2u;
      }
    break;

    case 2u:
      if (n_p >= 4llu) {
        ip = (last_ip & 0xFFFFFFFF00000000llu)                 |
             (((unsigned long long int) (x_p[ 3u ])) << 24llu) |
             (((unsigned long long int) (x_p[ 2u ])) << 16llu) |
             (((unsigned long long int) (x_p[ 1u ])) <<  8llu) |
             (((unsigned long long int) (x_p[ 0u ])) <<  0llu);
        x_p += 4llu;
        n_p -= 4llu;
      } else {
        last_ip_update = 2u;
      }
    break;

    case 3u:
      if (n_p >= 6llu) {
        ip = (((unsigned long long int) (x_p[ 5u ])) << 40llu) |
             (((unsigned long long int) (x_p[ 4u ])) << 32llu) |
             (((unsigned long long int) (x_p[ 3u ])) << 24llu) |
             (((unsigned long long int) (x_p[ 2u ])) << 16llu) |
             (((unsigned long long int) (x_p[ 1u ])) <<  8llu) |
             (((unsigned long long int) (x_p[ 0u ])) <<  0llu);
        {
          const unsigned long long int ip_47_bit = (ip >> 47llu) & 0x01llu;
          unsigned long long int       ip_bit    = 0llu;

          for (unsigned long long int i = 0llu; i < 16llu; i++) {
            ip_bit |= (ip_47_bit << i);
          }
          ip |= (ip_bit << 48llu);
        }
        x_p += 6llu;
        n_p -= 6llu;
      } else {
        last_ip_update = 2u;
      }
    break;

    case 4u:
      if (n_p >= 6llu) {
        ip = (last_ip & 0xFFFF000000000000llu)                 |
             (((unsigned long long int) (x_p[ 5u ])) << 40llu) |
             (((unsigned long long int) (x_p[ 4u ])) << 32llu) |
             (((unsigned long long int) (x_p[ 3u ])) << 24llu) |
             (((unsigned long long int) (x_p[ 2u ])) << 16llu) |
             (((unsigned long long int) (x_p[ 1u ])) <<  8llu) |
             (((unsigned long long int) (x_p[ 0u ])) <<  0llu);
        x_p += 6llu;
        n_p -= 6llu;
      } else {
        last_ip_update = 2u;
      }
    break;

    case 5u:
      last_ip_update = 2u;
    break;

    case 6u:
      if (n_p >= 8llu) {
        ip = (((unsigned long long int) (x_p[ 7u ])) << 56llu) |
             (((unsigned long long int) (x_p[ 6u ])) << 48llu) |
             (((unsigned long long int) (x_p[ 5u ])) << 40llu) |
             (((unsigned long long int) (x_p[ 4u ])) << 32llu) |
             (((unsigned long long int) (x_p[ 3u ])) << 24llu) |
             (((unsigned long long int) (x_p[ 2u ])) << 16llu) |
             (((unsigned long long int) (x_p[ 1u ])) <<  8llu) |
             (((unsigned long long int) (x_p[ 0u ])) <<  0llu);
        x_p += 8llu;
        n_p -= 8llu;
      } else {
        last_ip_update = 2u;
      }
    break;

    case 7u:
      last_ip_update = 2u;
    break;

    default:
      last_ip_update = 2u;
    break;
  }
  if (last_ip_update == 0u) {
  } else if (last_ip_update == 1u) {
#if defined(AUX_DBG)
    if ((ip != 0llu) && (xed_unwind_find_inst(ip) == NULL)) {
      xed_close();

      print_intel_pt_bytes(x_p, n_p, n_orig);
      print_intel_pt_pkts();
      fprintf(stderr, "ip_decode error :: ip_bytes = %u ip = %16llx last_ip = %16llx\n", ip_bytes, ip, last_ip); for (;;) { }
    }
#endif
    last_intel_pt_pkt->v.ip = ip;

    last_ip = ip;
  } else if (last_ip_update == 2u) {
    fprintf(stderr, "ip_decode error :: 2 ip_bytes = %u n = %llu\n", ip_bytes, n_p); for (;;) { }
  }
  *x = x_p;
  *n = n_p;

#if defined(EN_PTRACE_UNWIND)
  {
    unwind_insts_t* const unwind_insts = &unwind_queue[ unwind_queue_tail ];

    if ((unwind_insts->no_insts >= 1u) && (unwind_insts->tsc != 0llu) && (tsc_approx_ref >= ((double) (unwind_insts->tsc)))) {
      xed_ptrace_unwind(unwind_insts);

      memset(unwind_insts, 0, sizeof(unwind_insts_t));
      unwind_queue_tail = (unwind_queue_tail + 1u) % UNWIND_QUEUE_LEN;
    }
  }
#endif

#if defined(PRINT_PT)
  fprintf(stdout, "IP            = %20llx\n", ip);
#endif
  if (ip != 0llu) {
    if (pkt_type == INTEL_PT_PKT_FUP) {
      if (is_ovf_event(ip) == 1u) {
        last_intel_pt_pkt->type = INTEL_PT_PKT_FUP_OVF;

        xed_intel_pt_ovf_fup(ip, last_intel_pt_pkt_ovf->tsc_approx, last_intel_pt_pkt_ovf->cyc_cnt);
        xed_async_reset(ip, last_intel_pt_pkt_ovf->tsc_approx, last_intel_pt_pkt_ovf->cyc_cnt);
        xed_update_last_inst(ip);
        xed_process_branches(0u, 0u, 0llu, last_intel_pt_pkt_ovf->tsc_approx, last_intel_pt_pkt_ovf->cyc_cnt);

#if defined(PRINT_PT)
        fprintf(stdout, "FUP OVF       = %20llx\n", ip);
#endif
      }
    } else if (pkt_type == INTEL_PT_PKT_TIP_PGE) {
      xed_intel_pt_tip_enable(ip, tsc_approx_ref, cyc_cnt_ref);
      xed_async_reset(ip, tsc_approx_ref, cyc_cnt_ref);
      xed_update_last_inst(ip);
      xed_process_branches(0u, 0u, 0llu, tsc_approx_ref, cyc_cnt_ref);
    } else if (pkt_type == INTEL_PT_PKT_TIP) {
      if (is_async_event(ip) == 1u) {
        xed_async_enter(ip, tsc_approx_ref, cyc_cnt_ref);
        xed_update_last_inst(ip);
        xed_process_branches(0u, 0u, 0llu, tsc_approx_ref, cyc_cnt_ref);
      } else {
        xed_process_branches(0u, 0u, ip, tsc_approx_ref, cyc_cnt_ref);
      }
    } else if (pkt_type == INTEL_PT_PKT_TIP_PGD) {
      fprintf(stderr, "ip_decode error :: 0 ip = %16llx pkt_type = %3d\n", ip, pkt_type); for (;;) { }
    } else {
      fprintf(stderr, "ip_decode error :: 1 ip = %16llx pkt_type = %3d\n", ip, pkt_type); for (;;) { }
    }
  } else {
    xed_intel_pt_tip_disable(tsc_approx_ref, cyc_cnt_ref);
    xed_async_reset(ip, tsc_approx_ref, cyc_cnt_ref);

    if (pkt_type != INTEL_PT_PKT_TIP_PGD) {
      fprintf(stderr, "ip_decode error :: 2 ip = %16llx pkt_type = %3d\n", ip, pkt_type); for (;;) { }
    }
  }

  return ip;
}

unsigned long long int intel_pt_decode(const volatile unsigned char*    x,
                                       unsigned long long int           n,
#if defined(AUX_DBG)
                                       const unsigned long long int     h,
                                       volatile unsigned long long int* h_p) {
#else
                                       const unsigned long long int     h __attribute__((unused)),
                                       volatile unsigned long long int* h_p __attribute__((unused))) {
#endif
#if defined(AUX_DBG)
  const volatile unsigned char* x_orig = x;
#endif
  const unsigned long long int  n_orig = n;

  const volatile unsigned int*       x_32;
  const volatile unsigned short int* x_16;
  const volatile unsigned char*      x_8;

  //for (unsigned long long int i = 0llu; i < n; i++) {
  //  fprintf(stdout, "%02x ", ((unsigned int) (x[ i ])));
  //}
  //fprintf(stdout, "\n");

decode_again:
  if (n >= 1llu) {
#if defined(AUX_DBG)
    if ((last_intel_pt_pkt != NULL) && (*last_intel_pt_pkt->x != last_intel_pt_pkt->y)) {
      fprintf(stdout,
              "%4u " PRINT_FORMAT "%16llx %20llu :: %16llx %02x %02x %12llu :: %12llu %12llu :: %12llu %12llu\n",
              (INTEL_PT_PKT_HISTORY + intel_pt_pkt_hist_idx - 1u) % INTEL_PT_PKT_HISTORY,
              intel_pt_pkt_names[ last_intel_pt_pkt->type ],
              last_intel_pt_pkt->v.val,
              last_intel_pt_pkt->cyc_cnt,
              ((unsigned long long int) (last_intel_pt_pkt->x)),
              ((unsigned int) (last_intel_pt_pkt->y)),
              ((unsigned int) (*last_intel_pt_pkt->x)),
              last_intel_pt_pkt->n,
              ((unsigned long long int) (last_intel_pt_pkt->x)) - ((unsigned long long int) (x_orig)),
              n_orig - n,
              h,
              __atomic_load_n(h_p, __ATOMIC_ACQUIRE));

      xed_close();

      print_intel_pt_bytes(x, n, n_orig);
      print_intel_pt_pkts();
      fprintf(stderr, "0 intel_pt_decode error\n"); for (;;) { }
    }
#endif

    x_32 = ((volatile unsigned int*) (x));
    x_16 = ((volatile unsigned short int*) (x));
    x_8  = ((volatile unsigned char*) (x));
    //fprintf(stdout, "n = %12llu\n", n);

    if ((n >= 4llu) && (*x_32 == PSB)) {
      last_ip = 0llu;
      last_psb++;

      record_intel_pt_pkt(INTEL_PT_PKT_PSB, last_psb, cyc_cnt_ref, x, n);
      x += 4llu;
      n -= 4llu;
      goto decode_again;
    }
    if ((n >= 11llu) && (((*x_32) & MNT_MASK) == MNT)) {
      record_intel_pt_pkt(INTEL_PT_PKT_MNT, 0llu, cyc_cnt_ref, x, n);
      x += 11llu;
      n -= 11llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == OVF)) {
      // An OVF always ends a PSB+
      last_psb = 0u;

      // An OVF always ends a BBP
      last_bbp      = 0u;
      last_bbp_type = 0xFFFFFFFFu;
      last_bbp_sz   = 0u;

      bip_ip       = 0llu;
      bip_pmu_mask = 0llu;
      bip_tsc      = 0llu;

      bip_mem_access_addr = 0llu;
      bip_mem_aux_info    = 0llu;
      bip_mem_access_lat  = 0llu;
      bip_tsx_aux_info    = 0llu;

      last_bep_fup = 0u;

      // An OVF always resets the async
      xed_async_reset(0llu, 0.0f, 0llu);

#if defined(PRINT_PT)
      fprintf(stdout, "     OVF      = %20u\n", 0u);
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_OVF, 0llu, cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == VMCS)) {
      record_intel_pt_pkt(INTEL_PT_PKT_VMCS, 0llu, cyc_cnt_ref, x, n);
      x += 7llu;
      n -= 7llu;
      goto decode_again;
    }
    if ((n >= 10llu) && (*x_16 == MWAIT)) {
      record_intel_pt_pkt(INTEL_PT_PKT_MWAIT, 0llu, cyc_cnt_ref, x, n);
      x += 10llu;
      n -= 10llu;
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_16 == LONG_TNT)) {
//#if defined(PRINT_PT)
      fprintf(stdout, "LONG_TNT      = %20u\n", 0u); for (;;) { }
//#endif

      record_intel_pt_pkt(INTEL_PT_PKT_LONG_TNT, 0llu, cyc_cnt_ref, x, n);
      x += 8llu;
      n -= 8llu;
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == PWRX)) {
      record_intel_pt_pkt(INTEL_PT_PKT_PWRX, 0llu, cyc_cnt_ref, x, n);
      x += 7llu;
      n -= 7llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == TRACESTOP)) {
      record_intel_pt_pkt(INTEL_PT_PKT_TRACESTOP, 0llu, cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == TSC_MTC)) {
      const unsigned long long int ctc = (((unsigned long long int) (x[ 3u ])) << 8llu) |
                                         (((unsigned long long int) (x[ 2u ])) << 0llu);
      const unsigned long long int fc  = ((((unsigned long long int) (x[ 6u ])) & 0x01llu) << 8llu) |
                                         ((((unsigned long long int) (x[ 5u ])) & 0xFFllu) << 0llu);
      const intel_pt_pkt_t* const  pkt = prev_intel_pt_pkt(INTEL_PT_PKT_TSC);

      if (pkt != NULL) {
        const unsigned long long int art = (pkt->v.tsc - tsc_adj - fc) / tsc_ratio;

        if ((art & 0xFFFFllu) == ctc) {
          tsc_ref         = ((double) (pkt->v.tsc));
          tsc_ref_ctc_ovf = 0llu;
          tsc_ref_ctc     = ctc & 0xFFllu;
          tsc_ref_fc      = fc;
          tsc_ref_cyc_cnt = pkt->cyc_cnt;

#if defined(PRINT_PT)
          fprintf(stdout, "FC            = %20llx\n", tsc_ref_fc);
          fprintf(stdout, "FC CYC_CNT    = %20llx\n", tsc_ref_cyc_cnt);
#endif
        } else {
          fprintf(stderr, "tsc_mtc error 0!\n"); for (;;) { }
        }
      } else {
          fprintf(stderr, "tsc_mtc error 1!\n"); for (;;) { }
      }

      if (pkt != NULL) {
        // INTEL_PT_PKT_TSC_MTC is in fact a INTEL_PT_PKT_MTC packet.
        // TSC_MTC it not cyc eligibale but the corresponding TSC is.
        // As such, this fake MTC packet gets the cyc_cnt of the TSC packet.
        record_intel_pt_pkt(INTEL_PT_PKT_TSC_MTC, tsc_ref_ctc, tsc_ref_cyc_cnt, x, n);
      }
      x += 7llu;
      n -= 7llu;
      goto decode_again;
    }
    if ((n >= 3llu) && (*x_16 == BBP)) {
      last_bbp      = 1u;
      last_bbp_type = (((unsigned int) (x[ 2u ])) >> 0u) & 0x1Fu;
      last_bbp_sz   = (((unsigned int) (x[ 2u ])) >> 7u) & 0x01u;

      if ((last_bbp_type != 0x04u) && (last_bbp_type != 0x05u)) {
        fprintf(stderr, "BBP TYPE      = %20u\n", last_bbp_type); for (;;) { }
      }
      if (last_bbp_sz == 1u) {
        fprintf(stderr, "BBP SZ        = %20u\n", last_bbp_sz); for (;;) { }
      }

#if defined(PRINT_PT)
      fprintf(stdout, "BBP TYPE      = %20u\n", last_bbp_type);
      fprintf(stdout, "BBP SZ        = %20u\n", last_bbp_sz);
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_BBP, ((unsigned long long int) (last_bbp_type)), cyc_cnt_ref, x, n);
      x += 3llu;
      n -= 3llu;
      goto decode_again;
    }
    if ((n >= 11llu) && (*x_16 == EVD)) {
      record_intel_pt_pkt(INTEL_PT_PKT_EVD, 0llu, cyc_cnt_ref, x, n);
      x += 11llu;
      n -= 11llu;
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_16 == PIP)) {
      const unsigned long long int cr3 = (((unsigned long long int) (x[ 7u ])) << 44llu) |
                                         (((unsigned long long int) (x[ 6u ])) << 36llu) |
                                         (((unsigned long long int) (x[ 5u ])) << 28llu) |
                                         (((unsigned long long int) (x[ 4u ])) << 20llu) |
                                         (((unsigned long long int) (x[ 3u ])) << 12llu) |
                                         (((unsigned long long int) (x[ 2u ])) <<  4llu);

#if defined(PRINT_PT)
      fprintf(stdout, "PIP           = %20llx\n", cr3);
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_PIP, cr3, cyc_cnt_ref, x, n);
      x += 8llu;
      n -= 8llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == PSBEND)) {
      if (last_psb != 4u) {
        fprintf(stderr, "PSB error %u\n", last_psb); print_intel_pt_pkts(); for (;;) {}
      }
      last_psb = 0u;

      record_intel_pt_pkt(INTEL_PT_PKT_PSBEND, 0llu, cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == PWRE)) {
      record_intel_pt_pkt(INTEL_PT_PKT_PWRE, 0llu, cyc_cnt_ref, x, n);
      x += 4llu;
      n -= 4llu;
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == CFE)) {
      last_cfe_fup  = (((unsigned int) (x[ 2u ])) >> 7u) & 0x01u;
      last_cfe_type = (((unsigned int) (x[ 2u ])) >> 0u) & 0x1Fu;
      last_cfe_vct  = ((unsigned int) (x[ 3u ]));

#if defined(PRINT_PT)
      fprintf(stdout, "CFE FUP       = %20x\n", last_cfe_fup);
      fprintf(stdout, "CFE TYPE      = %20x\n", last_cfe_type);
      fprintf(stdout, "CFE VECTOR    = %20x\n", last_cfe_vct);
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_CFE, 0llu, cyc_cnt_ref, x, n);
      x += 4llu;
      n -= 4llu;
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == CBR)) {
      cbr         = ((unsigned long long int) (x[ 2u ]));
      tsc_factor  = ((double) (tsc_hz)) / 1e9;
      base_factor = ((double) (base_hz)) / 1e9;
      cbr_factor  = ((double) (cbr * bus_hz)) / 1e9;

#if defined(PRINT_PT)
      fprintf(stdout, "CBR           = %20.2lf GHz\n", ((double) (cbr_factor)));
      fprintf(stdout, "1_MTC_TO_CYCS = %20.2lf\n", ((double) (tsc_ratio)) * (cbr_factor / tsc_factor));
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_CBR, cbr, cyc_cnt_ref, x, n);
      x += 4llu;
      n -= 4llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (((*x_16) & EXSTOP_MASK) == EXSTOP)) {
      record_intel_pt_pkt(INTEL_PT_PKT_EXSTOP, 0llu, cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (((*x_16) & BEP_MASK) == BEP)) {
      last_bep_fup = (((unsigned int) (x[ 1u ])) >> 7u) & 0x01u;

      last_bbp      = 0u;
      last_bbp_type = 0xFFFFFFFFu;
      last_bbp_sz   = 0u;

#if defined(PRINT_PT)
      fprintf(stdout, "BEP FUP       = %20u\n", last_bep_fup);
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_BEP, ((unsigned long long int) (last_bep_fup)), cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 10llu) && (((*x_16) & PTW_MASK) == PTW)) {
      const unsigned long long int ptw = (((unsigned long long int) (x[ 9u ])) << 56llu) |
                                         (((unsigned long long int) (x[ 8u ])) << 48llu) |
                                         (((unsigned long long int) (x[ 7u ])) << 40llu) |
                                         (((unsigned long long int) (x[ 6u ])) << 32llu) |
                                         (((unsigned long long int) (x[ 5u ])) << 24llu) |
                                         (((unsigned long long int) (x[ 4u ])) << 16llu) |
                                         (((unsigned long long int) (x[ 3u ])) <<  8llu) |
                                         (((unsigned long long int) (x[ 2u ])) <<  0llu);

      last_ptw_fup = (((unsigned int) (x[ 1u ])) >> 7u) & 0x01u;

      record_intel_pt_pkt(INTEL_PT_PKT_PTW, ptw, cyc_cnt_ref, x, n); last_intel_pt_pkt_ptw = last_intel_pt_pkt;
      x += 10llu;
      n -= 10llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_8 == MODE)) {
      const unsigned int        leaf_id  = ((unsigned int) (x[ 1u ])) >> 5u;
      const intel_pt_pkt_type_t mode_pkt = (leaf_id == 0u) ? (INTEL_PT_PKT_MODE_EXEC) : (INTEL_PT_PKT_MODE_TSX);

      if (leaf_id >= 1u) {
        fprintf(stderr, "MODE error %u\n", leaf_id); print_intel_pt_pkts(); for (;;) {}
      }

#if defined(PRINT_PT)
      if (leaf_id == 0u) {
        fprintf(stdout, "MODE_EXEC     = %20u\n", leaf_id);
      } else {
        fprintf(stdout, "MODE_TSX      = %20u\n", leaf_id);
      }
#endif

      record_intel_pt_pkt(mode_pkt, ((unsigned long long int) (x[ 1u ])), cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_8 == MTC)) {
      const unsigned long long int ctc = ((unsigned long long int) (x[ 1u ]));

      record_intel_pt_pkt(INTEL_PT_PKT_MTC, ctc, cyc_cnt_ref, x, n);
      x += 2llu;
      n -= 2llu;
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_8 == TSC)) {
      const unsigned long long int tsc = (((unsigned long long int) (x[ 7u ])) << 48llu) |
                                         (((unsigned long long int) (x[ 6u ])) << 40llu) |
                                         (((unsigned long long int) (x[ 5u ])) << 32llu) |
                                         (((unsigned long long int) (x[ 4u ])) << 24llu) |
                                         (((unsigned long long int) (x[ 3u ])) << 16llu) |
                                         (((unsigned long long int) (x[ 2u ])) <<  8llu) |
                                         (((unsigned long long int) (x[ 1u ])) <<  0llu);

      tsc_ref         = tsc;
      tsc_ref_ctc_ovf = 0llu;
      tsc_ref_ctc     = 0llu;
      tsc_ref_fc      = 0llu;
      tsc_ref_cyc_cnt = cyc_cnt_ref;

#if defined(PRINT_PT)
      fprintf(stdout, "TSC           = %20.2lf\n", ((double) (tsc)));
#endif

      record_intel_pt_pkt(INTEL_PT_PKT_TSC, tsc, cyc_cnt_ref, x, n);
      x += 8llu;
      n -= 8llu;
      goto decode_again;
    }
    if ((n >= 1llu) && (*x_8 == PAD)) {
      record_intel_pt_pkt(INTEL_PT_PKT_PAD, 0llu, cyc_cnt_ref, x, n);
      x += 1llu;
      n -= 1llu;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & FUP_MASK) == FUP)) {
      const unsigned long long int fup = ip_decode(&x, &n, n_orig, INTEL_PT_PKT_FUP);

      if (last_bep_fup == 1u) {
#if defined(PRINT_PT)
        fprintf(stdout, "FUP BEP       = %20llx\n", fup);
#endif

        xed_intel_pt_bip_fup(bip_ip, fup, bip_tsc, bip_pmu_mask, bip_mem_access_addr);
        bip_ip       = 0llu;
        bip_pmu_mask = 0llu;
        bip_tsc      = 0llu;

        bip_mem_access_addr = 0llu;
        bip_mem_aux_info    = 0llu;
        bip_mem_access_lat  = 0llu;
        bip_tsx_aux_info    = 0llu;

        last_bep_fup = 0u;
        last_intel_pt_pkt->type = INTEL_PT_PKT_FUP_BEP;
      }
      if (last_ptw_fup == 1u) {
#if defined(PRINT_PT)
        fprintf(stdout, "FUP PTW       = %20llx\n", fup);
#endif

        xed_intel_pt_ptw_fup(fup, last_intel_pt_pkt_ptw->tsc_approx, last_intel_pt_pkt_ptw->cyc_cnt, last_intel_pt_pkt_ptw->v.ptw);

        last_ptw_fup = 0u;
        last_intel_pt_pkt->type = INTEL_PT_PKT_FUP_PTW;
      }
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_PGE_MASK) == TIP_PGE)) {
      ip_decode(&x, &n, n_orig, INTEL_PT_PKT_TIP_PGE);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_MASK) == TIP)) {
      ip_decode(&x, &n, n_orig, INTEL_PT_PKT_TIP);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_PGD_MASK) == TIP_PGD)) {
      ip_decode(&x, &n, n_orig, INTEL_PT_PKT_TIP_PGD);
      goto decode_again;
    }
    if ((n >= 9llu) && (((*x_8) & BIP_MASK) == BIP) && (last_bbp == 1u)) {
      const unsigned int           bip_id      = (((unsigned int) (x[ 0u ])) >> 3u) & 0x1Fu;
      const unsigned long long int bip_payload = (((unsigned long long int) (x[ 8u ])) << 56llu) |
                                                 (((unsigned long long int) (x[ 7u ])) << 48llu) |
                                                 (((unsigned long long int) (x[ 6u ])) << 40llu) |
                                                 (((unsigned long long int) (x[ 5u ])) << 32llu) |
                                                 (((unsigned long long int) (x[ 4u ])) << 24llu) |
                                                 (((unsigned long long int) (x[ 3u ])) << 16llu) |
                                                 (((unsigned long long int) (x[ 2u ])) <<  8llu) |
                                                 (((unsigned long long int) (x[ 1u ])) <<  0llu);

      if (last_bbp_type == 0x04u) {
        if (bip_id == 0x00u) {
          bip_ip = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP IP        = %20llx\n", bip_payload);
#endif
        } else if (bip_id == 0x01u) {
          bip_pmu_mask = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP PMU       = %20llx\n", bip_payload);
#endif
        } else if (bip_id == 0x02u) {
          bip_tsc = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP TSC       = %20.2lf\n", ((double) (bip_payload)));
#endif
        }
      } else if (last_bbp_type == 0x05u) {
        if (bip_id == 0x00u) {
          bip_mem_access_addr = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP MEM ADDR  = %20llx\n", bip_payload);
#endif
        } else if (bip_id == 0x01u) {
          bip_mem_aux_info = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP MEM AUX   = %20llx\n", bip_payload);
#endif
        } else if (bip_id == 0x02u) {
          bip_mem_access_lat = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP MEM LAT   = %20llx\n", bip_payload);
#endif
        } else if (bip_id == 0x03u) {
          bip_tsx_aux_info = bip_payload;

#if defined(PRINT_PT)
          fprintf(stdout, "BIP TSX AUX   = %20llx\n", bip_payload);
#endif
        }
      } else {

      }

      record_intel_pt_pkt(INTEL_PT_PKT_BIP, bip_payload, cyc_cnt_ref, x, n);
      x += 9llu;
      n -= 9llu;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & CYC_MASK) == CYC)) {
      unsigned long long int i   = 0llu;
      unsigned long long int cyc = (((unsigned long long int) (x[ 0u ])) >> 3llu) & 0x1Fllu;
      unsigned int           exp = (((unsigned int)           (x[ 0u ])) >> 2llu) & 0x01llu;

      x++;
      n--;
      if (exp == 1u) {
cyc_again:
        if (n >= 1llu) {
          cyc |= (((unsigned long long int) (x[ 0u ])) >> 1llu) << (5llu + 7llu * i);
          exp  = ((unsigned int) (x[ 0u ])) & 0x01u;

          x++;
          n--;
          i++;
          if (exp == 1u) {
            goto cyc_again;
          }
        } else {
          fprintf(stderr, "cyc error\n"); for (;;) { }
        }
      }
      cyc_cnt_ref += cyc;

      record_intel_pt_pkt(INTEL_PT_PKT_CYC, cyc, cyc_cnt_ref, x, n);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & SHORT_TNT_MASK) == SHORT_TNT)) {
      unsigned int       p_one     = 0u;
      const unsigned int short_tnt = ((unsigned int) (x[ 0u ]));

      asm volatile ("bsr %1, %0": "=r"(p_one) : "r"(short_tnt) : );

      record_intel_pt_pkt(INTEL_PT_PKT_SHORT_TNT, ((unsigned long long int) (short_tnt)), cyc_cnt_ref, x, n);
      xed_process_branches((short_tnt >> 1u) & ((1u << (p_one - 1u)) - 1u),
                           p_one - 1u,
                           0llu,
                           tsc_approx_ref,
                           cyc_cnt_ref);

#if defined(PRINT_PT)
      fprintf(stdout,
              "SHORT_TNT     = %20x %3u\n",
              (short_tnt >> 1u) & ((1u << (p_one - 1u)) - 1u),
              p_one - 1u);
#endif

      x += 1llu;
      n -= 1llu;
      goto decode_again;
    }
  }

  if (n >= 1llu) {
    xed_close();

    print_intel_pt_bytes(x, n, n_orig);
    print_intel_pt_pkts();
    fprintf(stderr, "1 intel_pt_decode error\n"); for (;;) { }
  }

  return n;
}

void intel_pt_enable(const int intel_pt_fd) {
  if (ioctl(intel_pt_fd, PERF_EVENT_IOC_ENABLE, 0) == 0) {
    intel_pt_status = INTEL_PT_STATUS_ENABLE;
  }
}

void intel_pt_disable(const int intel_pt_fd) {
  if (ioctl(intel_pt_fd, PERF_EVENT_IOC_DISABLE, 0) == 0) {
    intel_pt_status = INTEL_PT_STATUS_DISABLE;
  }
}
