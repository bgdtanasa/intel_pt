#include <stdio.h>
#include <string.h>

#include "intel_pt.h"
#include "pmu.h"
#include "xed.h"

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
#define TIP_PGE   (0x11u)       //                                  1_0001
#define FUP       (0x1Du)       //                                  1_1101
#define TIP       (0x0Du)       //                                  0_1101
#define TIP_PGD   (0x01u)       //                                  0_0001
#define BIP       (0x04u)       //                                     100
#define CYC       (0x03u)       //                                      11
#define SHORT_TNT (0x00u)       //                               1xxx_xxx0

#define MNT_MASK       (0xFFFFFFu)
#define EXSTOP_MASK    (0x7FFFu)
#define BEP_MASK       (0x7FFFu)
#define PTW_MASK       (0x1FFFu)
#define TIP_PGE_MASK   (0x1Fu)
#define FUP_MASK       (0x1Fu)
#define TIP_MASK       (0x1Fu)
#define TIP_PGD_MASK   (0x1Fu)
#define BIP_MASK       (0x07u)
#define CYC_MASK       (0x03u)
#define SHORT_TNT_MASK (0x01u)

#define PRINT_FORMAT "%10s :: "

#define INTEL_PT_PKT_HISTORY (1024u)

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
  INTEL_PT_PKT_MODE,
  INTEL_PT_PKT_MTC,
  INTEL_PT_PKT_TSC,
  INTEL_PT_PKT_PAD,
  INTEL_PT_PKT_TIP_PGE,
  INTEL_PT_PKT_FUP,
  INTEL_PT_PKT_TIP,
  INTEL_PT_PKT_TIP_PGD,
  INTEL_PT_PKT_BIP,
  INTEL_PT_PKT_CYC,
  INTEL_PT_PKT_SHORT_TNT
} intel_pt_pkt_type_t;

typedef struct {
  intel_pt_pkt_type_t      type;
  union {
    unsigned long long int val;
    unsigned long long int ptw; // INTEL_PT_PKT_PTW
    unsigned long long int ctc; // INTEL_PT_PKT_MTC
    unsigned long long int tsc; // INTEL_PT_PKT_TSC
    unsigned long long int ip;  // INTEL_PT_PKT_TIP_PGE, INTEL_PT_PKT_FUP, INTEL_PT_PKT_TIP, INTEL_PT_PKT_TIP_PGD
  } v;
  unsigned long long int   cyc_cnt;
} intel_pt_pkt_t;

unsigned int intel_pt_ovf;
unsigned int intel_pt_pge;
unsigned int intel_pt_pgd;

static const char*    intel_pt_pkt_names[ ] = {
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
  [ INTEL_PT_PKT_MODE ]      = "MODE",
  [ INTEL_PT_PKT_MTC ]       = "MTC",
  [ INTEL_PT_PKT_TSC ]       = "TSC",
  [ INTEL_PT_PKT_PAD ]       = "PAD",
  [ INTEL_PT_PKT_TIP_PGE ]   = "TIP_PGE",
  [ INTEL_PT_PKT_FUP ]       = "FUP",
  [ INTEL_PT_PKT_TIP ]       = "TIP",
  [ INTEL_PT_PKT_TIP_PGD ]   = "TIP_PGD",
  [ INTEL_PT_PKT_BIP ]       = "BIP",
  [ INTEL_PT_PKT_CYC ]       = "CYC",
  [ INTEL_PT_PKT_SHORT_TNT ] = "SHORT_TNT"
};
static intel_pt_pkt_t intel_pt_pkt_hist[ INTEL_PT_PKT_HISTORY ];
static unsigned int   intel_pt_pkt_hist_idx;

static unsigned int last_psb;
static unsigned int last_bbp;
static unsigned int last_bbp_type;
static unsigned int last_bbp_sz;

static double                 tsc_approx_ctc;
static unsigned long long int tsc_approx_fc;
static double                 tsc_ref;
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

static unsigned int xed_enable = 0u;

static inline __attribute__((always_inline)) double tsc_approx(void) {
  if (tsc_ref == 0.0f) {
    return 0.0f;
  }
  if (cbr_factor == 0.0f) {
    return 0.0f;
  }

  static double tsc_ret_prev = 0.0f;
  double        tsc_ret_curr = tsc_ref +
                               tsc_approx_ctc +
                               ((double) (tsc_approx_fc * base_factor)) / ((double) (cbr_factor));
  if (tsc_ret_curr < tsc_ret_prev) {
    fprintf(stdout, "\t\t\e[0;31mTSC_APPROX_ERR =  %16.2lf\e[0m\n", tsc_ret_curr - tsc_ret_prev);
  }
  tsc_ret_prev = tsc_ret_curr;

  return tsc_ret_curr;
}

static inline __attribute__((always_inline)) unsigned int is_cyc_eligible(const intel_pt_pkt_type_t type) {
  return ((type == INTEL_PT_PKT_OVF)       ||
          (type == INTEL_PT_PKT_VMCS)      ||
          (type == INTEL_PT_PKT_LONG_TNT)  ||
          (type == INTEL_PT_PKT_PIP)       ||
          (type == INTEL_PT_PKT_EXSTOP)    ||
          (type == INTEL_PT_PKT_PTW)       ||
          (type == INTEL_PT_PKT_MODE)      ||
          (type == INTEL_PT_PKT_MTC)       ||
          (type == INTEL_PT_PKT_TSC)       ||
          (type == INTEL_PT_PKT_TIP_PGE)   ||
          (type == INTEL_PT_PKT_TIP)       ||
          (type == INTEL_PT_PKT_TIP_PGD)   ||
          (type == INTEL_PT_PKT_SHORT_TNT)) ? (1u) : (0u);
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

static void record_intel_pt_pkt(intel_pt_pkt_type_t          type,
                                unsigned long long int       val,
                                const unsigned long long int cyc_cnt) {
  static const intel_pt_pkt_t* mtc_ref = NULL;

  const unsigned int a = is_cyc_eligible(type);
  const unsigned int b = (type == INTEL_PT_PKT_TSC_MTC) ? (1u) : (0u);

  if (b == 1u) {
    type = INTEL_PT_PKT_MTC;
  }
  if (a == 1u) {
    if (mtc_ref != NULL) {
      if (type == INTEL_PT_PKT_MTC) {
        unsigned long long int ctc_ovf = 0llu;
        unsigned long long int ctc     = val;

        ctc_ovf = (ctc < (mtc_ref->v.ctc & 0x00FFllu)) ? (1llu) : (0llu);
        ctc     = ((mtc_ref->v.ctc & 0xFFFFFFFFFFFFFF00llu) | ctc) + (ctc_ovf << 8llu);
        val     = ctc;

        tsc_approx_ctc = ((double) ((ctc - tsc_ref_ctc) * tsc_ratio));
        tsc_approx_fc  = cyc_cnt - mtc_ref->cyc_cnt;
      } else {
        tsc_approx_ctc  = tsc_approx_ctc;
        tsc_approx_fc  += cyc_cnt - mtc_ref->cyc_cnt;
      }
    }
  }
  fprintf(stdout,
          PRINT_FORMAT "%16.2lf %16llu %20.2lf :: %16llu\n",
          intel_pt_pkt_names[ type ],
          (a == 1u) ? (tsc_approx_ctc) : (0.0f),
          (a == 1u) ? (tsc_approx_fc)  : (0llu),
          (a == 1u) ? (tsc_approx())   : (0.0f),
          cyc_cnt_ref);

  intel_pt_pkt_hist[ intel_pt_pkt_hist_idx ] = (intel_pt_pkt_t) {
    .type    = type,
    .v       = {
      .val   = val
    },
    .cyc_cnt = ((a == 1u) || (b == 1u)) ? (cyc_cnt) : (0llu)
  };
  if (type == INTEL_PT_PKT_MTC) {
    mtc_ref = &intel_pt_pkt_hist[ intel_pt_pkt_hist_idx ];
  }
  intel_pt_pkt_hist_idx = (intel_pt_pkt_hist_idx + 1u) % INTEL_PT_PKT_HISTORY;
}

static unsigned long long int ip_decode(const unsigned char** const   x,
                                        unsigned long long int* const n,
                                        const intel_pt_pkt_type_t     pkt_type) {
  static unsigned long long int last_ip;

  const unsigned char*   x_p = *x;
  unsigned long long int n_p = *n;

  unsigned long long int ip             = 0llu;
  const unsigned char    ip_bytes       = (x_p[ 0u ] >> 5u) & 0x07u;
  unsigned char          ip_compressed  = ((ip_bytes == 3u) || ((ip_bytes == 6u))) ? (0u) : (1u);
  unsigned char          last_ip_update = 1u;

  (void) (ip_compressed);

  x_p++;
  n_p--;
  switch (ip_bytes) {
    case 0u:
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

          for (unsigned long long int i = 0lu; i < 16lu; i++) {
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
    break;

    default:
      last_ip_update = 2u;
    break;
  }
  if (last_ip_update == 1u) {
    last_ip = ip;
  } else if (last_ip_update >= 1u) {
    fprintf(stderr, "ip_decode error :: ip_bytes = %u\n", ip_bytes); for (;;) { }
  }
  *x = x_p;
  *n = n_p;

  fprintf(stdout, "IP            = %20llx\n", ip);
  if (ip != 0llu) {
    if (pkt_type == INTEL_PT_PKT_TIP_PGE) {
      if (xed_enable == 0u) {
        xed_enable = 1u;
      }
      if (xed_enable == 1u) {
        xed_update_last_inst(ip);
      }
    } else if (pkt_type == INTEL_PT_PKT_TIP) {
      if (xed_enable == 1u) {
        xed_process_branches(0u, 0u, ip);
      }
    } else if (pkt_type == INTEL_PT_PKT_TIP_PGD) {
      fprintf(stderr, "ip_decode error :: ip = %16llx\n", ip); for (;;) { }
    }
  } else {
    xed_enable = 0u;
    xed_reset_last_inst();
  }

  return ip;
}

unsigned long long int intel_pt_decode(const unsigned char*   x,
                                       unsigned long long int n,
                                       const double           ts) {
  (void) (ts);

  const unsigned char* const x_orig = x;

  unsigned int*       x_32;
  unsigned short int* x_16;
  unsigned char*      x_8;

  //for (unsigned long long int i = 0llu; i < n; i++) {
  //  fprintf(stdout, "%02x ", ((unsigned int) (x[ i ])));
  //}
  //fprintf(stdout, "\n");

decode_again:
  if (n >= 1llu) {
    //fprintf(stdout, "\tn = %12llu :: ", n);

    x_32 = ((unsigned int*) (x));
    x_16 = ((unsigned short int*) (x));
    x_8  = ((unsigned char*) (x));
    if ((n >= 4llu) && (*x_32 == PSB)) {
      intel_pt_ovf = 0u;
      intel_pt_pge = 0u;
      intel_pt_pgd = 0u;

      last_psb   = 1u;
      xed_enable = 0u;
      xed_reset_last_inst();

      x += 4llu;
      n -= 4llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PSB, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 11llu) && (((*x_32) & MNT_MASK) == MNT)) {
      x += 11llu;
      n -= 11llu;
      record_intel_pt_pkt(INTEL_PT_PKT_MNT, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == OVF)) {
      intel_pt_ovf += 1u;
      intel_pt_pge  = 0u;
      intel_pt_pgd  = 0u;

      xed_enable    = 0u;
      xed_reset_last_inst();

      x += 2llu;
      n -= 2llu;
      record_intel_pt_pkt(INTEL_PT_PKT_OVF, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == VMCS)) {
      x += 7llu;
      n -= 7llu;
      record_intel_pt_pkt(INTEL_PT_PKT_VMCS, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 10llu) && (*x_16 == MWAIT)) {
      x += 10llu;
      n -= 10llu;
      record_intel_pt_pkt(INTEL_PT_PKT_MWAIT, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_16 == LONG_TNT)) {
      x += 8llu;
      n -= 8llu;
      record_intel_pt_pkt(INTEL_PT_PKT_LONG_TNT, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == PWRX)) {
      x += 7llu;
      n -= 7llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PWRX, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == TRACESTOP)) {
      x += 2llu;
      n -= 2llu;
      record_intel_pt_pkt(INTEL_PT_PKT_TRACESTOP, 0llu, cyc_cnt_ref);
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
          tsc_ref_ctc     = ctc;
          tsc_ref_fc      = fc;
          tsc_ref_cyc_cnt = pkt->cyc_cnt;

          tsc_approx_ctc = 0.0f;
          tsc_approx_fc  = 0llu;
          tsc_approx();
        } else {
          fprintf(stderr, "tsc_mtc error 0!\n"); for (;;) {}
        }
      } else {
          fprintf(stderr, "tsc_mtc error 1!\n"); for (;;) {}
      }

      x += 7llu;
      n -= 7llu;
      if (pkt != NULL) {
        // INTEL_PT_PKT_TSC_MTC is in fact a INTEL_PT_PKT_MTC packet.
        // TSC_MTC it not cyc eligibale but the corresponding TSC is.
        // As such, this fake MTC packet gets the cyc_cnt of the TSC packet.
        record_intel_pt_pkt(INTEL_PT_PKT_TSC_MTC, tsc_ref_ctc, tsc_ref_cyc_cnt);
      }
      goto decode_again;
    }
    if ((n >= 3llu) && (*x_16 == BBP)) {
      last_bbp      = 1u;
      last_bbp_type = (((unsigned int) (x[ 2u ])) >> 0u) & 0x1Fu;
      last_bbp_sz   = (((unsigned int) (x[ 2u ])) >> 7u) & 0x01u;

      fprintf(stdout, "BBP TYPE      = %20u\n", last_bbp_type);
      fprintf(stdout, "BBP SZ        = %20u\n", last_bbp_sz);

      x += 3llu;
      n -= 3llu;
      record_intel_pt_pkt(INTEL_PT_PKT_BBP, ((unsigned long long int) (last_bbp_type)), cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 11llu) && (*x_16 == EVD)) {
      x += 11llu;
      n -= 11llu;
      record_intel_pt_pkt(INTEL_PT_PKT_EVD, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_16 == PIP)) {
      const unsigned long long int cr3 = ((((unsigned long long int) (x[ 7u ])) & 0xFFllu) << 44llu) |
                                         ((((unsigned long long int) (x[ 6u ])) & 0xFFllu) << 36llu) |
                                         ((((unsigned long long int) (x[ 5u ])) & 0xFFllu) << 28llu) |
                                         ((((unsigned long long int) (x[ 4u ])) & 0xFFllu) << 20llu) |
                                         ((((unsigned long long int) (x[ 3u ])) & 0xFFllu) << 12llu) |
                                         ((((unsigned long long int) (x[ 2u ])) & 0xFEllu) <<  4llu);

      x += 8llu;
      n -= 8llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PIP, cr3, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == PSBEND)) {
      last_psb   = 0u;
      xed_enable = 1u;

      x += 2llu;
      n -= 2llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PSBEND, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == PWRE)) {
      x += 4llu;
      n -= 4llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PWRE, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == CFE)) {
      x += 4llu;
      n -= 4llu;
      record_intel_pt_pkt(INTEL_PT_PKT_CFE, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == CBR)) {
      cbr         = ((unsigned long long int) (x[ 2u ]));
      tsc_factor  = ((double) (tsc_hz)) / 1e9;
      base_factor = ((double) (base_hz)) / 1e9;
      cbr_factor  = ((double) (cbr * bus_hz)) / 1e9;

      x += 4llu;
      n -= 4llu;
      record_intel_pt_pkt(INTEL_PT_PKT_CBR, cbr, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (((*x_16) & EXSTOP_MASK) == EXSTOP)) {
      x += 2llu;
      n -= 2llu;
      record_intel_pt_pkt(INTEL_PT_PKT_EXSTOP, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (((*x_16) & BEP_MASK) == BEP)) {
      const unsigned int bep_fup = (((unsigned int) (x[ 1u ])) >> 7u) & 0x01u;

      last_bbp      = 0u;
      last_bbp_type = 0xFFFFFFFFu;
      last_bbp_sz   = 0u;

      fprintf(stdout, "BEP FUP       = %20u\n", bep_fup);

      x += 2llu;
      n -= 2llu;
      record_intel_pt_pkt(INTEL_PT_PKT_BEP, ((unsigned long long int) ((x[ 1u ] >> 7u) & 0x01u)), cyc_cnt_ref);
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

      {
        const intel_pt_pkt_t* const  ptw_ref  = prev_intel_pt_pkt(INTEL_PT_PKT_PTW);
        const unsigned long long int ptw_diff = (ptw_ref != NULL) ? (cyc_cnt_ref - ptw_ref->cyc_cnt) : (0llu);

        fprintf(stdout,
                "PTW           = %20.2lf %20.2lf\n",
                ((double) (ptw_diff)) * tsc_factor / cbr_factor,
                ((double) (ptw)));
      }

      x += 10llu;
      n -= 10llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PTW, ptw, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_8 == MODE)) {
      x += 2llu;
      n -= 2llu;
      record_intel_pt_pkt(INTEL_PT_PKT_MODE, ((unsigned long long int) (x[ 1u ])), cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_8 == MTC)) {
      const unsigned long long int ctc = ((unsigned long long int) (x[ 1u ]));

      x += 2llu;
      n -= 2llu;
      if (tsc_ref != 0.0f) {
        record_intel_pt_pkt(INTEL_PT_PKT_MTC, ctc, cyc_cnt_ref);
      }
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

      x += 8llu;
      n -= 8llu;
      record_intel_pt_pkt(INTEL_PT_PKT_TSC, tsc, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (*x_8 == PAD)) {
      x += 1llu;
      n -= 1llu;
      record_intel_pt_pkt(INTEL_PT_PKT_PAD, 0llu, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_PGE_MASK) == TIP_PGE)) {
      intel_pt_pge++;

      record_intel_pt_pkt(INTEL_PT_PKT_TIP_PGE, ip_decode(&x, &n, INTEL_PT_PKT_TIP_PGE), cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & FUP_MASK) == FUP)) {
      record_intel_pt_pkt(INTEL_PT_PKT_FUP, ip_decode(&x, &n, INTEL_PT_PKT_FUP), cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_MASK) == TIP)) {
      record_intel_pt_pkt(INTEL_PT_PKT_TIP, ip_decode(&x, &n, INTEL_PT_PKT_TIP), cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_PGD_MASK) == TIP_PGD)) {
      intel_pt_pgd++;

      record_intel_pt_pkt(INTEL_PT_PKT_TIP_PGD, ip_decode(&x, &n, INTEL_PT_PKT_TIP_PGD), cyc_cnt_ref);
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
          fprintf(stdout, "BIP IP        = %20llx\n", bip_payload);
        } else if (bip_id == 0x01u) {
          pmu_info(bip_payload);
        } else if (bip_id == 0x02u) {
          fprintf(stdout, "BIP TSC       = %20.2lf\n", ((double) (bip_payload)));
        }
      }

      x += 9llu;
      n -= 9llu;
      record_intel_pt_pkt(INTEL_PT_PKT_BIP, bip_payload, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & CYC_MASK) == CYC)) {
      unsigned long long int i   = 0llu;
      unsigned long long int cyc = ((unsigned long long int) ((x[ 0u ] >> 3u) & 0x1Fu));
      unsigned char          exp = (x[ 0u ] >> 2u) & 0x01u;

      x++;
      n--;
      if (exp == 1u) {
cyc_again:
        if (n >= 1llu) {
          cyc |= (((unsigned long long int) (x[ 0u ])) >> 1llu) << (5llu + 7llu * i);
          exp  = x[ 0u ] & 0x01u;

          x++;
          n--;
          i++;
          if (exp == 1u) {
            goto cyc_again;
          }
        } else {
          fprintf(stderr, "cyc error\n"); for (;;) {}
        }
      }
      cyc_cnt_ref += cyc;

      record_intel_pt_pkt(INTEL_PT_PKT_CYC, cyc_cnt_ref, cyc_cnt_ref);
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & SHORT_TNT_MASK) == SHORT_TNT)) {
      unsigned int       p_one     = 0u;
      const unsigned int short_tnt = ((unsigned int) (x[ 0u ]));

      asm volatile ("bsr %1, %0": "=r"(p_one) : "r"(short_tnt) : );

      if (xed_enable == 1u) {
        xed_process_branches((short_tnt >> 1u) & ((1u << (p_one - 1u)) - 1u),
                             p_one - 1u,
                             0llu);
      }

      x += 1llu;
      n -= 1llu;
      record_intel_pt_pkt(INTEL_PT_PKT_SHORT_TNT, ((unsigned long long int) (short_tnt)), cyc_cnt_ref);
      goto decode_again;
    }
  }

  if (n >= 1llu) {
    for (unsigned long long int i = 0llu; i < ((n > 16llu) ? (16llu) : (n)); i++) {
      fprintf(stdout, "%02x ", ((unsigned int) (x[ i ])));
    }
    fprintf(stdout, "\n");

    memcpy(((void*) (x_orig)), ((void*) (x)), ((size_t) (n)));
  }
  return n;
}

void intel_pt_reset(void) {
  xed_enable = 0u;
  xed_reset_last_inst();
}
