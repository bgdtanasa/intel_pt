#include "x_elf.h"

#include <stdio.h>
#include <elf.h>

unsigned int binary_is_pie(const char* const path) {
  unsigned int is_pie = 1u;
  FILE* const  fp     = fopen(path, "r");

  if (fp != NULL) {
    Elf64_Ehdr hdr = { 0 };

    if (fread(&hdr, sizeof(hdr), 1, fp) == 1) {
      if (hdr.e_type == ET_EXEC) {
        is_pie = 0u;
      } else if (hdr.e_type == ET_DYN) {
        is_pie = 1u;
      }
    }
    fclose(fp);
  }

  return is_pie;
}
