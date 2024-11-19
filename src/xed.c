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

#define MAX_NO_INSTS (1000000)

inst_t* insts;

static unsigned long long  no_insts;
static xed_chip_features_t chip_features;
static signed long long    last_inst = -1ll;

static void my_xed_init(void) {
  insts = malloc(MAX_NO_INSTS * sizeof(inst_t));
  if (insts == NULL) {
    fprintf(stderr, "malloc failed\n");
  }

  xed_tables_init();
  xed_get_chip_features(&chip_features, XED_CHIP_SNOW_RIDGE);
}

static void parse_xed(const char* const xed_file) {
  FILE* const fp = fopen(xed_file, "r");
  if (fp != NULL) {
    char buffer[ 256u ];

    my_xed_init();
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
      line = fgets(&buffer[ 0u ], ((int) (sizeof(buffer))), fp);
      if (line != NULL) {
        if (xed_error == XED_ERROR_NONE) {
          sscanf(line, "%llx", &addr);
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

            insts[ no_insts ].addr               = addr;
            insts[ no_insts ].category           = xedd_category;
            insts[ no_insts ].iclass             = xedd_iclass;
            insts[ no_insts ].no_operands        = xedd_dec_no_ops;
            insts[ no_insts ].no_memory_operands = xedd_dec_no_mem_ops;

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
          }
        } else if (xed_error == XED_ERROR_BUFFER_TOO_SHORT) {
          goto xed_decode_inst;
        } else if (xed_error == XED_ERROR_INVALID_FOR_CHIP) {
          // ?!?!
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

    fprintf(stdout, "no_insts = %12llu\n", no_insts);
    fclose(fp);
  } else {
    fprintf(stderr,
            "fopen(%s) failed :: %s\n",
            xed_file,
            strerror(errno));
  }
}

void perfed_xed(const int perfed_pid) {
  char    exe_symlink[ 256u ];
  char    exe_path[ 256u ];
  ssize_t n;

  sprintf(&exe_symlink[ 0u ], "/proc/%d/exe", perfed_pid);
  if ((n = readlink(&exe_symlink[ 0u ], &exe_path[ 0u ], sizeof(exe_path))) == -1) {
    fprintf(stderr, "readlink failed %s\n", strerror(errno));
  } else {
    sprintf(&exe_path[ n ], "%s", ".objdump");
    parse_xed(&exe_path[ 0u ]);
  } 
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
              "             %16llx :: %12s %12s :: %u ::",
              insts[ i ].addr,
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
          "             %16llx :: %12s %12s",
          insts[ last_inst ].addr,
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