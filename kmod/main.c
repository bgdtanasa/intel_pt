#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/kallsyms.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/fs.h>
#include <linux/cdev.h>

#include <linux/sched/signal.h>

#include <uapi/linux/mman.h>

#define MAX_NO_PAGES (500000u)
#define MY_DEV_NAME  "my_char"

typedef struct __attribute__ ((__packed__)) {
  unsigned long perfed_a;
  unsigned long perfed_b;
  unsigned long perfing_a;
  unsigned long perfing_b;
} kmap_t;

typedef struct page* (*my_follow_page_t)(struct vm_area_struct*, unsigned long, unsigned int);

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

static int    perfing_pid;
unsigned long perfing_addr_a;

static int  do_mmap_probe_pre(struct kprobe* probe, struct pt_regs* regs);
static int  do_mprotect_pkey_probe_pre(struct kprobe* probe, struct pt_regs* regs);
static void undo_kmaps(void);
static int  do_kmaps(void);
static int     my_open(struct inode* inode, struct file* file);
static ssize_t my_write(struct file* file, const char __user* buf, size_t count, loff_t* offset);

static my_page_t              pages[ MAX_NO_PAGES ];
static unsigned int           no_pages      = 0u;
static struct kprobe          do_mmap_probe = {
  .symbol_name = "do_mmap",
  .pre_handler = do_mmap_probe_pre
};
static struct kprobe          do_mprotect_pkey_probe = {
  .symbol_name = "do_mprotect_pkey",
  .pre_handler = do_mprotect_pkey_probe_pre
};
static struct file_operations my_fops       = {
  .owner = THIS_MODULE,
  .open  = my_open,
  .write = my_write
};
static struct cdev            my_cdev;
static int                    my_cdev_major = 0;
static struct class*          my_cdev_class = NULL;

static int do_mmap_probe_pre(struct kprobe* probe, struct pt_regs* regs) {
  if (current->pid == perfed_pid) {
    struct file*  file     = ((struct file*) (regs->di));
    unsigned long addr     = regs->si;
    unsigned long len      = regs->dx;
    unsigned long prot     = regs->r10;
    unsigned long flags    = regs->r8;
    vm_flags_t    vm_flags = ((vm_flags_t) (regs->r9));

    kernel_siginfo_t    sig_info = {
      .si_code = SI_QUEUE
    };
    struct pid*         pid      = find_get_pid(perfing_pid);
    struct task_struct* task     = (pid != NULL) ? (get_pid_task(pid, PIDTYPE_PID)) : (NULL);
    unsigned int        is_so    = 0u;

    if (pid == NULL) {
      return 0;
    } else {
      put_pid(pid);
      if (task == NULL) {
        return 0;
      }
    }

    if (file != NULL) {
      is_so = (strstr(file->f_path.dentry->d_iname, ".so") != NULL) ? (1u) : (0u);
      if (is_so == 0u) {
        if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC)) {
          is_so = 1u;
        }
      }

      printk(KERN_INFO "0 FILE :: %lx %lx %s", prot, vm_flags, file->f_path.dentry->d_iname);
    } else {
      printk(KERN_INFO "1 FILE :: %lx %lx", prot, vm_flags);
    }

    if ((prot & PROT_EXEC) || (is_so == 1u)) {
      sig_info.si_signo = SIGRTMIN + 2 + 0;
    } else if ((prot & PROT_READ) && (prot & PROT_WRITE) && (flags & MAP_PRIVATE) && (file == NULL)) {
      sig_info.si_signo = SIGRTMIN + 2 + 1;
    } else {
      sig_info.si_signo = SIGRTMAX;
    }

    send_sig_info(sig_info.si_signo, &sig_info, task);
    put_task_struct(task);
  }
  return 0;
}

static int do_mprotect_pkey_probe_pre(struct kprobe* probe, struct pt_regs* regs) {
  if (current->pid == perfed_pid) {
    unsigned long start = regs->di;
    size_t        len   = regs->si;
    unsigned long prot  = regs->dx;

    kernel_siginfo_t    sig_info = {
      .si_code = SI_QUEUE
    };
    struct pid*         pid      = find_get_pid(perfing_pid);
    struct task_struct* task     = (pid != NULL) ? (get_pid_task(pid, PIDTYPE_PID)) : (NULL);

    if (pid == NULL) {
      return 0;
    } else {
      put_pid(pid);
      if (task == NULL) {
        return 0;
      }
    }

    printk(KERN_INFO "2 FILE :: %lx", prot);

    if (prot & PROT_EXEC) {
      sig_info.si_signo = SIGRTMIN + 2 + 0;
    } else {
      sig_info.si_signo = SIGRTMAX;
    }

    send_sig_info(sig_info.si_signo, &sig_info, task);
    put_task_struct(task);
  }
  return 0;
}

static void undo_kmaps(void) {
  if (no_pages >= 1u) {
    for (unsigned int i = 0u; i < no_pages; i++) {
      unpin_user_pages(&pages[ i ].page, 1);
    }
    vm_munmap(perfing_addr_a, ((unsigned long) (no_pages * 4096u)));
  }
  perfing_addr_a = 0lu;

  memset(&pages[ 0u ], 0, sizeof(pages));
  no_pages = 0u;
}

static int do_kmaps(void) {
  undo_kmaps();

  // perfed
  struct task_struct* perfed_task = get_pid_task(find_get_pid(perfed_pid), PIDTYPE_PID);
  if (perfed_task != NULL) {
    struct pt_regs*   perfed_regs = task_pt_regs(perfed_task);
    struct mm_struct* perfed_mm   = get_task_mm(perfed_task);
    unsigned long     s_brk       = perfed_mm->start_brk;
    unsigned long     e_brk       = perfed_mm->brk;

    printk(KERN_INFO "HEAP :: %016lx %16lx", s_brk, e_brk);
    printk(KERN_INFO " PGD :: %016lx", __pa(perfed_mm->pgd));
    no_pages = 0u;
    if (perfed_mm != NULL) {
      struct vm_area_struct* perfed_vma;

      mmap_read_lock(perfed_mm);
      VMA_ITERATOR(perfed_vmi, perfed_mm, 0);
      for_each_vma(perfed_vmi, perfed_vma) {
        unsigned long vmi_start = vma_iter_addr(&perfed_vmi);
        unsigned long vmi_end   = vma_iter_end(&perfed_vmi);
        unsigned int  in_stack  = ((vmi_start <= perfed_regs->sp) && (perfed_regs->sp < vmi_end)) ? (1u) : (0u);

        if ((s_brk <= perfed_regs->sp) && (perfed_regs->sp <= e_brk)) {
          // The particular case when the stack is allocated on the heap.
        } else {
          // The general case when the stack is allocated by the kernel.
          if ((vmi_start <= s_brk) && (e_brk <= vmi_end)) {
            // Ignore the heap mapping
            continue;
          }
        }

        printk(KERN_INFO "in_stack = %u %16lx %16lx %16lx", in_stack, vmi_start, perfed_regs->sp, vmi_end);
        if (((vmi_start & 0x0000700000000000lu) || (in_stack == 1u)) &&
            ((perfed_vma->vm_flags & VM_READ)      != 0u)            && // r
            ((perfed_vma->vm_flags & VM_WRITE)     != 0u)            && // w
            ((perfed_vma->vm_flags & VM_EXEC)      == 0u)            && // -
            ((perfed_vma->vm_flags & VM_SHARED)    == 0u)            && // p
            ((perfed_vma->vm_flags & VM_MERGEABLE) == 0u)            &&
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

              if (ret == 1l) {
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
      struct mm_struct* perfing_mm = get_task_mm(current);

      perfing_addr_a = vm_mmap(NULL,
                               0lu,
                               ((unsigned long) (no_pages * 4096u)),
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               0lu);
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

static int my_open(struct inode* inode, struct file* file) {
  return 0;
}

static ssize_t my_write(struct file* file, const char __user* buf, size_t count, loff_t* offset) {
  u64 a = ktime_get_ns();

  do_kmaps();
  printk(KERN_INFO "do_kmaps = %llu", ktime_get_ns() - a);
  return 0;
}

static int x_init(void) {
  my_follow_page_t my_follow_page = (my_follow_page_t) kallsyms_lookup_name("follow_page");
  if (my_follow_page == NULL) {
    return -1;
  }
  perfing_pid = current->pid;

  if ((register_kprobe(&do_mmap_probe) == 0) && (register_kprobe(&do_mprotect_pkey_probe) == 0)) {
    dev_t dev;

    if (alloc_chrdev_region(&dev, 0, 1u, MY_DEV_NAME) == 0) {
      my_cdev_major = MAJOR(dev);
      my_cdev_class = class_create(MY_DEV_NAME);

      cdev_init(&my_cdev, &my_fops);
      my_cdev.owner = THIS_MODULE;
      if (cdev_add(&my_cdev, MKDEV(my_cdev_major, 0), 1) == 0) {
        if (device_create(my_cdev_class, NULL, MKDEV(my_cdev_major, 0), NULL, MY_DEV_NAME"-%d", 0) != NULL) {
          return do_kmaps();
        }
      }
    }
    return -1;
  } else {
    return -1;
  }
}

static void x_exit(void) {
  undo_kmaps();

  device_destroy(my_cdev_class, MKDEV(my_cdev_major, 0));
  class_unregister(my_cdev_class);
  class_destroy(my_cdev_class);
  unregister_chrdev_region(MKDEV(my_cdev_major, 0), MINORMASK);

  unregister_kprobe(&do_mmap_probe);
  unregister_kprobe(&do_mprotect_pkey_probe);
}

module_init(x_init);
module_exit(x_exit);
