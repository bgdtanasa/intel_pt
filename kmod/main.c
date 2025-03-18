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

#define MAX_NO_PAGES (1024 * 1024u)

typedef struct page* (*my_follow_page_t)(struct vm_area_struct*, unsigned long, unsigned int);

MODULE_DESCRIPTION("Pinning pages in memory");
MODULE_AUTHOR("Bogdan Tanasa");
MODULE_LICENSE("GPL");

static int           perfed_pid;
static unsigned long perfee_vma_a;
static unsigned long perfee_vma_b;
static unsigned long perfed_vma_a;
static unsigned long perfed_vma_b;
module_param(perfed_pid,   int,   0644);
module_param(perfee_vma_a, ulong, 0644);
module_param(perfee_vma_b, ulong, 0644);
module_param(perfed_vma_a, ulong, 0644);
module_param(perfed_vma_b, ulong, 0644);

static struct task_struct* perfed_task;

static struct page*  pgs[ MAX_NO_PAGES ];
static unsigned int  no_pgs;
static unsigned long a;
static unsigned long b;

static int x_init(void) {
  my_follow_page_t my_follow_page = (my_follow_page_t) kallsyms_lookup_name("follow_page");
  if (my_follow_page == NULL) {
    return -1;
  }

  // perfed
  perfed_task = get_pid_task(find_get_pid(perfed_pid), PIDTYPE_PID);
  if (perfed_task != NULL) {
    struct mm_struct* perfed_mm   = get_task_mm(perfed_task);
    struct pt_regs*   perfed_regs = task_pt_regs(perfed_task);
    unsigned long     perfed_rsp  = perfed_regs->sp;

    if (perfed_mm != NULL) {
      struct vm_area_struct* perfed_vma;

      mmap_read_lock(perfed_mm);
      VMA_ITERATOR(perfed_vmi, perfed_mm, 0);
      for_each_vma(perfed_vmi, perfed_vma) {
        unsigned long vmi_start = vma_iter_addr(&perfed_vmi);
        unsigned long vmi_end   = vma_iter_end(&perfed_vmi);

        if ((vmi_start <= perfed_rsp) && (perfed_rsp <= vmi_end)) {
          a = vmi_start;
          b = vmi_end;

          for (unsigned long x = a; x < b; x += 4096lu) {
            struct page* perfed_page = my_follow_page(perfed_vma, x, FOLL_GET);

            if (perfed_page != NULL) {
            } else {
              vm_fault_t ret = handle_mm_fault(perfed_vma, x, FAULT_FLAG_WRITE, NULL);

              if (ret & VM_FAULT_COMPLETED) {
                
              } else if (ret & VM_FAULT_ERROR) {

              } else {

              }
            }
          }
        }
      }

      no_pgs = 0u;
      for (unsigned long x = a; x < b; x += 4096lu) {
        struct page* perfed_page = NULL;
        long         ret         = pin_user_pages_remote(perfed_mm,
                                                         x,
                                                         1,
                                                         FOLL_WRITE | FOLL_LONGTERM,
                                                         &perfed_page,
                                                         NULL);

        if (ret == 1) {
          pgs[ no_pgs++ ] = perfed_page;
        }
      }
      mmap_read_unlock(perfed_mm);
      mmput(perfed_mm);
    } else {
      no_pgs = 0u;
    }
    put_task_struct(perfed_task);

    // perfee
    if (no_pgs >= 1u) {
      unsigned long     perfee_addr_a = vm_mmap(NULL,
                                                0lu,
                                                ((unsigned long) (no_pgs * 4096u)),
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED,
                                                0lu);
      struct mm_struct* perfee_mm     = get_task_mm(current);

      if (perfee_mm != NULL) {
        struct vm_area_struct* perfee_vma = NULL;

        mmap_write_lock(perfee_mm);
        perfee_vma = find_vma(perfee_mm, perfee_addr_a);
        if (perfee_vma != NULL) {
          for (unsigned int i = 0u; i < no_pgs; i++) {
            (void) remap_pfn_range(perfee_vma,
                                   perfee_vma->vm_start + i * 4096u,
                                   page_to_pfn(pgs[ i ]),
                                   4096lu,
                                   perfee_vma->vm_page_prot);
#if 1
            vm_fault_t ret = handle_mm_fault(perfee_vma,
                                             perfee_vma->vm_start + i * 4096u,
                                             FAULT_FLAG_WRITE,
                                             NULL);
#endif
          }
          {
            unsigned long perfee_addr_b = perfee_addr_a + no_pgs * 4096u;

            (void) copy_to_user((void*) (perfee_vma_a), &perfee_addr_a, sizeof(perfee_addr_a));
            (void) copy_to_user((void*) (perfee_vma_b), &perfee_addr_b, sizeof(perfee_addr_b));
            (void) copy_to_user((void*) (perfed_vma_a), &a,             sizeof(a));
            (void) copy_to_user((void*) (perfed_vma_b), &b,             sizeof(b));
          }
        }
        mmap_write_unlock(perfee_mm);
        mmput(perfee_mm);
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
  if (no_pgs >= 1u) {
    unpin_user_pages(&pgs[ 0 ], no_pgs);
  }
}

module_init(x_init);
module_exit(x_exit);
