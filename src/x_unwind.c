#include "x_unwind.h"
#include "kmod.h"
#include "proc.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

#if 1
#define DO_STATS
#endif

#define CACHE_NO_BITS (20u)
#define CACHE_MASK    ((1 << (CACHE_NO_BITS)) - 1u)
#define CACHE_LENGTH  ((CACHE_MASK) + 1u)

static const inst_t*          unwind_cache[ CACHE_LENGTH ];
static unsigned long long int cfa_regs[ MAX_NO_REGS ];

unwind_insts_t unwind_queue[ UNWIND_QUEUE_LEN ];
unsigned int   unwind_queue_head;
unsigned int   unwind_queue_tail;

extern double tsc_hz_ns;

void perfed_unwind(const int perfed_pid __attribute__((unused))) {
}

unwind_insts_t* unwind(const int                            perfed_pid,
                       const struct user_regs_struct* const uregs) {
  unsigned long long int cfa;
  const inst_t*          inst;
  const dwarf_unwind_t*  dwarf_unwind;
  unwind_insts_t* const  unwind_insts = &unwind_queue[ unwind_queue_head ];

  unwind_insts->tsc      = read_tsc();
  unwind_insts->no_insts = 0u;

  cfa_regs[  0u ] = ((unsigned long long int) (uregs->rax));
  cfa_regs[  1u ] = ((unsigned long long int) (uregs->rdx));
  cfa_regs[  2u ] = ((unsigned long long int) (uregs->rcx));
  cfa_regs[  3u ] = ((unsigned long long int) (uregs->rbx));
  cfa_regs[  4u ] = ((unsigned long long int) (uregs->rsi));
  cfa_regs[  5u ] = ((unsigned long long int) (uregs->rdi));
  cfa_regs[  6u ] = ((unsigned long long int) (uregs->rbp));
  cfa_regs[  7u ] = ((unsigned long long int) (uregs->rsp));
  cfa_regs[  8u ] = ((unsigned long long int) (uregs->r8));
  cfa_regs[  9u ] = ((unsigned long long int) (uregs->r9));
  cfa_regs[ 10u ] = ((unsigned long long int) (uregs->r10));
  cfa_regs[ 11u ] = ((unsigned long long int) (uregs->r11));
  cfa_regs[ 12u ] = ((unsigned long long int) (uregs->r12));
  cfa_regs[ 13u ] = ((unsigned long long int) (uregs->r13));
  cfa_regs[ 14u ] = ((unsigned long long int) (uregs->r14));
  cfa_regs[ 15u ] = ((unsigned long long int) (uregs->r15));
  cfa_regs[ 16u ] = ((unsigned long long int) (uregs->rip));

  kmod_redo_kmaps();
unwind_again:
  if (cfa_regs[ 16u ] == 0llu) {
    return NULL;
  }
  if ((unwind_cache[ cfa_regs[ 16u ] & CACHE_MASK ] != NULL) && (unwind_cache[ cfa_regs[ 16u ] & CACHE_MASK ]->addr == cfa_regs[ 16u ])) {
    inst = unwind_cache[ cfa_regs[ 16u ] & CACHE_MASK ];
  } else {
    inst = xed_unwind_find_inst(cfa_regs[ 16u ]);

    unwind_cache[ cfa_regs[ 16u ] & CACHE_MASK ] = inst;
  }
  if (inst == NULL) {
    fprintf(stderr, "UNWIND Instruction %16llx not found\n", cfa_regs[ 16u ]); return NULL; for (;;) {}
  }
  dwarf_unwind = (inst != NULL) ? (inst->dwarf_unwind) : (NULL);
  if (dwarf_unwind != NULL) {
    unwind_insts->insts[ unwind_insts->no_insts++ ] = inst;

    // computing cfa
    if (dwarf_unwind->cfa.rule == CFA_RULE_REG) {
      cfa = cfa_regs[ dwarf_unwind->cfa.s.reg ] + dwarf_unwind->cfa.s.reg_offset;
    } else if (dwarf_unwind->cfa.rule == CFA_RULE_EXP) {
      cfa = execute_dwarf_cfa_exp(dwarf_unwind->cfa.s.exp, cfa_regs);
    } else {
      fprintf(stdout, "Cfa default for instruction %16llx\n", dwarf_unwind->addr); for (;;) {}
    }
    // updating cfa_regs
    for (unsigned int i = 0u; i <= 16u; i++) {
      switch (dwarf_unwind->regs[ i ].rule) {
        case REG_RULE_NONE: {
          //if (i == 7u) {
          //  fprintf(stdout, "\t\tR%02u %016llx\n", i, cfa_regs[ i ]);
          //}
        } break;

        case REG_RULE_CFA: {
          const unsigned long long int perfed_x  = cfa + dwarf_unwind->regs[ i ].u.cfa;
          unsigned long long int       perfing_x = kmod_find_addr(perfed_x);

          if (perfing_x != 0llu) {
            cfa_regs[ i ] = *((unsigned long long int*) (perfing_x));
          } else  {
            perfing_x = proc_read_perfed_vm(perfed_pid, perfed_x);
            if (perfing_x != 0llu) {
              cfa_regs[ i ] = perfing_x;
            } else {
              fprintf(stderr,
                      "Reg %2u outside range :: %16llx :: %64s %16llx %16llx\n",
                      i,
                      perfed_x,
                      inst->binary,
                      inst->addr - inst->base_addr,
                      dwarf_unwind->addr - dwarf_unwind->base_addr); for (;;) {}
            }
          }
        } break;

        case REG_RULE_REG: {
          fprintf(stdout, "Reg %2u REG_RULE_REG\n", i); for (;;) {}
        } break;

        case REG_RULE_EXP: {
          cfa_regs[ i ] = execute_dwarf_reg_exp(dwarf_unwind->regs[ i ].exp, cfa_regs);
        } break;

        case REG_RULE_UNDEFINED: {
          goto unwind_done;
        } break;

        default: {
          fprintf(stdout, "Reg %2u default[%d]\n", i, dwarf_unwind->regs[ i ].rule); for (;;) {}
        } break;
      }
    }
    // updating the RSP which is R7
    cfa_regs[ 7u ] = cfa;
    // unwinding more
    goto unwind_again;
  }
unwind_done:
  unwind_queue_head = (unwind_queue_head + 1u) % UNWIND_QUEUE_LEN;
#if defined(DO_STATS)
  fprintf(stdout,
          "unwind = %5u %12.2lf us %6u %6u :: ",
          unwind_insts->no_insts,
          ((double) (read_tsc() - unwind_insts->tsc)) * tsc_hz_ns / 1000.0f,
          unwind_queue_tail,
          unwind_queue_head);
#endif

  return unwind_insts;
}

void unwind_close(void) {
}
