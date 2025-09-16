#ifndef _KMOD_
#define _KMOD_

#define MAX_NO_KMAPS (512u)

typedef struct __attribute__ ((__packed__)) {
  unsigned long perfed_a;
  unsigned long perfed_b;
  unsigned long perfing_a;
  unsigned long perfing_b;
} kmap_t;

typedef kmap_t kmaps_t[ MAX_NO_KMAPS ];

extern kmaps_t       kmaps;
extern unsigned long no_kmaps;

extern void kmod_load(const int perfed_pid);
extern void kmod_unload(void);

extern unsigned long long int kmod_find_addr(const unsigned long long int addr);

#endif
