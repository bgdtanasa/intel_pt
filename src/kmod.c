#include "kmod.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/syscall.h>

#define MAX_PARAMS_SZ (512u)

unsigned long    perfed_pgd;
mapping_t        us_mappings[ MAX_NO_US_MAPPINGS ];
unsigned int     no_us_mappings;
mapping_t        ks_mappings[ MAX_NO_KS_MAPPINGS ];
unsigned int     no_ks_mappings;
static mapping_t stk_mapping;

unsigned long long int vm_us        = 0llu;
unsigned long long int vm_kernel_ks = 0llu;
unsigned long long int vm_kernel_ke = 0llu;
unsigned long long int vm_kernel_ms = 0llu;
unsigned long long int vm_kernel_me = 0llu;

static const char* module_name = "kmod/main.ko";
static char        module_params[ MAX_PARAMS_SZ ];
static int         my_char_fd;

void perfed_kmod(const int perfed_pid) {
  int fd = open(module_name, O_RDONLY);

  if (fd == -1) {
    fprintf(stderr,
            "open(%s) failed :: %s\n",
            module_name,
            strerror(errno)); for (;;) {}
  } else {
    long ret;

    // Fault this memory to help the kernel module
    perfed_pgd = 0lu;
    memset(&us_mappings[ 0u ], 0, sizeof(us_mappings)); no_us_mappings = 0u;
    memset(&ks_mappings[ 0u ], 0, sizeof(ks_mappings)); no_ks_mappings = 0u;
    memset(&stk_mapping,       0, sizeof(stk_mapping));

    // Prepare the list of parameters
    sprintf(&module_params[ 0u ],
            "perfed_pid=%d _perfed_pgd=%lu _us_mappings=%lu _no_us_mappings=%lu _ks_mappings=%lu _no_ks_mappings=%lu _stk_mapping=%lu",
            perfed_pid,
            ((unsigned long) (&perfed_pgd)),
            ((unsigned long) (&us_mappings[ 0u ])),
            ((unsigned long) (&no_us_mappings)),
            ((unsigned long) (&ks_mappings[ 0u ])),
            ((unsigned long) (&no_ks_mappings)),
            ((unsigned long) (&stk_mapping)));
    ret = syscall(SYS_finit_module, fd, module_params, 0);
    if (ret == 0l) {
      fprintf(stdout, " finit_module(%s) success\n", module_name);

      vm_us        = 0x00007FFFFFFFFFFFllu + 1llu;
      vm_kernel_ks = ((unsigned long long int) (ks_mappings[ 0u ].addr));
      vm_kernel_ke = vm_kernel_ks + ((unsigned long long int) (ks_mappings[ 0u ].sz));
      vm_kernel_ms = ((unsigned long long int) (ks_mappings[ 1u ].addr));
      vm_kernel_me = ((unsigned long long int) (ks_mappings[ no_ks_mappings - 1u ].addr + ks_mappings[ no_ks_mappings - 1u ].sz));

      my_char_fd = open("/dev/my_char-0", O_RDWR);
      if (my_char_fd == -1) {
        fprintf(stderr, "open(/dev/my_char-0) failed :: %s\n", strerror(errno)); for (;;) {}
      }
      fprintf(stdout, "====== KMOD ======\n");
      fprintf(stdout, "perfed_pgd     = %16lx\n", perfed_pgd);
      fprintf(stdout, "my_char_fd     = %16d\n", my_char_fd);
      fprintf(stdout, "no_us_mappings = %16u\n", no_us_mappings);
      fprintf(stdout, "no_ks_mappings = %16u\n", no_ks_mappings);
      fprintf(stdout, "vm_us          = %16llx\n", vm_us);
      fprintf(stdout, "vm_kernel_ks   = %16llx\n", vm_kernel_ks);
      fprintf(stdout, "vm_kernel_ke   = %16llx\n", vm_kernel_ke);
      fprintf(stdout, "vm_kernel_ms   = %16llx\n", vm_kernel_ms);
      fprintf(stdout, "vm_kernel_me   = %16llx\n", vm_kernel_me);
      fprintf(stdout, "====== KMOD ======\n");
    } else {
      fprintf(stderr,
              "finit_module(%s) failed :: %s\n",
              module_name,
              strerror(errno)); for (;;) {}
    }
    close(fd);
  }
}

void kmod_close(void) {
  if (my_char_fd != -1) {
    close(my_char_fd);
  }

  long ret = syscall(SYS_delete_module, "main", 0);
  if (ret == 0l) {
    fprintf(stdout, "delete_module(%s) success\n", "main");
  } else {
    fprintf(stderr,
            "delete_module(%s) failed :: %s\n",
            "main",
            strerror(errno));
  }
}

#if defined(EN_PTRACE_UNWIND)
unsigned long long int kmod_find_stk_addr(const unsigned long long int addr) {
  if (addr == 0llu) {
    return 0llu;
  }

  if ((stk_mapping.addr <= addr) && (addr < stk_mapping.addr + stk_mapping.sz)) {
    return ((unsigned long long int) (stk_mapping.perfing_addr + addr - stk_mapping.addr));
  }

  return 0llu;
}
#endif
