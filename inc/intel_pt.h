#ifndef _INTEL_PT_
#define _INTEL_PT_

extern unsigned long long int intel_pt_decode(const unsigned char*   x,
                                              unsigned long long int n,
                                              const double           ts);

#endif
