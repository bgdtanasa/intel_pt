#ifndef _XED_
#define _XED_

#include "xed-types.h"
#include "xed-category-enum.h"
#include "xed-iclass-enum.h"
#include "xed-reg-enum.h"
#include "xed-operand-enum.h"
#include "xed-common-defs.h"
#include "xed-flags.h"

#include "x_dwarf.h"

#include <sys/user.h>

#if 0
#define PRINT_XED
#else
#if 1
#define PRINT_XED_BRANCHES_ONLY
#endif
#endif
#if defined(PRINT_XED) || defined(PRINT_XED_BRANCHES_ONLY)
#if 0
#define PRINT_XED_OPCODE
#endif
#endif

#define COND_BRANCH          (1u << 0u)
#define UNCOND_DIRECT_BRANCH (1u << 1u)
#define INDIRECT_BRANCH      (1u << 2u)
#define FAR_TRANSFER         (1u << 3u)

#define MAX_NO_UNWINDS    (5000000llu)
#define MAX_NO_BINARIES   (512u)
#define MAX_BINARY_LENGTH (256u)
#define MAX_NO_INSTS      (70000000llu)

typedef enum {
  CFA_RULE_NONE,
  CFA_RULE_REG,
  CFA_RULE_EXP
} dwarf_cfa_rule_t;

typedef struct {
  dwarf_cfa_rule_t rule;
  struct {
    unsigned int reg;
    signed int   reg_offset;
    void*        exp;
  } s;
} dwarf_cfa_t;

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
  void* exp;
} dwarf_reg_t;

typedef struct {
  unsigned long long int base_addr;
  unsigned long long int addr;
  dwarf_cfa_t            cfa;
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
  const char*            binary;
  unsigned long long int base_addr;
  unsigned long long int addr;
  xed_category_enum_t    category;
  xed_iclass_enum_t      iclass;
  xed_uint_t             length;
  xed_uint8_t            bytes[ XED_MAX_INSTRUCTION_BYTES ];

  cofi_t                 cofi;

  const dwarf_unwind_t*  unwind;
} inst_t;

typedef struct {
  const inst_t* call;
  const inst_t* ret;
  double        tsc_c;
  double        tsc_r;

  unsigned long long int no_insts_c;
  unsigned long long int no_insts_r;
} call_stack_t;

#if defined(EN_PTRACE_UNWIND)
extern dwarf_unwind_t*    unwinds;
extern unsigned long long no_unwinds;
#endif
extern char               binaries[ MAX_NO_BINARIES ][ MAX_BINARY_LENGTH ];
extern unsigned int       no_binaries;
extern inst_t*            insts;
extern unsigned long long no_insts;

extern const char* parse_get_binary(const char* const xed_file, const unsigned int add_file);
#if defined(EN_PTRACE_UNWIND)
extern void        parse_dwarf(const char* const xed_file, const unsigned long long int base_addr);
#endif
extern void        parse_objdump(const int perfed_pid, const char* const xed_file, const unsigned long long int base_addr);

extern void perfed_xed(const int perfed_pid);

extern void xed_intel_pt_ovf_fup(const unsigned long long int ip,
                                 const double                 tsc,
                                 const unsigned long long int cyc_cnt);
extern void xed_intel_pt_tip_enable(const unsigned long long int tip,
                                    const double                 tsc,
                                    const unsigned long long int cyc_cnt);
extern void xed_intel_pt_bip_fup(const unsigned long long int a,
                                 const unsigned long long int b,
                                 const double                 tsc,
                                 const unsigned long long int pmu_mask,
                                 const unsigned long long int mem_addr);
extern void xed_intel_pt_ptw_fup(const unsigned long long int ip,
                                 const double                 tsc,
                                 const unsigned long long int cyc_cnt,
                                 const unsigned long long int ptw);
extern void xed_intel_pt_tip_disable(const double                 tsc,
                                     const unsigned long long int cyc_cnt);

extern void xed_tid_switch(const double       tsc,
                           const unsigned int sw_out);

extern void xed_ptrace_uregs(const double                         tsc,
                             const struct user_regs_struct* const uregs);

extern void xed_reset_call_stack(void);
extern void xed_reset_last_inst(void);
extern void xed_update_last_inst(const unsigned long long addr);
extern void xed_process_branches(const unsigned int           tnt,
                                 const unsigned int           tnt_len,
                                 const unsigned long long int tip,
                                 const double                 tsc,
                                 const unsigned long long int cyc_cnt);

extern void xed_async_reset(const unsigned long long int tip,
                            const double                 tsc,
                            const unsigned long long int cyc_cnt);
extern void xed_async_enter(const unsigned long long int tip,
                            const double                 tsc,
                            const unsigned long long int cyc_cnt);

extern void xed_tsc_err(const double tsc_err);

extern const inst_t*         xed_unwind_find_inst(const unsigned long long int addr);
#if defined(EN_PTRACE_UNWIND)
extern const dwarf_unwind_t* xed_unwind_find_dwarf(const unsigned long long int addr);
extern void                  xed_unwind_link_inst_and_dwarf(void);
#endif

extern void xed_close(void);

#endif
