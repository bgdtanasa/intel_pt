#ifndef _XED_
#define _XED_

#include "xed-category-enum.h"
#include "xed-iclass-enum.h"

typedef struct {
  unsigned long long  addr;
  xed_category_enum_t category;
  xed_iclass_enum_t   iclass;
  unsigned int        no_operands;
  unsigned int        no_memory_operands;

  unsigned long long  relbr_operand;
} inst_t;

extern inst_t* insts;

extern void perfed_xed(const int perfed_pid);
extern void xed_find_inst(const unsigned long long addr, const unsigned int execute_inst);
extern void xed_process_branches(unsigned int tnt, unsigned int tnt_len);
extern void xed_execute_current_inst(void);

#endif
