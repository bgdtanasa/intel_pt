#ifndef _XED_
#define _XED_

#include "xed-types.h"
#include "xed-category-enum.h"
#include "xed-iclass-enum.h"
#include "xed-reg-enum.h"
#include "xed-operand-enum.h"

#define MAX_NO_REGS (17u)

#if 1
#define PRINT_XED
#if 0
#define PRINT_XED_BRANCHES_ONLY
#endif
#endif

#define COND_BRANCH          (1u << 0u)
#define UNCOND_DIRECT_BRANCH (1u << 1u)
#define INDIRECT_BRANCH      (1u << 2u)
#define FAR_TRANSFER         (1u << 3u)

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
  unsigned long long int addr;
} ret_t;

typedef struct {
  unsigned long long int addr;
  ret_t                  ret_to;
} call_t;

typedef struct {
  unsigned long long int addr;
} jmp_t;

typedef struct {
  unsigned int type;
  union {
    call_t c;
    jmp_t  j;
    ret_t  r;
  } u;
} cofi_t;

typedef struct {
  char*                  binary;
  unsigned long long int base_addr;
  unsigned long long int addr;
  xed_category_enum_t    category;
  xed_iclass_enum_t      iclass;
  xed_uint_t             length;

  cofi_t                 cofi;

  dwarf_unwind_t*        unwind;
} inst_t;

extern dwarf_unwind_t* unwinds;
extern inst_t*         insts;

extern void parse_dwarf(const char* const xed_file, const unsigned long long int base_addr);
extern void parse_objdump(const int perfed_pid, const char* const xed_file, const unsigned long long int base_addr);
extern void perfed_xed(const int perfed_pid);
extern void xed_reset_last_inst(void);
extern void xed_update_last_inst(const unsigned long long addr);
extern void xed_process_branches(const unsigned int           tnt,
                                 const unsigned int           tnt_len,
                                 const unsigned long long int tip);

extern inst_t*         xed_unwind_find_inst(const unsigned long long int addr);
extern dwarf_unwind_t* xed_unwind_find_dwarf(const unsigned long long int addr);
extern void            xed_unwind_link_inst_and_dwarf(void);

#endif
