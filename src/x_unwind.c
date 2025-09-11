#include "x_unwind.h"
#include "kmod.h"
#include "xed.h"

#include <stdio.h>
#include <time.h>

#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN (128u)
#endif

static FILE*                  unwind_fp;
static unsigned long long int cfa_regs[ MAX_NO_REGS ];
static char                   perfed_name[ TASK_COMM_LEN ];

extern unsigned long long int tsc_hz;

static inline __attribute__((always_inline)) unsigned long long read_tsc(void) {
    unsigned int lo;
    unsigned int hi;

    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));

    return (((unsigned long long) (hi)) << 32llu) |
           (((unsigned long long) (lo)) <<  0llu);
}

void perfed_unwind(const int perfed_pid) {
  unwind_fp = fopen("unwind.log", "w");
  if (unwind_fp == NULL) {
    unwind_fp = stdout;
  }
  {
    char  tmp[ 64u ];
    FILE* fp;

    snprintf(&tmp[ 0u ], sizeof(tmp), "/proc/%d/comm", perfed_pid);
    fp = fopen(&tmp[ 0u ], "r");
    if (fp != NULL) {
      fscanf(fp, "%s", &perfed_name[ 0u ]);

      fclose(fp);
    }
  }
}

void unwind(const int                            perfed_pid,
            const int                            perfed_cpu,
            const struct user_regs_struct* const regs) {
  unsigned long long int cfa;
  const inst_t*          inst;
  const dwarf_unwind_t*  unwind;
  struct timespec        a;
  struct timespec        b;

  cfa_regs[  0u ] = ((unsigned long long int) (regs->rax));
  cfa_regs[  1u ] = ((unsigned long long int) (regs->rdx));
  cfa_regs[  2u ] = ((unsigned long long int) (regs->rcx));
  cfa_regs[  3u ] = ((unsigned long long int) (regs->rbx));
  cfa_regs[  4u ] = ((unsigned long long int) (regs->rsi));
  cfa_regs[  5u ] = ((unsigned long long int) (regs->rdi));
  cfa_regs[  6u ] = ((unsigned long long int) (regs->rbp));
  cfa_regs[  7u ] = ((unsigned long long int) (regs->rsp));
  cfa_regs[  8u ] = ((unsigned long long int) (regs->r8));
  cfa_regs[  9u ] = ((unsigned long long int) (regs->r9));
  cfa_regs[ 10u ] = ((unsigned long long int) (regs->r10));
  cfa_regs[ 11u ] = ((unsigned long long int) (regs->r11));
  cfa_regs[ 12u ] = ((unsigned long long int) (regs->r12));
  cfa_regs[ 13u ] = ((unsigned long long int) (regs->r13));
  cfa_regs[ 14u ] = ((unsigned long long int) (regs->r14));
  cfa_regs[ 15u ] = ((unsigned long long int) (regs->r15));
  cfa_regs[ 16u ] = ((unsigned long long int) (regs->rip));

  fprintf(unwind_fp,
          "%s\t%d [%03d] %12.5lf: cpu-clock:\n",
          &perfed_name[ 0u ],
          perfed_pid,
          perfed_cpu,
          ((double) (read_tsc()) / ((double) (tsc_hz))));
  clock_gettime(CLOCK_MONOTONIC, &a);
unwind_again:
  inst   = xed_unwind_find_inst(cfa_regs[ 16u ]);
  if (inst == NULL) {
    unwind_close();
    fprintf(stderr, "UNWIND Instruction %16llx not found\n", cfa_regs[ 16u ]); for (;;) {}
  }
  unwind = (inst != NULL) ? (inst->unwind) : (NULL);
  if (unwind != NULL) {
    fprintf(unwind_fp,
            "\t%016llx %08llx_%s ([%s %016llx])\n",
            cfa_regs[ 16u ],
            inst->addr - inst->base_addr,
            inst->binary,
            inst->binary,
            unwind->addr - unwind->base_addr);

    // computing cfa
    if (unwind->cfa.rule == CFA_RULE_REG) {
      cfa = cfa_regs[ unwind->cfa.s.reg ] + unwind->cfa.s.reg_offset;
    } else if (unwind->cfa.rule == CFA_RULE_EXP) {
      cfa = execute_dwarf_cfa_exp(unwind->cfa.s.exp, cfa_regs);
    } else {
      fprintf(stdout, "Cfa default for instruction %16llx\n", unwind->addr); for (;;) {}
    }
    // updating cfa_regs
    for (unsigned int i = 0u; i <= 16u; i++) {
      switch (unwind->regs[ i ].rule) {
        case REG_RULE_NONE: {
          //if (i == 7u) {
          //  fprintf(stdout, "\t\tR%02u %016llx\n", i, cfa_regs[ i ]);
          //}
        } break;

        case REG_RULE_CFA: {
          const unsigned long long int x = cfa + unwind->regs[ i ].u.cfa;

          if ((perfed_vma_a <= x) && (x < perfed_vma_b)) {
            cfa_regs[ i ] = *((unsigned long long int*) (perfing_vma_a + x - perfed_vma_a));
          } else  {
            fprintf(stdout, "Reg %2u outside range :: %016llx\n", i, x); for (;;) {}
          }
        } break;

        case REG_RULE_REG: {
          fprintf(stdout, "Reg %2u REG_RULE_REG\n", i); for (;;) {}
        } break;

        case REG_RULE_EXP: {
          cfa_regs[ i ] = execute_dwarf_reg_exp(unwind->regs[ i ].exp, cfa_regs);
        } break;

        case REG_RULE_UNDEFINED: {
          goto unwind_done;
        } break;

        default: {
          fprintf(stdout, "Reg %2u default[%d]\n", i, unwind->regs[ i ].rule); for (;;) {}
        } break;
      }
    }
    // updating the RSP which is R7
    cfa_regs[ 7u ] = cfa;
    // unwinding more
    goto unwind_again;
  }
unwind_done:
  clock_gettime(CLOCK_MONOTONIC, &b);
  fprintf(stdout,
          "unwind ts = %12lld ns\n",
          ((signed long long) (b.tv_sec - a.tv_sec)) * 1000000000ll + ((signed long long) (b.tv_nsec - a.tv_nsec)));
  fprintf(unwind_fp, "\n");
}

void unwind_close(void) {
  if ((unwind_fp != NULL) && (unwind_fp != stdout)) {
    fflush(unwind_fp);
    fclose(unwind_fp);
  }
}
