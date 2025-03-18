#include "kmod.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/syscall.h>

#define MAX_SZ_PARAMS (512u)

unsigned long perfee_vma_a;
unsigned long perfee_vma_b;
unsigned long perfed_vma_a;
unsigned long perfed_vma_b;

static const char* module_name = "kmod/main.ko";
static char        module_params[ MAX_SZ_PARAMS ];

void kmod_load(const int perfed_pid) {
  int fd = open(module_name, O_RDONLY);

  if (fd == -1) {
    fprintf(stderr,
            "open(%s) failed :: %s\n",
            module_name,
            strerror(errno));
  } else {
    long ret;

    sprintf(&module_params[ 0u ],
            "perfed_pid=%d perfee_vma_a=%lu perfee_vma_b=%lu perfed_vma_a=%lu perfed_vma_b=%lu",
            perfed_pid,
            ((unsigned long) (&perfee_vma_a)),
            ((unsigned long) (&perfee_vma_b)),
            ((unsigned long) (&perfed_vma_a)),
            ((unsigned long) (&perfed_vma_b)));
    ret = syscall(SYS_finit_module, fd, module_params, 0);
    if (ret == 0) {
      fprintf(stdout, " finit_module(%s) success\n", module_name);
      fprintf(stdout, "perfee_vma_a = %016lx\n", perfee_vma_a);
      fprintf(stdout, "perfee_vma_b = %016lx\n", perfee_vma_b);
      fprintf(stdout, "perfed_vma_a = %016lx\n", perfed_vma_a);
      fprintf(stdout, "perfed_vma_b = %016lx\n", perfed_vma_b);

#if 0
      {
        unsigned long long int* x = (unsigned long long int*) (perfee_vma_a);

        for (unsigned long i = 0lu; i < (perfee_vma_b - perfee_vma_a) / 8lu; i++) {
          fprintf(stdout, "%016llx ", x[ i ]);

          if (((i + 1lu) % 8lu) == 0lu) {
            fprintf(stdout, "\n");
          }
        }
        fprintf(stdout, "\n");
      }
#endif

#if 0
      ret = syscall(SYS_delete_module, "main", 0);
      if (ret == 0) {
        fprintf(stdout, "delete_module(%s) success\n", "main");
      } else {
        fprintf(stderr,
                "delete_module(%s) failed :: %s\n",
                "main",
                strerror(errno));
      }
#endif
    } else {
      fprintf(stderr,
              "finit_module(%s) failed :: %s\n",
              module_name,
              strerror(errno));
    }
    close(fd);
  }
}
