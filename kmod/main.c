#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/cdev.h>
#include <linux/file.h>
#include <linux/pagemap.h>

#include <uapi/linux/mman.h>

MODULE_AUTHOR("Bogdan Tanasa");
MODULE_LICENSE("GPL");

#define MAX_NO_US_MAPPINGS (256u)
#define MAX_NO_KS_MAPPINGS (512u)
#define MAX_NAME_LEN       (128u)
#define MAX_NO_STK_PAGES   (128u)

#define MY_DEV_NAME "my_char"

typedef struct __attribute__ ((__packed__)) {
  char          name[ MAX_NAME_LEN ];
  unsigned long addr;
  unsigned long sz;
  unsigned long perfing_addr;

  struct vm_area_struct* vma;
  struct vm_area_struct* perfing_vma;
} mapping_t;

static char buffer[ PATH_MAX ];

static mapping_t    us_mappings[ MAX_NO_US_MAPPINGS ];
static unsigned int no_us_mappings;
static mapping_t    ks_mappings[ MAX_NO_KS_MAPPINGS ];
static unsigned int no_ks_mappings;
static mapping_t    stk_mapping;

static int           perfing_pid;
static int           perfed_pid;
static unsigned long _perfed_pgd;
static unsigned long _us_mappings;
static unsigned long _no_us_mappings;
static unsigned long _ks_mappings;
static unsigned long _no_ks_mappings;
static unsigned long _stk_mapping;

module_param(perfed_pid,      int,   0644);
module_param(_perfed_pgd,     ulong, 0644);
module_param(_us_mappings,    ulong, 0644);
module_param(_no_us_mappings, ulong, 0644);
module_param(_ks_mappings,    ulong, 0644);
module_param(_no_ks_mappings, ulong, 0644);
module_param(_stk_mapping,    ulong, 0644);

#if 0
static unsigned int is_elf_file(struct file* const file) {
  char elf_magic[ 4u ] = { 0x00, 0x00, 0x00, 0x00 };

  if (file != NULL) {
    loff_t  p = 0ll;
    ssize_t n = kernel_read(file, ((void*) (&elf_magic[ 0u ])), sizeof(elf_magic), &p);
  }

  return ((elf_magic[ 0u ] == 0x7F) && (elf_magic[ 1u ] == 'E') && (elf_magic[ 2u ] == 'L') && (elf_magic[ 3u ] == 'F')) ? (1u) : (0u);
}
#endif

static void my_send_sig(void) {
  struct pid* const p_pid = find_get_pid(perfing_pid);

  if (p_pid != NULL) {
    struct task_struct* const perfing_task = get_pid_task(p_pid, PIDTYPE_PID);

    if (perfing_task != NULL) {
      kernel_siginfo_t sig_info = {
        .si_signo = SIGRTMIN + 2,
        .si_code  = SI_QUEUE
      };

      send_sig_info(sig_info.si_signo, &sig_info, perfing_task);
      put_task_struct(perfing_task);
    }
    put_pid(p_pid);
  }
}

static void print_mappings(const mapping_t* const mappings, const unsigned int no_mappings) {
  for (unsigned int i = 0u; i < no_mappings; i++) {
    printk(KERN_INFO "mapping :: %3u addr = 0x%016lx 0x%016lx sz = 0x%08lx :: %s", i, mappings[ i ].addr, mappings[ i ].perfing_addr, mappings[ i ].sz, &mappings[ i ].name[ 0u ]);
  }
}

static void add_mapping(const char* const            _name,
                        const unsigned long          addr,
                        const unsigned long          sz,
                        struct vm_area_struct* const vma,
                        mapping_t* const             mappings,
                        unsigned int* const          no_mappings) {
  const char* name = _name;

  if ((addr == 0lu) || (sz == 0lu)) {
    return;
  }

  name = strrchr(_name, '/');
  name = (name != NULL) ? (name + 1) : (_name);

  if (*no_mappings == 0u) {
    strncpy(&mappings[ 0u ].name[ 0u ], name, MAX_NAME_LEN - 1u);
    mappings[ 0u ].addr         = addr;
    mappings[ 0u ].sz           = sz;
    mappings[ 0u ].perfing_addr = 0lu;
    mappings[ 0u ].vma          = vma;
    mappings[ 0u ].perfing_vma  = NULL;
  } else {
    signed int i = 0;
    signed int n = ((signed int) (*no_mappings));

    for (; i < n; i++) {
      if (addr < mappings[ i ].addr) {
        break;
      }
    }
    if (i < n) {
      for (signed int j = n - 1; j >= i; j--) {
        mappings[ j + 1 ] = mappings[ j ];
      }
    }
    strncpy(&mappings[ i ].name[ 0u ], name, MAX_NAME_LEN - 1u);
    mappings[ i ].addr         = addr;
    mappings[ i ].sz           = sz;
    mappings[ i ].perfing_addr = 0lu;
    mappings[ i ].vma          = vma;
    mappings[ i ].perfing_vma  = NULL;
  }

  *no_mappings = *no_mappings + 1u;
}

static void unmap_mappings(mapping_t* const mappings, const unsigned int no_mappings) {
  if ((mappings == NULL) || (no_mappings == 0u)) {
    return;
  }

  for (unsigned int i = 0u; i < no_mappings; i++) {
    if ((mappings[ i ].vma != NULL) && (mappings[ i ].perfing_vma != NULL)) {
      mmap_read_lock(mappings[ i ].perfing_vma->vm_mm);
      for (unsigned long j = 0lu; j < mappings[ i ].sz; j += 4096lu) {
        pte_t*       pte      = NULL;
        spinlock_t*  spinlock = NULL;

        if (follow_pte(mappings[ i ].perfing_vma, mappings[ i ].perfing_addr + j, &pte, &spinlock) == 0) {
          const pte_t this_pte = ptep_get(pte);

          pte_unmap_unlock(pte, spinlock);
          if (pte_present(this_pte)) {
            const unsigned long pfn = pte_pfn(this_pte);
            struct page* const  pg  = (pfn_valid(pfn) == 1) ? (pfn_to_page(pfn)) : (NULL);

            if (pg != NULL) {
              unpin_user_page(pg);
            }
          }
        }
      }
      mmap_read_unlock(mappings[ i ].perfing_vma->vm_mm);
    }
    if (mappings[ i ].sz != 0lu) {
      vm_munmap(mappings[ i ].perfing_addr, ((size_t) (mappings[ i ].sz)));
    }
  }
  memset(mappings, 0, no_mappings * sizeof(mapping_t));
}

static void map_mappings(mapping_t* const mappings, const unsigned int no_mappings, const unsigned int gup_flags) {
  struct pid* const         p_pid       = find_get_pid(perfed_pid);
  struct task_struct* const perfed_task = (p_pid != NULL) ? (get_pid_task(p_pid, PIDTYPE_PID)) : (NULL);
  struct mm_struct* const   perfed_mm   = (perfed_task != NULL) ? (get_task_mm(perfed_task)) : (NULL);
  struct mm_struct* const   perfing_mm  = get_task_mm(current);

  if (perfing_mm != NULL) {
    for (unsigned int i = 0u; i < no_mappings; i++) {
      mappings[ i ].perfing_addr = vm_mmap(NULL, 0lu, mappings[ i ].sz, PROT_READ, MAP_SHARED, 0lu);

      if (IS_ERR_VALUE(mappings[ i ].perfing_addr)) {
        memset(&mappings[ i ], 0, sizeof(mapping_t));

        continue;
      }

      mmap_write_lock(perfing_mm);
      mappings[ i ].perfing_vma = vma_lookup(perfing_mm, mappings[ i ].perfing_addr);
      if (mappings[ i ].perfing_vma != NULL) {
        for (unsigned long j = 0lu; j < mappings[ i ].sz; j += 4096lu) {
          const unsigned long pg_addr = mappings[ i ].addr + j;
          struct page*        pg      = NULL;

          if (mappings[ i ].vma == NULL) {
            pg = vmalloc_to_page(((void*) (pg_addr)));

            if (pg == NULL) {
              printk(KERN_INFO "0 pg = %16lx %16lx %s", ((unsigned long) (pg)), pg_addr, &mappings[ i ].name[ 0u ]);
            }
          } else {
            mmap_read_lock(perfed_mm);
            const unsigned long ret = ((unsigned long) (pin_user_pages_remote(perfed_mm, pg_addr, 1, gup_flags | FOLL_LONGTERM, &pg, NULL)));
            mmap_read_unlock(perfed_mm);

            if (ret != 1lu) {
              printk(KERN_INFO "1 pg = %16lx %16lx %s ret = %lx", ((unsigned long) (pg)), pg_addr, &mappings[ i ].name[ 0u ], ret);
            }
          }
          if (pg != NULL) {
            const long ret = remap_pfn_range(mappings[ i ].perfing_vma,
                                             mappings[ i ].perfing_addr + j,
                                             page_to_pfn(pg),
                                             4096lu,
                                             mappings[ i ].perfing_vma->vm_page_prot);

            if (ret == 0l) {
            } else {
              printk(KERN_INFO "remap_pfn_range :: %016lx %ld", mappings[ i ].addr + j, ret);
            }
          }
        }
      }
      mmap_write_unlock(perfing_mm);
    }
    mmput(perfing_mm);
  }
  if (perfed_mm != NULL) {
    mmput(perfed_mm);
  }
  if (perfed_task != NULL) {
    put_task_struct(perfed_task);
  }
  if (p_pid != NULL) {
    put_pid(p_pid);
  }
}

static void iterate_kprobe_insn_cache(struct kprobe_insn_cache* const insn_cache) {
  struct kprobe_insn_page {
    struct list_head          list;
    kprobe_opcode_t*          insns;
    struct kprobe_insn_cache* cache;
    int                       nused;
    int                       ngarbage;
    char                      slot_used[ ];
  };

  if (insn_cache != NULL) {
    struct kprobe_insn_page* insn_page = NULL;

    mutex_lock(&insn_cache->mutex);
    rcu_read_lock();
    list_for_each_entry_rcu(insn_page, &insn_cache->pages, list) {
      add_mapping(insn_cache->sym, ((unsigned long) (insn_page->insns)), 4096lu, NULL, &ks_mappings[ 0u ], &no_ks_mappings);
    }
    rcu_read_unlock();
    mutex_unlock(&insn_cache->mutex);
  }
}

static void do_us_mappings(void) {
  struct pid* const         p_pid       = find_get_pid(perfed_pid);
  struct task_struct* const perfed_task = (p_pid != NULL) ? (get_pid_task(p_pid, PIDTYPE_PID)) : (NULL);

  if (perfed_task != NULL) {
    struct mm_struct* const perfed_mm = get_task_mm(perfed_task);
    const unsigned long     pgd       = __pa(perfed_mm->pgd);

    (void) copy_to_user((void*) (_perfed_pgd), &pgd, sizeof(pgd));
    if (perfed_mm != NULL) {
      struct vm_area_struct* perfed_vma;

      mmap_read_lock(perfed_mm);
      VMA_ITERATOR(perfed_vmi, perfed_mm, 0);
      for_each_vma(perfed_vmi, perfed_vma) {
        struct file* const  file      = perfed_vma->vm_file;
        const unsigned long vmi_start = vma_iter_addr(&perfed_vmi);
        const unsigned long vmi_end   = vma_iter_end(&perfed_vmi);

        // Executable mappings
        if (perfed_vma->vm_flags & VM_EXEC) {
          if (file != NULL) {
            const char* const file_path = d_path(&file->f_path, &buffer[ 0u ], sizeof(buffer));

            if (IS_ERR(file_path)) {
              printk(KERN_INFO "do_us_mappings :: %s addr = 0x%lx sz = 0x%lx vm_flags = 0x%lx file = %s", perfed_task->comm, vmi_start, vmi_end - vmi_start, perfed_vma->vm_flags, "path_error");
            } else {
              add_mapping(file_path, vmi_start, vmi_end - vmi_start, perfed_vma, &us_mappings[ 0u ], &no_us_mappings);
            }
          } else {
            if (perfed_vma->vm_start == ((unsigned long) (perfed_mm->context.vdso))) {
              add_mapping("[vdso]", vmi_start, vmi_end - vmi_start, perfed_vma, &us_mappings[ 0u ], &no_us_mappings);
            } else {
              static unsigned int no_anons = 0u;
              static char         anon[ ]  = { 'a', 'n', 'o', 'n', '_', 'x', 'x', 'x', '\0', '\0' };

              sprintf(&anon[ 0u ], "anon_%03u", no_anons); no_anons++;
              add_mapping(&anon[ 0u ], vmi_start, vmi_end - vmi_start, perfed_vma, &us_mappings[ 0u ], &no_us_mappings);
            }
          }
        }
      }
      mmap_read_unlock(perfed_mm);
      mmput(perfed_mm);
    }
    put_task_struct(perfed_task);
  }
  if (p_pid != NULL) {
    put_pid(p_pid);
  }
}

static void do_ks_mappings(void) {
  unsigned long _text = kallsyms_lookup_name("_text");
  unsigned long _end  = kallsyms_lookup_name("_end");
  struct mutex*     module_mutex = (struct mutex*) kallsyms_lookup_name("module_mutex");
  struct list_head* modules      = (struct list_head*) kallsyms_lookup_name("modules");
  struct kprobe_insn_cache* kprobe_insn_slots    = (struct kprobe_insn_cache*) kallsyms_lookup_name("kprobe_insn_slots");
  struct kprobe_insn_cache* kprobe_optinsn_slots = (struct kprobe_insn_cache*) kallsyms_lookup_name("kprobe_optinsn_slots");

  struct module* module = NULL;

  // kernel code
  add_mapping("vmlinux", _text, _end - _text, NULL, &ks_mappings[ 0u ], &no_ks_mappings);
  // kprobe_insn_slots
  iterate_kprobe_insn_cache(kprobe_insn_slots);
  // kprobe_optinsn_slots
  iterate_kprobe_insn_cache(kprobe_optinsn_slots);
  // module code
  mutex_lock(module_mutex);
  list_for_each_entry(module, modules, list) {
    if ((module->state == MODULE_STATE_LIVE) || (module->state == MODULE_STATE_COMING)) {
      add_mapping(&module->name[ 0u ],
                  ((unsigned long) (module->mem[ MOD_TEXT ].base)),
                  ((unsigned long) (module->mem[ MOD_TEXT ].size)),
                  NULL,
                  &ks_mappings[ 0u ],
                  &no_ks_mappings);
    }
  }
  mutex_unlock(module_mutex);
}

static int mmap_pre(struct kprobe* probe, struct pt_regs* _regs) {
  struct pt_regs* regs  = ((struct pt_regs*) (_regs->di));
  unsigned long   addr  = regs->di;
  unsigned long   len   = regs->si;
  unsigned long   prot  = regs->dx;
  unsigned long   flags = regs->r10;
  unsigned long   fd    = regs->r8;
  unsigned long   pgoff = regs->r9;

  if (current->pid == perfed_pid) {
    if (prot & PROT_EXEC) {
    } else if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC)) {
    } else {
      return 0;
    }

    struct file* file = ((flags & MAP_ANONYMOUS) || (fd == -1)) ? (NULL) : (fget(fd));
    if (file != NULL) {
      char* file_path = d_path(&file->f_path, &buffer[ 0u ], sizeof(buffer));

      if (IS_ERR(file_path)) {
        printk(KERN_INFO "kprobe mmap :: %s addr = 0x%lx len = 0x%lx prot = 0x%lx flags = 0x%lx fd = %s pgoff = 0x%lx", current->comm, addr, len, prot, flags, "path_error", pgoff);
      } else {
        printk(KERN_INFO "kprobe mmap :: %s addr = 0x%lx len = 0x%lx prot = 0x%lx flags = 0x%lx fd = %s pgoff = 0x%lx", current->comm, addr, len, prot, flags, file_path, pgoff);

        my_send_sig();
      }
      fput(file);
    } else {
      printk(KERN_INFO "kprobe mmap :: %s addr = 0x%lx len = 0x%lx prot = 0x%lx flags = 0x%lx fd = %s pgoff = 0x%lx", current->comm, addr, len, prot, flags, "path_null", pgoff);

      my_send_sig();
    }
  }
  return 0;
}

static int munmap_pre(struct kprobe* probe, struct pt_regs* _regs) {
  struct pt_regs* regs = ((struct pt_regs*) (_regs->di));
  unsigned long   addr = regs->di;
  size_t          len  = ((size_t) (regs->si));

  if (current->pid == perfed_pid) {
    struct mm_struct*      perfed_mm  = get_task_mm(current);
    struct vm_area_struct* perfed_vma = NULL;

    mmap_write_lock(perfed_mm);
    perfed_vma = vma_lookup(perfed_mm, addr);
    mmap_write_unlock(perfed_mm);
    mmput(perfed_mm);

    if (perfed_vma != NULL) {
      struct file* file = perfed_vma->vm_file;

      if (file != NULL) {
        char* file_path = d_path(&file->f_path, &buffer[ 0u ], sizeof(buffer));

        if (IS_ERR(file_path)) {
          printk(KERN_INFO "kprobe munmap :: %s addr = 0x%lx len = 0x%lx vm_flags = 0x%lx file = %s", current->comm, addr, len, perfed_vma->vm_flags, "path_error");
        } else {
          if ((strstr(file_path, "lib") != NULL) || (strstr(file_path, ".so") != NULL)) {
            printk(KERN_INFO "kprobe munmap :: %s addr = 0x%lx len = 0x%lx vm_flags = 0x%lx file = %s", current->comm, addr, len, perfed_vma->vm_flags, file_path);

            my_send_sig();
          }
        }
      } else {
        if (perfed_vma->vm_flags & VM_EXEC) {
          printk(KERN_INFO "kprobe munmap :: %s addr = 0x%lx len = 0x%lx vm_flags = 0x%lx file = %s", current->comm, addr, len, perfed_vma->vm_flags, "path_null");

          my_send_sig();
        }
      }
    }
  }
  return 0;
}

static int mremap_pre(struct kprobe* probe, struct pt_regs* _regs) {
  struct pt_regs* regs     = ((struct pt_regs*) (_regs->di));
  unsigned long   addr     = regs->di;
  unsigned long   old_len  = regs->si;
  unsigned long   new_len  = regs->dx;
  unsigned long   flags    = regs->r10;
  unsigned long   new_addr = regs->r8;

  if (current->pid == perfed_pid) {
    struct mm_struct*      perfed_mm  = get_task_mm(current);
    struct vm_area_struct* perfed_vma = NULL;

    mmap_write_lock(perfed_mm);
    perfed_vma = vma_lookup(perfed_mm, addr);
    mmap_write_unlock(perfed_mm);
    mmput(perfed_mm);

    if ((perfed_vma != NULL) && (perfed_vma->vm_flags & (VM_EXEC | VM_MAYEXEC))) {
      printk(KERN_INFO "kprobe mremap :: %s addr = 0x%lx old_len = 0x%lx new_len = 0x%lx flags = 0x%lx new_addr = 0x%lx", current->comm, addr, old_len, new_len, flags, new_addr);

      my_send_sig();
    }
  }
  return 0;
}

static int mprotect_pre(struct kprobe* probe, struct pt_regs* _regs) {
  struct pt_regs* regs  = ((struct pt_regs*) (_regs->di));
  unsigned long   start = regs->di;
  size_t          len   = ((size_t) (regs->si));
  unsigned long   prot  = regs->dx;

  if (current->pid == perfed_pid) {
    if (prot & PROT_EXEC) {
    } else if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC)) {
    } else {
      return 0;
    }

    struct mm_struct*      perfed_mm  = get_task_mm(current);
    struct vm_area_struct* perfed_vma = NULL;

    mmap_write_lock(perfed_mm);
    perfed_vma = vma_lookup(perfed_mm, start);
    mmap_write_unlock(perfed_mm);
    mmput(perfed_mm);

    if (perfed_vma != NULL) {
      struct file* file = perfed_vma->vm_file;

      if (file != NULL) {
        char* file_path = d_path(&file->f_path, &buffer[ 0u ], sizeof(buffer));

        if (IS_ERR(file_path)) {
          printk(KERN_INFO "kprobe mprotect :: %s start = 0x%lx len = 0x%lx prot = 0x%lx file = %s", current->comm, start, len, prot, "path_error");
        } else {
          printk(KERN_INFO "kprobe mprotect :: %s start = 0x%lx len = 0x%lx prot = 0x%lx file = %s", current->comm, start, len, prot, file_path);

          my_send_sig();
        }
      } else {
        printk(KERN_INFO "kprobe mprotect :: %s start = 0x%lx len = 0x%lx prot = 0x%lx file = %s", current->comm, start, len, prot, "path_null");

        my_send_sig();
      }
    }
  }
  return 0;
}

static int madvise_pre(struct kprobe* probe, struct pt_regs* _regs) {
  struct pt_regs* regs     = ((struct pt_regs*) (_regs->di));
  unsigned long   start    = regs->di;
  size_t          len_in   = ((size_t) (regs->si));
  int             behavior = ((int) (regs->dx));

  if (current->pid == perfed_pid) {
    struct mm_struct*      perfed_mm  = get_task_mm(current);
    struct vm_area_struct* perfed_vma = NULL;

    mmap_write_lock(perfed_mm);
    perfed_vma = vma_lookup(perfed_mm, start);
    mmap_write_unlock(perfed_mm);
    mmput(perfed_mm);

    if ((perfed_vma != NULL) && (perfed_vma->vm_flags & VM_EXEC)) {
      printk(KERN_INFO "kprobe madvise :: %s start = 0x%lx len_in = 0x%lx behavior = %d", current->comm, start, len_in, behavior);

      my_send_sig();
    }
  }
  return 0;
}

static struct kprobe  mmap_probe       = {
  .symbol_name = "__x64_sys_mmap",
  .pre_handler = mmap_pre
};
static struct kprobe  mmap_pgoff_probe = {
  .symbol_name = "__x64_sys_mmap_pgoff",
  .pre_handler = mmap_pre
};
static struct kprobe  munmap_probe     = {
  .symbol_name = "__x64_sys_munmap",
  .pre_handler = munmap_pre
};
static struct kprobe  mremap_probe     = {
  .symbol_name = "__x64_sys_mremap",
  .pre_handler = mremap_pre
};
static struct kprobe  mprotect_probe   = {
  .symbol_name = "__x64_sys_mprotect",
  .pre_handler = mprotect_pre
};
static struct kprobe  madvise_probe    = {
  .symbol_name = "__x64_sys_madvise",
  .pre_handler = madvise_pre
};
static struct kprobe* my_kprobes[ ]    = {
  [ 0u ] = &mmap_probe,
  [ 1u ] = &mmap_pgoff_probe,
  [ 2u ] = &munmap_probe,
  [ 3u ] = &mremap_probe,
  [ 4u ] = &mprotect_probe,
  [ 5u ] = &madvise_probe,
};
static unsigned int   no_my_kprobes    = ((unsigned int) (sizeof(my_kprobes) / sizeof(my_kprobes[ 0u ])));

static int my_mmap(struct file* file, struct vm_area_struct* vma) {
  return 0;
}

static int my_open(struct inode* inode, struct file* file) {
  const u64                 a           = ktime_get_ns();
  struct pid* const         p_pid       = find_get_pid(perfed_pid);
  struct task_struct* const perfed_task = (p_pid != NULL) ? (get_pid_task(p_pid, PIDTYPE_PID)) : (NULL);

  if (perfed_task != NULL) {
    struct pt_regs* const   perfed_regs = task_pt_regs(perfed_task);
    struct mm_struct* const perfed_mm   = get_task_mm(perfed_task);

    // perfed
    if (perfed_mm != NULL) {
      struct vm_area_struct* perfed_vma;

      mmap_read_lock(perfed_mm);
      VMA_ITERATOR(perfed_vmi, perfed_mm, 0);
      for_each_vma(perfed_vmi, perfed_vma) {
        const unsigned long vmi_start = vma_iter_addr(&perfed_vmi);
        const unsigned long vmi_end   = vma_iter_end(&perfed_vmi);

        if ((vmi_start <= perfed_regs->sp) && (perfed_regs->sp < vmi_end)) {
          strncpy(&stk_mapping.name[ 0u ], "stack", MAX_NAME_LEN - 1u);
          stk_mapping.addr         = vmi_start;
          stk_mapping.sz           = vmi_end - vmi_start;
          stk_mapping.perfing_addr = 0lu;
          stk_mapping.vma          = perfed_vma;
          stk_mapping.perfing_vma  = NULL;
          break;
        }
      }
      mmap_read_unlock(perfed_mm);
      mmput(perfed_mm);
    }
    put_task_struct(perfed_task);

    // perfing
    map_mappings(&stk_mapping, 1u, FOLL_WRITE);

    (void) copy_to_user(((void*) (_stk_mapping)), &stk_mapping, sizeof(stk_mapping));
  }
  if (p_pid != NULL) {
    put_pid(p_pid);
  }

  printk(KERN_INFO "my_open = %llu stk_sz = %lu", ktime_get_ns() - a, stk_mapping.sz);
  return 0;
}

static int my_release(struct inode* inode, struct file* file) {
  unmap_mappings(&stk_mapping, 1u);

  return 0;
}

static struct file_operations my_fops       = {
  .owner   = THIS_MODULE,
  .mmap    = my_mmap,
  .open    = my_open,
  .release = my_release
};
static struct cdev            my_cdev;
static int                    my_cdev_major = 0;
static struct class*          my_cdev_class = NULL;

static int x_init(void) {
  dev_t dev;

  perfing_pid = current->pid;

  for (unsigned int i = 0u; i < no_my_kprobes; i++) {
    if (register_kprobe(my_kprobes[ i ]) != 0) {
      for (unsigned int j = 0u; j < i; j++) {
        unregister_kprobe(my_kprobes[ j ]);
      }
      return -1;
    }
  }

  if (alloc_chrdev_region(&dev, 0, 1u, MY_DEV_NAME) == 0) {
    my_cdev_major = MAJOR(dev);
    my_cdev_class = class_create(MY_DEV_NAME);

    cdev_init(&my_cdev, &my_fops);
    my_cdev.owner = THIS_MODULE;
    if (cdev_add(&my_cdev, MKDEV(my_cdev_major, 0), 1) == 0) {
      if (device_create(my_cdev_class, NULL, MKDEV(my_cdev_major, 0), NULL, MY_DEV_NAME"-%d", 0) != NULL) {
        do_us_mappings(); map_mappings(&us_mappings[ 0u ], no_us_mappings, 0u); print_mappings(&us_mappings[ 0u ], no_us_mappings);
        do_ks_mappings(); map_mappings(&ks_mappings[ 0u ], no_ks_mappings, 0u); print_mappings(&ks_mappings[ 0u ], no_ks_mappings);

        (void) copy_to_user(((void*) (_us_mappings)),    &us_mappings[ 0u ], sizeof(us_mappings));
        (void) copy_to_user(((void*) (_no_us_mappings)), &no_us_mappings,    sizeof(no_us_mappings));
        (void) copy_to_user(((void*) (_ks_mappings)),    &ks_mappings[ 0u ], sizeof(ks_mappings));
        (void) copy_to_user(((void*) (_no_ks_mappings)), &no_ks_mappings,    sizeof(no_ks_mappings));

        return 0;
      }
    }
  }
  return -1;
}

static void x_exit(void) {
  for (unsigned int i = 0u; i < no_my_kprobes; i++) {
    unregister_kprobe(my_kprobes[ i ]);
  }

  device_destroy(my_cdev_class, MKDEV(my_cdev_major, 0));
  cdev_del(&my_cdev);
  class_unregister(my_cdev_class);
  class_destroy(my_cdev_class);
  unregister_chrdev_region(MKDEV(my_cdev_major, 0), MINORMASK);

  unmap_mappings(&us_mappings[ 0u ], no_us_mappings);
  unmap_mappings(&ks_mappings[ 0u ], no_ks_mappings);
}

module_init(x_init);
module_exit(x_exit);
