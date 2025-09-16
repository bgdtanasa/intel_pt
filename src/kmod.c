#include "kmod.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/syscall.h>

#define MAX_SZ_PARAMS (512u)

kmaps_t       kmaps;
unsigned long no_kmaps;

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

    // Fault this memory to help the kernel module
    memset(&kmaps[ 0u ], 0, sizeof(kmaps));
    sprintf(&module_params[ 0u ],
            "perfed_pid=%d kmaps=%lu no_kmaps=%lu",
            perfed_pid,
            ((unsigned long) (&kmaps[ 0u ])),
            ((unsigned long) (&no_kmaps)));
    ret = syscall(SYS_finit_module, fd, module_params, 0);
    if (ret == 0l) {
      fprintf(stdout, " finit_module(%s) success\n", module_name);

      for (unsigned long i = 0lu; i < no_kmaps; i++) {
        fprintf(stdout,
                "kmap[ %4lu %16llx ] %016lx %016lx %8llu KB\n",
                i,
                ((unsigned long long int) (&kmaps[ i ])),
                kmaps[ i ].perfed_a,
                kmaps[ i ].perfing_a,
                (kmaps[ i ].perfed_b - kmaps[ i ].perfed_a) / 1024llu);
      }
    } else {
      fprintf(stderr,
              "finit_module(%s) failed :: %s\n",
              module_name,
              strerror(errno));
    }
    close(fd);
  }
}

void kmod_unload(void) {
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

unsigned long long int kmod_find_addr(const unsigned long long int addr) {
  for (unsigned long i = 0lu; i < no_kmaps; i++) {
    if ((kmaps[ i ].perfed_a <= addr) && (addr < kmaps[ i ].perfed_b)) {
      return ((unsigned long long int) (kmaps[ i ].perfing_a + addr - kmaps[ i ].perfed_a));
    }
  }

  return 0llu;
}
