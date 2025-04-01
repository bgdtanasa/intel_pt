#include "xed.h"

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

#define MAX_NO_DWARF_UNWINDS (50000llu)
#define MAX_NO_INSTS         (2000000llu)

dwarf_unwind_t* unwinds;
inst_t*         insts;

static unsigned long long  no_unwinds;
static unsigned long long  no_insts;
static xed_chip_features_t chip_features;
static signed long long    last_inst = -1ll;

static unsigned int no_binaries;
static char         binaries[ 50 ][ 250 ];

static unsigned long long int read_perfed_vm(const int perfed_pid, const unsigned long long int addr) {
  unsigned long long int got_entry = 0llu;
  struct iovec           local     = {
    .iov_base = &got_entry,
    .iov_len  = sizeof(got_entry)
  };
  struct iovec           remote    = {
    .iov_base = ((void*) (addr)),
    .iov_len  = sizeof(got_entry)
  };
  ssize_t                n         = process_vm_readv(perfed_pid, &local, 1, &remote, 1, 0);

  return (n == -1) ? (0llu) : (got_entry);
}

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

static void xed_execute_last_inst(void) {
  if (last_inst == -1ll) {
    return;
  }

#if defined(PRINT_XED)
  fprintf(stdout,
          "%16llx %16llx %16s :: %12s %12s",
          insts[ last_inst ].addr - insts[ last_inst ].base_addr,
          insts[ last_inst ].addr,
          insts[ last_inst ].binary,
          xed_category_enum_t2str(insts[ last_inst ].category),
          xed_iclass_enum_t2str(insts[ last_inst ].iclass));
#endif
  if ((insts[ last_inst ].category == XED_CATEGORY_COND_BR) || (insts[ last_inst ].category == XED_CATEGORY_UNCOND_BR)) {
#if defined(PRINT_XED)
    fprintf(stdout, " ::          Jumping to %16llx\n", insts[ last_inst ].jmp_to.addr);
#endif

    xed_find_inst(insts[ last_inst ].jmp_to.addr, 1u);
  } else {
#if defined(PRINT_XED)
    fprintf(stdout, "\n");
#endif

    last_inst++;
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
        if (a[ 0 ] == ',') {
          a += 2;
          goto reg_again;
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

          xed_buffer[ 0u ] = '\0';
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
            const unsigned int        xedd_dec_no_ops     = xed_inst_noperands(xedd_dec);
            const xed_uint_t          xedd_dec_no_mem_ops = xed_decoded_inst_number_of_memory_operands(&xedd);

            insts[ no_insts ].binary             = get_binary(xed_file);
            insts[ no_insts ].base_addr          = base_addr;
            insts[ no_insts ].addr               = addr;
            insts[ no_insts ].category           = xedd_category;
            insts[ no_insts ].iclass             = xedd_iclass;
            //insts[ no_insts ].no_operands        = xedd_dec_no_ops;
            //insts[ no_insts ].no_memory_operands = xedd_dec_no_mem_ops;
            insts[ no_insts ].length             = xedd_length;

            if (xedd_category == XED_CATEGORY_CALL) {

            } else if ((xedd_category == XED_CATEGORY_COND_BR) || (xedd_category == XED_CATEGORY_UNCOND_BR)) {
              memset(&insts[ no_insts ].jmp_to, 0, sizeof(jmp_t));
              if (xedd_dec_no_mem_ops >= 1u) {
                if (xed_operand_values_has_memory_displacement(&xedd)) {
                  const xed_reg_enum_t xed_reg = xed_decoded_inst_get_reg(&xedd, xed_operand_name(xed_inst_operand(xedd_dec, 1u)));

                  insts[ no_insts ].jmp_to.mem_disp = xed_decoded_inst_get_memory_displacement(&xedd, 0u);
                  insts[ no_insts ].jmp_to.mem_reg  = xed_reg;
                  if (xed_reg == XED_REG_RIP) {
                    insts[ no_insts ].jmp_to.addr   = read_perfed_vm(perfed_pid, addr + insts[ no_insts ].jmp_to.mem_disp + ((int64_t) (xedd_length)));
                  } else {
                    // What to do in this case?!
                  }
                } else {
                  // What to do in this case?!
                }
              } else if (xedd_dec_no_ops >= 1u) {
                const xed_operand_enum_t xed_op = xed_operand_name(xed_inst_operand(xedd_dec, 0u));

                insts[ no_insts ].jmp_to.op = xed_op;
                if ((XED_OPERAND_REG0 <= xed_op) && (xed_op <= XED_OPERAND_REG9)) {
                  insts[ no_insts ].jmp_to.op_reg = xed_decoded_inst_get_reg(&xedd, xed_op);

                  // What to do in this case?!
                } else if (xed_op == XED_OPERAND_RELBR) {
                  insts[ no_insts ].jmp_to.addr   = addr + xed_decoded_inst_get_branch_displacement(&xedd) + ((int64_t) (xedd_length));
                } else {
                  // What to do in this case?!
                }
              } else {
                // What to do in this case?!
              }
            }

#if 0
            if (xedd_category == XED_CATEGORY_CALL) {
              fprintf(stdout,
                      "\n%016llx :: %28s :: %16s %12s :: NO_OPS = %3u NO_MEM_OPS = %3u",
                      addr,
                      &xed_buffer[ 0u ],
                      xed_category_enum_t2str(xedd_category),
                      xed_iclass_enum_t2str(xedd_iclass),
                      xedd_dec_no_ops,
                      xedd_dec_no_mem_ops);

              for (unsigned int i = 0u; i < xedd_dec_no_ops; i++) {
                const xed_operand_t*     xedd_dec_op      = xed_inst_operand(xedd_dec, i);
                const xed_operand_enum_t xedd_dec_op_name = xed_operand_name(xedd_dec_op);

                fprintf(stdout,
                        "\nOPS %2u %28s ",
                        i,
                        xed_operand_enum_t2str(xedd_dec_op_name));

                if (xedd_dec_op_name == XED_OPERAND_MEM0) {
                } else if ((xedd_dec_op_name >= XED_OPERAND_REG0) && (xedd_dec_op_name <= XED_OPERAND_REG9)) {
                  fprintf(stdout, "[%s]", xed_reg_enum_t2str(xed_decoded_inst_get_reg(&xedd, xedd_dec_op_name)));
                } else if (xedd_dec_op_name == XED_OPERAND_RELBR) {
                  const xed_int64_t a = xed_decoded_inst_get_branch_displacement(&xedd);
                  const xed_uint_t  b = xed_decoded_inst_get_branch_displacement_width(&xedd);
                  const xed_uint_t  c = xed_decoded_inst_get_branch_displacement_width_bits(&xedd);

                  fprintf(stdout,
                          "[%016llx %2u %2u]",
                          addr + a + xedd_length,
                          b,
                          c);
                }
              }
              for (xed_uint_t i = 0u; i < xedd_dec_no_mem_ops; i++) {
                if (xed_operand_values_has_memory_displacement(&xedd)) {
                  const xed_int64_t a = xed_decoded_inst_get_memory_displacement(&xedd, i);
                  const xed_uint_t  b = xed_decoded_inst_get_memory_displacement_width(&xedd, i);

                  fprintf(stdout, "\nMEM_OPS %2u [%lx %u]", i, a, b);
                }
              }
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
            if (no_insts >= MAX_NO_INSTS) {
              fprintf(stdout, "Not enough space to load the instructions\n");
            }
          }
        } else if (xed_error == XED_ERROR_BUFFER_TOO_SHORT) {
          goto xed_decode_inst;
        //} else if (xed_error == XED_ERROR_INVALID_FOR_CHIP) {
        //  // ?!?!
        } else {
          fprintf(stderr, "%016llx %3d :: %2u :: ", addr, xed_error, n_bytes);
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
    fprintf(stderr, "malloc failed\n");
  } else {
    fprintf(stdout, "unwinds size = %4llu MB\n", (MAX_NO_DWARF_UNWINDS * sizeof(inst_t)) / 1024llu / 1024llu);
  }
  insts = malloc(MAX_NO_INSTS * sizeof(inst_t));
  if (insts == NULL) {
    fprintf(stderr, "malloc failed\n");
  } else {
    fprintf(stdout, "insts size   = %4llu MB\n", (MAX_NO_INSTS * sizeof(inst_t)) / 1024llu / 1024llu);
  }

  xed_tables_init();
  xed_get_chip_features(&chip_features, XED_CHIP_ALL); //XED_CHIP_SNOW_RIDGE);
}

void xed_reset_last_inst(void) {
  last_inst = -1ll;
}

void xed_find_inst(const unsigned long long addr, const unsigned int execute_last_inst) {
  inst_t* inst = xed_unwind_find_inst(addr);

  if (inst != NULL) {
    last_inst = ((signed long long) (inst - &insts[ 0u ]));

    if (execute_last_inst) {
      xed_execute_last_inst();
    }
  } else {
    last_inst = -1ll;

    fprintf(stdout, "%016llx NOT FOUND\n", addr); for (;;) {}
  }
}

void xed_process_branches(unsigned int tnt, unsigned int tnt_len) {
  if (last_inst == -1ll) {
    return;
  }

  while (tnt_len >= 1u) {
    const unsigned int br = tnt & 0x01u;

    for (signed long long i = last_inst; i < ((signed long long) (no_insts)); i++) {
#if defined(PRINT_XED)
      fprintf(stdout,
              "%16llx %16llx %16s :: %12s %12s :: %u ::",
              insts[ i ].addr - insts[ i ].base_addr,
              insts[ i ].addr,
              insts[ i ].binary,
              xed_category_enum_t2str(insts[ i ].category),
              xed_iclass_enum_t2str(insts[ i ].iclass),
              br);
#endif
      last_inst++;

      if ((insts[ i ].category == XED_CATEGORY_COND_BR) || (insts[ i ].category == XED_CATEGORY_UNCOND_BR)) {
        if (br == 1u) {
#if defined(PRINT_XED)
          fprintf(stdout, "     Jumping to %16llx\n", insts[ i ].jmp_to.addr);
#endif

          xed_find_inst(insts[ i ].jmp_to.addr, 1u);
        } else {
#if defined(PRINT_XED)
          fprintf(stdout, " Not Jumping to %16llx\n", insts[ i ].jmp_to.addr);
#endif
        }
        break;
      }
#if defined(PRINT_XED)
      fprintf(stdout, "\n");
#endif
    }

    tnt     >>= 1u;
    tnt_len  -= 1u;
  }
}

inst_t* xed_unwind_find_inst(const unsigned long long int addr) {
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

dwarf_unwind_t* xed_unwind_find_dwarf(const unsigned long long int addr) {
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
  for (unsigned long long int i = 0llu; i < no_insts; i++) {
    const unsigned long long int addr_offset = ((unsigned long long int) (insts[ i ].length)) - 1llu;

    insts[ i ].unwind = xed_unwind_find_dwarf(insts[ i ].addr + addr_offset);
  }
}
