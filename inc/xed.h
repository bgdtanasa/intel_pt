#ifndef _XED_
#define _XED_

#include "xed-types.h"
#include "xed-category-enum.h"
#include "xed-iclass-enum.h"

#define MAX_NO_REGS (17u)

//#define PRINT_XED

typedef enum {
  REG_RULE_NONE,
  REG_RULE_CFA,
  REG_RULE_REG,
  REG_RULE_EXP,
  REG_RULE_UNDEFINED
} dwarf_reg_rule_t;

typedef struct {
  dwarf_reg_rule_t rule;
  union {
    signed int   cfa;
    unsigned int reg;
  } u;
} dwarf_reg_t;

typedef struct {
  unsigned long long int base_addr;
  unsigned long long int addr;
  unsigned char          cfa_reg;
  signed int             cfa_reg_offset;
  dwarf_reg_t            regs[ MAX_NO_REGS ];
} dwarf_unwind_t;

typedef struct {
  char*                  binary;
  unsigned long long int base_addr;
  unsigned long long int addr;
  xed_category_enum_t    category;
  xed_iclass_enum_t      iclass;
  //unsigned int           no_operands;
  //unsigned int           no_memory_operands;
  xed_uint_t             length;

  unsigned long long int relbr_operand;

  dwarf_unwind_t*        unwind;
} inst_t;

extern dwarf_unwind_t* unwinds;
extern inst_t*         insts;

extern void parse_dwarf(const char* const xed_file, const unsigned long long int base_addr);
extern void parse_objdump(const char* const xed_file, const unsigned long long int base_addr);
extern void perfed_xed(const int perfed_pid);
extern void xed_reset_last_inst(void);
extern void xed_find_inst(const unsigned long long addr, const unsigned int execute_last_inst);
extern void xed_process_branches(unsigned int tnt, unsigned int tnt_len);

extern inst_t*         xed_unwind_find_inst(const unsigned long long int addr);
extern dwarf_unwind_t* xed_unwind_find_dwarf(const unsigned long long int addr);
extern void            xed_unwind_link_inst_and_dwarf(void);

#endif
