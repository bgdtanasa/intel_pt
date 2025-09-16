#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/kallsyms.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>

#include <linux/sched/signal.h>

#include <uapi/linux/mman.h>

#define MAX_NO_PAGES (200000u)

typedef struct page* (*my_follow_page_t)(struct vm_area_struct*, unsigned long, unsigned int);

typedef struct __attribute__ ((__packed__)) {
  unsigned long perfed_a;
  unsigned long perfed_b;
  unsigned long perfing_a;
  unsigned long perfing_b;
} kmap_t;

typedef struct {
  struct page*  page;
  unsigned long addr;
  unsigned long s;
  unsigned long e;
} my_page_t;

MODULE_DESCRIPTION("Pinning pages in memory");
MODULE_AUTHOR("Bogdan Tanasa");
MODULE_LICENSE("GPL");

static int           perfed_pid;
static unsigned long kmaps;
static unsigned long no_kmaps;
module_param(perfed_pid, int,   0644);
module_param(kmaps,      ulong, 0644);
module_param(no_kmaps,   ulong, 0644);

static struct task_struct* perfed_task;

static my_page_t    pages[ MAX_NO_PAGES ];
static unsigned int no_pages;

static int x_init(void) {
  my_follow_page_t my_follow_page = (my_follow_page_t) kallsyms_lookup_name("follow_page");
  if (my_follow_page == NULL) {
    return -1;
  }

  // perfed
  perfed_task = get_pid_task(find_get_pid(perfed_pid), PIDTYPE_PID);
  if (perfed_task != NULL) {
    struct mm_struct* perfed_mm = get_task_mm(perfed_task);
    unsigned long     s_brk     = perfed_mm->start_brk;
    unsigned long     e_brk     = perfed_mm->brk;

    printk(KERN_INFO "HEAP :: %016lx %16lx", s_brk, e_brk);
    no_pages = 0u;
    if (perfed_mm != NULL) {
      struct vm_area_struct* perfed_vma;

      mmap_read_lock(perfed_mm);
      VMA_ITERATOR(perfed_vmi, perfed_mm, 0);
      for_each_vma(perfed_vmi, perfed_vma) {
        unsigned long vmi_start = vma_iter_addr(&perfed_vmi);
        unsigned long vmi_end   = vma_iter_end(&perfed_vmi);

        if ((vmi_start <= s_brk) && (e_brk <= vmi_end)) {
          continue;
        }

        if (((perfed_vma->vm_flags & VM_READ)      != 0u) && // r
            ((perfed_vma->vm_flags & VM_WRITE)     != 0u) && // w
            ((perfed_vma->vm_flags & VM_EXEC)      == 0u) && // -
            ((perfed_vma->vm_flags & VM_SHARED)    == 0u) && // p
            ((perfed_vma->vm_flags & VM_MERGEABLE) == 0u) &&
            ((perfed_vma->vm_pgoff == 0lu) || (perfed_vma->vm_file == NULL))) {
          for (unsigned long x = vmi_start; x < vmi_end; x += 4096lu) {
            vm_fault_t ret = handle_mm_fault(perfed_vma, x, FAULT_FLAG_WRITE | FAULT_FLAG_REMOTE | FAULT_FLAG_VMA_LOCK, NULL);

            if (ret != 0) {
              printk(KERN_INFO "handle_mm_fault :: %016lx %u", x, ret);
            } else {
              struct page* perfed_page = NULL;
              long         ret         = pin_user_pages_remote(perfed_mm,
                                                               x,
                                                               1,
                                                               FOLL_WRITE | FOLL_LONGTERM,
                                                               &perfed_page,
                                                               NULL);

              if (ret == 1) {
                if (no_pages >= MAX_NO_PAGES - 1u) {
                  printk(KERN_INFO "Not enough space to load the page %016lx %016lx", x, vmi_end);
                  break;
                } else {
                  pages[ no_pages ] = (my_page_t) {
                    .page = perfed_page,
                    .addr = x,
                    .s    = vmi_start,
                    .e    = vmi_end
                  };
                  no_pages++;
                }
              } else {
                printk(KERN_INFO "pin_user_pages_remote :: %016lx %ld %d", x, ret, no_pages);
              }
            }
          }
          if (no_pages >= MAX_NO_PAGES - 1u) {
            break;
          }
        }
      }
      mmap_read_unlock(perfed_mm);
      mmput(perfed_mm);
    }
    put_task_struct(perfed_task);

    // perfing
    printk(KERN_INFO "no_pages = %u %u", no_pages, MAX_NO_PAGES);
    if (no_pages >= 1u) {
      unsigned long     perfing_addr_a = vm_mmap(NULL,
                                                 0lu,
                                                 ((unsigned long) (no_pages * 4096u)),
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED,
                                                 0lu);
      struct mm_struct* perfing_mm     = get_task_mm(current);

      printk(KERN_INFO "perfing_addr_a = %lx", perfing_addr_a);
      if (perfing_mm != NULL) {
        struct vm_area_struct* perfing_vma = NULL;

        mmap_write_lock(perfing_mm);
        perfing_vma = find_vma(perfing_mm, perfing_addr_a);
        if (perfing_vma != NULL) {
          unsigned long no_p = 0u;

          for (unsigned int i = 0u; i < no_pages; i++) {
            long ret = remap_pfn_range(perfing_vma,
                                       perfing_vma->vm_start + i * 4096u,
                                       page_to_pfn(pages[ i ].page),
                                       4096lu,
                                       perfing_vma->vm_page_prot);
            if (ret == 0l) {
              if (pages[ i ].addr == pages[ i ].s) {
                const kmap_t p = (kmap_t) {
                  .perfed_a  = pages[ i ].s,
                  .perfed_b  = pages[ i ].e,
                  .perfing_a = perfing_vma->vm_start + i * 4096u,
                  .perfing_b = 0lu
                };

                (void) copy_to_user((void*) (kmaps + no_p * sizeof(kmap_t)), &p,    sizeof(p));
                no_p++;
                (void) copy_to_user((void*) (no_kmaps),                      &no_p, sizeof(no_p));
              }
            }
          }
          {
            unsigned long perfing_addr_b = perfing_addr_a + no_pages * 4096u;

            printk(KERN_INFO "perfing_addr_a = %lx", perfing_addr_a);
            printk(KERN_INFO "perfing_addr_b = %lx", perfing_addr_b);
          }
        }
        mmap_write_unlock(perfing_mm);
        mmput(perfing_mm);
      }
    } else {
      return -1;
    }
  } else {
    return -1;
  }

  return 0;
}

static void x_exit(void) {
  for (unsigned int i = 0u; i < no_pages; i++) {
    unpin_user_pages(&pages[ i ].page, 1);
  }
}

module_init(x_init);
module_exit(x_exit);
