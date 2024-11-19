#include <stdio.h>
#include <string.h>

#include "intel_pt.h"
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

#define PRINT_CBR
#define PRINT_PTW
//#define PRINT_MTC
#define PRINT_TSC
#define PRINT_FUP
#define PRINT_TIP
#define PRINT_BIP
#define PRINT_CYC
#define PRINT_SHORT_TNT

typedef enum {
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
} intel_pt_pkt_t;

typedef struct {
  unsigned long long int tsc;
  unsigned long long int cyc;
} tsc_cyc_t;

static intel_pt_pkt_t last_intel_pt_pkt;

static unsigned int last_psb;

static unsigned int last_bbp;
static unsigned int last_bbp_type;

static unsigned long long int last_ip;

static unsigned char curr_mtc;
static unsigned char prev_mtc;
static unsigned long long int tsc_mtc_cnt;
static unsigned long long int tsc_cnt;
static unsigned long long int cyc_cnt;
static tsc_cyc_t curr_tsc_cyc;
static tsc_cyc_t prev_tsc_cyc;

static double total_inactive_time;
static double inactive_time;

static unsigned long long int curr_ptw_tsc_mtc;
static unsigned long long int prev_ptw_tsc_mtc;
static unsigned long long int curr_ptw_cyc;
static unsigned long long int prev_ptw_cyc;

extern unsigned long long int tsc_hz;
extern unsigned long long int tsc_ratio;
extern unsigned long long int bus_hz;
unsigned long long int        cbr_hz;

double                        tsc_factor;
double                        cbr_factor;

static unsigned int tnt_enable = 0u;

static unsigned long long int ip_decode(const unsigned char** const   x,
                                        unsigned long long int* const n,
                                        const char*                   s) {
  const unsigned char*   x_p = *x;
  unsigned long long int n_p = *n;

  unsigned long long int ip             = 0llu;
  const unsigned char    ip_bytes       = (x_p[ 0u ] >> 5u) & 0x07u;
  unsigned char          update_last_ip = 1u;

  x_p++;
  n_p--;
  switch (ip_bytes) {
    case 0u:
      update_last_ip = 0u;
    break;

    case 1u:
      if (n_p >= 2llu) {
        ip = (last_ip & 0xFFFFFFFFFFFF0000llu)                |
             (((unsigned long long int) (x_p[ 1u ])) << 8llu) |
             (((unsigned long long int) (x_p[ 0u ])) << 0llu);
        x_p += 2llu;
        n_p -= 2llu;
      } else {
        update_last_ip = 2u;
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
        update_last_ip = 2u;
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
        update_last_ip = 2u;
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
        update_last_ip = 2u;
      }
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
        update_last_ip = 2u;
      }
    break;

    default:
      update_last_ip = 2u;
    break;
  }
  if (update_last_ip == 1u) {
    last_ip = ip;
  } else if (update_last_ip == 2u) {
    fprintf(stderr, "ip_decode error!\n");

    for (;;) { }
  }

  *x = x_p;
  *n = n_p;

  fprintf(stdout, "%s %16llx\n", s, ip);
  xed_find_inst(ip, 1u);

  return ip;
}

unsigned long long int intel_pt_decode(const unsigned char*   x,
                                       unsigned long long int n,
                                       const double           ts) {
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
    //fprintf(stdout, "%12llu :: ", n);

    x_32 = ((unsigned int*) (x));
    x_16 = ((unsigned short int*) (x));
    x_8  = ((unsigned char*) (x));
    if ((n >= 4llu) && (*x_32 == PSB)) {
      last_psb = 1u;
      last_ip  = 0u;

      fprintf(stdout, "      PSB\n");

      x += 4llu;
      n -= 4llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PSB;
      goto decode_again;
    }
    if ((n >= 11llu) && (((*x_32) & MNT_MASK) == MNT)) {
      fprintf(stdout, "      MNT\n");

      x += 11llu;
      n -= 11llu;
      last_intel_pt_pkt = INTEL_PT_PKT_MNT;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == OVF)) {
      fprintf(stdout, "      OVF\n");

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_OVF;
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == VMCS)) {
      fprintf(stdout, "     VMCS\n");

      x += 7llu;
      n -= 7llu;
      last_intel_pt_pkt = INTEL_PT_PKT_VMCS;
      goto decode_again;
    }
    if ((n >= 10llu) && (*x_16 == MWAIT)) {
      fprintf(stdout, "    MWAIT\n");

      x += 10llu;
      n -= 10llu;
      last_intel_pt_pkt = INTEL_PT_PKT_MWAIT;
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_16 == LONG_TNT)) {
      fprintf(stdout, " LONG_TNT\n");

      x += 8llu;
      n -= 8llu;
      last_intel_pt_pkt = INTEL_PT_PKT_LONG_TNT;
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == PWRX)) {
      fprintf(stdout, "     PWRX\n");

      x += 7llu;
      n -= 7llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PWRX;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == TRACESTOP)) {
      fprintf(stdout, "TRACESTOP\n");

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_TRACESTOP;
      goto decode_again;
    }
    if ((n >= 7llu) && (*x_16 == TSC_MTC)) {
      const unsigned int ctc = (((unsigned int) (x[ 3u ])) << 8u) | 
                               (((unsigned int) (x[ 2u ])) << 0u);
      const unsigned int fast_counter = ((((unsigned int) (x[ 6u ])) & 0x01u) << 8u) |
                                        ((((unsigned int) (x[ 5u ])) & 0xFFu) << 0u);

      prev_mtc    = ctc & 0xFFu;
      tsc_mtc_cnt = 0llu;

      fprintf(stdout, "  TSC_MTC :: %16x %16x\n", ctc, fast_counter);

      x += 7llu;
      n -= 7llu;
      last_intel_pt_pkt = INTEL_PT_PKT_TSC_MTC;
      goto decode_again;
    }
    if ((n >= 3llu) && (*x_16 == BBP)) {
      last_bbp      = 1u;
      last_bbp_type = (((unsigned int) (x[ 2u ])) >> 0u) & 0x1Fu;

      fprintf(stdout,
              "      BBP :: SZ = %8x :: TYPE    = %16x\n",
              (x[ 2u ] >> 7u) & 0x01u,
              last_bbp_type);

      x += 3llu;
      n -= 3llu;
      last_intel_pt_pkt = INTEL_PT_PKT_BBP;
      goto decode_again;
    }
    if ((n >= 11llu) && (*x_16 == EVD)) {
      fprintf(stdout, "      EVD\n");

      x += 11llu;
      n -= 11llu;
      last_intel_pt_pkt = INTEL_PT_PKT_EVD;
      goto decode_again;
    }
    if ((n >= 8llu) && (*x_16 == PIP)) {
      const unsigned long long int cr3 = ((((unsigned long long int) (x[ 7u ])) & 0xFFllu) << 44llu) |
                                         ((((unsigned long long int) (x[ 6u ])) & 0xFFllu) << 36llu) |
                                         ((((unsigned long long int) (x[ 5u ])) & 0xFFllu) << 28llu) |
                                         ((((unsigned long long int) (x[ 4u ])) & 0xFFllu) << 20llu) |
                                         ((((unsigned long long int) (x[ 3u ])) & 0xFFllu) << 12llu) |
                                         ((((unsigned long long int) (x[ 2u ])) & 0xFEllu) <<  4llu);

      fprintf(stdout, "      PIP :: CR3 = %16llx :: %08x\n", cr3, x[ 2u ]);

      x += 8llu;
      n -= 8llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PIP;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_16 == PSBEND)) {
      last_psb = 0u;

      fprintf(stdout, "   PSBEND\n");

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PSBEND;
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == PWRE)) {
      fprintf(stdout, "     PWRE\n");

      x += 4llu;
      n -= 4llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PWRE;
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == CFE)) {
      fprintf(stdout, "      CFE\n");

      x += 4llu;
      n -= 4llu;
      last_intel_pt_pkt = INTEL_PT_PKT_CFE;
      goto decode_again;
    }
    if ((n >= 4llu) && (*x_16 == CBR)) {
      cbr_hz     = ((unsigned long long int) (x[ 2u ])) * bus_hz;
      cbr_factor = ((double) (cbr_hz)) / 1e9;
      tsc_factor = ((double) (tsc_hz)) / 1e9;

#if defined(PRINT_CBR)
      fprintf(stdout, "      CBR :: %12llu Hz :: %12.5lf\n", cbr_hz, cbr_factor);
#endif

      x += 4llu;
      n -= 4llu;
      last_intel_pt_pkt = INTEL_PT_PKT_CBR;
      goto decode_again;
    }
    if ((n >= 2llu) && (((*x_16) & EXSTOP_MASK) == EXSTOP)) {
      fprintf(stdout, "   EXSTOP\n");

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_EXSTOP;
      goto decode_again;
    }
    if ((n >= 2llu) && (((*x_16) & BEP_MASK) == BEP)) {
      last_bbp      = 0u;
      last_bbp_type = 0xFFFFFFFFu;

      fprintf(stdout, "      BEP :: IP = %8x\n", (x[ 1u ] >> 7u) & 0x01u);

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_BEP;
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

      curr_ptw_tsc_mtc = tsc_cnt / tsc_ratio + tsc_mtc_cnt;
      curr_ptw_cyc     = cyc_cnt;

#if defined(PRINT_PTW)
      fprintf(stdout,
              "      PTW :: %16lld :: %12llu %12lld :: %12llu %12lld\n",
              ptw,
              curr_ptw_cyc - prev_ptw_cyc,
              ((signed long long int) (curr_ptw_cyc - prev_ptw_cyc)) - ((signed long long int) (ptw)),
              (curr_ptw_tsc_mtc - prev_ptw_tsc_mtc) * tsc_ratio,
              ((signed long long int) ((curr_ptw_tsc_mtc - prev_ptw_tsc_mtc) * tsc_ratio)) - ((signed long long int) (ptw)));
#else
      (void) (ptw);
#endif

      prev_ptw_tsc_mtc = curr_ptw_tsc_mtc;
      prev_ptw_cyc     = curr_ptw_cyc;

      x += 10llu;
      n -= 10llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PTW;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_8 == MODE)) {
      const unsigned char mode    = (x[ 1u ] >> 0u) & 0x1Fu;
      const unsigned char leaf_id = (x[ 1u ] >> 5u) & 0x07u;

      if (leaf_id == 0u) {
        const unsigned char mode_csl_lma = (mode >> 0u) & 0x01u;
        const unsigned char mode_csd     = (mode >> 1u) & 0x01u;
        const unsigned char mode_if      = (mode >> 2u) & 0x01u;

        fprintf(stdout, "MODE_EXEC :: %u %u %u\n", mode_if, mode_csd, mode_csl_lma);
      } else if (leaf_id == 1u) {
        fprintf(stdout, " MODE_TSX\n");
      } else {
        fprintf(stdout, "     MODE\n");
      }

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_MODE;
      goto decode_again;
    }
    if ((n >= 2llu) && (*x_8 == MTC)) {
      curr_mtc = ((unsigned char) (x[ 1u ]));
      if (prev_mtc != 0u) {
        unsigned char diff_mtc = curr_mtc - prev_mtc;

        tsc_mtc_cnt += ((unsigned long long int) (diff_mtc));
      }

#if defined(PRINT_MTC)
      fprintf(stdout,
              "      MTC :: %16x %16llx %16llx\n",
              curr_mtc,
              tsc_mtc_cnt,
              tsc_cnt / tsc_ratio + tsc_mtc_cnt);
#endif

      prev_mtc = curr_mtc;

      x += 2llu;
      n -= 2llu;
      last_intel_pt_pkt = INTEL_PT_PKT_MTC;
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

      tsc_cnt = tsc;
      if (last_psb == 1u) {
        curr_mtc    = 0u;
        prev_mtc    = 0u;
        tsc_mtc_cnt = 0llu;

        prev_tsc_cyc = (tsc_cyc_t) {
          .tsc = tsc_cnt,
          .cyc = cyc_cnt
        };
      } else {
        curr_tsc_cyc = (tsc_cyc_t) {
          .tsc = tsc_cnt,
          .cyc = cyc_cnt
        };

        if ((prev_tsc_cyc.tsc != 0llu) && (prev_tsc_cyc.cyc != 0llu)) {
          double a = ((double) (curr_tsc_cyc.tsc - prev_tsc_cyc.tsc)) * tsc_factor;
          double b = ((double) (curr_tsc_cyc.cyc - prev_tsc_cyc.cyc)) * cbr_factor;

          if (a - b >= 1e-8) {
            inactive_time        = a - b;
            total_inactive_time += inactive_time;
            cyc_cnt             += inactive_time / cbr_factor;
          }
        } else {
          cyc_cnt = tsc_cnt;
        }

        prev_tsc_cyc = (tsc_cyc_t) {
          .tsc = tsc_cnt,
          .cyc = cyc_cnt
        };
      }

#if defined(PRINT_TSC)
      fprintf(stdout,
              "      TSC :: %16llx %16llx %12.6lf %12.6lf\n",
              tsc_cnt / tsc_ratio,
              cyc_cnt,
              inactive_time,
              total_inactive_time);
#endif

      x += 8llu;
      n -= 8llu;
      last_intel_pt_pkt = INTEL_PT_PKT_TSC;
      goto decode_again;
    }
    if ((n >= 1llu) && (*x_8 == PAD)) {
      fprintf(stdout, "      PAD\n");

      x += 1llu;
      n -= 1llu;
      last_intel_pt_pkt = INTEL_PT_PKT_PAD;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_PGE_MASK) == TIP_PGE)) {
      (void) ip_decode(&x, &n, "  TIP_PGE ::");

      last_intel_pt_pkt = INTEL_PT_PKT_TIP_PGE;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & FUP_MASK) == FUP)) {
      (void) ip_decode(&x, &n, "      FUP ::");

      last_intel_pt_pkt = INTEL_PT_PKT_FUP;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_MASK) == TIP)) {
      (void) ip_decode(&x, &n, "      TIP ::");

      last_intel_pt_pkt = INTEL_PT_PKT_TIP;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & TIP_PGD_MASK) == TIP_PGD)) {
      (void) ip_decode(&x, &n, "  TIP_PGD ::");

      last_intel_pt_pkt = INTEL_PT_PKT_TIP_PGD;
      goto decode_again;
    }
    if ((n >= 9llu) && (((*x_8) & BIP_MASK) == BIP) && (last_bbp == 1u)) {
      const unsigned int           bip_id  = (((unsigned int) (x[ 0u ])) >> 3u) & 0x1Fu;
      const unsigned long long int payload = (((unsigned long long int) (x[ 8u ])) << 56llu) |
                                             (((unsigned long long int) (x[ 7u ])) << 48llu) |
                                             (((unsigned long long int) (x[ 6u ])) << 40llu) |
                                             (((unsigned long long int) (x[ 5u ])) << 32llu) |
                                             (((unsigned long long int) (x[ 4u ])) << 24llu) |
                                             (((unsigned long long int) (x[ 3u ])) << 16llu) |
                                             (((unsigned long long int) (x[ 2u ])) <<  8llu) |
                                             (((unsigned long long int) (x[ 1u ])) <<  0llu);

#if defined(PRINT_BIP)
        fprintf(stdout, "      BIP :: ID = %8x :: PAYLOAD = %16llx\n", bip_id, payload);
        if ((last_bbp_type == 0x04u) && (bip_id == 0x00u)) {
          //xed_find_inst(payload, 0u);
        }

        if ((bip_id == 0x01u) && (payload == 0x100000001)) {
          tnt_enable = 1u;
        }
#endif

      x += 9llu;
      n -= 9llu;
      last_intel_pt_pkt = INTEL_PT_PKT_BIP;
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
          // Incomplete cyc data
          fprintf(stderr, "\tCYC error\n");
        }
      }
      // Accumulate active tracing time
      cyc_cnt += cyc;

#if defined(PRINT_CYC)
      fprintf(stdout,
              "      CYC :: %16llx %16llx\n",
              cyc,
              cyc_cnt);
#endif

      last_intel_pt_pkt = INTEL_PT_PKT_CYC;
      goto decode_again;
    }
    if ((n >= 1llu) && (((*x_8) & SHORT_TNT_MASK) == SHORT_TNT)) {
      unsigned int p_one     = 0u;
      unsigned int short_tnt = ((unsigned int) (x[ 0u ]));

      asm volatile ("bsr %1, %0": "=r"(p_one) : "r"(short_tnt) : );

#if defined(PRINT_SHORT_TNT)
      fprintf(stdout,
              "SHORT_TNT :: %02x :: %u %02x\n",
              short_tnt,
              p_one,
              (short_tnt >> 1u) & ((1u << (p_one - 1u)) - 1u));
#endif

      if (tnt_enable == 1u) {
        xed_process_branches((short_tnt >> 1u) & ((1u << (p_one - 1u)) - 1u), p_one - 1u);
      }

      x += 1llu;
      n -= 1llu;
      last_intel_pt_pkt = INTEL_PT_PKT_SHORT_TNT;
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