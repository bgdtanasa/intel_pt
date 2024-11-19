#include "proc.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

static char buffer[ 256u ];

static void parse_pagemap(const int                    perfed_pid,
                          const unsigned long long int vaddr_a,
                          const unsigned long long int vaddr_b) {
    int fd;

    (void) (vaddr_b);

    sprintf(&buffer[ 0u ], "/proc/%d/pagemap", perfed_pid);
    fd = open(&buffer[ 0u ], O_RDWR);
    if (fd != -1) {
        unsigned long long int pfn;

        if (pread(fd, &pfn, sizeof(pfn), (vaddr_a / 4096llu) * 8llu) == sizeof(pfn)) {
            fprintf(stdout,
                    "PFN[%016llx] = %016llx :: %llx %llx %llx %016llx :: %016llx\n",
                    vaddr_a,
                    pfn,
                    (pfn >> 63llu) & 0x01llu,
                    (pfn >> 62llu) & 0x01llu,
                    (pfn >> 61llu) & 0x01llu,
                    (pfn >>  0llu) & 0x007FFFFFFFFFFFFFllu,
                    ((pfn >>  0llu) & 0x007FFFFFFFFFFFFFllu) * 4096llu);
        }
    } else {
        fprintf(stderr,
                "open(%s) failed :: %s\n",
                &buffer[ 0u ],
                strerror(errno));
    }
}

void perfed_proc(const int perfed_pid) {
    FILE* fp;

    sprintf(&buffer[ 0u ], "/proc/%d/maps", perfed_pid);
    fp = fopen(&buffer[ 0u ], "r");
    if (fp != NULL) {
        fprintf(stdout, "====== PROC ======\n");
        for (;;) {
            char* line = fgets(&buffer[ 0u ],
                               ((int) (sizeof(buffer))),
                               fp);
            if (line != NULL) {
                unsigned long long int a;
                unsigned long long int b;

                char r;
                char w;
                char x;
                char p;

                sscanf(line,
                       "%016llx-%016llx %c%c%c%c",
                       &a,
                       &b,
                       &r,
                       &w,
                       &x,
                       &p);
                if (x == 'x') {
                    parse_pagemap(perfed_pid, a, b);
                }
            } else {
                break;
            }
        }
        fprintf(stdout, "====== PROC ======\n");

        fclose(fp);
    } else {
        fprintf(stderr,
                "fopen(%s) failed :: %s\n",
                &buffer[ 0u ],
                strerror(errno));
    }
}
