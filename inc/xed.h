#ifndef _XED_
#define _XED_

#include "xed-category-enum.h"
#include "xed-iclass-enum.h"

#define MAX_NO_REGS (32u)

typedef struct {
  unsigned long long int base_addr;
  unsigned long long int addr;
  unsigned char          cfa_reg;
  signed int             cfa_offset;
  signed int             reg_offset[ MAX_NO_REGS ];
} dwarf_unwind_t;

typedef struct {
  char*                  binary;
  unsigned long long int base_addr;
  unsigned long long int addr;
  xed_category_enum_t    category;
  xed_iclass_enum_t      iclass;
  //unsigned int           no_operands;
  //unsigned int           no_memory_operands;

  unsigned long long int relbr_operand;

  dwarf_unwind_t*        dwarf_unwind;
} inst_t;

extern dwarf_unwind_t* unwinds;
extern inst_t*         insts;

extern void parse_dwarf(const char* const xed_file, const unsigned long long int base_addr);
extern void parse_objdump(const char* const xed_file, const unsigned long long int base_addr);
extern void perfed_xed(const int perfed_pid);
extern void xed_find_inst(const unsigned long long addr, const unsigned int execute_inst);
extern void xed_process_branches(unsigned int tnt, unsigned int tnt_len);
extern void xed_execute_current_inst(void);

#endif
