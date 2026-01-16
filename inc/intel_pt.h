#ifndef _INTEL_PT_
#define _INTEL_PT_

#if 0
#define AUX_DBG
#endif

typedef enum {
  INTEL_PT_STATUS_DISABLE = 0,
  INTEL_PT_STATUS_ENABLE,
} intel_pt_status_t;

extern intel_pt_status_t intel_pt_status;

extern unsigned long long int intel_pt_decode(const volatile unsigned char*    x,
                                              unsigned long long int           n,
#if defined(AUX_DBG)
                                              const unsigned long long int     h,
                                              volatile unsigned long long int* h_p);
#else
                                              const unsigned long long int     h __attribute__((unused)),
                                              volatile unsigned long long int* h_p __attribute__((unused)));
#endif

extern void intel_pt_enable(const int intel_pt_fd);
extern void intel_pt_disable(const int intel_pt_fd);

#endif
