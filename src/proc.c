#include "proc.h"
#include "xed.h"
#include "x_elf.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/mman.h>
#include <sys/uio.h>

#include <linux/kernel-page-flags.h>

amaps_t      amaps;
unsigned int no_amaps;

static char buffer[ 512u ];

static void parse_pagemap(const int                    perfed_pid,
                          unsigned long long int       vaddr_a,
                          const unsigned long long int vaddr_b) {
    int fd;
    int fd_kpagecount;
    int fd_kpageflags;

    sprintf(&buffer[ 0u ], "/proc/%d/pagemap", perfed_pid);
    fd = open(&buffer[ 0u ], O_RDWR);
    fd_kpagecount = open("/proc/kpagecount", O_RDWR);
    fd_kpageflags = open("/proc/kpageflags", O_RDWR);
    if (fd != -1) {
        unsigned long long int pfn;
        unsigned long long int pfn_count = 0llu;
        unsigned long long int pfn_flags = 0llu;

pfn_again:
        if (pread(fd, &pfn, sizeof(pfn), (vaddr_a / 4096llu) * 8llu) == sizeof(pfn)) {
            (void) pread(fd_kpagecount, &pfn_count, sizeof(pfn_count), (pfn & 0x007FFFFFFFFFFFFFllu) * 8llu);
            (void) pread(fd_kpageflags, &pfn_flags, sizeof(pfn_flags), (pfn & 0x007FFFFFFFFFFFFFllu) * 8llu);

#if 0
            fprintf(stdout,
                    "PFN[%016llx] :: %12s %12s %12s %12s :: %016llx :: %2llu %016llx :: ",
                    vaddr_a,
                    ((pfn >> 63llu) & 0x01llu) ? ("present")     : (""),
                    ((pfn >> 62llu) & 0x01llu) ? ("swapped")     : (""),
                    ((pfn >> 61llu) & 0x01llu) ? ("file-page")   : ("shared-anon"),
                    ((pfn >> 56llu) & 0x01llu) ? ("exclusively") : (""),
                    ((pfn >>  0llu) & 0x007FFFFFFFFFFFFFllu) * 4096llu,
                    pfn_count,
                    pfn_flags);
            for (unsigned long long int i = 0llu; i <= 63llu; i++) {
                if (pfn_flags & (1llu << i)) {
                    fprintf(stdout, "%2llu ", i);
                }
            }
            fprintf(stdout, "\n");
#endif

            if (vaddr_a + 4096llu < vaddr_b) {
                vaddr_a += 4096llu;
                goto pfn_again;
            }
        } else {
            fprintf(stdout, "\n");
        }
        close(fd);
    } else {
        fprintf(stderr,
                "open(%s) failed :: %s\n",
                &buffer[ 0u ],
                strerror(errno));
    }
    close(fd_kpagecount);
    close(fd_kpageflags);
}

#if defined(EN_VMLINUX)
static void update_modules(void) {
    FILE* fp = fopen("resources/modules_addrs", "r");

    if (fp != NULL) {
        unsigned long long int a;
        unsigned long long int b;
        char                   name[ 128u ];

        fprintf(stdout, "====== KERNEL MODULES ======\n");
        for (;;) {
            const int n = fscanf(fp, "%s %llx %llx\n", &name[ 0u ], &a, &b);

            if (n == 3) {
                const char* binary = parse_get_binary(&name[ 0u ], 1u);

                fprintf(stdout, "%llx %llx %64s :: no_binaries = %9u\n", a, b, &name[ 0u ], no_binaries);
                for (unsigned long long int i = 0llu; i < no_insts; i++) {
                    if ((a <= insts[ i ].addr) && (insts[ i ].addr < b)) {
                        insts[ i ].binary    = binary;
                        insts[ i ].base_addr = a;
                    }
                }
            } else {
                break;
            }
        }
        fprintf(stdout, "====== KERNEL MODULES ======\n");
        fclose(fp);
    }
}
#endif

void perfed_proc(const int perfed_pid, struct user_regs_struct* regs) {
    FILE* fp;

    sprintf(&buffer[ 0u ], "/proc/%d/maps", perfed_pid);
    fp = fopen(&buffer[ 0u ], "r");
    if (fp != NULL) {
        fprintf(stdout, "====== PROC ======\n");
        for (;;) {
            char* line = fgets(&buffer[ 0u ], ((int) (sizeof(buffer))), fp);
            if (line != NULL) {
                unsigned long long int a;
                unsigned long long int b;

                char r;
                char w;
                char x;
                char p;

                unsigned int o;

                sscanf(line, "%016llx-%016llx %c%c%c%c %08x", &a, &b, &r, &w, &x, &p, &o);
                if (strstr(line, "[vsyscall]") != NULL) {
                    continue;
                }
                if ((x == 'x') || (strstr(line, "[vdso]") != NULL)) {
                    while ((line[ 0u ] != '/') && (line[ 0u ] != '[')) {
                        if (line[ 0u ] == '\n') {
                            break;
                        }
                        line++;
                    }
                    line[ strlen(line) - 1 ] = '\0';
                    {
                        const char* const binary = basename(line);

                        if ((binary == NULL) || (strlen(binary) == 0)) {
                            fprintf(stdout, "Anonymous executable mapping %16llx - %16llx %s\n", a, b, line);

                            amaps[ no_amaps ] = (amap_t) {
                                .a = a,
                                .b = b
                            };
                            no_amaps++;
                            if (no_amaps >= MAX_NO_AMAPS) {
                                fprintf(stderr, "Not enough space to load the anonymous mappings\n"); for (;;) {}
                            }
                        } else {
                            if (parse_get_binary(binary, 0u) == NULL) {
                                const unsigned int           is_pie    = binary_is_pie(line);
                                const unsigned long long int base_addr = (is_pie == 1u) ? (a - o) : (0llu);

                                fprintf(stdout, "%64s %08x %u :: ", binary, o, is_pie);
#if defined(EN_PTRACE_UNWIND)
                                parse_dwarf(binary, base_addr);
#endif
                                parse_objdump(perfed_pid, binary, base_addr);
                            }
                        }
                    }
                }
                if (regs != NULL) {
                    if ((a <= regs->rsp) && (regs->rsp <= b)) {
                        parse_pagemap(perfed_pid, a, b);
                    }
                }
            } else {
                break;
            }
        }
        fprintf(stdout, "====== PROC ======\n");
#if defined(EN_VMLINUX)
        fprintf(stdout, "====== KERNEL ======\n");
        fprintf(stdout, "%64s %08llx %u :: ", "vmlinux", 0llu, 0u);
#if defined(EN_PTRACE_UNWIND)
        parse_dwarf("vmlinux", 0llu);
#endif
        parse_objdump(perfed_pid, "vmlinux", 0llu);
        fprintf(stdout, "====== KERNEL ======\n");

        update_modules();
#endif

#if defined(EN_PTRACE_UNWIND)
        xed_unwind_link_inst_and_dwarf();
#endif
        fclose(fp);
    } else {
        fprintf(stderr,
                "fopen(%s) failed :: %s\n",
                &buffer[ 0u ],
                strerror(errno)); for (;;) {}
    }
}

unsigned long long int proc_read_perfed_vm(const int                    perfed_pid,
                                           const unsigned long long int addr) {
    if (addr == 0llu) {
        return  0llu;
    }

    unsigned long long int vm_entry = 0llu;
    struct iovec           local    = {
        .iov_base = &vm_entry,
        .iov_len  = sizeof(vm_entry)
    };
    struct iovec           remote   = {
        .iov_base = ((void*) (addr)),
        .iov_len  = sizeof(vm_entry)
    };
    ssize_t                n        = process_vm_readv(perfed_pid, &local, 1, &remote, 1, 0);

    if (n == -1) {
        fprintf(stderr,
                "process_vm_readv(%016llx) failed :: %s\n",
                addr,
                strerror(errno));
    } else {
        fprintf(stdout,
                "process_vm_readv(%016llx) = %16llx\n",
                addr,
                vm_entry);
    }

    return (n == -1) ? (0llu) : (vm_entry);
}
