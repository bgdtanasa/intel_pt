#include <stdio.h>
#include <string.h>

#include "intel_pt.h"

#define PSB             (0x82028202u)    // 1000_0010_0000_0010_1000_0010_0000_0010
#define MNT             (0x88C302u)      //           1000_1000_1100_0011_0000_0010
#define OVF             (0xF302u)        //                     1111_0011_0000_0010
#define VMCS            (0xC802u)        //                     1100_1000_0000_0010
#define MWAIT           (0xC202u)        //                     1100_0010_0000_0010
#define LONG_TNT        (0xA302u)        //                     1010_0011_0000_0010
#define PWRX            (0xA202u)        //                     1010_0010_0000_0010
#define TRACESTOP       (0x8302u)        //                     1000_0011_0000_0010
#define TSC_MTC         (0x7302u)        //                     0111_0011_0000_0010
#define BBP             (0x6302u)        //                     0110_0011_0000_0010
#define EVD             (0x5302u)        //                     0101_0011_0000_0010
#define PIP             (0x4302u)        //                     0100_0011_0000_0010
#define PSBEND          (0x2302u)        //                     0010_0011_0000_0010
#define PWRE            (0x2202u)        //                     0010_0010_0000_0010
#define CFE             (0x1302u)        //                     0001_0011_0000_0010
#define CBR             (0x0302u)        //                     0000_0011_0000_0010
#define EXSTOP          (0x6202u)        //                      110_0010_0000_0010
#define BEP             (0x3302u)        //                      011_0011_0000_0010
#define PTW             (0x1202u)        //                        1_0010_0000_0010
#define MODE            (0x99u)          //                               1001_1001
#define MTC             (0x59u)          //                               0101_1001
#define TSC             (0x19u)          //                               0001_1001
#define PAD             (0x00u)          //                               0000_0000
#define TIP_PGE         (0x11u)          //                                  1_0001
#define FUP             (0x1Du)          //                                  1_1101
#define TIP             (0x0Du)          //                                  0_1101
#define TIP_PGD         (0x01u)          //                                  0_0001
#define BIP             (0x04u)          //                                     100
#define CYC             (0x03u)          //                                      11
#define SHORT_TNT       (0x00u)          //                               1xxx_xxx0

#define MNT_MASK        (0xFFFFFFu)
#define EXSTOP_MASK     (0x7FFFu)
#define BEP_MASK        (0x7FFFu)
#define PTW_MASK        (0x1FFFu)
#define TIP_PGE_MASK    (0x1Fu)
#define FUP_MASK        (0x1Fu)
#define TIP_MASK        (0x1Fu)
#define TIP_PGD_MASK    (0x1Fu)
#define BIP_MASK        (0x07u)
#define CYC_MASK        (0x03u)
#define SHORT_TNT_MASK  (0x01u)

//#define PRINT_CBR
#define PRINT_PTW
#define PRINT_TSC
#define PRINT_FUP
#define PRINT_CYC

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
static tsc_cyc_t      tsc_cyc;

static unsigned long long int last_ip;
static unsigned long long int last_tsc;
static unsigned long long int last_cyc_val;
static unsigned long long int last_cyc_cnt;
static double                 curr_ptw_ts = 0.0;
static double                 prev_ptw_ts = 0.0;

extern unsigned long long int tsc_hz;
extern unsigned long long int bus_hz;
unsigned long long int        cbr_hz;
double                        tsc_factor;
double                        cbr_factor;

static unsigned long long int ip_decode(const unsigned char** const x, unsigned long long int* const n) {
    const unsigned char*   x_p = *x;
    unsigned long long int n_p = *n;

    unsigned long long int ip             = 0llu;
    unsigned char          ip_bytes       = (x_p[ 0u ] >> 5u) & 0x07u;
    unsigned char          update_last_ip = 1u;

    x_p++;
    n_p--;
    switch (ip_bytes) {
        case 0u:
            update_last_ip = 0u;
        break;

        case 1u:
            if (n_p >= 2llu) {
                ip   = (last_ip & 0xFFFFFFFFFFFF0000llu)                |
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
                ip   = (last_ip & 0xFFFFFFFF00000000llu)                 |
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
                ip   = (((unsigned long long int) (x_p[ 5u ])) << 40llu) |
                       (((unsigned long long int) (x_p[ 4u ])) << 32llu) |
                       (((unsigned long long int) (x_p[ 3u ])) << 24llu) |
                       (((unsigned long long int) (x_p[ 2u ])) << 16llu) |
                       (((unsigned long long int) (x_p[ 1u ])) <<  8llu) |
                       (((unsigned long long int) (x_p[ 0u ])) <<  0llu);
                {
                    unsigned long long int ip_47_bit = (ip >> 47llu) & 0x01llu;
                    unsigned long long int ip_bit    = 0llu;

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
                ip   = (last_ip & 0xFFFF000000000000llu)                 |
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
                ip   = (((unsigned long long int) (x_p[ 7u ])) << 56llu) |
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
    return ip;
}

unsigned long long int intel_pt_decode(const unsigned char*   x,
                                       unsigned long long int n,
                                       const double           ts) {
    const unsigned char* const x_orig = x;

    unsigned int*        x_32;
    unsigned short int*  x_16;
    unsigned char*       x_8;

    tsc_factor = ((double) (tsc_hz)) / 1e9;
    //cbr_factor = 0.0;

    //for (unsigned long long int i = 0llu; i < n; i++) {
    //    fprintf(stdout, "%02x ", ((unsigned int) (x[ i ])));
    //}
    //fprintf(stdout, "\n");

decode_again:
    if (n >= 1llu) {
        //fprintf(stdout, "%12llu :: ", n);

        x_32 = ((unsigned int*) (x));
        x_16 = ((unsigned short int*) (x));
        x_8  = ((unsigned char*) (x));
        if ((n >= 4llu) && (*x_32 == PSB)) {
            last_ip = 0llu;
            fprintf(stdout, "      PSB\n");

            x += 4llu;
            n -= 4llu;
            last_intel_pt_pkt = INTEL_PT_PKT_PSB;
            goto decode_again;
        }
        if ((n >= 11llu) && (((*x_32) & MNT_MASK) == MNT)) {
            fprintf(stdout, "    MNT\n");

            x += 11llu;
            n -= 11llu;
            last_intel_pt_pkt = INTEL_PT_PKT_MNT;
            goto decode_again;
        }
        if ((n >= 2llu) && (*x_16 == OVF)) {
            fprintf(stdout, "    OVF\n");

            x += 2llu;
            n -= 2llu;
            last_intel_pt_pkt = INTEL_PT_PKT_OVF;
            goto decode_again;
        }
        if ((n >= 7llu) && (*x_16 == VMCS)) {
            fprintf(stdout, "   VMCS\n");

            x += 7llu;
            n -= 7llu;
            last_intel_pt_pkt = INTEL_PT_PKT_VMCS;
            goto decode_again;
        }
        if ((n >= 10llu) && (*x_16 == MWAIT)) {
            fprintf(stdout, "  MWAIT\n");

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
            fprintf(stdout, "   PWRX\n");

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
            fprintf(stdout, "TSC_MTC\n");

            x += 7llu;
            n -= 7llu;
            last_intel_pt_pkt = INTEL_PT_PKT_TSC_MTC;
            goto decode_again;
        }
        if ((n >= 3llu) && (*x_16 == BBP)) {
            fprintf(stdout, "BBP\n");

            x += 3llu;
            n -= 3llu;
            last_intel_pt_pkt = INTEL_PT_PKT_BBP;
            goto decode_again;
        }
        if ((n >= 11llu) && (*x_16 == EVD)) {
            fprintf(stdout, "EVD\n");

            x += 11llu;
            n -= 11llu;
            last_intel_pt_pkt = INTEL_PT_PKT_EVD;
            goto decode_again;
        }
        if ((n >= 8llu) && (*x_16 == PIP)) {
            //fprintf(stdout, "      PIP\n");

            x += 8llu;
            n -= 8llu;
            last_intel_pt_pkt = INTEL_PT_PKT_PIP;
            goto decode_again;
        }
        if ((n >= 2llu) && (*x_16 == PSBEND)) {
            fprintf(stdout, "   PSBEND\n");

            x += 2llu;
            n -= 2llu;
            last_intel_pt_pkt = INTEL_PT_PKT_PSBEND;
            goto decode_again;
        }
        if ((n >= 4llu) && (*x_16 == PWRE)) {
            fprintf(stdout, "PWRE\n");

            x += 4llu;
            n -= 4llu;
            last_intel_pt_pkt = INTEL_PT_PKT_PWRE;
            goto decode_again;
        }
        if ((n >= 4llu) && (*x_16 == CFE)) {
            fprintf(stdout, "CFE\n");

            x += 4llu;
            n -= 4llu;
            last_intel_pt_pkt = INTEL_PT_PKT_CFE;
            goto decode_again;
        }
        if ((n >= 4llu) && (*x_16 == CBR)) {
            cbr_hz     = ((unsigned long long int) (x[ 2u ])) * bus_hz;
            cbr_factor = ((double) (cbr_hz)) / 1e9;

#if defined(PRINT_CBR)
            fprintf(stdout, "      CBR = %12llu Hz :: %12.5lf\n", cbr_hz, cbr_factor);
#endif

            x += 4llu;
            n -= 4llu;
            last_intel_pt_pkt = INTEL_PT_PKT_CBR;
            goto decode_again;
        }
        if ((n >= 2llu) && (((*x_16) & EXSTOP_MASK) == EXSTOP)) {
            fprintf(stdout, "EXSTOP\n");

            x += 2llu;
            n -= 2llu;
            last_intel_pt_pkt = INTEL_PT_PKT_EXSTOP;
            goto decode_again;
        }
        if ((n >= 2llu) && (((*x_16) & BEP_MASK) == BEP)) {
            fprintf(stdout, "BEP\n");

            x += 2llu;
            n -= 2llu;
            last_intel_pt_pkt = INTEL_PT_PKT_BEP;
            goto decode_again;
        }
        if ((n >= 10llu) && (((*x_16) & PTW_MASK) == PTW)) {
            unsigned long long int ptw = (((unsigned long long int) (x[ 9u ])) << 56llu) |
                                         (((unsigned long long int) (x[ 8u ])) << 48llu) |
                                         (((unsigned long long int) (x[ 7u ])) << 40llu) |
                                         (((unsigned long long int) (x[ 6u ])) << 32llu) |
                                         (((unsigned long long int) (x[ 5u ])) << 24llu) |
                                         (((unsigned long long int) (x[ 4u ])) << 16llu) |
                                         (((unsigned long long int) (x[ 3u ])) <<  8llu) |
                                         (((unsigned long long int) (x[ 2u ])) <<  0llu);

            curr_ptw_ts = ((double) (tsc_cyc.tsc)) / tsc_factor + ((double) (tsc_cyc.cyc + last_cyc_cnt)) / cbr_factor;
#if defined(PRINT_PTW)
            fprintf(stdout,
                    "      PTW = %016llx :: %12llu %12llu :: %12.5lf\n",
                    ptw,
                    tsc_cyc.tsc,
                    tsc_cyc.cyc + last_cyc_cnt,
                    (prev_ptw_ts != 0.0) ? (curr_ptw_ts - prev_ptw_ts) : (0.0));
#else
            (void) (ptw);
#endif
            prev_ptw_ts = curr_ptw_ts;

            x += 10llu;
            n -= 10llu;
            last_intel_pt_pkt = INTEL_PT_PKT_PTW;
            goto decode_again;
        }
        if ((n >= 2llu) && (*x_8 == MODE)) {
            unsigned char mode    = (x[ 1u ] >> 0u) & 0x1Fu;
            unsigned char leaf_id = (x[ 1u ] >> 5u) & 0x07u;
            if (leaf_id == 0u) {
                unsigned char mode_csl_lma = (mode >> 0u) & 0x01u;
                unsigned char mode_csd     = (mode >> 1u) & 0x01u;
                unsigned char mode_if      = (mode >> 2u) & 0x01u;

                fprintf(stdout, "MODE_EXEC = %u %u %u\n", mode_if, mode_csd, mode_csl_lma);
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
            fprintf(stdout, "MTC\n");

            x += 2llu;
            n -= 2llu;
            last_intel_pt_pkt = INTEL_PT_PKT_MTC;
            goto decode_again;
        }
        if ((n >= 8llu) && (*x_8 == TSC)) {
            unsigned long long int tsc = (((unsigned long long int) (x[ 7u ])) << 48llu) |
                                         (((unsigned long long int) (x[ 6u ])) << 40llu) |
                                         (((unsigned long long int) (x[ 5u ])) << 32llu) |
                                         (((unsigned long long int) (x[ 4u ])) << 24llu) |
                                         (((unsigned long long int) (x[ 3u ])) << 16llu) |
                                         (((unsigned long long int) (x[ 2u ])) <<  8llu) |
                                         (((unsigned long long int) (x[ 1u ])) <<  0llu);
            last_tsc = tsc;
            if (last_intel_pt_pkt == INTEL_PT_PKT_CYC) {
                tsc_cyc.tsc = last_tsc;
                tsc_cyc.cyc = last_cyc_val;

                last_cyc_val = 0llu;
                last_cyc_cnt = 0llu;
            }

#if defined(PRINT_TSC)
            fprintf(stdout,
                    "      TSC = %16s :: %12llu %12llu :: %12.5lf\n",
                    "",
                    tsc_cyc.tsc,
                    tsc_cyc.cyc,
                    (cbr_factor != 0.0) ? (((double) (tsc_cyc.tsc)) / tsc_factor + ((double) (tsc_cyc.cyc)) / cbr_factor) : (0.0));
#endif

            x += 8llu;
            n -= 8llu;
            last_intel_pt_pkt = INTEL_PT_PKT_TSC;
            goto decode_again;
        }
        if ((n >= 1llu) && (*x_8 == PAD)) {
            fprintf(stdout, "PAD\n");

            x += 1llu;
            n -= 1llu;
            last_intel_pt_pkt = INTEL_PT_PKT_PAD;
            goto decode_again;
        }
        if ((n >= 1llu) && (((*x_8) & TIP_PGE_MASK) == TIP_PGE)) {
            fprintf(stdout, "  TIP_PGE = %016llx\n", ip_decode(&x, &n));

            last_intel_pt_pkt = INTEL_PT_PKT_TIP_PGE;
            goto decode_again;
        }
        if ((n >= 1llu) && (((*x_8) & FUP_MASK) == FUP)) {
            unsigned long long int fup = ip_decode(&x, &n);

#if defined(PRINT_FUP)
            fprintf(stdout, "      FUP = %016llx\n", fup);
#else
            (void) (fup);
#endif

            last_intel_pt_pkt = INTEL_PT_PKT_FUP;
            goto decode_again;
        }
        if ((n >= 1llu) && (((*x_8) & TIP_MASK) == TIP)) {
            fprintf(stdout, "      TIP = %016llx\n", ip_decode(&x, &n));

            last_intel_pt_pkt = INTEL_PT_PKT_TIP;
            goto decode_again;
        }
        if ((n >= 1llu) && (((*x_8) & TIP_PGD_MASK) == TIP_PGD)) {
            fprintf(stdout, "  TIP_PGD = %016llx\n", ip_decode(&x, &n));

            last_intel_pt_pkt = INTEL_PT_PKT_TIP_PGD;
            goto decode_again;
        }
        // BIP
        if ((n >= 1llu) && (((*x_8) & CYC_MASK) == CYC)) {
            unsigned long long int i   = 0u;
            unsigned long long int cyc = (x[ 0u ] >> 3u) & 0x1Fu;
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
                }
            }
            last_cyc_val  = cyc;
            last_cyc_cnt += cyc;

#if defined(PRINT_CYC)
            fprintf(stdout,
                    "      CYC = %12llu/%12.5lf %12llu/%12.5lf\n",
                    last_cyc_val,
                    ((double) (last_cyc_val)) / cbr_factor,
                    last_cyc_cnt,
                    ((double) (last_cyc_cnt)) / cbr_factor);
#endif

            last_intel_pt_pkt = INTEL_PT_PKT_CYC;
            goto decode_again;
        }
        if ((n >= 1llu) && (((*x_8) & SHORT_TNT_MASK) == SHORT_TNT)) {
            fprintf(stdout, "SHORT_TNT\n");

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