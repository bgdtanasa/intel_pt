#ifndef _X_ELF_
#define _X_ELF_

extern unsigned int   binary_is_pie(const char* const path);
extern unsigned char* get_phdr_buffer(const char* const             path,
                                      const unsigned long long int  addr,
                                      unsigned long long int* const buffer_len);

#endif
