#include "kmod.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/syscall.h>

#define MAX_SZ_PARAMS (512u)

unsigned long         perfed_pgd;
kmaps_t               kmaps;
unsigned long         no_kmaps;
volatile unsigned int no_new_a_maps;

static const char* module_name = "kmod/main.ko";
static char        module_params[ MAX_SZ_PARAMS ];
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
    memset(&kmaps[ 0u ], 0, sizeof(kmaps));
    no_kmaps = 0lu;
    // Prepare the list of parameters
    sprintf(&module_params[ 0u ],
            "perfed_pid=%d perfed_pgd=%lu kmaps=%lu no_kmaps=%lu",
            perfed_pid,
            ((unsigned long) (&perfed_pgd)),
            ((unsigned long) (&kmaps[ 0u ])),
            ((unsigned long) (&no_kmaps)));
    ret = syscall(SYS_finit_module, fd, module_params, 0);
    if (ret == 0l) {
      fprintf(stdout, " finit_module(%s) success\n", module_name);

      my_char_fd = open("/dev/my_char-0", O_RDWR);
      if (my_char_fd == -1) {
        fprintf(stderr,
                "open(/dev/my_char-0) failed :: %s\n",
                strerror(errno)); for (;;) {}
      }
      fprintf(stdout, "perfed_pgd = %16lx\n", perfed_pgd);
      fprintf(stdout,
              "my_char_fd = %d no_kmaps = %lu no_new_a_maps = %2u\n",
              my_char_fd,
              no_kmaps,
              no_new_a_maps);
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

unsigned long long int kmod_find_addr(const unsigned long long int addr) {
  if (addr == 0llu) {
    return 0llu;
  }

  signed long long int        a = 0ll;
  signed long long int        b = ((signed long long int) (no_kmaps - 1llu));
  static signed long long int m = 0ll;

  if (m != 0ll) {
    if ((kmaps[ m ].perfed_a <= addr) && (addr <= kmaps[ m ].perfed_b)) {
      return ((unsigned long long int) (kmaps[ m ].perfing_a + addr - kmaps[ m ].perfed_a));
    } else if (kmaps[ m ].perfed_a < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }

  while (a <= b) {
    m = (a + b) / 2ll;

    if ((kmaps[ m ].perfed_a <= addr) && (addr <= kmaps[ m ].perfed_b)) {
      return ((unsigned long long int) (kmaps[ m ].perfing_a + addr - kmaps[ m ].perfed_a));
    } else if (kmaps[ m ].perfed_a < addr) {
      a = m + 1ll;
    } else {
      b = m - 1ll;
    }
  }

  return 0llu;
}

void kmod_redo_kmaps(void) {
  if (no_new_a_maps >= 1u) {
    write(my_char_fd, "X\n", sizeof("X\n"));

    fprintf(stdout,
            "no_kmaps = %lu no_new_a_maps = %2u\n",
            no_kmaps,
            no_new_a_maps);
    no_new_a_maps = 0u;
  }
}
