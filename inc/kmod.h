#ifndef _KMOD_
#define _KMOD_

#include <signal.h>

#define MAX_NO_KMAPS (512u)
#define MAX_NO_MODS  (256u)

typedef struct __attribute__ ((__packed__)) {
  unsigned long perfed_a;
  unsigned long perfed_b;
  unsigned long perfing_a;
  unsigned long perfing_b;
} kmap_t;

typedef kmap_t kmaps_t[ MAX_NO_KMAPS ];

typedef struct __attribute__ ((__packed__)) {
  char          name[ 128u ];
  unsigned long addr;
  unsigned long sz;

  unsigned long perfing_addr;
} mod_t;

typedef mod_t mods_t[ MAX_NO_MODS ];

extern unsigned long         perfed_pgd;
extern kmaps_t               kmaps;
extern unsigned long         no_kmaps;
extern mods_t                mods;
extern unsigned long         no_mods;
extern volatile sig_atomic_t redo_kmaps;

extern unsigned long long int vm_us;
extern unsigned long long int vm_kernel_ks;
extern unsigned long long int vm_kernel_ke;
extern unsigned long long int vm_kernel_ms;
extern unsigned long long int vm_kernel_me;

extern void perfed_kmod(const int perfed_pid);
extern void kmod_close(void);

extern unsigned long long int kmod_find_addr(const unsigned long long int addr);
extern void kmod_redo_kmaps(void);

#endif
