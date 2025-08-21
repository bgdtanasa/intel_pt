#ifndef _INTEL_PT_
#define _INTEL_PT_

extern unsigned int intel_pt_ovf;
extern unsigned int intel_pt_pge;
extern unsigned int intel_pt_pgd;

extern unsigned long long int intel_pt_decode(const volatile unsigned char* x,
                                              unsigned long long int        n);
extern void intel_pt_reset(void);

#endif
