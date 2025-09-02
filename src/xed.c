#include "xed.h"
#include "pmu.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/uio.h>

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

#define MAX_NO_DWARF_UNWINDS (5000000llu)
#define MAX_NO_INSTS         (10000000llu)
#define STACK_LEN            (1u * 256u)
#define TNT_QUEUE_LEN        (1u * 1024u)
#define TIP_QUEUE_LEN        (1u * 1024u)

typedef struct {
  unsigned long long int ret;
} call_stack_t;

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

dwarf_unwind_t* unwinds;
inst_t*         insts;

static unsigned long long  no_unwinds;
static unsigned long long  no_insts;
static xed_chip_features_t chip_features;

static unsigned int no_binaries;
static char         binaries[ 50u ][ 250u ];

static signed long long last_inst = -1ll;
static call_stack_t     call_stack[ STACK_LEN ];
static unsigned int     call_stack_idx;
static unsigned int     call_stack_idx_max;
static tnt_t            tnt_queue[ TNT_QUEUE_LEN ];
static unsigned int     tnt_queue_head;
static unsigned int     tnt_queue_tail;
static tip_t            tip_queue[ TIP_QUEUE_LEN ];
static unsigned int     tip_queue_head;
static unsigned int     tip_queue_tail;

static FILE*                  branches_fp;
static unsigned long long int branches_n;
static unsigned long long int branches_n_en;
static double                 branches_tsc_en;
static unsigned long long int branches_cyc_cnt_en;

extern double tsc_factor;
extern double cbr_factor;

#if 0
static unsigned long long int read_perfed_vm(const int perfed_pid, const unsigned long long int addr) {
  unsigned long long int vm_entry = 0llu;
  struct iovec           local    = {
    .iov_base = &vm_entry,
    .iov_len  = sizeof(vm_entry)
  };
  struct iovec           remote   = {
    .iov_base = ((void*) (addr)),
    .iov_len  = sizeof(vm_entry)
  };
  ssize_t                n        = process_vm_readv(perfed_pid, &local, 1, &remote, 1, 0);

  if (n == -1) {
    fprintf(stderr,
            "process_vm_readv(%016llx) failed :: %s\n",
            addr,
            strerror(errno));
  }

  return (n == -1) ? (0llu) : (vm_entry);
}
#endif

static char* get_binary(const char* const xed_file) {
  for (unsigned int i = 0u; i < no_binaries; i++) {
    if (strcmp(binaries[ i ], xed_file) == 0) {
      return &binaries[ i ][ 0u ];
    }
  }
  strcpy(&binaries[ no_binaries ][ 0u ], xed_file);
  no_binaries++;

  return &binaries[ no_binaries - 1u ][ 0u ];
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
#define MAX_BUFFER_SZ (256u)
    char buffer_0[ MAX_BUFFER_SZ ];
    char buffer_1[ MAX_BUFFER_SZ ];

#if 0
    fprintf(stdout, "\n");
#endif
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
          continue;
        }
        c = a; a = b; b = c; b[ 0u ] = '\0';

        memset(&regs[ 0u ], 0, sizeof(regs));
reg_again:
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
              continue;
            }
          }
        } else {
          continue;
        }
        c = a; a = b; b = c; b[ 0u ] = '\0';
        if (a[ 0u ] == ',') {
          a += 2;
          goto reg_again;
        }

        if (no_unwinds >= MAX_NO_DWARF_UNWINDS) {
          fprintf(stderr, "Not enough space to load the unwinds\n"); for (;;) {}
        }
        unwinds[ no_unwinds ].base_addr      = base_addr;
        unwinds[ no_unwinds ].addr           = base_addr + addr;
        unwinds[ no_unwinds ].cfa_reg        = cfa_reg;
        unwinds[ no_unwinds ].cfa_reg_offset = cfa_reg_offset;
        memcpy(&unwinds[ no_unwinds ].regs[ 0u ], &regs[ 0u ], sizeof(regs));
        no_unwinds++;

#if 0
        fprintf(stdout, "%016llx %02u %5d :: ", base_addr + addr, cfa_reg, cfa_reg_offset);
        for (unsigned int i = 0u; i < MAX_NO_REGS; i++) {
          if (regs[ i ].rule == REG_RULE_CFA) {
            fprintf(stdout, "%5d ", regs[ i ].u.cfa);
          } else if (regs[ i ].rule == REG_RULE_REG) {
            fprintf(stdout, "%5u ", regs[ i ].u.reg);
          } else if (regs[ i ].rule == REG_RULE_UNDEFINED) {
            fprintf(stdout, "%5s ", "x");
          } else {
            fprintf(stdout, "%5s ", "none");
          }
        }
        fprintf(stdout, "%s\n", a);
#endif
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
            strerror(errno));
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
            insts[ no_insts ].binary    = get_binary(xed_file);
            insts[ no_insts ].base_addr = base_addr;
            insts[ no_insts ].addr      = addr;
            insts[ no_insts ].category  = xedd_category;
            insts[ no_insts ].iclass    = xedd_iclass;
            memcpy(&insts[ no_insts ].bytes[ 0u ], &inst_bytes[ 0u ], n_bytes);
            insts[ no_insts ].length    = xedd_length;

            if (xed_inst_get_attribute(xedd_dec, XED_ATTRIBUTE_INDIRECT_BRANCH) == 1u) {
              insts[ no_insts ].cofi.type |= INDIRECT_BRANCH;
            }
            if (xed_inst_get_attribute(xedd_dec, XED_ATTRIBUTE_FAR_XFER) == 1u) {
              insts[ no_insts ].cofi.type |= FAR_TRANSFER;
            }
            if (xedd_category == XED_CATEGORY_CALL) {
              if (insts[ no_insts ].cofi.type == 0u) {
                insts[ no_insts ].cofi.type |= UNCOND_DIRECT_BRANCH;
              }

              if (xedd_dec_no_mem_ops >= 1u) {
                if (xed_operand_values_has_memory_displacement(&xedd)) {
                  const xed_reg_enum_t xed_reg = xed_decoded_inst_get_base_reg(&xedd, 0u);

                  if (xed_reg == XED_REG_RIP) {
                    //insts[ no_insts ].cofi.u.c.addr = read_perfed_vm(perfed_pid, addr + xed_decoded_inst_get_memory_displacement(&xedd, 0u) + ((int64_t) (xedd_length)));
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
              if (insts[ no_insts ].cofi.type == 0u) {
                insts[ no_insts ].cofi.type |= (xedd_category == XED_CATEGORY_COND_BR) ? (COND_BRANCH) : (0u);
                insts[ no_insts ].cofi.type |= (xedd_category == XED_CATEGORY_UNCOND_BR) ? (UNCOND_DIRECT_BRANCH) : (0u);
              }

              if (xedd_dec_no_mem_ops >= 1u) {
                if (xed_operand_values_has_memory_displacement(&xedd)) {
                  const xed_reg_enum_t xed_reg = xed_decoded_inst_get_base_reg(&xedd, 0u);

                  if (xed_reg == XED_REG_RIP) {
                    //insts[ no_insts ].cofi.u.j.addr = read_perfed_vm(perfed_pid, addr + xed_decoded_inst_get_memory_displacement(&xedd, 0u) + ((int64_t) (xedd_length)));
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
              if (insts[ no_insts ].cofi.type == 0u) {
                insts[ no_insts ].cofi.type |= INDIRECT_BRANCH;
              }

              //fprintf(stdout, "\nRET %02x", insts[ no_insts ].cofi.type);
            } else if (xedd_category == XED_CATEGORY_SYSCALL) {
              if (insts[ no_insts ].cofi.type == 0u) {
                insts[ no_insts ].cofi.type |= FAR_TRANSFER;
              }
            }

#if 0
            if (1) {//if (xedd_category == XED_CATEGORY_CALL) {
              fprintf(stdout,
                      "\n%016llx :: %28s :: %16s %12s :: NO_OPS = %3u NO_MEM_OPS = %3u",
                      addr,
                      &xed_buffer[ 0u ],
                      xed_category_enum_t2str(xedd_category),
                      xed_iclass_enum_t2str(xedd_iclass),
                      xedd_dec_no_ops,
                      xedd_dec_no_mem_ops);
              }
#endif

#if 0
            fprintf(stdout, "\nINST ");
            for (unsigned int i = 0u; i < n_bytes; i++) {
              fprintf(stdout, "%02x ", inst_bytes[ i ]);
            }
            fprintf(stdout, "\n");
#endif

            no_insts++;
          }
        } else if (xed_error == XED_ERROR_BUFFER_TOO_SHORT) {
          goto xed_decode_inst;
        //} else if (xed_error == XED_ERROR_INVALID_FOR_CHIP) {
        // ?!?!
        //} else if (xed_error == XED_ERROR_INSTR_TOO_LONG) {
        // ?!?!
        } else {
          fprintf(stderr, "%016llx %3d :: %2u :: ", addr - base_addr, xed_error, n_bytes);
          for (unsigned int i = 0u; i < n_bytes; i++) {
            fprintf(stderr, "%02x ", inst_bytes[ i ]);
          }
          fprintf(stderr, "\n");
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
            strerror(errno));
  }
}

void perfed_xed(const int perfed_pid) {
  (void) perfed_pid;

  unwinds = malloc(MAX_NO_DWARF_UNWINDS * sizeof(dwarf_unwind_t));
  if (unwinds == NULL) {
    fprintf(stderr, "malloc failed\n"); for (;;) {}
  } else {
    fprintf(stdout, "unwinds size = %4llu MB\n", (MAX_NO_DWARF_UNWINDS * sizeof(inst_t)) / 1024llu / 1024llu);
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
}

void xed_intel_pt_ovf_fup(const unsigned long long int ip,
                          const double                 tsc,
                          const unsigned long long int cyc_cnt) {
  branches_n_en       = branches_n;
  branches_tsc_en     = tsc;
  branches_cyc_cnt_en = cyc_cnt;

  fprintf(branches_fp, "O :: %20.2lf %16llx\n", tsc, ip);
}

void xed_intel_pt_tip_enable(const unsigned long long int tip,
                             const double                 tsc,
                             const unsigned long long int cyc_cnt) {
  branches_n_en       = branches_n;
  branches_tsc_en     = tsc;
  branches_cyc_cnt_en = cyc_cnt;

  fprintf(branches_fp, "E :: %20.2lf %16llx\n", tsc, tip);
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
  (void) (cyc_cnt);

  fprintf(branches_fp, "W :: %20.2lf %16llx\n", tsc, ip);
}

void xed_intel_pt_tip_disable(const double                 tsc,
                              const unsigned long long int cyc_cnt) {
  (void) (cyc_cnt);

  if (tnt_queue_head != tnt_queue_tail) {
    fflush(branches_fp); fprintf(stderr, " BR Queue TIP_PGD\n"); for (;;) {}
  }
  if (tip_queue_head != tip_queue_tail) {
    fflush(branches_fp); fprintf(stderr, "TIP Queue TIP_PGD\n"); for (;;) {}
  }

  fprintf(branches_fp, "D :: %20.2lf\n", tsc);

  xed_reset_call_stack();
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
  {
    const inst_t* const i = xed_unwind_find_inst(uregs->rip);

    if (i != NULL) {
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

      if (i->cofi.type & COND_BRANCH) {
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

          case XED_ICLASS_JNZ: {
            const unsigned long long int zf = ZF(uregs->eflags);

            tnt = (zf == 0llu) ? (1u) : (0u);
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
  }
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
  call_stack_idx     = 0u;
  call_stack_idx_max = 0u;
}

void xed_reset_last_inst(void) {
  last_inst = -1ll;

  tnt_queue_head = tnt_queue_tail = 0u;
  tip_queue_head = tip_queue_tail = 0u;
}

void xed_update_last_inst(const unsigned long long addr) {
  const inst_t* const inst = xed_unwind_find_inst(addr);

  if (inst != NULL) {
    last_inst = ((signed long long) (inst - &insts[ 0u ]));
  } else {
    last_inst = -1ll;

    xed_close();
    fprintf(stderr, "Instruction %16llx not found\n", addr); for (;;) {}
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

  if (last_inst == -1ll) {
    return;
  }

#if 0
  fprintf(stdout, "XED TSC       = %20.2lf\n", tsc);
#endif

  tnt_t*       x_tnt = NULL;
  const tip_t* x_tip = NULL;

  if (tnt_len >= 1u) {
    const unsigned int tnt_queue_head_next = (tnt_queue_head + 1u) % TNT_QUEUE_LEN;

    if (tnt_queue_head_next != tnt_queue_tail) {
      tnt_queue[ tnt_queue_head ] = (tnt_t) {
        .tnt     = tnt,
        .tnt_len = tnt_len,
        .tsc     = tsc,
        .cyc_cnt = cyc_cnt
      };
      tnt_queue_head              = tnt_queue_head_next;
    } else {
      fprintf(stderr, "BR Queue full!\n"); for (;;) {}
    }
  }
  if (tip != 0llu) {
    const unsigned int tip_queue_head_next = (tip_queue_head + 1u) % TIP_QUEUE_LEN;

    if (tip_queue_head_next != tip_queue_tail) {
      tip_queue[ tip_queue_head ] = (tip_t) {
        .tip     = tip,
        .tsc     = tsc,
        .cyc_cnt = cyc_cnt
      };
      tip_queue_head              = tip_queue_head_next;
    } else {
      fprintf(stderr, "TIP Queue full!\n"); for (;;) {}
    }
  }

#if !defined(PRINT_XED) && !defined(PRINT_XED_BRANCHES_ONLY)
  fprintf(branches_fp, "%16llx\n", insts[ last_inst ].addr);
#endif
  for (;;) {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
#if defined(PRINT_XED_BRANCHES_ONLY)
    if (insts[ last_inst ].cofi.type != 0u) {
#endif
    sprintf(&branches_buffer[ 0u ],
            "%10llu %8llx %16llx %16.16s %10s %12s",
            branches_n,
            insts[ last_inst ].addr - insts[ last_inst ].base_addr,
            insts[ last_inst ].addr,
            insts[ last_inst ].binary,
            xed_category_enum_t2str(insts[ last_inst ].category),
            xed_iclass_enum_t2str(insts[ last_inst ].iclass));
#if defined(PRINT_XED_OPCODE)
    for (unsigned int j = 0u; j < 10u; j++) {
      const size_t k = strlen(&branches_buffer[ 0u ]);

      if (j < insts[ last_inst ].length) {
        sprintf(&branches_buffer[ k ], " %02x", ((unsigned int) (insts[ last_inst ].bytes[ j ])));
      } else {
        sprintf(&branches_buffer[ k ], "   ");
      }
    }
#endif
#if defined(PRINT_XED_BRANCHES_ONLY)
    }
#endif
#endif

    if (insts[ last_inst ].cofi.type == 0u) {
#if defined(PRINT_XED)
      fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ last_inst + 1ll ].addr);
#endif
      branches_n++;
      last_inst++;
    } else {
      if (tnt_queue_tail != tnt_queue_head) {
        x_tnt = &tnt_queue[ tnt_queue_tail ];
        if (x_tnt->tnt_len == 0u) {
          tnt_queue_tail = (tnt_queue_tail + 1u) % TNT_QUEUE_LEN;
          if (tnt_queue_tail != tnt_queue_head) {
            x_tnt = &tnt_queue[ tnt_queue_tail ];
          } else {
            x_tnt = NULL;
          }
        }
      } else {
        x_tnt = NULL;
      }
      if (tip_queue_tail != tip_queue_head) {
        x_tip = &tip_queue[ tip_queue_tail ];
      } else {
        x_tip = NULL;
      }

      if ((insts[ last_inst ].cofi.type & COND_BRANCH) != 0u) {
        if (x_tnt != NULL) {
          const unsigned int br = x_tnt->tnt & (1 << (x_tnt->tnt_len - 1u));

          x_tnt->tnt_len--;
          if (br != 0u) {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
            fprintf(branches_fp, "%s -> %16llx 1 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ last_inst ].cofi.u.j.addr, x_tnt->tsc);
#else
            fprintf(branches_fp, "%16llx 1 :: %20.2lf\n", insts[ last_inst ].cofi.u.j.addr, x_tnt->tsc);
#endif
            branches_n++;
            xed_update_last_inst(insts[ last_inst ].cofi.u.j.addr);
          } else {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
            fprintf(branches_fp, "%s -> %16llx 0 :: %20.2lf\n", &branches_buffer[ 0u ], insts[ last_inst + 1ll ].addr, x_tnt->tsc);
#else
            fprintf(branches_fp, "%16llx 0 :: %20.2lf\n", insts[ last_inst + 1ll ].addr, x_tnt->tsc);
#endif
            branches_n++;
            last_inst++;
          }
        } else {
          return;
        }
      } else if ((insts[ last_inst ].cofi.type & UNCOND_DIRECT_BRANCH) != 0u) {
        if (insts[ last_inst ].category == XED_CATEGORY_CALL) {
          call_stack[ call_stack_idx++ ] = (call_stack_t) {
            .ret = insts[ last_inst + 1ll ].addr
          };
          if (call_stack_idx > call_stack_idx_max) {
            call_stack_idx_max = call_stack_idx;
          }

#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ last_inst ].cofi.u.c.addr);
#endif
          branches_n++;
          xed_update_last_inst(insts[ last_inst ].cofi.u.c.addr);
        } else if (insts[ last_inst ].category == XED_CATEGORY_UNCOND_BR) {
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ last_inst ].cofi.u.j.addr);
#endif
          branches_n++;
          xed_update_last_inst(insts[ last_inst ].cofi.u.j.addr);
        } else {
          // What to do in this case?!
        }
      } else if ((insts[ last_inst ].cofi.type & INDIRECT_BRANCH) != 0u) {
#if defined(EN_RET_COMPRESSION)
        if (insts[ last_inst ].category == XED_CATEGORY_RET) {
          if (x_tnt != NULL) {
            const unsigned int br = x_tnt->tnt & (1 << (x_tnt->tnt_len - 1u));

            if (br != 0u) {
              if (call_stack_idx >= 1u) {
                const unsigned long long int ret = call_stack[ --call_stack_idx ].ret;

                x_tnt->tnt_len--;

#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
                fprintf(branches_fp, "%s -> %16llx 1 :: %20.2lf %8u\n", &branches_buffer[ 0u ], ret, x_tnt->tsc, call_stack_idx_max);
#else
                fprintf(branches_fp, "%16llx 1 :: %20.2lf %8u\n", ret, x_tnt->tsc, call_stack_idx_max);
#endif
                branches_n++;
                xed_update_last_inst(ret);
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
          const double ipc = ((double) (branches_n - branches_n_en)) / ((double) (x_tip->cyc_cnt - branches_cyc_cnt_en));

          if (insts[ last_inst ].category == XED_CATEGORY_CALL) {
            call_stack[ call_stack_idx++ ] = (call_stack_t) {
              .ret = insts[ last_inst + 1ll ].addr
            };
            if (call_stack_idx > call_stack_idx_max) {
              call_stack_idx_max = call_stack_idx;
            }
          } else if (insts[ last_inst ].category == XED_CATEGORY_UNCOND_BR) {
          } else if (insts[ last_inst ].category == XED_CATEGORY_RET) {
            if (call_stack_idx >= 1u) {
              call_stack_idx--;
            }
          } else {
            // What to do in this case?!
          }
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
          fprintf(branches_fp, "%s -> %16llx T :: %20.2lf %8.3lf\n", &branches_buffer[ 0u ], x_tip->tip, x_tip->tsc, ipc);
#else
          fprintf(branches_fp, "%16llx T :: %20.2lf %8.3lf\n", x_tip->tip, x_tip->tsc, ipc);
#endif
          branches_n++;
          xed_update_last_inst(x_tip->tip);
          tip_queue_tail = (tip_queue_tail + 1u) % TIP_QUEUE_LEN;
        } else {
          return;
        }
      } else if ((insts[ last_inst ].cofi.type & FAR_TRANSFER) != 0u) {
        if (insts[ last_inst ].category == XED_CATEGORY_CALL) {
          fprintf(stdout, "FAR CALL\n"); for (;;) {}
        } else if (insts[ last_inst ].category == XED_CATEGORY_RET) {
          fprintf(stdout, "FAR RET\n"); for (;;) {}
        }

#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
        fprintf(branches_fp, "%s -> %16llx\n", &branches_buffer[ 0u ], insts[ last_inst + 1ll ].addr);
#endif
        branches_n++;
        last_inst++;
        return;
      } else {
        fprintf(stderr, "Unknown cofi type\n"); for (;;) {}
      }
    }
  }
}

const inst_t* xed_unwind_find_inst(const unsigned long long int addr) {
  signed long long int a = 0ll;
  signed long long int b = ((signed long long int) (no_insts - 1llu));
  signed long long int m;

  while (a <= b) {
    m = (a + b) / 2ll;

    if (insts[ m ].addr == addr) {
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
      fprintf(stderr, "Unsorted instructions\n"); for (;;) {}
    }
  }
  for (unsigned long long int i = 0llu; i < no_insts; i++) {
    const unsigned long long int addr_offset = ((unsigned long long int) (insts[ i ].length)) - 1llu;

    insts[ i ].unwind = xed_unwind_find_dwarf(insts[ i ].addr + addr_offset);
    if (insts[ i ].unwind == NULL) {
      fprintf(stdout,
              "Unlinked unwind for instruction %16llx in %s\n",
              insts[ i ].addr - insts[ i ].base_addr,
              insts[ i ].binary);
    }
  }
}

void xed_close(void) {
  free(unwinds);
  free(insts);

  if ((branches_fp != NULL) && (branches_fp != stdout)) {
    fflush(branches_fp);
    fclose(branches_fp);
  }
}
