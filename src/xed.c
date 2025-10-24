#include "xed.h"
#include "pmu.h"
#include "proc.h"
#if defined(EN_JSON_TRACE)
#include "x_json.h"
#endif

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

#define CF(eflags) ((eflags >>  0llu) & 0x01llu)
#define PF(eflags) ((eflags >>  2llu) & 0x01llu)
#define AF(eflags) ((eflags >>  4llu) & 0x01llu)
#define ZF(eflags) ((eflags >>  6llu) & 0x01llu)
#define SF(eflags) ((eflags >>  7llu) & 0x01llu)
#define OF(eflags) ((eflags >> 11llu) & 0x01llu)

#define CALL_STACK_LEN       (1u * 256u)
#define TNT_QUEUE_LEN        (1u * 1024u)
#define TIP_QUEUE_LEN        (1u * 1024u)

typedef struct {
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

dwarf_unwind_t*    unwinds;
unsigned long long no_unwinds;
char               binaries[ MAX_NO_BINARIES ][ 250u ];
unsigned int       no_binaries;
inst_t*            insts;
unsigned long long no_insts;

static ctx_t        ctx[ 10u ] = {
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
  },
  [ 5u ] = {
    .last_inst = -1ll
  },
  [ 6u ] = {
    .last_inst = -1ll
  },
  [ 7u ] = {
    .last_inst = -1ll
  },
  [ 8u ] = {
    .last_inst = -1ll
  },
  [ 9u ] = {
    .last_inst = -1ll
  }
};
static ctx_t*       this_ctx   = &ctx[ 0u ];
static unsigned int ctx_idx    = 0u;

static FILE*                  branches_fp;
static unsigned long long int branches_n;

static xed_chip_features_t chip_features;
static inst_stats_t        inst_stats;

extern double tsc_factor;
extern double cbr_factor;

static inline __attribute__((always_inline)) void update_inst_stats(const xed_iclass_enum_t iclass) {
  inst_stats.iclass_cnt[ iclass ]++;
}

static void reset_inst_stats(const unsigned long long int cyc_cnt) {
  const double ipc_0 = (inst_stats.n_ipc  >= 1u) ? (inst_stats.t_ipc  / ((double) (inst_stats.n_ipc)))  : (0.0f);
  const double ipc_1 = (inst_stats.t_cycs >= 1u) ? (inst_stats.t_cnts / ((double) (inst_stats.t_cycs))) : (0.0f);

  fprintf(branches_fp, "Eq e :: ####### %12.3lf %12.3lf\n", ipc_0, ipc_1);
  fprintf(branches_fp, "Eq b :: #######\n");
  for (xed_iclass_enum_t i = XED_ICLASS_INVALID; i < XED_ICLASS_LAST; i++) {
    inst_stats.iclass_cnt[ i ] = 0u;
  }
  inst_stats.cyc_cnt_ref = cyc_cnt;
  inst_stats.t_ipc       = 0.0f;
  inst_stats.n_ipc       = 0u;
  inst_stats.t_cnts      = 0llu;
  inst_stats.t_cycs      = 0llu;
}

static void print_inst_stats(const unsigned long long int cyc_cnt) {
  unsigned int                 cnts = 0u;
  const unsigned long long int cycs = cyc_cnt - inst_stats.cyc_cnt_ref;
  double                       ipc  = 0.0f;

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
    sprintf(&dwarf_file[ 0u ], "resources/dwarf.%s", xed_file);
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
              unwinds[ no_unwinds ].base_addr = base_addr;
              unwinds[ no_unwinds ].addr      = base_addr + addr;
              unwinds[ no_unwinds ].cfa.rule  = CFA_RULE_EXP;
              strcpy(b, parse_dwarf_exp(b, ':', 2, &unwinds[ no_unwinds ].cfa.s.exp));
            } else {
              fprintf(stderr, "0 dwarf_cfa_err\n"); for (;;) {}
            }
          } else {
            unwinds[ no_unwinds ].base_addr        = base_addr;
            unwinds[ no_unwinds ].addr             = base_addr + addr;
            unwinds[ no_unwinds ].cfa.rule         = CFA_RULE_REG;
            unwinds[ no_unwinds ].cfa.s.reg        = cfa_reg;
            unwinds[ no_unwinds ].cfa.s.reg_offset = 0;
          }
        } else {
          unwinds[ no_unwinds ].base_addr        = base_addr;
          unwinds[ no_unwinds ].addr             = base_addr + addr;
          unwinds[ no_unwinds ].cfa.rule         = CFA_RULE_REG;
          unwinds[ no_unwinds ].cfa.s.reg        = cfa_reg;
          unwinds[ no_unwinds ].cfa.s.reg_offset = cfa_reg_offset;
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

        if (no_unwinds >= MAX_NO_UNWINDS) {
          fprintf(stderr, "Not enough space to load the unwinds\n"); for (;;) {}
        }
        memcpy(&unwinds[ no_unwinds ].regs[ 0u ], &regs[ 0u ], sizeof(regs));
        no_unwinds++;
      } else {
        break;
      }
    }

    fprintf(stdout, "no_unwinds  = %9llu :: ", no_unwinds);
    fprintf(stdout, "no_binaries = %9u :: ", no_binaries);
    fclose(fp);
  } else {
    fprintf(stderr,
            "fopen(%s) failed :: %s\n",
            dwarf_file,
            strerror(errno)); for (;;) {}
  }
}

void parse_objdump(const int perfed_pid, const char* const xed_file, const unsigned long long int base_addr) {
  (void) (perfed_pid);

  char obj_file[ 256u ];

  if (strstr(xed_file, "stack") != NULL) {
    return;
  }
  if (strstr(xed_file, "vsyscall") != NULL) {
    return;
  }

  obj_file[ 0u ] = '\0';
  if (xed_file != NULL) {
    sprintf(&obj_file[ 0u ], "resources/objdump.%s", xed_file);
  }

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
            const xed_category_enum_t xedd_category       = xed_decoded_inst_get_category(&xedd);
            const xed_iclass_enum_t   xedd_iclass         = xed_decoded_inst_get_iclass(&xedd);
            const xed_uint_t          xedd_length         = xed_decoded_inst_get_length(&xedd);
            const xed_inst_t*         xedd_dec            = xed_decoded_inst_inst(&xedd);
            const unsigned int        xedd_dec_no_ops     = xed_decoded_inst_noperands(&xedd);
            const xed_uint_t          xedd_dec_no_mem_ops = xed_decoded_inst_number_of_memory_operands(&xedd);

            if (no_insts >= MAX_NO_INSTS) {
              fprintf(stderr, "Not enough space to load the instructions\n"); for (;;) {}
            }
            memset(&insts[ no_insts ], 0, sizeof(inst_t));
            insts[ no_insts ].binary    = parse_get_binary(xed_file, 1u);
            insts[ no_insts ].base_addr = base_addr;
            insts[ no_insts ].addr      = addr;
            insts[ no_insts ].category  = xedd_category;
            insts[ no_insts ].iclass    = xedd_iclass;
            memcpy(&insts[ no_insts ].bytes[ 0u ], &inst_bytes[ 0u ], n_bytes);
            insts[ no_insts ].length    = xedd_length;

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
                if (xed_operand_values_has_memory_displacement(&xedd)) {
                  const xed_reg_enum_t xed_reg = xed_decoded_inst_get_base_reg(&xedd, 0u);

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
                  insts[ no_insts ].cofi.u.c.addr = addr + xed_decoded_inst_get_branch_displacement(&xedd) + ((int64_t) (xedd_length));
                } else {
                  // What to do in this case?!
                }
              }
              if (xedd_iclass == XED_ICLASS_CALL_NEAR) {
                insts[ no_insts ].cofi.u.c.ret_to.addr = addr + ((int64_t) (xedd_length));
              }
              //fprintf(stdout,
              //         "\nCALL = %016llx -> %016llx :: %016llx %02x",
              //        addr,
              //        insts[ no_insts ].cofi.u.c.addr,
              //        insts[ no_insts ].cofi.u.c.ret_to.addr,
              //        insts[ no_insts ].cofi.type);
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
                if (xed_operand_values_has_memory_displacement(&xedd)) {
                  const xed_reg_enum_t xed_reg = xed_decoded_inst_get_base_reg(&xedd, 0u);

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
                  insts[ no_insts ].cofi.u.j.addr = addr + xed_decoded_inst_get_branch_displacement(&xedd) + ((int64_t) (xedd_length));
                } else {
                  // What to do in this case?!
                }
              }
              //fprintf(stdout, "\nJMP  = %016llx -> %016llx %02x", addr, insts[ no_insts ].cofi.u.j.addr, insts[ no_insts ].cofi.type);
            } else if (xedd_category == XED_CATEGORY_RET) {
              if ((xedd_iclass == XED_ICLASS_IRET)  ||
                  (xedd_iclass == XED_ICLASS_IRETD) ||
                  (xedd_iclass == XED_ICLASS_IRETQ) ||
                  (xedd_iclass == XED_ICLASS_RET_FAR)) {
                insts[ no_insts ].cofi.type |= FAR_TRANSFER;
              } else {
                insts[ no_insts ].cofi.type |= INDIRECT_BRANCH;
              }
              //fprintf(stdout, "\nRET %02x", insts[ no_insts ].cofi.type);
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
            no_insts++;
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

void perfed_xed(const int perfed_pid) {
  unwinds = malloc(MAX_NO_UNWINDS * sizeof(dwarf_unwind_t));
  if (unwinds == NULL) {
    fprintf(stderr, "malloc failed\n"); for (;;) {}
  } else {
    fprintf(stdout, "unwinds size = %4llu MB\n", (MAX_NO_UNWINDS * sizeof(inst_t)) / 1024llu / 1024llu);
  }
  insts = malloc(MAX_NO_INSTS * sizeof(inst_t));
  if (insts == NULL) {
    fprintf(stderr, "malloc failed\n"); for (;;) {}
  } else {
    fprintf(stdout, "insts size   = %4llu MB\n", (MAX_NO_INSTS * sizeof(inst_t)) / 1024llu / 1024llu);
  }

  xed_tables_init();
  xed_get_chip_features(&chip_features, XED_CHIP_ALL);

  branches_fp = fopen("branches.log", "w");
  if (branches_fp == NULL) {
    branches_fp = stdout;
  }

#if defined(EN_JSON_TRACE)
  perfed_json(perfed_pid);
#else
  (void) (perfed_pid);
#endif
}

void xed_intel_pt_ovf_fup(const unsigned long long int ip,
                          const double                 tsc,
                          const unsigned long long int cyc_cnt) {
  reset_inst_stats(cyc_cnt);

  fprintf(branches_fp, "O :: %20.2lf %20llu %16llx\n", tsc, cyc_cnt, ip);
}

void xed_intel_pt_tip_enable(const unsigned long long int tip,
                             const double                 tsc,
                             const unsigned long long int cyc_cnt) {
  reset_inst_stats(cyc_cnt);

  fprintf(branches_fp, "E :: %20.2lf %20llu %16llx\n", tsc, cyc_cnt, tip);
}

void xed_intel_pt_bip_fup(const unsigned long long int a,
                          const unsigned long long int b,
                          const double                 tsc,
                          const unsigned long long int pmu_mask,
                          const unsigned long long int mem_addr) {
  fprintf(branches_fp, "F :: %20.2lf %16llx %16llx :: %016llx %16llx\n", tsc, a, b, pmu_mask, mem_addr);
  pmu_info(pmu_mask, branches_fp);
}

void xed_intel_pt_ptw_fup(const unsigned long long int ip,
                          const double                 tsc,
                          const unsigned long long int cyc_cnt) {
  fprintf(branches_fp, "W :: %20.2lf %20llu %16llx\n", tsc, cyc_cnt, ip);
}

void xed_intel_pt_tip_disable(const double                 tsc,
                              const unsigned long long int cyc_cnt) {
  if (this_ctx->tnt_queue_head != this_ctx->tnt_queue_tail) {
    fflush(branches_fp); fprintf(stderr, " BR Queue TIP_PGD\n"); for (;;) {}
  }
  if (this_ctx->tip_queue_head != this_ctx->tip_queue_tail) {
    fflush(branches_fp); fprintf(stderr, "TIP Queue TIP_PGD\n"); for (;;) {}
  }

  fprintf(branches_fp, "D :: %20.2lf %20llu\n", tsc, cyc_cnt);

  xed_reset_call_stack();
  xed_reset_last_inst();
}

void xed_tid_switch(const double       tsc,
                    const unsigned int sw_out) {

  if (sw_out == 1u) {
    fprintf(branches_fp, "Y :: %20.2lf\n", tsc);
  } else {
    fprintf(branches_fp, "X :: %20.2lf\n", tsc);
  }
}

void xed_ptrace_uregs(const double                         tsc,
                      const struct user_regs_struct* const uregs) {
  fprintf(branches_fp, "R :: %20.2lf RAX = %16llx\n", tsc, uregs->rax);
  fprintf(branches_fp, "R :: %20.2lf RDX = %16llx\n", tsc, uregs->rdx);
  fprintf(branches_fp, "R :: %20.2lf RCX = %16llx\n", tsc, uregs->rcx);
  fprintf(branches_fp, "R :: %20.2lf RBX = %16llx\n", tsc, uregs->rbx);
  fprintf(branches_fp, "R :: %20.2lf RSI = %16llx\n", tsc, uregs->rsi);
  fprintf(branches_fp, "R :: %20.2lf RDI = %16llx\n", tsc, uregs->rdi);
  fprintf(branches_fp, "R :: %20.2lf RBP = %16llx\n", tsc, uregs->rbp);
  fprintf(branches_fp, "R :: %20.2lf RSP = %16llx\n", tsc, uregs->rsp);
  fprintf(branches_fp, "R :: %20.2lf R08 = %16llx\n", tsc, uregs->r8);
  fprintf(branches_fp, "R :: %20.2lf R09 = %16llx\n", tsc, uregs->r9);
  fprintf(branches_fp, "R :: %20.2lf R10 = %16llx\n", tsc, uregs->r10);
  fprintf(branches_fp, "R :: %20.2lf R11 = %16llx\n", tsc, uregs->r11);
  fprintf(branches_fp, "R :: %20.2lf R12 = %16llx\n", tsc, uregs->r12);
  fprintf(branches_fp, "R :: %20.2lf R13 = %16llx\n", tsc, uregs->r13);
  fprintf(branches_fp, "R :: %20.2lf R14 = %16llx\n", tsc, uregs->r14);
  fprintf(branches_fp, "R :: %20.2lf R15 = %16llx\n", tsc, uregs->r15);
  fprintf(branches_fp, "R :: %20.2lf RIP = %16llx\n", tsc, uregs->rip);
#if 0
  {
    const inst_t* const i = xed_unwind_find_inst(uregs->rip);

    if (i != NULL) {
      const unsigned int this_ctx_idx = ctx_idx;

      fprintf(branches_fp,
              "\e[0;31m%10s %8llx %16llx %16.16s %10s %12s",
              "",
              i->addr - i->base_addr,
              i->addr,
              i->binary,
              xed_category_enum_t2str(i->category),
              xed_iclass_enum_t2str(i->iclass));

#if defined(PRINT_XED_OPCODE)
      for (unsigned int j = 0u; j < 10u; j++) {
        if (j < i->length) {
          fprintf(branches_fp, " %02x", ((unsigned int) (i->bytes[ j ])));
        } else {
          fprintf(branches_fp, "   ");
        }
      }
#endif
      fprintf(branches_fp, "\e[0m\n");

      this_ctx = &ctx[ 0u ];
      if ((this_ctx->last_inst != -1ll) && (insts[ this_ctx->last_inst ].addr < 0xFFFFFFFF8000000llu)) {
        if (i->cofi.type == 0u) {
          xed_update_last_inst(uregs->rip);
          xed_process_branches(0u, 0u, 0llu, 0.0f, 0llu);
        } else if (i->cofi.type & COND_BRANCH) {
          unsigned int tnt = 2u;

          switch (i->iclass) {
            case XED_ICLASS_JB: {
              const unsigned long long int cf = CF(uregs->eflags);

              tnt = (cf == 1llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JBE: {
              const unsigned long long int cf = CF(uregs->eflags);
              const unsigned long long int zf = ZF(uregs->eflags);

              tnt = ((cf == 1llu) || (zf == 1llu)) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JL: {
              const unsigned long long int sf = SF(uregs->eflags);
              const unsigned long long int of = OF(uregs->eflags);

              tnt = (sf != of) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JLE: {
              const unsigned long long int zf = ZF(uregs->eflags);
              const unsigned long long int sf = SF(uregs->eflags);
              const unsigned long long int of = OF(uregs->eflags);

              tnt = ((zf == 1llu) || (sf != of)) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNB: {
              const unsigned long long int cf = CF(uregs->eflags);

              tnt = (cf == 0llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNBE: {
              const unsigned long long int cf = CF(uregs->eflags);
              const unsigned long long int zf = ZF(uregs->eflags);

              tnt = ((cf == 0llu) && (zf == 0llu)) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNL: {
              const unsigned long long int sf = SF(uregs->eflags);
              const unsigned long long int of = OF(uregs->eflags);

              tnt = (sf == of) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNLE: {
              const unsigned long long int zf = ZF(uregs->eflags);
              const unsigned long long int sf = SF(uregs->eflags);
              const unsigned long long int of = OF(uregs->eflags);

              tnt = ((zf == 0llu) && (sf == of)) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNP: {
              const unsigned long long int pf = PF(uregs->eflags);

              tnt = (pf == 0llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNS: {
              const unsigned long long int sf = SF(uregs->eflags);

              tnt = (sf == 0llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JNZ: {
              const unsigned long long int zf = ZF(uregs->eflags);

              tnt = (zf == 0llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JO: {
              const unsigned long long int of = OF(uregs->eflags);

              tnt = (of == 1llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JP: {
              const unsigned long long int pf = PF(uregs->eflags);

              tnt = (pf == 1llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JS: {
              const unsigned long long int sf = SF(uregs->eflags);

              tnt = (sf == 1llu) ? (1u) : (0u);
            } break;

            case XED_ICLASS_JZ: {
              const unsigned long long int zf = ZF(uregs->eflags);

              tnt = (zf == 1llu) ? (1u) : (0u);
            } break;

            default: {
              fprintf(stderr, "ICLASS = %d\n", i->iclass); for (;;) {}
              tnt = 2u;
            } break;
          }

          if (tnt <= 1u) {
            xed_update_last_inst(uregs->rip);
            xed_process_branches(tnt, 1u, 0llu, 0.0f, 0llu);
          }
        }
      }
      this_ctx = &ctx[ this_ctx_idx ];
    }
  }
#endif
  fprintf(branches_fp, "R :: %20.2lf FLG = %16llx\n", tsc, uregs->eflags);
  fprintf(branches_fp, "R :: %20.2lf  ES = %16llx\n", tsc, uregs->es);
  fprintf(branches_fp, "R :: %20.2lf  CS = %16llx\n", tsc, uregs->cs);
  fprintf(branches_fp, "R :: %20.2lf  SS = %16llx\n", tsc, uregs->ss);
  fprintf(branches_fp, "R :: %20.2lf  DS = %16llx\n", tsc, uregs->ds);
  fprintf(branches_fp, "R :: %20.2lf  FS = %16llx\n", tsc, uregs->fs);
  fprintf(branches_fp, "R :: %20.2lf  GS = %16llx\n", tsc, uregs->gs);
  fprintf(branches_fp, "R :: %20.2lf FSB = %16llx\n", tsc, uregs->fs_base);
  fprintf(branches_fp, "R :: %20.2lf GSB = %16llx\n", tsc, uregs->gs_base);
}

void xed_reset_call_stack(void) {
  this_ctx->call_stack_idx = 0u;
}

void xed_reset_last_inst(void) {
  this_ctx->last_inst = -1ll;

  this_ctx->tnt_queue_head = this_ctx->tnt_queue_tail = 0u;
  this_ctx->tip_queue_head = this_ctx->tip_queue_tail = 0u;
}

void xed_update_last_inst(const unsigned long long addr) {
  const inst_t* const inst = xed_unwind_find_inst(addr);

  if (inst != NULL) {
    this_ctx->last_inst = ((signed long long) (inst - &insts[ 0u ]));
  } else {
    this_ctx->last_inst = -1ll;

    for (unsigned int i = 0u; i < no_amaps; i++) {
      if ((amaps[ i ].a <= addr) && (addr < amaps[ i ].b)) {
        fprintf(stdout,
                "XED Instruction %16llx found in anonymous mapping %16llx %16llx\n",
                addr,
                amaps[ i ].a,
                amaps[ i ].b);
        return;
      }
    }
    xed_close();
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

#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
  fprintf(branches_fp, "    TSC :: %20.2lf\n", tsc);
  fprintf(branches_fp, "    CYC :: %20llu\n", cyc_cnt);
#endif

  tnt_t*       x_tnt = NULL;
  const tip_t* x_tip = NULL;

  if (tnt_len >= 1u) {
    const unsigned int tnt_queue_head_next = (this_ctx->tnt_queue_head + 1u) % TNT_QUEUE_LEN;

    if (tnt_queue_head_next != this_ctx->tnt_queue_tail) {
      this_ctx->tnt_queue[ this_ctx->tnt_queue_head ] = (tnt_t) {
        .tnt     = tnt,
        .tnt_len = tnt_len,
        .tsc     = tsc,
        .cyc_cnt = cyc_cnt
      };
      this_ctx->tnt_queue_head                        = tnt_queue_head_next;
    } else {
      fflush(branches_fp); fprintf(stderr, "BR Queue full!\n"); for (;;) {}
    }
  }
  if (tip != 0llu) {
    const unsigned int tip_queue_head_next = (this_ctx->tip_queue_head + 1u) % TIP_QUEUE_LEN;

    if (tip_queue_head_next != this_ctx->tip_queue_tail) {
      this_ctx->tip_queue[ this_ctx->tip_queue_head ] = (tip_t) {
        .tip     = tip,
        .tsc     = tsc,
        .cyc_cnt = cyc_cnt
      };
      this_ctx->tip_queue_head                        = tip_queue_head_next;
    } else {
      fflush(branches_fp); fprintf(stderr, "TIP Queue full!\n"); for (;;) {}
    }
  }

#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
  fprintf(branches_fp, "%16llx\n", insts[ this_ctx->last_inst ].addr);
#endif
  for (;;) {
    if (this_ctx->last_inst == -1ll) {
      break;
    }
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
#if defined(PRINT_XED_BRANCHES_ONLY)
    if (insts[ this_ctx->last_inst ].cofi.type != 0u) {
#endif
    sprintf(&branches_buffer[ 0u ],
            "%10llu %8llx %16llx %16.16s %10s %12s",
            branches_n,
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
      branches_n++;
      this_ctx->last_inst++;
#if defined(PRINT_XED)
      fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr);
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
          const unsigned int br = x_tnt->tnt & (1 << (x_tnt->tnt_len - 1u));

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
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
            fprintf(branches_fp, "%s -> %16llx 0 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tnt->tsc);
#endif
          }
          if (x_tnt->cyc_cnt != 0llu) {
            print_inst_stats(x_tnt->cyc_cnt);

            x_tnt->cyc_cnt = 0llu;
          }
        } else {
          return;
        }
      } else if ((insts[ this_ctx->last_inst ].cofi.type & UNCOND_DIRECT_BRANCH) != 0u) {
        if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_CALL) {
          this_ctx->call_stack[ this_ctx->call_stack_idx ] = (call_stack_t) {
            .call  = &insts[ this_ctx->last_inst + 0ll ],
            .ret   = &insts[ this_ctx->last_inst + 1ll ],
            .tsc_c = 0.0f
          };
#if defined(EN_JSON_TRACE)
          json_enter_call(&this_ctx->call_stack[ this_ctx->call_stack_idx ]);
#endif
          this_ctx->call_stack_idx++;

          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(insts[ this_ctx->last_inst ].cofi.u.c.addr);
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr);
#endif
        } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_UNCOND_BR) {
          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(insts[ this_ctx->last_inst ].cofi.u.j.addr);
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr);
#endif
        } else {
          // What to do in this case?!
        }
      } else if ((insts[ this_ctx->last_inst ].cofi.type & INDIRECT_BRANCH) != 0u) {
#if defined(EN_RET_COMPRESSION)
        if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_RET) {
          if (x_tnt != NULL) {
            const unsigned int br = x_tnt->tnt & (1 << (x_tnt->tnt_len - 1u));

            x_tnt->tnt_len--;
            if (br != 0u) {
              if (this_ctx->call_stack_idx >= 1u) {
                unsigned long long int ret;

                this_ctx->call_stack_idx--;
                ret = this_ctx->call_stack[ this_ctx->call_stack_idx ].ret->addr;
                this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r = 0.0f;
#if defined(EN_JSON_TRACE)
                json_exit_call(&this_ctx->call_stack[ this_ctx->call_stack_idx ]);
#endif

                update_inst_stats(insts[ this_ctx->last_inst ].iclass);
                branches_n++;
                xed_update_last_inst(ret);
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
                fprintf(branches_fp, "%s -> %16llx 1 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tnt->tsc);
#endif
                if (x_tnt->cyc_cnt != 0llu) {
                  print_inst_stats(x_tnt->cyc_cnt);

                  x_tnt->cyc_cnt = 0llu;
                }
                continue;
              } else {
                fflush(branches_fp); fprintf(stderr, "0 RET TNT\n"); for (;;) {}
              }
            } else {
              fflush(branches_fp); fprintf(stderr, "1 RET TNT\n"); for (;;) {}
            }
          }
        }
#endif

        if (x_tip != NULL) {
          if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_CALL) {
            this_ctx->call_stack[ this_ctx->call_stack_idx ] = (call_stack_t) {
              .call  = &insts[ this_ctx->last_inst + 0ll ],
              .ret   = &insts[ this_ctx->last_inst + 1ll ],
              .tsc_c = x_tip->tsc
            };
#if defined(EN_JSON_TRACE)
            json_enter_call(&this_ctx->call_stack[ this_ctx->call_stack_idx ]);
#endif
            this_ctx->call_stack_idx++;
          } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_UNCOND_BR) {
          } else if (insts[ this_ctx->last_inst ].category == XED_CATEGORY_RET) {
            if (this_ctx->call_stack_idx >= 1u) {
              unsigned long long int ret;

              this_ctx->call_stack_idx--;
              ret = this_ctx->call_stack[ this_ctx->call_stack_idx ].ret->addr;
              this_ctx->call_stack[ this_ctx->call_stack_idx ].tsc_r = x_tip->tsc;
#if defined(EN_JSON_TRACE)
              json_exit_call(&this_ctx->call_stack[ this_ctx->call_stack_idx ]);
#endif

              if (ret != x_tip->tip) {
                fprintf(stderr, "Broken call stack!\n"); for (;;) {}
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
          fprintf(branches_fp, "%s -> %16llx T :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tip->tsc);
#endif
          print_inst_stats(x_tip->cyc_cnt);
        } else {
          return;
        }
      } else if ((insts[ this_ctx->last_inst ].cofi.type & FAR_TRANSFER) != 0u) {
        if (x_tip != NULL) {
          const xed_iclass_enum_t this_iclass = insts[ this_ctx->last_inst ].iclass;

          update_inst_stats(insts[ this_ctx->last_inst ].iclass);
          branches_n++;
          xed_update_last_inst(x_tip->tip);
          this_ctx->tip_queue_tail = (this_ctx->tip_queue_tail + 1u) % TIP_QUEUE_LEN;
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx T :: %20.2lf\n", &branches_buffer[ 0u ], insts[ this_ctx->last_inst ].addr, x_tip->tsc);
#endif
          print_inst_stats(x_tip->cyc_cnt);

          if ((ctx_idx >= 1u) && ((this_iclass == XED_ICLASS_IRET) || (this_iclass == XED_ICLASS_IRETD) || (this_iclass == XED_ICLASS_IRETQ))) {
            reset_inst_stats(cyc_cnt);

            fprintf(branches_fp, "Async  Exit :: %20.2lf %16llx\n", x_tip->tsc, x_tip->tip);
            if (this_ctx->call_stack_idx != 0u) {
              fflush(branches_fp); for (;;) {}
            }
            if (this_ctx->tnt_queue_head != this_ctx->tnt_queue_tail) {
              fflush(branches_fp); for (;;) {}
            }
            if (this_ctx->tip_queue_head != this_ctx->tip_queue_tail) {
              fflush(branches_fp); for (;;) {}
            }
            ctx_idx--;
            this_ctx = &ctx[ ctx_idx ];
          }
        } else {
          return;
        }
      } else {
        fprintf(stderr, "Unknown cofi type\n"); for (;;) {}
      }
    }
  }
}

void xed_async_reset(const unsigned long long int tip,
                     const double                 tsc,
                     const unsigned long long int cyc_cnt) {
  reset_inst_stats(cyc_cnt);

  for (unsigned int i = 0u; i <= ctx_idx; i++) {
    this_ctx = &ctx[ i ];
    xed_reset_call_stack();
    xed_reset_last_inst();
  }
  fprintf(branches_fp, "Async Reset :: %20.2lf %16llx\n", tsc, tip);
  ctx_idx  = 0u;
  this_ctx = &ctx[ ctx_idx ];

#if defined(EN_JSON_TRACE)
  json_reset_call(tsc);
#endif
}

void xed_async_enter(const unsigned long long int tip,
                     const double                 tsc,
                     const unsigned long long int cyc_cnt) {
  reset_inst_stats(cyc_cnt);

  if (this_ctx->tnt_queue_head != this_ctx->tnt_queue_tail) {
    fflush(branches_fp); for (;;) {}
  }
  if (this_ctx->tip_queue_head != this_ctx->tip_queue_tail) {
    fflush(branches_fp); for (;;) {}
  }
  ctx_idx++;
  this_ctx = &ctx[ ctx_idx ];
  fprintf(branches_fp, "Async Enter :: %20.2lf %16llx\n", tsc, tip);
}

void xed_tsc_err(const double tsc_err) {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
  fprintf(branches_fp, "TSC_ERR :: %20.2lf\n", tsc_err);
#else
  (void) (tsc_err);
#endif
}

const inst_t* xed_unwind_find_inst(const unsigned long long int addr) {
  if (addr == 0llu) {
    return NULL;
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
      return &insts[ m ];
    } else if (insts[ m ].addr < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }

  return NULL;
}

const dwarf_unwind_t* xed_unwind_find_dwarf(const unsigned long long int addr) {
  signed long long int   a = 0ll;
  signed long long int   b = ((signed long long int) (no_unwinds - 1llu));
  signed long long int   m;
  unsigned long long int x;
  unsigned long long int y;

  while (a <= b) {
    m = (a + b) / 2ll;

    x = unwinds[ m + 0 ].addr;
    y = unwinds[ m + 1 ].addr;
    if ((x <= addr) && (addr < y)) {
      return &unwinds[ m ];
    } else if (x < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }

  return NULL;
}

void xed_unwind_link_inst_and_dwarf(void) {
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

    insts[ i ].unwind = xed_unwind_find_dwarf(insts[ i ].addr + addr_offset);
    if (insts[ i ].unwind == NULL) {
#if 0
      fprintf(stdout,
              "Unlinked unwind for instruction %16llx in %s\n",
              insts[ i ].addr - insts[ i ].base_addr,
              insts[ i ].binary);
#endif
    }
  }
}

void xed_close(void) {
  for (unsigned int i = 0u; i < no_unwinds; i++) {
    if (unwinds[ i ].cfa.rule == CFA_RULE_EXP) {
      free(unwinds[ i ].cfa.s.exp);
    }
    for (unsigned j = 0u; j < MAX_NO_REGS; j++) {
      if (unwinds[ i ].regs[ j ].rule == REG_RULE_EXP) {
        free(unwinds[ i ].regs[ j ].exp);
      }
    }
  }
  free(unwinds);
  free(insts);

  if ((branches_fp != NULL) && (branches_fp != stdout)) {
    fflush(branches_fp);
    fclose(branches_fp);
  }

#if defined(EN_JSON_TRACE)
  json_close();
#endif
}
