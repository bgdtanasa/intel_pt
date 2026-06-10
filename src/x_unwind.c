#include "x_unwind.h"
#include "kmod.h"
#include "proc.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

#define CACHE_NO_BITS (20u)
#define CACHE_MASK    ((1 << (CACHE_NO_BITS)) - 1u)
#define CACHE_LENGTH  ((CACHE_MASK) + 1u)

static const inst_t*          unwind_cache[ CACHE_LENGTH ];
static unsigned long long int cfa_regs[ MAX_NO_REGS ];

unwind_insts_t* unwind_queue;
unsigned int    unwind_queue_head;
unsigned int    unwind_queue_tail;

extern char   perfed_name[ ];
extern double tsc_hz_ns;

void perfed_unwind(const int perfed_pid __attribute__((unused))) {
  unwind_queue = (unwind_insts_t*) malloc(UNWIND_QUEUE_LEN * sizeof(unwind_insts_t));
  if (unwind_queue == NULL) {
    fprintf(stderr, "malloc failed\n"); for (;;) {}
  } else {
    memset(unwind_queue, 0, UNWIND_QUEUE_LEN * sizeof(unwind_insts_t));
    fprintf(stdout, "unwind_queue size  = %4llu MB\n", (UNWIND_QUEUE_LEN * sizeof(unwind_insts_t)) / 1024llu / 1024llu);
  }
}

unwind_insts_t* unwind(const int                            perfed_pid,
                       const struct user_regs_struct* const uregs) {
  unsigned long long int cfa;
  const inst_t*          inst;
  const dwarf_unwind_t*  dwarf_unwind;
  unwind_insts_t* const  unwind_insts = &unwind_queue[ unwind_queue_head ];

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
  dwarf_unwind = inst->dwarf_unwind;
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

  return unwind_insts;
}

unsigned int drop_unwinds(const double a, const double b) {
  unsigned int n = 0u;

  if ((a == 0.0) || (b == 0.0)) {
    return 0u;
  }
  for (;;) {
    unwind_insts_t* const unwind_insts = &unwind_queue[ unwind_queue_tail ];
    const double          attach_tsc   = ((double) (unwind_insts->attach_tsc));

    if ((a <= attach_tsc) && (attach_tsc <= b)) {
      n++;
      fprintf(stdout, "Dropping unwind at %20.2lf %20.2lf %20.2lf :: %6u %6u\n", a, attach_tsc, b, unwind_queue_tail, unwind_queue_head);

      memset(unwind_insts, 0, sizeof(unwind_insts_t));
      unwind_queue_tail = (unwind_queue_tail + 1u) % UNWIND_QUEUE_LEN;
      if (unwind_queue_tail > unwind_queue_head) {
        unwind_queue_head = 0u;
        unwind_queue_tail = 0u;

        break;
      }
    } else {
      break;
    }
  }

  return n;
}

void print_unwind(FILE* const fp) {
  unwind_insts_t* const unwind_insts = &unwind_queue[ unwind_queue_tail ];
  const double          attach_tsc   = ((double) (unwind_insts->attach_tsc));

  if (unwind_insts->no_insts >= 1u) {
    fprintf(stdout, "Printing unwind at %20.2lf %20.2lf %20.2lf :: %6u %6u\n", 0.0, attach_tsc, 0.0, unwind_queue_tail, unwind_queue_head);

    fprintf(fp, "AT :: %20.2lf\n", ((double) (unwind_insts->attach_tsc)));

    fprintf(fp,
            "%s\t%d [%03d] %20.2lf: cpu-clock: %6u %6u\n",
            &perfed_name[ 0u ],
            0,
            0,
            0.0,
            unwind_queue_tail,
            unwind_queue_head);
    for (unsigned int i = 0u; i < unwind_insts->no_insts; i++) {
      const inst_t* const inst = unwind_insts->insts[ i ];

      fprintf(fp,
              "\t%16llx %08llx_%s ([%s %016llx])\n",
              inst->addr,
              inst->addr - inst->base_addr,
              inst->binary,
              inst->binary,
              inst->dwarf_unwind->addr - inst->dwarf_unwind->base_addr);
    }
    fprintf(fp, "\n");

    fprintf(fp, "DT :: %20.2lf\n", ((double) (unwind_insts->detach_tsc)));

    memset(unwind_insts, 0, sizeof(unwind_insts_t));
    unwind_queue_tail = (unwind_queue_tail + 1u) % UNWIND_QUEUE_LEN;
  }
}

void unwind_close(void) {
  free(unwind_queue);
}
