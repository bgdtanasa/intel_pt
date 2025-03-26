#ifndef _KMOD_
#define _KMOD_

extern unsigned long perfing_vma_a;
extern unsigned long perfing_vma_b;
extern unsigned long perfed_vma_a;
extern unsigned long perfed_vma_b;

extern void kmod_load(const int perfed_pid);
extern void kmod_unload(void);

#endif
