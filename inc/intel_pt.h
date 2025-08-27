#ifndef _INTEL_PT_
#define _INTEL_PT_

extern unsigned long long int intel_pt_decode(const volatile unsigned char*    x,
                                              unsigned long long int           n,
                                              const unsigned long long int     h,
                                              volatile unsigned long long int* h_p);
extern void intel_pt_reset(void);

#endif
