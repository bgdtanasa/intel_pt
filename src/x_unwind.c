#include "x_unwind.h"
#include "kmod.h"
#include "xed.h"

#include <stdio.h>
#include <time.h>

static unsigned long long int cfa_regs[ MAX_NO_REGS ];

void unwind(struct user_regs_struct* regs) {
  unsigned long long int cfa;
  inst_t*                inst;
  dwarf_unwind_t*        dw;
  struct timespec        a;
  struct timespec        b;
  unsigned int           cfa_idx = 0u;

  cfa_regs[  0u ] = ((unsigned long long int) (regs->rax));
  cfa_regs[  1u ] = ((unsigned long long int) (regs->rdx));
  cfa_regs[  2u ] = ((unsigned long long int) (regs->rcx));
  cfa_regs[  3u ] = ((unsigned long long int) (regs->rbx));
  cfa_regs[  4u ] = ((unsigned long long int) (regs->rsi));
  cfa_regs[  5u ] = ((unsigned long long int) (regs->rdi));
  cfa_regs[  6u ] = ((unsigned long long int) (regs->rbp));
  cfa_regs[  7u ] = ((unsigned long long int) (regs->rsp));
  cfa_regs[ 16u ] = ((unsigned long long int) (regs->rip));

  clock_gettime(CLOCK_MONOTONIC, &a);
unwind_again:
  inst = xed_unwind_find_inst(cfa_regs[ 16u ]);
  dw   = inst->unwind;
  if (dw != NULL) {
#if 0
    fprintf(stdout, "%2u :: %12s %016llx :: ", cfa_idx, inst->binary, inst->addr - inst->base_addr);
    fprintf(stdout,
            "%016llx %02u %5d :: ",
            dw->addr - dw->base_addr,
            dw->cfa_reg,
            dw->cfa_reg_offset);
#endif

    // computing cfa
    cfa_idx++;
    cfa = perfee_vma_a + (cfa_regs[ dw->cfa_reg ] + dw->cfa_reg_offset - perfed_vma_a);

    // updating cfa_regs
    for (unsigned int i = 0u; i <= 7u; i++) {
      switch (dw->regs[ i ].rule) {
        case REG_RULE_CFA:
          cfa_regs[ i ] = *((unsigned long long int*) (cfa + dw->regs[ i ].u.cfa));
        break;

        case REG_RULE_REG:
        break;

        default:
        break;
      }
    }

    // computing ra
    switch (dw->regs[ 16u ].rule) {
      case REG_RULE_CFA:
        cfa_regs[ 16u ] = *((unsigned long long int*) (cfa + dw->regs[ 16u ].u.cfa));

        fprintf(stdout, "%2u :: RA = %016llx\n", cfa_idx, cfa_regs[ 16u ]);
        goto unwind_again;
      break;

      case REG_RULE_REG:
      break;

      default:
      break;
    }
  }
  fprintf(stdout, "\n");
  clock_gettime(CLOCK_MONOTONIC, &b);
  fprintf(stdout,
          "unwind ts = %12lld ns\n",
          ((signed long long) (b.tv_sec - a.tv_sec)) * 1000000000ll + ((signed long long) (b.tv_nsec - a.tv_nsec)));
}
