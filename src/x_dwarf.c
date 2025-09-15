#include "x_dwarf.h"
#include "kmod.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_DWARF_STK (64u)

typedef enum {
  X_DWARF_NONE,
  X_DWARF_AND,
  X_DWARF_BREG,
  X_DWARF_DEREF,
  X_DWARF_GE,
  X_DWARF_LIT,
  X_DWARF_MUL,
  X_DWARF_PLUS_UCONST,
  X_DWARF_PLUS,
  X_DWARF_SHL,
  X_DWARF_MAX
} dwarf_op_type_t;

typedef struct {
  dwarf_op_type_t      type;
  signed long long int op1;
  signed long long int op2;
} dwarf_op_t;

typedef dwarf_op_t dwarf_ops_t[ MAX_DWARF_STK ];

typedef struct {
  dwarf_ops_t  ops;
  unsigned int no_ops;
} dwarf_stk_t;

static char buffer_0[ MAX_DWARF_LINE ];
static char buffer_1[ MAX_DWARF_LINE ];

char* parse_dwarf_exp(const char* const line,
                      const char        terminator,
                      const signed int  terminator_pos,
                      void**            exp) {
  int   n;
  char* a = &buffer_0[ 0u ];
  char* b = &buffer_1[ 0u ];
  char* c = NULL;

  dwarf_stk_t* dwarf_stk = (dwarf_stk_t*) malloc(sizeof(dwarf_stk_t));
  memset(dwarf_stk, 0, sizeof(dwarf_stk_t));

  strcpy(a, line);
exp_again:
  if (strncmp(a, "and", 3) == 0) {
    n = sscanf(a, "and%[^\n]", b);
    if (n == 1) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_AND;
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "breg", 4) == 0) {
    unsigned int breg;
    unsigned int reg;
    signed int   reg_offset;

    n = sscanf(a, "breg%u R%u%d%[^\n]", &breg, &reg, &reg_offset, b);
    if (n == 4) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_BREG;
      dwarf_stk->ops[ dwarf_stk->no_ops ].op1  = ((signed long long int) (reg));
      dwarf_stk->ops[ dwarf_stk->no_ops ].op2  = ((signed long long int) (reg_offset));
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "deref", 5) == 0) {
    n = sscanf(a, "deref%[^\n]", b);
    if (n == 1) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_DEREF;
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "ge", 2) == 0) {
    n = sscanf(a, "ge%[^\n]", b);
    if (n == 1) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_GE;
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "lit", 3) == 0) {
    unsigned int lit;

    n = sscanf(a, "lit%u%[^\n]", &lit, b);
    if (n == 2) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_LIT;
      dwarf_stk->ops[ dwarf_stk->no_ops ].op1  = ((signed long long int) (lit));
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "mul", 3) == 0) {
    n = sscanf(a, "mul%[^\n]", b);
    if (n == 1) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_MUL;
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "plus_uconst", 11) == 0) {
    unsigned int uconst;

    n = sscanf(a, "plus_uconst %x%[^\n]", &uconst, b);
    if (n == 2) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_PLUS_UCONST;
      dwarf_stk->ops[ dwarf_stk->no_ops ].op1  = ((signed long long int) (uconst));
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "plus", 4) == 0) {
    n = sscanf(a, "plus%[^\n]", b);
    if (n == 1) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_PLUS;
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  } else if (strncmp(a, "shl", 3) == 0) {
    n = sscanf(a, "shl%[^\n]", b);
    if (n == 1) {
      dwarf_stk->ops[ dwarf_stk->no_ops ].type = X_DWARF_SHL;
    } else {
      fprintf(stdout, "line = %s\n", a); for (;;) {}
    }
  }
  dwarf_stk->no_ops++;
  if (dwarf_stk->no_ops >= MAX_DWARF_STK) {
    fprintf(stderr, "Not enough space to load the dwarf stack\n"); for (;;) {}
  }

  if (b[ 0u ] == ',') {
    b += 2;
    if (strncmp(b, "DW_OP_", 6) == 0) {
      b += 6;

      c = a; a = b; b = c; b[ 0u ] = '\0';
      goto exp_again;
    } else {
      fprintf(stderr, "0 b = %s\n", b); for (;;) {}
    }
  } else if (b[ 0u ] == terminator) {
    b += terminator_pos;
  } else {
    fprintf(stderr, "1 b = %s\n", b); for (;;) {}
  }

  *exp = dwarf_stk;
  return b;
}

unsigned long long int execute_dwarf_cfa_exp(const void* const      exp,
                                             unsigned long long int cfa_regs[ MAX_NO_REGS ]) {
  const dwarf_stk_t* const dwarf_stk = ((const dwarf_stk_t*) (exp));

  unsigned long long int stk[ MAX_DWARF_STK ];
  unsigned int           stk_idx = 0u;

  for (unsigned int i = 0u; i < dwarf_stk->no_ops; i++) {
    switch (dwarf_stk->ops[ i ].type) {
      case X_DWARF_AND: {
        if (stk_idx >= 2u) {
          const unsigned long int a = stk[ stk_idx - 1u ];
          const unsigned long int b = stk[ stk_idx - 2u ];

          stk_idx -= 2u;
          stk[ stk_idx ] = a & b;
          stk_idx++;
        } else {
          fprintf(stderr, "X_DWARF_AND broken stack\n"); for (;;) {}
        }
      } break;

      case X_DWARF_BREG: {
        stk[ stk_idx ] = cfa_regs[ dwarf_stk->ops[ i ].op1 ] + dwarf_stk->ops[ i ].op2;
        stk_idx++;
      } break;

      case X_DWARF_DEREF: {
        if (stk_idx >= 1u) {
          stk_idx--;
          if ((perfed_vma_a <= stk[ stk_idx ]) && (stk[ stk_idx ] < perfed_vma_b)) {
            stk[ stk_idx ] = *((unsigned long long int*) (perfing_vma_a + stk[ stk_idx ] - perfed_vma_a));
            stk_idx++;
          } else  {
            fprintf(stderr, "X_DWARF_DEREF outside range :: %016llx\n", stk[ stk_idx ]); for (;;) {}
          }
        } else {
          fprintf(stderr, "X_DWARF_DEREF broken stack\n"); for (;;) {}
        }
      } break;

      case X_DWARF_GE: {
        if (stk_idx >= 2u) {
          const unsigned long int a = stk[ stk_idx - 1u ];
          const unsigned long int b = stk[ stk_idx - 2u ];

          stk_idx -= 2u;
          stk[ stk_idx ] = (b >= a) ? (1llu) : (0llu);
          stk_idx++;
        } else {
          fprintf(stderr, "X_DWARF_GE broken stack\n"); for (;;) {}
        }
      } break;

      case X_DWARF_LIT: {
        stk[ stk_idx ] = dwarf_stk->ops[ i ].op1;
        stk_idx++;
      } break;

      case X_DWARF_PLUS_UCONST: {
        if (stk_idx >= 1u) {
          stk_idx--;
          stk[ stk_idx ] += dwarf_stk->ops[ i ].op1;
          stk_idx++;
        } else {
          fprintf(stderr, "X_DWARF_PLUS_UCONST broken stack\n"); for (;;) {}
        }
      } break;

      case X_DWARF_PLUS: {
        if (stk_idx >= 2u) {
          const unsigned long int a = stk[ stk_idx - 1u ];
          const unsigned long int b = stk[ stk_idx - 2u ];

          stk_idx -= 2u;
          stk[ stk_idx ] = a + b;
          stk_idx++;
        } else {
          fprintf(stderr, "X_DWARF_PLUS broken stack\n"); for (;;) {}
        }
      } break;

      case X_DWARF_SHL: {
        if (stk_idx >= 2u) {
          const unsigned long int a = stk[ stk_idx - 1u ];
          const unsigned long int b = stk[ stk_idx - 2u ];

          stk_idx -= 2u;
          stk[ stk_idx ] = b << a;
          stk_idx++;
        } else {
          fprintf(stderr, "X_DWARF_SHL broken stack\n"); for (;;) {}
        }
      } break;

      default: {
        fprintf(stderr, "CFA_EXP unhandled op %d\n", dwarf_stk->ops[ i ].type); for (;;) {}
      } break;
    }
  }

  if (stk_idx == 1u) {
    return stk[ 0u ];
  } else {
    fprintf(stderr, "CFA_EXP broken stack\n"); for (;;) {}
    return 0llu;
  }
}

unsigned long long int execute_dwarf_reg_exp(const void* const      exp,
                                             unsigned long long int cfa_regs[ MAX_NO_REGS ]) {
  const unsigned long long int x = execute_dwarf_cfa_exp(exp, cfa_regs);

  if ((perfed_vma_a <= x) && (x < perfed_vma_b)) {
    return *((unsigned long long int*) (perfing_vma_a + x - perfed_vma_a));
  } else  {
    fprintf(stdout, "REG_EXP outside range\n"); for (;;) {}
    return 0llu;
  }
}