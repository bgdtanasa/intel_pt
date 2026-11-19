#ifndef _KMOD_
#define _KMOD_

#include <signal.h>

#define MAX_NO_US_MAPPINGS (256u)
#define MAX_NO_KS_MAPPINGS (512u)
#define MAX_NAME_LEN       (128u)

typedef struct __attribute__ ((__packed__)) {
  char          name[ MAX_NAME_LEN ];
  unsigned long addr;
  unsigned long sz;
  unsigned long perfing_addr;

  void*         vma;
  void*         perfing_vma;
} mapping_t;

extern unsigned long perfed_pgd;
extern mapping_t     us_mappings[ MAX_NO_US_MAPPINGS ];
extern unsigned int  no_us_mappings;
extern mapping_t     ks_mappings[ MAX_NO_KS_MAPPINGS ];
extern unsigned int  no_ks_mappings;

extern unsigned long long int vm_us;
extern unsigned long long int vm_kernel_ks;
extern unsigned long long int vm_kernel_ke;
extern unsigned long long int vm_kernel_ms;
extern unsigned long long int vm_kernel_me;

extern void perfed_kmod(const int perfed_pid);
extern void kmod_close(void);

#if defined(EN_PTRACE_UNWIND)
extern unsigned long long int kmod_find_stk_addr(const unsigned long long int addr);
#endif

#endif
