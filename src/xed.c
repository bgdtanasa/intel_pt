#include "xed.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

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

static char* get_binary(const char* const xed_file) {
  (void) (xed_file);

  return NULL;
}

void parse_dwarf(const char* const xed_file, const unsigned long long int base_addr) {
  char dwarf_file[ 256u ];

  dwarf_file[ 0u ] = '\0';
  if (xed_file != NULL) {
    sprintf(&dwarf_file[ 0u ], "resources/%s.dwarf", xed_file);
  }

  FILE* const fp = fopen(dwarf_file, "r");
  if (fp != NULL) {
    char buffer[ 256u ];

    for (;;) {
      unsigned int  addr;
      unsigned char cfa_reg    = 0xFFu;
      signed int    cfa_offset = 0;
      char*         line       = fgets(&buffer[ 0u ], ((int) (sizeof(buffer))), fp);

      if (line != NULL) {
        // instruction address
        sscanf(line, "%x", &addr);
        while ((line[ 0u ] != 'C') && (line[ 1u ] != 'F') && (line[ 2u ] != 'A') && (line[ 3u ] != '=')) {
          line++;
        }
        line += 4u;

        // cfa reg
        if ((line[ 0u ] == 'R') && (line[ 1u ] == 'B') && (line[ 2u ] == 'P')) {
          cfa_reg = 6u;
        } else if ((line[ 0u ] == 'R') && (line[ 1u ] == 'S') && (line[ 2u ] == 'P')) {
          cfa_reg = 7u;
        } else {
          // ?!?!
        }
        line += 3u;

        // cfa offset
        sscanf(line, "%d", &cfa_offset);
        while (line[ 0u ] != 'R') {
          line++;
        }

        // regs
        fprintf(stdout, "%08x %03u %2d :: %s", addr, cfa_reg, cfa_offset, line);
      } else {
        break;
      }
    }

    fprintf(stdout, "no_unwinds = %9llu :: ", no_unwinds);
    fclose(fp);
  } else {
    fprintf(stderr,
            "fopen(%s) failed :: %s\n",
            dwarf_file,
            strerror(errno));
  }
}

void parse_objdump(const char* const xed_file, const unsigned long long int base_addr) {
  char obj_file[ 256u ];

  obj_file[ 0u ] = '\0';
  if (xed_file != NULL) {
    sprintf(&obj_file[ 0u ], "resources/%s.objdump", xed_file);
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
            const unsigned int        xedd_dec_no_mem_ops = xed_decoded_inst_number_of_memory_operands(&xedd);

            insts[ no_insts ].binary             = get_binary(xed_file);
            insts[ no_insts ].base_addr          = base_addr;
            insts[ no_insts ].addr               = addr;
            insts[ no_insts ].category           = xedd_category;
            insts[ no_insts ].iclass             = xedd_iclass;
            //insts[ no_insts ].no_operands        = xedd_dec_no_ops;
            //insts[ no_insts ].no_memory_operands = xedd_dec_no_mem_ops;

#if 0
            fprintf(stdout,
                    "%016llx :: %28s :: %16s %12s :: NO_OPS = %3u NO_MEM_OPS = %3u :: %2u :: ",
                    addr,
                    &xed_buffer[ 0u ],
                    xed_category_enum_t2str(xedd_category),
                    xed_iclass_enum_t2str(xedd_iclass),
                    xedd_dec_no_ops,
                    xedd_dec_no_mem_ops, 
                    xedd_length);
#endif

            for (unsigned int i = 0u; i < xedd_dec_no_ops; i++) {
              const xed_operand_t*     xedd_dec_op      = xed_inst_operand(xedd_dec, i);
              const xed_operand_enum_t xedd_dec_op_name = xed_operand_name(xedd_dec_op);

#if 0
              fprintf(stdout,
                      "\n\t%28s",
                      xed_operand_enum_t2str(xedd_dec_op_name));
#endif
              if (xedd_dec_op_name == XED_OPERAND_RELBR) {
                const xed_int64_t a = xed_decoded_inst_get_branch_displacement(&xedd);
                const xed_uint_t  b = xed_decoded_inst_get_branch_displacement_width(&xedd);
                const xed_uint_t  c = xed_decoded_inst_get_branch_displacement_width_bits(&xedd);

                insts[ no_insts ].relbr_operand = addr + a + xedd_length;
#if 0
                fprintf(stdout,
                        " :: %016llx %2u %2u",
                        addr + a + xedd_length,
                        b,
                        c);
#endif
              }
              if ((xedd_dec_op_name >= XED_OPERAND_REG0) && (xedd_dec_op_name <= XED_OPERAND_REG9)) {
#if 0
                fprintf(stdout, " :: %s ", xed_reg_enum_t2str(xed_decoded_inst_get_reg(&xedd, xedd_dec_op_name)));
#endif
              }
            }
            for (unsigned int i = 0u; i < xedd_dec_no_mem_ops; i++) {
              if (xed_operand_values_has_memory_displacement(&xedd)) {
                const xed_int64_t a = xed_decoded_inst_get_memory_displacement(&xedd, i);
                const xed_uint_t  b = xed_decoded_inst_get_memory_displacement_width(&xedd, i);

#if 0
                fprintf(stdout, " :: %lx %u", a, b);
#endif
              }
            }

#if 0
            fprintf(stdout, "\n\t:: ");
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

    fprintf(stdout, "no_insts = %9llu :: ", no_insts);
    fclose(fp);
  } else {
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

void xed_find_inst(const unsigned long long addr, const unsigned int execute_inst) {
  signed long long a = 0ll;
  signed long long b = ((signed long long) (no_insts - 1llu));

  while (a <= b) {
    last_inst = (a + b) / 2ll;

    if (insts[ last_inst ].addr == addr) {
      if (execute_inst) {
        xed_execute_current_inst();
      }
      return;
    }
    if (insts[ last_inst ].addr < addr) {
        a = last_inst + 1ll;
    } else {
        b = last_inst - 1ll;
    }
  }

  if (addr != 0llu) {
    fprintf(stdout, "%016llx NOT FOUND\n", addr);
  }
  last_inst = -1ll;
}

void xed_process_branches(unsigned int tnt, unsigned int tnt_len) {
  if (last_inst == -1ll) {
    return;
  }

  while (tnt_len >= 1u) {
    const unsigned int br = tnt & 0x01u;

    for (signed long long i = last_inst; i < ((signed long long) (no_insts)); i++) {
      fprintf(stdout,
              "%16llx %16llx %16s :: %12s %12s :: %u ::",
              insts[ i ].addr - insts[ i ].base_addr,
              insts[ i ].addr,
              insts[ i ].binary,
              xed_category_enum_t2str(insts[ i ].category),
              xed_iclass_enum_t2str(insts[ i ].iclass),
              br);
      last_inst++;

      if ((insts[ i ].category == XED_CATEGORY_COND_BR) || (insts[ i ].category == XED_CATEGORY_UNCOND_BR)) {
        if (br == 1u) {
          fprintf(stdout, "     Jumping to %16llx\n", insts[ i ].relbr_operand);

          xed_find_inst(insts[ i ].relbr_operand, 1u);
        } else {
          fprintf(stdout, " Not Jumping to %16llx\n", insts[ i ].relbr_operand);
        }
        break;
      }
      fprintf(stdout, "\n");
    }

    tnt     >>= 1u;
    tnt_len  -= 1u;
  }
}

void xed_execute_current_inst(void) {
  if (last_inst == -1ll) {
    return;
  }

  fprintf(stdout,
          "%16llx %16llx %16s :: %12s %12s",
          insts[ last_inst ].addr - insts[ last_inst ].base_addr,
          insts[ last_inst ].addr,
          insts[ last_inst ].binary,
          xed_category_enum_t2str(insts[ last_inst ].category),
          xed_iclass_enum_t2str(insts[ last_inst ].iclass));
  if ((insts[ last_inst ].category == XED_CATEGORY_COND_BR) || (insts[ last_inst ].category == XED_CATEGORY_UNCOND_BR)) {
    fprintf(stdout, " ::          Jumping to %16llx\n", insts[ last_inst ].relbr_operand);

    xed_find_inst(insts[ last_inst ].relbr_operand, 1u);
  } else {
    fprintf(stdout, "\n");

    last_inst++;
  }
}