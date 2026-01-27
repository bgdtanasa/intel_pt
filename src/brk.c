#include "brk.h"
//#include "x_unwind.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include <sys/wait.h>
#include <sys/ptrace.h>

#define MAX_NO_BRKS (4u)

typedef struct {
  unsigned long long addr;
  char               binary[ 256u ];
} brk_t;

brk_t        brks[ MAX_NO_BRKS ] = {
  [ 0u ]                         = {
    .addr                        = 0x000AD650llu, // malloc
    .binary                      = "libc.so.6"
  },
  [ 1u ]                         = {
    .addr                        = 0x000AD75Ellu, // malloc ret
    .binary                      = "libc.so.6"
  },
  [ 2u ]                         = {
    .addr                        = 0x000AD7DFllu, // malloc ret
    .binary                      = "libc.so.6"
  },
  [ 3u ]                         = {
    .addr                        = 0x000ADD30llu, // free
    .binary                      = "libc.so.6"
  }
};
unsigned int no_brks             = 0u;
int          brking_cpu;
unsigned     brking_done         = 0u;

static void* brking_main(void* args) {
  const int perfed_pid = *((int*) (args));

  int       ret;
  cpu_set_t cpu_set;
  int       status;

  CPU_ZERO(&cpu_set);
  CPU_SET(brking_cpu, &cpu_set);

  ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
  if (ret == 0) {
    if (ptrace(PTRACE_ATTACH, perfed_pid, NULL, NULL) != -1l) {
      const pid_t perfed_pid_wait = waitpid(perfed_pid, &status, 0);

      if (perfed_pid_wait == perfed_pid) {
        if (WIFSTOPPED(status)) {
          const unsigned long long int dr7 = (1llu <<  0llu) | (1llu <<  2llu) | (1llu <<  4llu) | (1llu <<  6llu) |
                                             (0llu << 16llu) | (0llu << 20llu) | (0llu << 24llu) | (0llu << 28llu) |
                                             (0llu << 18llu) | (0llu << 22llu) | (0llu << 26llu) | (0llu << 30llu);

          for (unsigned int i = 0u; i < no_brks; i++) {
            if (ptrace(PTRACE_POKEUSER, perfed_pid, offsetof(struct user, u_debugreg[ i ]), brks[ i ].addr) == -1l) {
              fprintf(stderr, "PTRACE_POKEUSER DR[ %2u ] failed %s\n", i, strerror(errno)); for (;;) {}
            } else {
              fprintf(stdout, "Breakpoint at %16llx in %s\n", brks[ i ].addr, brks[ i ].binary);
            }
          }
          if (ptrace(PTRACE_POKEUSER, perfed_pid, offsetof(struct user, u_debugreg[ 7u ]), dr7) == -1l) {
            fprintf(stderr, "PTRACE_POKEUSER DR[  7 ] failed %s\n", strerror(errno)); for (;;) {}
          }
          if (ptrace(PTRACE_CONT, perfed_pid, 0, NULL) == -1l) {
            fprintf(stderr, "PTRACE_CONT failed %s\n", strerror(errno)); for (;;) {}
          }
          __atomic_store_n(&brking_done, 1u, __ATOMIC_RELEASE);
          fprintf(stdout, "brking from cpu %2d on pid %6d\n", brking_cpu, perfed_pid);
        }
      }
    } else {
      fprintf(stderr, "PTRACE_ATTACH failed %s\n", strerror(errno)); for (;;) {}
    }

    for (;;) {
      const pid_t perfed_pid_wait = waitpid(perfed_pid, &status, 0);

      if (perfed_pid_wait == perfed_pid) {
        if (WIFSTOPPED(status)) {
#if 1
          const int sig = WSTOPSIG(status);

          if (sig == SIGTRAP) {
            siginfo_t si;

            if (ptrace(PTRACE_GETSIGINFO, perfed_pid, 0, &si) != -1l) {
              if (si.si_code == TRAP_HWBKPT) {
                for (unsigned int i = 0u; i < no_brks; i++) {
                  if (brks[ i ].addr == ((unsigned long long int) (si.si_addr))) {
                    fprintf(stdout, "Breakpoint hit at %16llx in %s\n", brks[ i ].addr, brks[ i ].binary);
                    break;
                  }
                }
              }
            } else {
              fprintf(stderr, "PTRACE_GETSIGINFO failed %s\n", strerror(errno)); for (;;) {}
            }
          }
#endif
          //if (ptrace(PTRACE_GETREGS, perfed_pid, NULL, &ptrace_uregs) != 1l) {
          //  unwind(perfed_pid, 0, 0.0f, &ptrace_uregs, &ptrace_unwind_insts);
          //}
          if (__atomic_load_n(&brking_done, __ATOMIC_ACQUIRE) == 2u) {
            break;
          } else {
            if (ptrace(PTRACE_CONT, perfed_pid, 0, NULL) == -1l) {
              fprintf(stderr, "PTRACE_CONT failed %s\n", strerror(errno)); for (;;) {}
            }
          }
        }
      } else {
        if (__atomic_load_n(&brking_done, __ATOMIC_ACQUIRE) == 2u) {
          break;
        }
      }
    }
  }

  if (ptrace(PTRACE_POKEUSER, perfed_pid, offsetof(struct user, u_debugreg[ 7u ]), 0llu) == -1l) {
    fprintf(stderr, "PTRACE_POKEUSER DR[  7 ] failed %s\n", strerror(errno)); for (;;) {}
  }
  if (ptrace(PTRACE_DETACH, perfed_pid, NULL, NULL) == -1l) {
    fprintf(stderr, "PTRACE_DETACH failed %s\n", strerror(errno)); for (;;) {}
  }
  __atomic_store_n(&brking_done, 3u, __ATOMIC_RELEASE);
  pthread_exit(NULL);
}

void install_brk(const inst_t* const inst) {
  if ((inst == NULL) || (no_brks >= MAX_NO_BRKS)) {
    return;
  }

  if (((inst->addr - inst->base_addr) == brks[ no_brks ].addr) && (strcmp(inst->binary, brks[ no_brks ].binary) == 0)) {
    brks[ no_brks ].addr = inst->addr;

    no_brks++;
  }
}

void perfed_brks(const int perfed_pid) {
  int       ret;
  pthread_t brking_thread;

  ret = pthread_create(&brking_thread, NULL, brking_main, ((void*) (&perfed_pid)));
  if (ret == 0) {
    while (__atomic_load_n(&brking_done, __ATOMIC_ACQUIRE) != 1u) {}
  } else {
    fprintf(stderr, "pthread_create failed %s\n", strerror(ret));
  }
}

void brk_close(void) {
  __atomic_store_n(&brking_done, 2u, __ATOMIC_RELEASE);
  //while (__atomic_load_n(&brking_done, __ATOMIC_ACQUIRE) != 3u) {}
}
