#ifndef _INTEL_PT_
#define _INTEL_PT_

typedef enum {
  INTEL_PT_STATUS_DISABLE = 0,
  INTEL_PT_STATUS_ENABLE,
} intel_pt_status_t;

extern intel_pt_status_t intel_pt_status;

extern unsigned long long int intel_pt_decode(const volatile unsigned char*    x,
                                              unsigned long long int           n,
                                              const unsigned long long int     h,
                                              volatile unsigned long long int* h_p);

extern void intel_pt_enable(const int intel_pt_fd);
extern void intel_pt_disable(const int intel_pt_fd);

#endif
