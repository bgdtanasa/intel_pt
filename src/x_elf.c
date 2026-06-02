#include "x_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <string.h>
#include <errno.h>

unsigned int binary_is_pie(const char* const path) {
  unsigned int is_pie = 1u;
  FILE* const  fp     = fopen(path, "r");

  if (fp != NULL) {
    Elf64_Ehdr ehdr = { 0 };

    if (fread(&ehdr, sizeof(ehdr), 1, fp) == 1) {
      if (ehdr.e_type == ET_EXEC) {
        is_pie = 0u;
      } else if (ehdr.e_type == ET_DYN) {
        is_pie = 1u;
      }
    }
    fclose(fp);
  }

  return is_pie;
}

unsigned char* get_phdr_buffer(const char* const             path,
                               const unsigned long long int  addr,
                               unsigned long long int* const buffer_len) {
  FILE* const    fp     = fopen(path, "r");
  unsigned char* buffer = NULL;

  *buffer_len = 0llu;
  if (fp != NULL) {
    Elf64_Ehdr ehdr = { 0 };

    if (fread(&ehdr, sizeof(ehdr), 1, fp) == 1) {
      if ((ehdr.e_phoff != 0) && (ehdr.e_phnum != 0)) {
        for (unsigned int i = 0u; i < ((unsigned int) (ehdr.e_phnum)); i++) {
          Elf64_Phdr phdr  = { 0 };
          const long phoff = ((long) (ehdr.e_phoff + i * sizeof(phdr)));

          if (fseek(fp, phoff, SEEK_SET) == 0) {
            if (fread(&phdr, sizeof(phdr), 1, fp) == 1) {
              if ((phdr.p_type == PT_LOAD) && ((phdr.p_flags & PF_X) != 0) && (phdr.p_vaddr == addr)) {
                const size_t buffer_sz  = sizeof(unsigned char) * ((size_t) (phdr.p_filesz));
                const long   buffer_off = ((long) (phdr.p_offset));

                buffer = (unsigned char*) malloc(buffer_sz);
                if (buffer != NULL) {
                  if (fseek(fp, buffer_off, SEEK_SET) == 0) {
                    if (fread(buffer, buffer_sz, 1, fp) == 1) {
                      *buffer_len = ((unsigned long long int) (buffer_sz));
                    } else {
                      free(buffer); buffer = NULL;
                    }
                  } else {
                    free(buffer); buffer = NULL;
                  }
                }
                break;
              }
            }
          }
        }
      }
    }
    fclose(fp);
  } else {
    fprintf(stderr,
            "fopen(%s) failed :: %s\n",
            path,
            strerror(errno)); for (;;) {}
  }

  return buffer;
}
