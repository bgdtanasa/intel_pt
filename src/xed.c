#include "xed.h"
#include "intel_pt.h"
#if defined(EN_PMU)
#include "pmu.h"
#endif
#include "proc.h"
#if defined(EN_PTRACE_BRK)
#include "brk.h"
#endif
#if defined(EN_PTRACE_UNWIND)
#include "x_unwind.h"
#endif
#include "kmod.h"
#include "utils.h"
#include "x_elf.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/types.h>

#include "xed-build-defines.h"
#include "xed-common-defs.h"
#include "xed-common-hdrs.h"

#include "xed-init.h"
#include "xed-chip-features.h"
#include "xed-decode.h"
#include "xed-decoded-inst.h"
#include "xed-decoded-inst-api.h"

#define CACHE_NO_BITS (20u)
#define CACHE_MASK    ((1 << (CACHE_NO_BITS)) - 1u)
#define CACHE_LENGTH  ((CACHE_MASK) + 1u)

#define CF(eflags) ((eflags >>  0llu) & 0x01llu)
#define PF(eflags) ((eflags >>  2llu) & 0x01llu)
#define AF(eflags) ((eflags >>  4llu) & 0x01llu)
#define ZF(eflags) ((eflags >>  6llu) & 0x01llu)
#define SF(eflags) ((eflags >>  7llu) & 0x01llu)
#define OF(eflags) ((eflags >> 11llu) & 0x01llu)

#define CALL_STACK_LEN (1u * 256u)
#define TNT_QUEUE_LEN  (1u * 1024u)
#define TIP_QUEUE_LEN  (1u * 1024u)

typedef struct {
#if defined(EN_RET_COMPRESSION)
  unsigned int en_ret_compression;
#endif

  unsigned int           tnt;
  unsigned int           tnt_len;
  double                 tsc;
  unsigned long long int cyc_cnt;
} tnt_t;

typedef struct {
  unsigned long long int tip;
  double                 tsc;
  unsigned long long int cyc_cnt;
} tip_t;

typedef struct {
  unsigned long long int async_src;
  unsigned long long int async_dst;

  signed long long last_inst;

  call_stack_t     call_stack[ CALL_STACK_LEN ];
  unsigned int     call_stack_idx;

  tnt_t            tnt_queue[ TNT_QUEUE_LEN ];
  unsigned int     tnt_queue_head;
  unsigned int     tnt_queue_tail;

  tip_t            tip_queue[ TIP_QUEUE_LEN ];
  unsigned int     tip_queue_head;
  unsigned int     tip_queue_tail;
} ctx_t;

typedef struct {
  unsigned int           iclass_cnt[ XED_ICLASS_LAST ];
  unsigned long long int cyc_cnt_ref;

  double       t_ipc;
  unsigned int n_ipc;

  unsigned long long int t_cnts;
  unsigned long long int t_cycs;
} inst_stats_t;

#if defined(EN_PTRACE_UNWIND)
dwarf_unwind_t*    dwarf_unwinds;
unsigned long long no_dwarf_unwinds;
#endif
char               binaries[ MAX_NO_BINARIES ][ MAX_BINARY_LENGTH ];
unsigned int       no_binaries;
inst_t*            insts;
unsigned long long no_insts;
unsigned long long no_insts_vmlinux;

#if defined(EN_SW_UTIL)
sw_util_t    sw_util_queue[ SW_UTIL_QUEUE_LEN ];
unsigned int sw_util_queue_head;
unsigned int sw_util_queue_tail;
#endif

static ctx_t        ctx[ MAX_NO_CTXS ] = {
  [ 0u ] = {
    .last_inst = -1ll
  },
  [ 1u ] = {
    .last_inst = -1ll
  },
  [ 2u ] = {
    .last_inst = -1ll
  },
  [ 3u ] = {
    .last_inst = -1ll
  },
  [ 4u ] = {
    .last_inst = -1ll
  }
};
static ctx_t*       this_ctx           = &ctx[ 0u ];
static unsigned int ctx_idx            = 0u;
static unsigned long long int this_pgd = 0llu;

static FILE*                  branches_fp;
static unsigned long long int branches_n;

static xed_chip_features_t chip_features;
static inst_stats_t        inst_stats;

#if defined(EN_VMLINUX)
static const inst_t* unwind_cache_high[ CACHE_LENGTH ];
#endif
static const inst_t* unwind_cache_low[ CACHE_LENGTH ];

extern double tsc_hz_ns;
extern double tsc_factor;
extern double cbr_factor;

#if defined(EN_RET_COMPRESSION)
static unsigned int en_ret_compression = 0u;
#endif

static inline __attribute__((always_inline)) void add_tnt(const unsigned int           tnt,
                                                          const unsigned int           tnt_len,
                                                          const double                 tsc,
                                                          const unsigned long long int cyc_cnt) {
  if (tnt_len >= 1u) {
    const unsigned int tnt_queue_head_next = (this_ctx->tnt_queue_head + 1u) % TNT_QUEUE_LEN;

    if (tnt_queue_head_next != this_ctx->tnt_queue_tail) {
      this_ctx->tnt_queue[ this_ctx->tnt_queue_head ] = (tnt_t) {
#if defined(EN_RET_COMPRESSION)
        .en_ret_compression = en_ret_compression,
#endif

        .tnt     = tnt,
        .tnt_len = tnt_len,
        .tsc     = tsc,
        .cyc_cnt = cyc_cnt
      };
      this_ctx->tnt_queue_head                        = tnt_queue_head_next;
    } else {
      fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "BR Queue full!\n"); for (;;) {}
    }
  }
}

static inline __attribute__((always_inline)) void add_tip(const unsigned long long int tip,
                                                          const double                 tsc,
                                                          const unsigned long long int cyc_cnt) {
  if (tip != 0llu) {
    if (tip > insts[ no_insts - 1llu ].addr) {
      fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "TIP out of bounds :: %16llx!\n", tip); for (;;) {}
    }
    const unsigned int tip_queue_head_next = (this_ctx->tip_queue_head + 1u) % TIP_QUEUE_LEN;

    if (tip_queue_head_next != this_ctx->tip_queue_tail) {
      this_ctx->tip_queue[ this_ctx->tip_queue_head ] = (tip_t) {
        .tip     = tip,
        .tsc     = tsc,
        .cyc_cnt = cyc_cnt
      };
      this_ctx->tip_queue_head                        = tip_queue_head_next;
    } else {
      fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "TIP Queue full!\n"); for (;;) {}
    }
  }
}

static void add_inst(const unsigned char* const   inst_bytes,
                     xed_decoded_inst_t* const    xedd,
                     const char* const            binary,
                     const unsigned long long int base_addr,
                     const unsigned long long int addr) {
  const xed_category_enum_t xedd_category       = xed_decoded_inst_get_category(xedd);
  const xed_iclass_enum_t   xedd_iclass         = xed_decoded_inst_get_iclass(xedd);
  const xed_uint_t          xedd_length         = xed_decoded_inst_get_length(xedd);
  const xed_inst_t*         xedd_dec            = xed_decoded_inst_inst(xedd);
  const unsigned int        xedd_dec_no_ops     = xed_decoded_inst_noperands(xedd);
  const xed_uint_t          xedd_dec_no_mem_ops = xed_decoded_inst_number_of_memory_operands(xedd);

  if (no_insts >= MAX_NO_INSTS) {
    fprintf(stderr, "Not enough space to load the instructions\n"); for (;;) {}
  }
  memset(&insts[ no_insts ], 0, sizeof(inst_t));
  insts[ no_insts ].binary    = parse_get_binary(binary, 1u);
  insts[ no_insts ].base_addr = base_addr;
  insts[ no_insts ].addr      = addr;
  insts[ no_insts ].category  = xedd_category;
  insts[ no_insts ].iclass    = xedd_iclass;
  insts[ no_insts ].length    = xedd_length;
  memcpy(&insts[ no_insts ].inst_bytes[ 0u ], inst_bytes, xedd_length);

  if (xed_inst_get_attribute(xedd_dec, XED_ATTRIBUTE_FAR_XFER) == 1u) {
    insts[ no_insts ].cofi.type |= FAR_TRANSFER;
  }
  if (xedd_category == XED_CATEGORY_CALL) {
    if (xedd_iclass == XED_ICLASS_CALL_FAR) {
      insts[ no_insts ].cofi.type |= FAR_TRANSFER;
    } else if (xedd_iclass == XED_ICLASS_CALL_NEAR) {
      if (xed_inst_get_attribute(xedd_dec, XED_ATTRIBUTE_INDIRECT_BRANCH) == 1u) {
        insts[ no_insts ].cofi.type |= INDIRECT_BRANCH;
      } else {
        insts[ no_insts ].cofi.type |= UNCOND_DIRECT_BRANCH;
      }
    }

    if (xedd_dec_no_mem_ops >= 1u) {
      if (xed_operand_values_has_memory_displacement(xedd)) {
        const xed_reg_enum_t xed_reg = xed_decoded_inst_get_base_reg(xedd, 0u);

        if (xed_reg == XED_REG_RIP) {
        } else {
          // What to do in this case?!
        }
      } else {
        // What to do in this case?!
      }
    }
    if (xedd_dec_no_ops >= 1u) {
      const xed_operand_enum_t xed_op = xed_operand_name(xed_inst_operand(xedd_dec, 0u));

      if ((XED_OPERAND_REG0 <= xed_op) && (xed_op <= XED_OPERAND_REG9)) {
        // What to do in this case?!
      } else if (xed_op == XED_OPERAND_RELBR) {
        insts[ no_insts ].cofi.u.c.addr = addr + xed_decoded_inst_get_branch_displacement(xedd) + ((int64_t) (xedd_length));
      } else {
        // What to do in this case?!
      }
    }
    if (xedd_iclass == XED_ICLASS_CALL_NEAR) {
      insts[ no_insts ].cofi.u.c.ret_to.addr = addr + ((int64_t) (xedd_length));
    }
  } else if ((xedd_category == XED_CATEGORY_COND_BR) || (xedd_category == XED_CATEGORY_UNCOND_BR)) {
    if (xedd_iclass == XED_ICLASS_JMP_FAR) {
      insts[ no_insts ].cofi.type |= FAR_TRANSFER;
    } else {
      if (xed_inst_get_attribute(xedd_dec, XED_ATTRIBUTE_INDIRECT_BRANCH) == 1u) {
        insts[ no_insts ].cofi.type |= INDIRECT_BRANCH;
      } else {
        insts[ no_insts ].cofi.type |= (xedd_category == XED_CATEGORY_COND_BR)   ? (COND_BRANCH)          : (0u);
        insts[ no_insts ].cofi.type |= (xedd_category == XED_CATEGORY_UNCOND_BR) ? (UNCOND_DIRECT_BRANCH) : (0u);
      }
    }

    if (xedd_dec_no_mem_ops >= 1u) {
      if (xed_operand_values_has_memory_displacement(xedd)) {
        const xed_reg_enum_t xed_reg = xed_decoded_inst_get_base_reg(xedd, 0u);

        if (xed_reg == XED_REG_RIP) {
        } else {
          // What to do in this case?!
        }
      } else {
        // What to do in this case?!
      }
    }
    if (xedd_dec_no_ops >= 1u) {
      const xed_operand_enum_t xed_op = xed_operand_name(xed_inst_operand(xedd_dec, 0u));

      if ((XED_OPERAND_REG0 <= xed_op) && (xed_op <= XED_OPERAND_REG9)) {
        // What to do in this case?!
      } else if (xed_op == XED_OPERAND_RELBR) {
        insts[ no_insts ].cofi.u.j.addr = addr + xed_decoded_inst_get_branch_displacement(xedd) + ((int64_t) (xedd_length));
      } else {
        // What to do in this case?!
      }
    }
  } else if (xedd_category == XED_CATEGORY_RET) {
    if ((xedd_iclass == XED_ICLASS_IRET)  ||
        (xedd_iclass == XED_ICLASS_IRETD) ||
        (xedd_iclass == XED_ICLASS_IRETQ) ||
        (xedd_iclass == XED_ICLASS_RET_FAR)) {
      insts[ no_insts ].cofi.type |= FAR_TRANSFER;
    } else if (xedd_iclass == XED_ICLASS_RET_NEAR) {
      insts[ no_insts ].cofi.type |= INDIRECT_BRANCH;
    } else {
      fprintf(stderr, "Invalid COFI %u\n", insts[ no_insts ].cofi.type); for (;;) {}
    }
  } else if ((xedd_category == XED_CATEGORY_INTERRUPT) ||
             (xedd_category == XED_CATEGORY_SYSCALL)   ||
             (xedd_category == XED_CATEGORY_SYSRET)    ||
             (xedd_iclass == XED_ICLASS_INT1)          ||
             (xedd_iclass == XED_ICLASS_INT3)          ||
             (xedd_iclass == XED_ICLASS_INTO)          ||
             (xedd_iclass == XED_ICLASS_SYSENTER)      ||
             (xedd_iclass == XED_ICLASS_SYSEXIT)       ||
             (xedd_iclass == XED_ICLASS_VMLAUNCH)      ||
             (xedd_iclass == XED_ICLASS_VMRESUME)) {
    insts[ no_insts ].cofi.type |= FAR_TRANSFER;
  }

  if ((insts[ no_insts ].cofi.type != 0u)                   &&
      (insts[ no_insts ].cofi.type != COND_BRANCH)          &&
      (insts[ no_insts ].cofi.type != UNCOND_DIRECT_BRANCH) &&
      (insts[ no_insts ].cofi.type != INDIRECT_BRANCH)      &&
      (insts[ no_insts ].cofi.type != FAR_TRANSFER)) {
    fprintf(stderr, "Invalid COFI %u\n", insts[ no_insts ].cofi.type); for (;;) {}
  }
#if defined(EN_PTRACE_BRK)
  install_brk(&insts[ no_insts ]);
#endif
  no_insts++;
}

static void print_call_stack(FILE* const fp) {
  for (signed int i = ((signed int) (ctx_idx)); i >= 0; i--) {
    for (signed int j = ((signed int) (ctx[ i ].call_stack_idx - 1u)); j >= 0 ; j--) {
      fprintf(fp,
              "%2d/%2d :: %16llx -> %16llx -> %16llx :: %20.2lf %20.2lf :: %u\n",
              j,
              i,
              ctx[ i ].call_stack[ j ].call->addr,
              ctx[ i ].call_stack[ j ].t_call,
              ctx[ i ].call_stack[ j ].ret->addr,
              ctx[ i ].call_stack[ j ].tsc_c,
              ctx[ i ].call_stack[ j ].tsc_r,
#if defined(EN_RET_COMPRESSION)
              ctx[ i ].call_stack[ j ].en_ret_compression);
#else
              0u);
#endif
    }
  }
}

static inline __attribute__((always_inline)) void reset_call_stack(void) {
  memset(&this_ctx->call_stack[ 0u ], 0, sizeof(this_ctx->call_stack));
  this_ctx->call_stack_idx = 0u;
}

static inline __attribute__((always_inline)) void reset_last_inst(void) {
  this_ctx->last_inst = -1ll;

  this_ctx->tnt_queue_head = this_ctx->tnt_queue_tail = 0u;
  this_ctx->tip_queue_head = this_ctx->tip_queue_tail = 0u;
}

static inline __attribute__((always_inline)) void update_inst_stats(const xed_iclass_enum_t iclass) {
  inst_stats.iclass_cnt[ iclass ]++;
}

static void reset_inst_stats(const unsigned long long int cyc_cnt) {
#if 0
  const double ipc_0 = (inst_stats.n_ipc  >= 1u) ? (inst_stats.t_ipc  / ((double) (inst_stats.n_ipc)))  : (0.0);
  const double ipc_1 = (inst_stats.t_cycs >= 1u) ? (inst_stats.t_cnts / ((double) (inst_stats.t_cycs))) : (0.0);

  fprintf(branches_fp, "Eq e :: ####### %12.3lf %12.3lf\n", ipc_0, ipc_1);
  fprintf(branches_fp, "Eq b :: #######\n");
  for (xed_iclass_enum_t i = XED_ICLASS_INVALID; i < XED_ICLASS_LAST; i++) {
    inst_stats.iclass_cnt[ i ] = 0u;
  }
  inst_stats.cyc_cnt_ref = cyc_cnt;
  inst_stats.t_ipc       = 0.0;
  inst_stats.n_ipc       = 0u;
  inst_stats.t_cnts      = 0llu;
  inst_stats.t_cycs      = 0llu;
#else
  (void) (cyc_cnt);
#endif
}

static void print_inst_stats(const unsigned long long int cyc_cnt) {
  unsigned int                 cnts = 0u;
  const unsigned long long int cycs = cyc_cnt - inst_stats.cyc_cnt_ref;
  double                       ipc  = 0.0;

  if (cycs >= 1llu) {
    //fprintf(branches_fp, "Eq %u :: ", ctx_idx);
    for (xed_iclass_enum_t i = XED_ICLASS_INVALID; i < XED_ICLASS_LAST; i++) {
      //if ((i >= XED_ICLASS_NOP) && (i <= XED_ICLASS_NOP9)) {
      //  continue;
      //} else {
        const unsigned int cnt = inst_stats.iclass_cnt[ i ];

        cnts += cnt;
        if (cnt >= 1u) {
          //fprintf(branches_fp, "%4u * x_%04u + ", cnt, i);
        }
      //}
      inst_stats.iclass_cnt[ i ] = 0u;
    }
    ipc = ((double) (cnts)) / ((double) (cycs));

    inst_stats.cyc_cnt_ref  = cyc_cnt;
    inst_stats.t_ipc       += ipc;
    inst_stats.n_ipc       += 1u;
    inst_stats.t_cnts      += cnts;
    inst_stats.t_cycs      += cycs;
    //fprintf(branches_fp, "\b\b= %6llu :: %12.2lf\n", cycs, ipc);
  }
}

const char* parse_get_binary(const char* const xed_file, const unsigned int add_file) {
  for (unsigned int i = 0u; i < no_binaries; i++) {
    if (strcmp(binaries[ i ], xed_file) == 0) {
      return &binaries[ i ][ 0u ];
    }
  }
  if (add_file == 1u) {
    strcpy(&binaries[ no_binaries ][ 0u ], xed_file);
    no_binaries++;
    if (no_binaries >= MAX_NO_BINARIES) {
      fprintf(stderr, "Not enough space to load the binaries\n"); for (;;) {}
    }

    return &binaries[ no_binaries - 1u ][ 0u ];
  } else {
    return NULL;
  }
}

#if defined(EN_PTRACE_UNWIND)
void parse_dwarf(const char* const xed_file, const unsigned long long int base_addr) {
  char dwarf_file[ 256u ];

  if (strstr(xed_file, "stack") != NULL) {
    fprintf(stdout, "\n");
    return;
  }
  if (strstr(xed_file, "vsyscall") != NULL) {
    fprintf(stdout, "\n");
    return;
  }

  dwarf_file[ 0u ] = '\0';
  if (xed_file != NULL) {
    sprintf(&dwarf_file[ 0u ], "resources/dwarfs/dwarf.%s", xed_file);
  }

  FILE* const fp = fopen(dwarf_file, "r");
  if (fp != NULL) {
    char buffer_0[ MAX_DWARF_LINE ];
    char buffer_1[ MAX_DWARF_LINE ];

    for (;;) {
      unsigned long long int addr           = 0llu;
      unsigned int           cfa_reg        = 0u;
      signed int             cfa_reg_offset = 0;
      unsigned int           reg_idx        = 0;
      signed int             reg_idx_cfa    = 0;
      unsigned int           reg_idx_reg    = 0u;
      dwarf_reg_t            regs[ MAX_NO_REGS ];

      char* a = fgets(&buffer_0[ 0u ], ((int) (sizeof(buffer_0))), fp);
      char* b = &buffer_1[ 0u ];
      char* c = NULL;

      if (a != NULL) {
        int n = sscanf(a, "  0x%llx: CFA=R%u+%d: %[^\n]", &addr, &cfa_reg, &cfa_reg_offset, b);

        if (n != 4) {
          n = sscanf(a, "  0x%llx: CFA=R%u: %[^\n]", &addr, &cfa_reg, b);
          if (n != 3) {
            n = sscanf(a, "  0x%llx: CFA=DW_OP_%[^\n]", &addr, b);
            if (n == 2) {
              dwarf_unwinds[ no_dwarf_unwinds ].base_addr = base_addr;
              dwarf_unwinds[ no_dwarf_unwinds ].addr      = base_addr + addr;
              dwarf_unwinds[ no_dwarf_unwinds ].cfa.rule  = CFA_RULE_EXP;
              strcpy(b, parse_dwarf_exp(b, ':', 2, &dwarf_unwinds[ no_dwarf_unwinds ].cfa.s.exp));
            } else {
              fprintf(stderr, "0 dwarf_cfa_err\n"); for (;;) {}
            }
          } else {
            dwarf_unwinds[ no_dwarf_unwinds ].base_addr        = base_addr;
            dwarf_unwinds[ no_dwarf_unwinds ].addr             = base_addr + addr;
            dwarf_unwinds[ no_dwarf_unwinds ].cfa.rule         = CFA_RULE_REG;
            dwarf_unwinds[ no_dwarf_unwinds ].cfa.s.reg        = cfa_reg;
            dwarf_unwinds[ no_dwarf_unwinds ].cfa.s.reg_offset = 0;
          }
        } else {
          dwarf_unwinds[ no_dwarf_unwinds ].base_addr        = base_addr;
          dwarf_unwinds[ no_dwarf_unwinds ].addr             = base_addr + addr;
          dwarf_unwinds[ no_dwarf_unwinds ].cfa.rule         = CFA_RULE_REG;
          dwarf_unwinds[ no_dwarf_unwinds ].cfa.s.reg        = cfa_reg;
          dwarf_unwinds[ no_dwarf_unwinds ].cfa.s.reg_offset = cfa_reg_offset;
        }
        c = a; a = b; b = c; b[ 0u ] = '\0';

        memset(&regs[ 0u ], 0, sizeof(regs));
reg_again:
        n = sscanf(a, "R%u=[DW_OP_%[^\n]", &reg_idx, b);
        if (n == 2) {
          regs[ reg_idx ] = (dwarf_reg_t) {
            .rule  = REG_RULE_EXP
          };
          strcpy(b, parse_dwarf_exp(b, ']', 1, &regs[ reg_idx ].exp));
        } else {
          n = sscanf(a, "R%u=[CFA%d]%[^\n]", &reg_idx, &reg_idx_cfa, b);
          if ((n == 3) || ((n == 2) && (b[ 0u ] == '\0'))) {
            regs[ reg_idx ] = (dwarf_reg_t) {
              .rule  = REG_RULE_CFA,
              .u     = {
                .cfa = reg_idx_cfa
              }
            };
          } else if (n <= 2) {
            n = sscanf(a, "R%u=R%u%[^\n]", &reg_idx, &reg_idx_reg, b);
            if ((n == 3) || ((n == 2) && (b[ 0u ] == '\0'))) {
              regs[ reg_idx ] = (dwarf_reg_t) {
                .rule  = REG_RULE_REG,
                .u     = {
                  .reg = reg_idx_reg
                }
              };
            } else if (n <= 1) {
              n = sscanf(a, "R%u=X%[^\n]", &reg_idx, b);
              if ((n == 2) || ((n == 1) && (b[ 0u ] == '\0'))) {
                regs[ reg_idx ] = (dwarf_reg_t) {
                  .rule  = REG_RULE_UNDEFINED
                };
              } else {
                n = sscanf(a, "XMM%u=[CFA%d]%[^\n]", &reg_idx, &reg_idx_cfa, b);
                if ((n == 3) || ((n == 2) && (b[ 0u ] == '\0'))) {
                  // Ignore XMM registers for the moment
                } else {
                  fprintf(stderr, "0 dwarf_reg_err\n"); for (;;) {}
                }
              }
            }
          } else {
            fprintf(stderr, "1 dwarf_reg_err\n"); for (;;) {}
          }
        }
        c = a; a = b; b = c; b[ 0u ] = '\0';
        if (a[ 0u ] == ',') {
          a += 2;
          goto reg_again;
        }

        if (no_dwarf_unwinds >= MAX_NO_DWARF_UNWINDS) {
          fprintf(stderr, "Not enough space to load the unwinds\n"); for (;;) {}
        }
        memcpy(&dwarf_unwinds[ no_dwarf_unwinds ].regs[ 0u ], &regs[ 0u ], sizeof(regs));
        no_dwarf_unwinds++;
      } else {
        break;
      }
    }

    fprintf(stdout, "no_dwarf_unwinds = %9llu :: ", no_dwarf_unwinds);
    fprintf(stdout, "no_binaries = %9u :: ", no_binaries);
    fclose(fp);
  } else {
    fprintf(stderr,
            "fopen(%s) failed :: %s\n",
            dwarf_file,
            strerror(errno)); for (;;) {}
  }
}
#endif

void parse_objdump(const int perfed_pid __attribute__((unused)), const char* const xed_file, const unsigned long long int base_addr) {
  char obj_file[ 256u ];

  if (xed_file == NULL) {
    return;
  }

  if (strstr(xed_file, "stack") != NULL) {
    return;
  }
  if (strstr(xed_file, "vsyscall") != NULL) {
    return;
  }

  obj_file[ 0u ] = '\0';
  sprintf(&obj_file[ 0u ], "resources/objdumps/objdump.%s", xed_file);

  FILE* const fp = fopen(obj_file, "r");
  if (fp != NULL) {
    char buffer[ 256u ];

    for (;;) {
      xed_uint8_t            inst_bytes[ XED_MAX_INSTRUCTION_BYTES ];
      unsigned int           n_bytes;
      unsigned int           byte;
      xed_decoded_inst_t     xedd;
      xed_error_enum_t       xed_error;
      unsigned long long int addr;
      const char*            line;

      memset(&inst_bytes[ 0u ], 0, sizeof(inst_bytes));
      n_bytes   = 0u;
      xed_error = XED_ERROR_NONE;
      addr      = 0llu;

xed_decode_inst:
      line      = fgets(&buffer[ 0u ], ((int) (sizeof(buffer))), fp);
      if (line != NULL) {
        if (xed_error == XED_ERROR_NONE) {
          sscanf(line, "%llx", &addr);
          addr += base_addr;
        } else {
#if 0
          fprintf(stderr, "%016llx %3d :: %2u :: ", addr, xed_error, n_bytes);
          for (unsigned int i = 0u; i < n_bytes; i++) {
            fprintf(stderr, "%02x ", inst_bytes[ i ]);
          }
          fprintf(stderr, "\n");
#endif
        }

        while (line[ 0u ] != ' ') {
          line++;
        }
        line++;
        for (; n_bytes < XED_MAX_INSTRUCTION_BYTES;) {
          sscanf(line, "%02x", &byte);
          inst_bytes[ n_bytes ] = ((xed_uint8_t) (byte));
          n_bytes++;

          line += 3;
          if (line[ 0u ] == '\n') {
            break;
          }
        }

        xed_decoded_inst_zero(&xedd);
        xed_decoded_inst_set_mode(&xedd, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
        xed_error = xed_decode_with_features(&xedd, &inst_bytes[ 0u ], n_bytes, &chip_features);
        if (xed_error == XED_ERROR_NONE) {
          char xed_buffer[ 1024u ];

          memset(&xed_buffer[ 0u ], 0, sizeof(xed_buffer));
          if (xed_format_context(XED_SYNTAX_ATT,
                                 &xedd,
                                 &xed_buffer[ 0u ],
                                 ((int) (sizeof(xed_buffer))),
                                 0,
                                 NULL,
                                 NULL)) {
            add_inst(&inst_bytes[ 0u ], &xedd, xed_file, base_addr, addr);
          }
        } else if (xed_error == XED_ERROR_BUFFER_TOO_SHORT) {
          goto xed_decode_inst;
        //} else if (xed_error == XED_ERROR_INVALID_FOR_CHIP) {
        // ?!?!
        //} else if (xed_error == XED_ERROR_INSTR_TOO_LONG) {
        // ?!?!
        } else {
#if 0
          fprintf(stderr, "%016llx %3d :: %2u :: ", addr - base_addr, xed_error, n_bytes);
          for (unsigned int i = 0u; i < n_bytes; i++) {
            fprintf(stderr, "%02x ", inst_bytes[ i ]);
          }
          fprintf(stderr, "\n");
#endif
        }
      } else {
        break;
      }
    }

    fprintf(stdout, "no_insts = %9llu\n", no_insts);
    fclose(fp);
  } else {
    fprintf(stdout, "\n");
    fprintf(stderr,
            "fopen(%s) failed :: %s\n",
            obj_file,
            strerror(errno)); for (;;) {}
  }
}

#if defined(EN_VMLINUX)
void parse_vmlinux(void) {
  fprintf(stdout, "====== KERNEL ======\n");
  no_insts_vmlinux = no_insts;
  for (unsigned long i = 0lu; i < no_mods; i++) {
    unsigned char* inst_buffer     = ((unsigned char*) (mods[ i ].perfing_addr));
    unsigned int   inst_buffer_len = ((unsigned int) (mods[ i ].sz));
    unsigned char* inst_bytes      = inst_buffer;

    xed_decoded_inst_t xedd;
    xed_error_enum_t   xed_error;

    fprintf(stdout,
            "%32s :: %16lx %16lx %8lu :: no_binaries = %9u no_insts = %9llu\n",
            &mods[ i ].name[ 0u ],
            mods[ i ].addr,
            mods[ i ].perfing_addr,
            mods[ i ].sz,
            no_binaries,
            no_insts);

    for (;;) {
      unsigned int xed_max_inst_bytes = ((unsigned int) (&inst_buffer[ inst_buffer_len ] - inst_bytes));

      if (xed_max_inst_bytes >= XED_MAX_INSTRUCTION_BYTES) {
        xed_max_inst_bytes = XED_MAX_INSTRUCTION_BYTES;
      }

      xed_decoded_inst_zero(&xedd);
      xed_decoded_inst_set_mode(&xedd, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
      xed_error = xed_decode_with_features(&xedd, inst_bytes, xed_max_inst_bytes, &chip_features);
      if (xed_error == XED_ERROR_NONE) {
        add_inst(inst_bytes, &xedd, mods[ i ].name, mods[ i ].addr, mods[ i ].addr + ((unsigned long long int) (inst_bytes - inst_buffer)));

        inst_bytes += xed_decoded_inst_get_length(&xedd);
      } else {
        inst_bytes += 1;
      }

      if (inst_bytes >= &inst_buffer[ inst_buffer_len ]) {
        break;
      }
      if (no_insts >= MAX_NO_INSTS) {
        break;
      }
    }
    if (no_insts >= MAX_NO_INSTS) {
      break;
    }
  }
  fprintf(stdout, "====== KERNEL ======\n");
  fprintf(stdout, "no_insts_vmlinux = %9llu\n", no_insts_vmlinux);
  fprintf(stdout, "no_insts         = %9llu\n", no_insts);
}
#endif

void perfed_xed(const int perfed_pid __attribute__((unused))) {
#if defined(EN_PTRACE_UNWIND)
  dwarf_unwinds = (dwarf_unwind_t*) malloc(MAX_NO_DWARF_UNWINDS * sizeof(dwarf_unwind_t));
  if (dwarf_unwinds == NULL) {
    fprintf(stderr, "malloc failed\n"); for (;;) {}
  } else {
    memset(dwarf_unwinds, 0, MAX_NO_DWARF_UNWINDS * sizeof(dwarf_unwind_t));
    fprintf(stdout, "dwarf_unwinds size = %4llu MB\n", (MAX_NO_DWARF_UNWINDS * sizeof(dwarf_unwind_t)) / 1024llu / 1024llu);
  }
#endif
  insts = (inst_t*) malloc(MAX_NO_INSTS * sizeof(inst_t));
  if (insts == NULL) {
    fprintf(stderr, "malloc failed\n"); for (;;) {}
  } else {
    memset(insts, 0, MAX_NO_INSTS * sizeof(inst_t));
    fprintf(stdout, "insts size         = %4llu MB\n", (MAX_NO_INSTS * sizeof(inst_t)) / 1024llu / 1024llu);
  }

  xed_tables_init();
  xed_get_chip_features(&chip_features, XED_CHIP_ALL);

  branches_fp = fopen("branches.log", "w");
  if (branches_fp == NULL) {
    branches_fp = stdout;
  }
}

void xed_intel_pt_psb(const unsigned int           psb,
                      const double                 tsc,
                      const unsigned long long int cyc_cnt) {
#if 0
  //fprintf(branches_fp, "PSB :: %20.2lf %20llu\n", tsc, cyc_cnt);
#else
  (void) (tsc);
  (void) (cyc_cnt);
#endif

#if defined(EN_RET_COMPRESSION)
  en_ret_compression = psb;
#else
  (void) (psb);
#endif
}

void xed_intel_pt_ovf(const double                 tsc,
                      const unsigned long long int cyc_cnt) {
  fprintf(branches_fp, "Pid Reset %16llx :: %20.2lf %20llu\n", this_pgd, tsc, cyc_cnt);
  this_pgd = 0llu;

  fprintf(branches_fp, "O :: %20.2lf %20llu\n", tsc, cyc_cnt);

  xed_async_reset(0llu, tsc, cyc_cnt, __FILE__, __LINE__);
#if defined(EN_RET_COMPRESSION)
  en_ret_compression = 0u;
#endif
}

void xed_intel_pt_pip(const unsigned long long int pgd,
                      const double                 tsc,
                      const unsigned long long int cyc_cnt) {
  if (this_pgd == pgd) {
    fprintf(branches_fp, "Pid Cont  %16llx :: %20.2lf %20llu\n", pgd, tsc, cyc_cnt);
  } else {
    if (this_pgd != 0llu) {
      fprintf(branches_fp, "Pid Exit  %16llx :: %20.2lf %20llu\n", this_pgd, tsc, cyc_cnt);
    }
    fprintf(branches_fp, "Pid Enter %16llx :: %20.2lf %20llu\n", pgd, tsc, cyc_cnt);
  }
  this_pgd = pgd;
}

void xed_intel_pt_tsc(const double                 tsc,
                      const unsigned long long int cyc_cnt __attribute__((unused))) {
  fprintf(branches_fp, "TSC :: %20.2lf\n", tsc);
}

void xed_intel_pt_tip_enable(const unsigned long long int tip,
                             const double                 tsc,
                             const unsigned long long int cyc_cnt) {
  reset_inst_stats(cyc_cnt);

  fprintf(branches_fp, "E :: %20.2lf %20llu %16llx\n", tsc, cyc_cnt, tip);

#if defined(EN_RET_COMPRESSION)
  en_ret_compression = 1u;
#endif
}

void xed_intel_pt_bip_fup(const unsigned long long int a,
                          const unsigned long long int b,
                          const double                 tsc,
                          const unsigned long long int pmu_mask,
                          const unsigned long long int mem_addr) {
#if defined(EN_PMU)
  pmu_info(pmu_mask, branches_fp);
#else
  (void) (pmu_mask);
#endif

  fprintf(branches_fp, "F :: %20.2lf :: %16llx -> %16llx MEM = %16llx\n", tsc, a, b, mem_addr);
}

void xed_intel_pt_ptw_fup(const unsigned long long int ip,
                          const double                 tsc,
                          const unsigned long long int cyc_cnt,
                          const unsigned long long int ptw) {
  fprintf(branches_fp, "W :: %20.2lf %20llu %16llx :: %llx\n", tsc, cyc_cnt, ip, ptw);
}

void xed_intel_pt_tip_disable(const double                 tsc,
                              const unsigned long long int cyc_cnt) {
  if (this_ctx->tnt_queue_head != this_ctx->tnt_queue_tail) {
    fflush(branches_fp); fclose(branches_fp); fprintf(stderr, " BR Queue TIP_PGD :: %6u %6u\n", this_ctx->tnt_queue_head, this_ctx->tnt_queue_tail); for (;;) {}
  }
  if (this_ctx->tip_queue_head != this_ctx->tip_queue_tail) {
    fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "TIP Queue TIP_PGD\n"); for (;;) {}
  }

  fprintf(branches_fp, "D :: %20.2lf %20llu\n", tsc, cyc_cnt);

#if defined(EN_RET_COMPRESSION)
  en_ret_compression = 0u;
#endif
}

#if defined(EN_SW_UTIL)
void xed_tid_switch(const sw_util_t* const sw_util) {
  if (sw_util->out == 1u) {
    fprintf(branches_fp, "Y :: %20.2lf %6.2lf\n", ((double) (sw_util->tsc)), sw_util->util);
  } else {
    fprintf(branches_fp, "X :: %20.2lf %6.2lf\n", ((double) (sw_util->tsc)), sw_util->util);
  }
}
#endif

void xed_update_last_inst(const unsigned long long addr) {
  const inst_t* const inst = xed_unwind_find_inst(addr);

  if (inst != NULL) {
    this_ctx->last_inst = ((signed long long) (inst - &insts[ 0u ]));
  } else {
    this_ctx->last_inst = -1ll;

    if (xed_is_enabled(addr) == 0u) {
      fprintf(branches_fp, "PGD :: %16llx %16lx :: %16llx\n", this_pgd, perfed_pgd, addr);
      return;
    }

#if defined(EN_VMLINUX)
    if ((vm_kernel_ks <= addr) && (addr < vm_kernel_me)) {
      for (unsigned long i = 0lu; i < no_mods; i++) {
        const unsigned long long int a = ((unsigned long long int) (mods[ i ].addr));
        const unsigned long long int b = a + ((unsigned long long int) (mods[ i ].sz));

        if ((a <= addr) && (addr < b)) {
          fprintf(stdout, "XED Instruction %16llx found in module %s range %16llx - %16llx\n", addr, mods[ i ].name, a, b); for (;;) {}
          return;
        }
      }
    }
#endif

    for (unsigned int i = 0u; i < no_amaps; i++) {
      if ((amaps[ i ].a <= addr) && (addr < amaps[ i ].b)) {
        fprintf(stdout, "XED Instruction %16llx found in anonymous mapping %16llx %16llx\n", addr, amaps[ i ].a, amaps[ i ].b);
        return;
      }
    }

    fprintf(stderr, "XED Instruction %16llx not found\n", addr); for (;;) {}
  }
}

void xed_process_branches(const unsigned int           tnt,
                          const unsigned int           tnt_len,
                          const unsigned long long int tip,
                          const double                 tsc,
                          const unsigned long long int cyc_cnt) {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
  static char branches_buffer[ 512u ];
#endif

  if (this_ctx->last_inst == -1ll) {
    return;
  }

  tnt_t*       x_tnt = NULL;
  const tip_t* x_tip = NULL;

  add_tnt(tnt, tnt_len, tsc, cyc_cnt);
  add_tip(tip, tsc, cyc_cnt);

  for (;;) {
    if (this_ctx->last_inst == -1ll) {
      break;
    }
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
#if defined(PRINT_XED_BRANCHES_ONLY)
    if (insts[ this_ctx->last_inst ].cofi.type != 0u) {
#endif
    sprintf(&branches_buffer[ 0u ],
            "%10llu %2u %8llx %16llx %32.32s %10s %12s",
            branches_n,
            ctx_idx,
            insts[ this_ctx->last_inst ].addr - insts[ this_ctx->last_inst ].base_addr,
            insts[ this_ctx->last_inst ].addr,
            insts[ this_ctx->last_inst ].binary,
            xed_category_enum_t2str(insts[ this_ctx->last_inst ].category),
            xed_iclass_enum_t2str(insts[ this_ctx->last_inst ].iclass));
#if defined(PRINT_XED_OPCODE)
    for (unsigned int j = 0u; j < 10u; j++) {
      const size_t k = strlen(&branches_buffer[ 0u ]);

      if (j < insts[ this_ctx->last_inst ].length) {
        sprintf(&branches_buffer[ k ], " %02x", ((unsigned int) (insts[ this_ctx->last_inst ].bytes[ j ])));
      } else {
        sprintf(&branches_buffer[ k ], "   ");
      }
    }
#endif
#if defined(PRINT_XED_BRANCHES_ONLY)
    }
#endif
#endif

    if (insts[ this_ctx->last_inst ].cofi.type == 0u) {
      update_inst_stats(insts[ this_ctx->last_inst ].iclass);
#if defined(PRINT_XED)
      branches_n++;
#endif
      this_ctx->last_inst++;
      if (this_ctx->last_inst >= ((signed long long) (no_insts))) {
        fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "last_inst >= no_insts :: 0\n"); for (;;) {}
      }
#if defined(PRINT_XED)
      fprintf(branches_fp, "%s -> %16llx _ :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, 0.0);
#endif
    } else {
      if (this_ctx->tnt_queue_tail != this_ctx->tnt_queue_head) {
        x_tnt = &this_ctx->tnt_queue[ this_ctx->tnt_queue_tail ];
        if (x_tnt->tnt_len == 0u) {
          this_ctx->tnt_queue_tail = (this_ctx->tnt_queue_tail + 1u) % TNT_QUEUE_LEN;
          if (this_ctx->tnt_queue_tail != this_ctx->tnt_queue_head) {
            x_tnt = &this_ctx->tnt_queue[ this_ctx->tnt_queue_tail ];
          } else {
            x_tnt = NULL;
          }
        }
      } else {
        x_tnt = NULL;
      }
      if (this_ctx->tip_queue_tail != this_ctx->tip_queue_head) {
        x_tip = &this_ctx->tip_queue[ this_ctx->tip_queue_tail ];
      } else {
        x_tip = NULL;
      }

      if ((insts[ this_ctx->last_inst ].cofi.type & COND_BRANCH) != 0u) {
        if (x_tnt != NULL) {
          const unsigned int br = x_tnt->tnt & (1u << (x_tnt->tnt_len - 1u));

          x_tnt->tnt_len--;
          if (br != 0u) {
            update_inst_stats(insts[ this_ctx->last_inst ].iclass);
            branches_n++;
            xed_update_last_inst(insts[ this_ctx->last_inst ].cofi.u.j.addr);
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
            fprintf(branches_fp, "%s -> %16llx 1 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tnt->tsc);
#endif
          } else {
            update_inst_stats(insts[ this_ctx->last_inst ].iclass);
            branches_n++;
            this_ctx->last_inst++;
            if (this_ctx->last_inst >= ((signed long long) (no_insts))) {
              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "last_inst >= no_insts :: 1\n"); for (;;) {}
            }
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
            fprintf(branches_fp, "%s -> %16llx 0 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tnt->tsc);
#endif
          }
          if (x_tnt->cyc_cnt != 0llu) {
            print_inst_stats(x_tnt->cyc_cnt);

            x_tnt->tsc     = 0.0;
            x_tnt->cyc_cnt = 0llu;
          }
        } else {
          return;
        }
      } else if ((insts[ this_ctx->last_inst ].cofi.type & UNCOND_DIRECT_BRANCH) != 0u) {
        if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_CALL) {
          this_ctx->call_stack[ this_ctx->call_stack_idx ] = (call_stack_t) {
#if defined(EN_RET_COMPRESSION)
            .en_ret_compression = en_ret_compression,
#endif

            .call       = &insts[ this_ctx->last_inst + 0ll ],
            .t_call     = insts[ this_ctx->last_inst ].cofi.u.c.addr,
            .ret        = &insts[ this_ctx->last_inst + 1ll ],
            .tsc_c      = 0.0,
            .no_insts_c = 0llu
          };
          this_ctx->call_stack_idx++;
          if (this_ctx->call_stack_idx >= CALL_STACK_LEN) {
            fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "Full stack\n"); for (;;) {}
          }

          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(insts[ this_ctx->last_inst ].cofi.u.c.addr);
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx U :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, 0.0);
#endif
        } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_UNCOND_BR) {
          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(insts[ this_ctx->last_inst ].cofi.u.j.addr);
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx U :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, 0.0);
#endif
        } else {
          // What to do in this case?!
        }
      } else if ((insts[ this_ctx->last_inst ].cofi.type & INDIRECT_BRANCH) != 0u) {
#if defined(EN_RET_COMPRESSION)
        if ((insts[ this_ctx->last_inst ].iclass == XED_ICLASS_RET_NEAR) && (this_ctx->call_stack_idx >= 1u) && (x_tnt != NULL)) {
          const unsigned int do_br = this_ctx->call_stack[ this_ctx->call_stack_idx - 1u ].en_ret_compression;

          if (x_tnt->tnt_len == 0u) {
            fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "x_tnt->tnt_len == 0u\n"); for (;;) {}
          }

          if (do_br == 1u) {
            const unsigned int br = x_tnt->tnt & (1u << (x_tnt->tnt_len - 1u));

            if (x_tnt->en_ret_compression == 0u) {
              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "x_tnt->en_ret_compression == 0u\n"); for (;;) {}
            }

            x_tnt->tnt_len--;
            if (br != 0u) {
              unsigned long long int ret;

              this_ctx->call_stack_idx--;
              ret = this_ctx->call_stack[ this_ctx->call_stack_idx ].ret->addr;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r      = x_tnt->tsc;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].no_insts_r = 0llu;

              update_inst_stats(insts[ this_ctx->last_inst ].iclass);
              branches_n++;
              xed_update_last_inst(ret);
  #if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
              fprintf(branches_fp, "%s -> %16llx 1 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tnt->tsc);
  #endif
              if (x_tnt->cyc_cnt != 0llu) {
                print_inst_stats(x_tnt->cyc_cnt);

                x_tnt->tsc     = 0.0;
                x_tnt->cyc_cnt = 0llu;
              }
              continue;
            } else {
              print_call_stack(branches_fp);

              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "RET TNT :: %16llx :: %llu\n", insts[ this_ctx->last_inst ].addr, no_insts_vmlinux); for (;;) {}
            }
          }
        }
#endif

        if (x_tip != NULL) {
          if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_CALL) {
            this_ctx->call_stack[ this_ctx->call_stack_idx ] = (call_stack_t) {
#if defined(EN_RET_COMPRESSION)
              .en_ret_compression = en_ret_compression,
#endif

              .call       = &insts[ this_ctx->last_inst + 0ll ],
              .t_call     = x_tip->tip,
              .ret        = &insts[ this_ctx->last_inst + 1ll ],
              .tsc_c      = x_tip->tsc,
              .no_insts_c = branches_n
            };
            this_ctx->call_stack_idx++;
            if (this_ctx->call_stack_idx >= CALL_STACK_LEN) {
              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "Full stack\n"); for (;;) {}
            }
          } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_UNCOND_BR) {
          } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_RET) {
            if (this_ctx->call_stack_idx >= 1u) {
              unsigned long long int ret;

              this_ctx->call_stack_idx--;
              ret = this_ctx->call_stack[ this_ctx->call_stack_idx ].ret->addr;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r      = x_tip->tsc;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].no_insts_r = branches_n;

              if (ret != x_tip->tip) {
                reset_call_stack();
                //fprintf(stderr, "0 Broken call stack :: Invalid RET %16llx %16llx\n", ret, x_tip->tip); //for (;;) {}
              } else {
                const double tsc_c = this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_c;
                const double tsc_r = this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r;

                if ((tsc_c != 0.0) && (tsc_r != 0.0)) {
                  const double tsc_rc = tsc_r - tsc_c;

                  if (tsc_rc < 0.0) {
                    fprintf(stdout, "0 Broken call stack :: Negative call time %20.2lf %20.2lf => %20.2lf\n", tsc_c, tsc_r, tsc_rc); //for (;;) {}
                  }
                }
              }
            }
          } else {
            // What to do in this case?!
          }
          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(x_tip->tip);
          this_ctx->tip_queue_tail = (this_ctx->tip_queue_tail + 1u) % TIP_QUEUE_LEN;
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx T :: %20.2lf\n", &branches_buffer[ 0u ], x_tip->tip, x_tip->tsc);
#endif
          print_inst_stats(x_tip->cyc_cnt);
        } else {
          return;
        }
      } else if ((insts[ this_ctx->last_inst ].cofi.type & FAR_TRANSFER) != 0u) {
        if (x_tip != NULL) {
          const xed_iclass_enum_t this_iclass = insts[ this_ctx->last_inst ].iclass;

          if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_SYSCALL) {
            this_ctx->call_stack[ this_ctx->call_stack_idx ] = (call_stack_t) {
              .call       = &insts[ this_ctx->last_inst + 0ll ],
              .t_call     = x_tip->tip,
              .ret        = &insts[ this_ctx->last_inst + 1ll ],
              .tsc_c      = x_tip->tsc,
              .no_insts_c = branches_n
            };
            this_ctx->call_stack_idx++;
            if (this_ctx->call_stack_idx >= CALL_STACK_LEN) {
              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "Full stack\n"); for (;;) {}
            }
          } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_SYSRET) {
            if (this_ctx->call_stack_idx >= 1u) {
              unsigned long long int ret;

              this_ctx->call_stack_idx--;
              ret = this_ctx->call_stack[ this_ctx->call_stack_idx ].ret->addr;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r      = x_tip->tsc;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].no_insts_r = branches_n;

              if (ret != x_tip->tip) {
                reset_call_stack();
                //fprintf(stderr, "1 Broken call stack :: Invalid RET %16llx %16llx\n", ret, x_tip->tip); //for (;;) {}
              } else {
                const double tsc_c = this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_c;
                const double tsc_r = this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r;

                if ((tsc_c != 0.0) && (tsc_r != 0.0)) {
                  const double tsc_rc = tsc_r - tsc_c;

                  if (tsc_rc < 0.0) {
                    fprintf(stdout, "1 Broken call stack :: Negative call time %20.2lf %20.2lf => %20.2lf\n", tsc_c, tsc_r, tsc_rc); //for (;;) {}
                  }
                }
              }
            }
          }
          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(x_tip->tip);
          this_ctx->tip_queue_tail = (this_ctx->tip_queue_tail + 1u) % TIP_QUEUE_LEN;
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx T :: %20.2lf\n", &branches_buffer[ 0u ], x_tip->tip, x_tip->tsc);
#endif
          print_inst_stats(x_tip->cyc_cnt);

          if ((this_iclass == XED_ICLASS_IRET) || (this_iclass == XED_ICLASS_IRETD) || (this_iclass == XED_ICLASS_IRETQ)) {
            reset_inst_stats(cyc_cnt);

            fprintf(branches_fp, "Async   Exit :: %u :: %20.2lf %16llx %16llx\n", ctx_idx, x_tip->tsc, x_tip->tip, this_ctx->async_src);
            if (this_ctx->call_stack_idx != 0u) {
              print_call_stack(branches_fp);

              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "Non-empty stack\n"); for (;;) {}
            }
            if (this_ctx->tnt_queue_head != this_ctx->tnt_queue_tail) {
              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, " BR Queue async exit :: %6u %6u\n", this_ctx->tnt_queue_head, this_ctx->tnt_queue_tail); for (;;) {}
            }
            if (this_ctx->tip_queue_head != this_ctx->tip_queue_tail) {
              fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "TIP Queue async exit\n"); for (;;) {}
            }
            if ((this_ctx->async_src != 0llu) && (this_ctx->async_src != x_tip->tip)) {
              fprintf(branches_fp, "Async Adjust :: %u :: %20.2lf %16llx %16llx\n", ctx_idx, x_tip->tsc, x_tip->tip, this_ctx->async_src);
              xed_async_reset(x_tip->tip, x_tip->tsc, x_tip->cyc_cnt, __FILE__, __LINE__);
            }

            if (this_ctx->last_inst == -1ll) {
              xed_async_reset(x_tip->tip, x_tip->tsc, x_tip->cyc_cnt, __FILE__, __LINE__);
            }
            if (ctx_idx >= 1u) {
              ctx_idx--;
            }
            this_ctx = &ctx[ ctx_idx ];
          }
        } else {
          return;
        }
      } else {
        fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "Unknown cofi type\n"); for (;;) {}
      }
    }
  }
}

void xed_async_reset(const unsigned long long int tip,
                     const double                 tsc,
                     const unsigned long long int cyc_cnt,
                     const char* const            file,
                     const unsigned int           line) {
  reset_inst_stats(cyc_cnt);

  for (unsigned int i = 0u; i <= ctx_idx; i++) {
    this_ctx = &ctx[ i ];
    reset_call_stack();
    reset_last_inst();
  }
  fprintf(branches_fp, "Async  Reset :: x :: %20.2lf %16llx :: %s %u\n", tsc, tip, file, line);
  ctx_idx  = 0u;
  this_ctx = &ctx[ ctx_idx ];
}

void xed_async_enter(const unsigned long long int async_src,
                     const unsigned long long int async_dst,
                     const double                 tsc,
                     const unsigned long long int cyc_cnt) {
  reset_inst_stats(cyc_cnt);

  if (this_ctx->tnt_queue_head != this_ctx->tnt_queue_tail) {
    fflush(branches_fp); fclose(branches_fp); fprintf(stderr, " BR Queue async enter :: %6u %6u\n", this_ctx->tnt_queue_head, this_ctx->tnt_queue_tail); for (;;) {}
  }
  if (this_ctx->tip_queue_head != this_ctx->tip_queue_tail) {
    fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "TIP Queue async enter\n"); for (;;) {}
  }
  ctx_idx++;
  if (ctx_idx >= MAX_NO_CTXS) {
    fflush(branches_fp); fclose(branches_fp); fprintf(stderr, "Full ctxs\n"); for (;;) {}
  }
  this_ctx            = &ctx[ ctx_idx ];
  this_ctx->async_src = async_src;
  this_ctx->async_dst = async_dst;
  fprintf(branches_fp, "Async  Enter :: %u :: %20.2lf %16llx %16llx\n", ctx_idx, tsc, async_src, async_dst);
}

void xed_tsc_err(const double tsc_err) {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
  static unsigned int no_tsc_errs;

  fprintf(branches_fp, "TSC_ERR :: %20.2lf %6u\n", tsc_err, ++no_tsc_errs);
#else
  (void) (tsc_err);
#endif
}

const inst_t* xed_unwind_find_inst(const unsigned long long int addr) {
  if (addr == 0llu) {
    return NULL;
  }
#if defined(EN_VMLINUX)
  if ((vm_us <= addr) && (addr < vm_kernel_ks)) {
    fprintf(stdout, "XED Instruction %16llx found in range %16llx - %16llx\n", addr, vm_us, vm_kernel_ks);
    return NULL;
  }
  if (vm_kernel_me <= addr) {
    fprintf(stdout, "XED Instruction %16llx found in range %16llx - %16llx\n", addr, vm_kernel_me, 0xFFFFFFFFFFFFFFFFllu);
    return NULL;
  }
  if ((addr >= vm_kernel_ks) && (unwind_cache_high[ addr & CACHE_MASK ] != NULL) && (unwind_cache_high[ addr & CACHE_MASK ]->addr == addr)) {
    return unwind_cache_high[ addr & CACHE_MASK ];
  }
#endif
  if ((unwind_cache_low[ addr & CACHE_MASK ] != NULL) && (unwind_cache_low[ addr & CACHE_MASK ]->addr == addr)) {
    return unwind_cache_low[ addr & CACHE_MASK ];
  }

  signed long long int        a = 0ll;
  signed long long int        b = ((signed long long int) (no_insts - 1llu));
  static signed long long int m = 0ll;

  if (m != 0ll) {
    if ((insts[ m ].addr <= addr) && (addr <= insts[ m ].addr + insts[ m ].length - 1llu)) {
      return &insts[ m ];
    } else if (insts[ m ].addr < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }

  while (a <= b) {
    m = (a + b) / 2ll;

    if ((insts[ m ].addr <= addr) && (addr <= insts[ m ].addr + insts[ m ].length - 1llu)) {
#if defined(EN_VMLINUX)
      if (addr >= vm_kernel_ks) {
        unwind_cache_high[ addr & CACHE_MASK ] = &insts[ m ];
      } else {
        unwind_cache_low[ addr & CACHE_MASK ]  = &insts[ m ];
      }
#else
      unwind_cache_low[ addr & CACHE_MASK ] = &insts[ m ];
#endif

      return &insts[ m ];
    } else if (insts[ m ].addr < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }
  return NULL;
}

#if defined(EN_PTRACE_UNWIND)
void xed_unwind(void) {
  print_unwind(branches_fp);
  print_call_stack(branches_fp);
}

const dwarf_unwind_t* xed_unwind_find_dwarf(const unsigned long long int addr) {
  signed long long int   a = 0ll;
  signed long long int   b = ((signed long long int) (no_dwarf_unwinds - 1llu));
  signed long long int   m;
  unsigned long long int x;
  unsigned long long int y;

  while (a <= b) {
    m = (a + b) / 2ll;

    x = dwarf_unwinds[ m + 0 ].addr;
    y = dwarf_unwinds[ m + 1 ].addr;
    if ((x <= addr) && (addr < y)) {
      return &dwarf_unwinds[ m ];
    } else if (x < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }

  return NULL;
}

void xed_unwind_link_inst_to_dwarf(void) {
  for (unsigned long long int i = 0llu; i < no_insts - 1llu; i++) {
    if (insts[ i ].addr > insts[ i + 1llu ].addr) {
      fprintf(stderr,
              "Unsorted instructions %s %s :: %16llx %16llx\n",
              insts[ i ].binary,
              insts[ i + 1llu ].binary,
              insts[ i ].addr,
              insts[ i + 1llu ].addr); for (;;) {}
    }
  }
  for (unsigned long long int i = 0llu; i < no_insts; i++) {
    const unsigned long long int addr_offset = ((unsigned long long int) (insts[ i ].length)) - 1llu;

    insts[ i ].dwarf_unwind = xed_unwind_find_dwarf(insts[ i ].addr + addr_offset);
    if (insts[ i ].dwarf_unwind == NULL) {
#if 0
      fprintf(stdout,
              "Unlinked unwind for instruction %16llx in %s\n",
              insts[ i ].addr - insts[ i ].base_addr,
              insts[ i ].binary);
#endif
    }
  }
}
#endif

void xed_close(void) {
#if defined(EN_PTRACE_UNWIND)
  for (unsigned int i = 0u; i < no_dwarf_unwinds; i++) {
    if (dwarf_unwinds[ i ].cfa.rule == CFA_RULE_EXP) {
      free(dwarf_unwinds[ i ].cfa.s.exp);
    }
    for (unsigned j = 0u; j < MAX_NO_REGS; j++) {
      if (dwarf_unwinds[ i ].regs[ j ].rule == REG_RULE_EXP) {
        free(dwarf_unwinds[ i ].regs[ j ].exp);
      }
    }
  }
  free(dwarf_unwinds);
#endif
  free(insts);

  if ((branches_fp != NULL) && (branches_fp != stdout)) {
    fflush(branches_fp);
    fclose(branches_fp);
  }
}

unsigned int xed_is_enabled(const unsigned long long int addr) {
#if defined(EN_VMLINUX) && defined(EN_VMLINUX_WIDE)
  return ((this_pgd == ((unsigned long long int) (perfed_pgd))) || (addr >= vm_kernel_ks)) ? (1u) : (0u);
#else
  (void) (addr);

  return 1u;
#endif
}
