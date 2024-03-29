/*
 *  linux/fs/exec.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */
/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 *
 * Demand loading changed July 1993 by Eric Youngdale.   Use mmap instead,
 * current->executable is only used by the procfs.  This allows a dispatch
 * table to check for several different types  of binary formats.  We keep
 * trying until we recognize the file or we run out of supported binary
 * formats. 
 */

#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/vmacache.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/swap.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/perf_event.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include <linux/key.h>
#include <linux/personality.h>
#include <linux/binfmts.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/tsacct_kern.h>
#include <linux/cn_proc.h>
#include <linux/audit.h>
#include <linux/tracehook.h>
#include <linux/kmod.h>
#include <linux/fsnotify.h>
#include <linux/fs_struct.h>
#include <linux/pipe_fs_i.h>
#include <linux/oom.h>
#include <linux/compat.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/coredump.h>
#include <linux/mman.h>

#ifdef CONFIG_PAX_REFCOUNT
#include <linux/kallsyms.h>
#include <linux/kdebug.h>
#endif

#include <trace/events/fs.h>

#include <asm/uaccess.h>
#include <asm/sections.h>
#include <asm/mmu_context.h>
#include <asm/tlb.h>

#include <trace/events/task.h>
#include "internal.h"

#include <trace/events/sched.h>

#ifdef CONFIG_PAX_HAVE_ACL_FLAGS
void __weak pax_set_initial_flags(struct linux_binprm *bprm)
{
	pr_warn_once("PAX: PAX_HAVE_ACL_FLAGS was enabled without providing the pax_set_initial_flags callback, this is probably not what you wanted.\n");
}
#endif

#ifdef CONFIG_PAX_HOOK_ACL_FLAGS
void (*pax_set_initial_flags_func)(struct linux_binprm *bprm);
EXPORT_SYMBOL(pax_set_initial_flags_func);
#endif

int suid_dumpable = 0;

static LIST_HEAD(formats);
static DEFINE_RWLOCK(binfmt_lock);

extern int gr_process_kernel_exec_ban(void);
extern int gr_process_sugid_exec_ban(const struct linux_binprm *bprm);

void __register_binfmt(struct linux_binfmt * fmt, int insert)
{
	BUG_ON(!fmt);
	if (WARN_ON(!fmt->load_binary))
		return;
	write_lock(&binfmt_lock);
	insert ? pax_list_add((struct list_head *)&fmt->lh, &formats) :
		 pax_list_add_tail((struct list_head *)&fmt->lh, &formats);
	write_unlock(&binfmt_lock);
}

EXPORT_SYMBOL(__register_binfmt);

void unregister_binfmt(struct linux_binfmt * fmt)
{
	write_lock(&binfmt_lock);
	pax_list_del((struct list_head *)&fmt->lh);
	write_unlock(&binfmt_lock);
}

EXPORT_SYMBOL(unregister_binfmt);

static inline void put_binfmt(struct linux_binfmt * fmt)
{
	module_put(fmt->module);
}

bool path_noexec(const struct path *path)
{
	return (path->mnt->mnt_flags & MNT_NOEXEC) ||
	       (path->mnt->mnt_sb->s_iflags & SB_I_NOEXEC);
}

#ifdef CONFIG_USELIB
/*
 * Note that a shared library must be both readable and executable due to
 * security reasons.
 *
 * Also note that we take the address to load from from the file itself.
 */
SYSCALL_DEFINE1(uselib, const char __user *, library)
{
	struct linux_binfmt *fmt;
	struct file *file;
	struct filename *tmp = getname(library);
	int error = PTR_ERR(tmp);
	static const struct open_flags uselib_flags = {
		.open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
		.acc_mode = MAY_READ | MAY_EXEC,
		.intent = LOOKUP_OPEN,
		.lookup_flags = LOOKUP_FOLLOW,
	};

	if (IS_ERR(tmp))
		goto out;

	file = do_filp_open(AT_FDCWD, tmp, &uselib_flags);
	putname(tmp);
	error = PTR_ERR(file);
	if (IS_ERR(file))
		goto out;

	error = -EINVAL;
	if (!S_ISREG(file_inode(file)->i_mode))
		goto exit;

	error = -EACCES;
	if (path_noexec(&file->f_path))
		goto exit;

	fsnotify_open(file);

	error = -ENOEXEC;

	read_lock(&binfmt_lock);
	list_for_each_entry(fmt, &formats, lh) {
		if (!fmt->load_shlib)
			continue;
		if (!try_module_get(fmt->module))
			continue;
		read_unlock(&binfmt_lock);
		error = fmt->load_shlib(file);
		read_lock(&binfmt_lock);
		put_binfmt(fmt);
		if (error != -ENOEXEC)
			break;
	}
	read_unlock(&binfmt_lock);
exit:
	fput(file);
out:
  	return error;
}
#endif /* #ifdef CONFIG_USELIB */

#ifdef CONFIG_MMU
/*
 * The nascent bprm->mm is not visible until exec_mmap() but it can
 * use a lot of memory, account these pages in current->mm temporary
 * for oom_badness()->get_mm_rss(). Once exec succeeds or fails, we
 * change the counter back via acct_arg_size(0).
 */
static void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
	struct mm_struct *mm = current->mm;
	long diff = (long)(pages - bprm->vma_pages);

	if (!mm || !diff)
		return;

	bprm->vma_pages = pages;
	add_mm_counter(mm, MM_ANONPAGES, diff);
}

static struct page *get_arg_page(struct linux_binprm *bprm, unsigned long pos,
		int write)
{
	struct page *page;

	if (0 > expand_downwards(bprm->vma, pos))
		return NULL;
	/*
	 * We are doing an exec().  'current' is the process
	 * doing the exec and bprm->mm is the new process's mm.
	 */
	if (0 >= get_user_pages_remote(current, bprm->mm, pos, 1, write,
			1, &page, NULL))
		return NULL;

	if (write) {
		unsigned long size = bprm->vma->vm_end - bprm->vma->vm_start;
		struct rlimit *rlim;

		acct_arg_size(bprm, size / PAGE_SIZE);

		/*
		 * We've historically supported up to 32 pages (ARG_MAX)
		 * of argument strings even with small stacks
		 */
		if (size <= ARG_MAX)
			return page;

#ifdef CONFIG_GRKERNSEC_PROC_MEMMAP
		// only allow 512KB for argv+env on suid/sgid binaries
		// to prevent easy ASLR exhaustion
		if (((!uid_eq(bprm->cred->euid, current_euid())) ||
		     (!gid_eq(bprm->cred->egid, current_egid()))) &&
		    (size > (512 * 1024))) {
			put_page(page);
			return NULL;
		}
#endif

		/*
		 * Limit to 1/4-th the stack size for the argv+env strings.
		 * This ensures that:
		 *  - the remaining binfmt code will not run out of stack space,
		 *  - the program will have a reasonable amount of stack left
		 *    to work from.
		 */
		rlim = current->signal->rlim;
		if (size > ACCESS_ONCE(rlim[RLIMIT_STACK].rlim_cur) / 4) {
			put_page(page);
			return NULL;
		}
	}

	return page;
}

static void put_arg_page(struct page *page)
{
	put_page(page);
}

static void free_arg_pages(struct linux_binprm *bprm)
{
}

static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos,
		struct page *page)
{
	flush_cache_page(bprm->vma, pos, page_to_pfn(page));
}

static int __bprm_mm_init(struct linux_binprm *bprm)
{
	int err;
	struct vm_area_struct *vma = NULL;
	struct mm_struct *mm = bprm->mm;

	bprm->vma = vma = kmem_cache_zalloc(vm_area_cachep, GFP_KERNEL);
	if (!vma)
		return -ENOMEM;

	if (down_write_killable(&mm->mmap_sem)) {
		err = -EINTR;
		goto err_free;
	}
	vma->vm_mm = mm;

	/*
	 * Place the stack at the largest stack address the architecture
	 * supports. Later, we'll move this to an appropriate place. We don't
	 * use STACK_TOP because that can depend on attributes which aren't
	 * configured yet.
	 */
	BUILD_BUG_ON(VM_STACK_FLAGS & VM_STACK_INCOMPLETE_SETUP);
	vma->vm_end = STACK_TOP_MAX;
	vma->vm_start = vma->vm_end - PAGE_SIZE;
	vma->vm_flags = VM_SOFTDIRTY | VM_STACK_FLAGS | VM_STACK_INCOMPLETE_SETUP;

#ifdef CONFIG_PAX_SEGMEXEC
	vma->vm_flags &= ~(VM_EXEC | VM_MAYEXEC);
#endif

	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	INIT_LIST_HEAD(&vma->anon_vma_chain);

	err = insert_vm_struct(mm, vma);
	if (err)
		goto err;

	mm->stack_vm = mm->total_vm = 1;
	arch_bprm_mm_init(mm, vma);
	up_write(&mm->mmap_sem);
	bprm->p = vma->vm_end - sizeof(void *);

#ifdef CONFIG_PAX_RANDUSTACK
	if (randomize_va_space)
		bprm->p ^= prandom_u32() & ~PAGE_MASK;
#endif

	return 0;
err:
	up_write(&mm->mmap_sem);
err_free:
	bprm->vma = NULL;
	kmem_cache_free(vm_area_cachep, vma);
	return err;
}

static bool valid_arg_len(struct linux_binprm *bprm, long len)
{
	return len <= MAX_ARG_STRLEN;
}

#else

static inline void acct_arg_size(struct linux_binprm *bprm, unsigned long pages)
{
}

static struct page *get_arg_page(struct linux_binprm *bprm, unsigned long pos,
		int write)
{
	struct page *page;

	page = bprm->page[pos / PAGE_SIZE];
	if (!page && write) {
		page = alloc_page(GFP_HIGHUSER|__GFP_ZERO);
		if (!page)
			return NULL;
		bprm->page[pos / PAGE_SIZE] = page;
	}

	return page;
}

static void put_arg_page(struct page *page)
{
}

static void free_arg_page(struct linux_binprm *bprm, int i)
{
	if (bprm->page[i]) {
		__free_page(bprm->page[i]);
		bprm->page[i] = NULL;
	}
}

static void free_arg_pages(struct linux_binprm *bprm)
{
	int i;

	for (i = 0; i < MAX_ARG_PAGES; i++)
		free_arg_page(bprm, i);
}

static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos,
		struct page *page)
{
}

static int __bprm_mm_init(struct linux_binprm *bprm)
{
	bprm->p = PAGE_SIZE * MAX_ARG_PAGES - sizeof(void *);
	return 0;
}

static bool valid_arg_len(struct linux_binprm *bprm, long len)
{
	return len <= bprm->p;
}

#endif /* CONFIG_MMU */

/*
 * Create a new mm_struct and populate it with a temporary stack
 * vm_area_struct.  We don't have enough context at this point to set the stack
 * flags, permissions, and offset, so we use temporary values.  We'll update
 * them later in setup_arg_pages().
 */
static int bprm_mm_init(struct linux_binprm *bprm)
{
	int err;
	struct mm_struct *mm = NULL;

	bprm->mm = mm = mm_alloc();
	err = -ENOMEM;
	if (!mm)
		goto err;

	err = __bprm_mm_init(bprm);
	if (err)
		goto err;

	return 0;

err:
	if (mm) {
		bprm->mm = NULL;
		mmdrop(mm);
	}

	return err;
}

struct user_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;
#endif
	union {
		const char __user *const __user *native;
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;
#endif
	} ptr;
};

const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

#ifdef CONFIG_COMPAT
	if (unlikely(argv.is_compat)) {
		compat_uptr_t compat;

		if (get_user(compat, argv.ptr.compat + nr))
			return (const char __force_user *)ERR_PTR(-EFAULT);

		return compat_ptr(compat);
	}
#endif

	if (get_user(native, argv.ptr.native + nr))
		return (const char __force_user *)ERR_PTR(-EFAULT);

	return native;
}

/*
 * count() counts the number of strings in array ARGV.
 */
static int count(struct user_arg_ptr argv, int max)
{
	int i = 0;

	if (argv.ptr.native != NULL) {
		for (;;) {
			const char __user *p = get_user_arg_ptr(argv, i);

			if (!p)
				break;

			if (IS_ERR((const char __force_kernel *)p))
				return -EFAULT;

			if (i >= max)
				return -E2BIG;
			++i;

			if (fatal_signal_pending(current))
				return -ERESTARTNOHAND;
			cond_resched();
		}
	}
	return i;
}

/*
 * 'copy_strings()' copies argument/environment strings from the old
 * processes's memory to the new process's stack.  The call to get_user_pages()
 * ensures the destination page is created and not swapped out.
 */
static int copy_strings(int argc, struct user_arg_ptr argv,
			struct linux_binprm *bprm)
{
	struct page *kmapped_page = NULL;
	char *kaddr = NULL;
	unsigned long kpos = 0;
	int ret;

	while (argc-- > 0) {
		const char __user *str;
		int len;
		unsigned long pos;

		ret = -EFAULT;
		str = get_user_arg_ptr(argv, argc);
		if (IS_ERR((const char __force_kernel *)str))
			goto out;

		len = strnlen_user(str, MAX_ARG_STRLEN);
		if (!len)
			goto out;

		ret = -E2BIG;
		if (!valid_arg_len(bprm, len))
			goto out;

		/* We're going to work our way backwords. */
		pos = bprm->p;
		str += len;
		bprm->p -= len;

		while (len > 0) {
			int offset, bytes_to_copy;

			if (fatal_signal_pending(current)) {
				ret = -ERESTARTNOHAND;
				goto out;
			}
			cond_resched();

			offset = pos % PAGE_SIZE;
			if (offset == 0)
				offset = PAGE_SIZE;

			bytes_to_copy = offset;
			if (bytes_to_copy > len)
				bytes_to_copy = len;

			offset -= bytes_to_copy;
			pos -= bytes_to_copy;
			str -= bytes_to_copy;
			len -= bytes_to_copy;

			if (!kmapped_page || kpos != (pos & PAGE_MASK)) {
				struct page *page;

				page = get_arg_page(bprm, pos, 1);
				if (!page) {
					ret = -E2BIG;
					goto out;
				}

				if (kmapped_page) {
					flush_kernel_dcache_page(kmapped_page);
					kunmap(kmapped_page);
					put_arg_page(kmapped_page);
				}
				kmapped_page = page;
				kaddr = kmap(kmapped_page);
				kpos = pos & PAGE_MASK;
				flush_arg_page(bprm, kpos, kmapped_page);
			}
			if (copy_from_user(kaddr+offset, str, bytes_to_copy)) {
				ret = -EFAULT;
				goto out;
			}
		}
	}
	ret = 0;
out:
	if (kmapped_page) {
		flush_kernel_dcache_page(kmapped_page);
		kunmap(kmapped_page);
		put_arg_page(kmapped_page);
	}
	return ret;
}

/*
 * Like copy_strings, but get argv and its values from kernel memory.
 */
int copy_strings_kernel(int argc, const char *const *__argv,
			struct linux_binprm *bprm)
{
	int r;
	mm_segment_t oldfs = get_fs();
	struct user_arg_ptr argv = {
		.ptr.native = (const char __user * const __force_user *)__argv,
	};

	set_fs(KERNEL_DS);
	r = copy_strings(argc, argv, bprm);
	set_fs(oldfs);

	return r;
}
EXPORT_SYMBOL(copy_strings_kernel);

#ifdef CONFIG_MMU

/*
 * During bprm_mm_init(), we create a temporary stack at STACK_TOP_MAX.  Once
 * the binfmt code determines where the new stack should reside, we shift it to
 * its final location.  The process proceeds as follows:
 *
 * 1) Use shift to calculate the new vma endpoints.
 * 2) Extend vma to cover both the old and new ranges.  This ensures the
 *    arguments passed to subsequent functions are consistent.
 * 3) Move vma's page tables to the new range.
 * 4) Free up any cleared pgd range.
 * 5) Shrink the vma to cover only the new range.
 */
static int shift_arg_pages(struct vm_area_struct *vma, unsigned long shift)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long old_start = vma->vm_start;
	unsigned long old_end = vma->vm_end;
	unsigned long length = old_end - old_start;
	unsigned long new_start = old_start - shift;
	unsigned long new_end = old_end - shift;
	struct mmu_gather tlb;

	if (new_start >= new_end || new_start < mmap_min_addr)
		return -ENOMEM;

	/*
	 * ensure there are no vmas between where we want to go
	 * and where we are
	 */
	if (vma != find_vma(mm, new_start))
		return -EFAULT;

#ifdef CONFIG_PAX_SEGMEXEC
	BUG_ON(pax_find_mirror_vma(vma));
#endif

	/*
	 * cover the whole range: [new_start, old_end)
	 */
	if (vma_adjust(vma, new_start, old_end, vma->vm_pgoff, NULL))
		return -ENOMEM;

	/*
	 * move the page tables downwards, on failure we rely on
	 * process cleanup to remove whatever mess we made.
	 */
	if (length != move_page_tables(vma, old_start,
				       vma, new_start, length, false))
		return -ENOMEM;

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm, old_start, old_end);
	if (new_end > old_start) {
		/*
		 * when the old and new regions overlap clear from new_end.
		 */
		free_pgd_range(&tlb, new_end, old_end, new_end,
			vma->vm_next ? vma->vm_next->vm_start : USER_PGTABLES_CEILING);
	} else {
		/*
		 * otherwise, clean from old_start; this is done to not touch
		 * the address space in [new_end, old_start) some architectures
		 * have constraints on va-space that make this illegal (IA64) -
		 * for the others its just a little faster.
		 */
		free_pgd_range(&tlb, old_start, old_end, new_end,
			vma->vm_next ? vma->vm_next->vm_start : USER_PGTABLES_CEILING);
	}
	tlb_finish_mmu(&tlb, old_start, old_end);

	/*
	 * Shrink the vma to just the new range.  Always succeeds.
	 */
	vma_adjust(vma, new_start, new_end, vma->vm_pgoff, NULL);

	return 0;
}

/*
 * Finalizes the stack vm_area_struct. The flags and permissions are updated,
 * the stack is optionally relocated, and some extra space is added.
 */
int setup_arg_pages(struct linux_binprm *bprm,
		    unsigned long stack_top,
		    int executable_stack)
{
	unsigned long ret;
	unsigned long stack_shift;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = bprm->vma;
	struct vm_area_struct *prev = NULL;
	unsigned long vm_flags;
	unsigned long stack_base;
	unsigned long stack_size;
	unsigned long stack_expand;
	unsigned long rlim_stack;

#ifdef CONFIG_STACK_GROWSUP
	/* Limit stack size */
	stack_base = rlimit_max(RLIMIT_STACK);
	if (stack_base > STACK_SIZE_MAX)
		stack_base = STACK_SIZE_MAX;

	/* Add space for stack randomization. */
	stack_base += (STACK_RND_MASK << PAGE_SHIFT);

	/* Make sure we didn't let the argument array grow too large. */
	if (vma->vm_end - vma->vm_start > stack_base)
		return -ENOMEM;

	stack_base = PAGE_ALIGN(stack_top - stack_base);

	stack_shift = vma->vm_start - stack_base;
	mm->arg_end = mm->arg_start = bprm->p - stack_shift;
	bprm->p = vma->vm_end - stack_shift;
#else
	stack_top = arch_align_stack(stack_top);
	stack_top = PAGE_ALIGN(stack_top);

	stack_shift = vma->vm_end - stack_top;

	bprm->p -= stack_shift;
	mm->arg_end = mm->arg_start = bprm->p;
#endif

	if (bprm->loader)
		bprm->loader -= stack_shift;
	bprm->exec -= stack_shift;

	if (down_write_killable(&mm->mmap_sem))
		return -EINTR;

	/* Move stack pages down in memory. */
	if (stack_shift) {
		ret = shift_arg_pages(vma, stack_shift);
		if (ret)
			goto out_unlock;
	}

	vm_flags = VM_STACK_FLAGS;

#if defined(CONFIG_PAX_PAGEEXEC) || defined(CONFIG_PAX_SEGMEXEC)
	if (mm->pax_flags & (MF_PAX_PAGEEXEC | MF_PAX_SEGMEXEC)) {
		vm_flags &= ~VM_EXEC;

#ifdef CONFIG_PAX_MPROTECT
		if (mm->pax_flags & MF_PAX_MPROTECT)
			vm_flags &= ~VM_MAYEXEC;
#endif

	}
#endif

	/*
	 * Adjust stack execute permissions; explicitly enable for
	 * EXSTACK_ENABLE_X, disable for EXSTACK_DISABLE_X and leave alone
	 * (arch default) otherwise.
	 */
	if (unlikely(executable_stack == EXSTACK_ENABLE_X))
		vm_flags |= VM_EXEC;
	else if (executable_stack == EXSTACK_DISABLE_X)
		vm_flags &= ~VM_EXEC;
	vm_flags |= mm->def_flags;
	vm_flags |= VM_STACK_INCOMPLETE_SETUP;

	ret = mprotect_fixup(vma, &prev, vma->vm_start, vma->vm_end,
			vm_flags);
	if (ret)
		goto out_unlock;
	BUG_ON(prev != vma);

	/* mprotect_fixup is overkill to remove the temporary stack flags */
	vma->vm_flags &= ~VM_STACK_INCOMPLETE_SETUP;

	stack_expand = 131072UL; /* randomly 32*4k (or 2*64k) pages */
	stack_size = vma->vm_end - vma->vm_start;
	/*
	 * Align this down to a page boundary as expand_stack
	 * will align it up.
	 */
	rlim_stack = rlimit(RLIMIT_STACK) & PAGE_MASK;
#ifdef CONFIG_STACK_GROWSUP
	if (stack_size + stack_expand > rlim_stack)
		stack_base = vma->vm_start + rlim_stack;
	else
		stack_base = vma->vm_end + stack_expand;
#else
	if (stack_size + stack_expand > rlim_stack)
		stack_base = vma->vm_end - rlim_stack;
	else
		stack_base = vma->vm_start - stack_expand;
#endif
	current->mm->start_stack = bprm->p;
	ret = expand_stack(vma, stack_base);

#if !defined(CONFIG_STACK_GROWSUP) && defined(CONFIG_PAX_RANDMMAP)
	if (!ret && (mm->pax_flags & MF_PAX_RANDMMAP) && STACK_TOP <= 0xFFFFFFFFU && STACK_TOP > vma->vm_end) {
		unsigned long size;
		vm_flags_t vm_flags;

		size = STACK_TOP - vma->vm_end;
		vm_flags = VM_NONE | VM_DONTEXPAND | VM_DONTDUMP;

		ret = vma->vm_end != mmap_region(NULL, vma->vm_end, size, vm_flags, 0);

#ifdef CONFIG_X86
		if (!ret) {
			size = PAGE_SIZE + mmap_min_addr + ((mm->delta_mmap ^ mm->delta_stack) & (0xFFUL << PAGE_SHIFT));
			ret = 0 != mmap_region(NULL, 0, PAGE_ALIGN(size), vm_flags, 0);
		}
#endif

	}
#endif

	if (ret)
		ret = -EFAULT;

out_unlock:
	up_write(&mm->mmap_sem);
	return ret;
}
EXPORT_SYMBOL(setup_arg_pages);

#endif /* CONFIG_MMU */

static struct file *do_open_execat(int fd, struct filename *name, int flags)
{
	struct file *file;
	int err;
	int unsafe_flags = 0;
	struct open_flags open_exec_flags = {
		.open_flag = O_LARGEFILE | O_RDONLY | __FMODE_EXEC,
		.acc_mode = MAY_EXEC,
		.intent = LOOKUP_OPEN,
		.lookup_flags = LOOKUP_FOLLOW,
	};

	if ((flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0)
		return ERR_PTR(-EINVAL);
	if (flags & AT_SYMLINK_NOFOLLOW)
		open_exec_flags.lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & AT_EMPTY_PATH)
		open_exec_flags.lookup_flags |= LOOKUP_EMPTY;

	file = do_filp_open(fd, name, &open_exec_flags);
	if (IS_ERR(file))
		goto out;

	err = -EACCES;
	if (!S_ISREG(file_inode(file)->i_mode))
		goto exit;

	if (path_noexec(&file->f_path))
		goto exit;

	if (current->ptrace && !(current->ptrace & PT_PTRACE_CAP))
		unsafe_flags = LSM_UNSAFE_PTRACE;

	if (gr_ptrace_readexec(file, unsafe_flags)) {
		err = -EPERM;
		goto exit;
	}

	err = deny_write_access(file);
	if (err)
		goto exit;

	if (name->name[0] != '\0') {
		fsnotify_open(file);
		trace_open_exec(name->name);
	}

out:
	return file;

exit:
	fput(file);
	return ERR_PTR(err);
}

struct file *open_exec(const char *name)
{
	struct filename *filename = getname_kernel(name);
	struct file *f = ERR_CAST(filename);

	if (!IS_ERR(filename)) {
		f = do_open_execat(AT_FDCWD, filename, 0);
		putname(filename);
	}
	return f;
}
EXPORT_SYMBOL(open_exec);

int kernel_read(struct file *file, loff_t offset,
		char *addr, unsigned long count)
{
	mm_segment_t old_fs;
	loff_t pos = offset;
	int result;

	if (count > INT_MAX)
		return -EINVAL;

	old_fs = get_fs();
	set_fs(get_ds());
	/* The cast to a user pointer is valid due to the set_fs() */
	result = vfs_read(file, (void __force_user *)addr, count, &pos);
	set_fs(old_fs);
	return result;
}

EXPORT_SYMBOL(kernel_read);

int kernel_read_file(struct file *file, void **buf, loff_t *size,
		     loff_t max_size, enum kernel_read_file_id id)
{
	loff_t i_size, pos;
	ssize_t bytes = 0;
	int ret;

	if (!S_ISREG(file_inode(file)->i_mode) || max_size < 0)
		return -EINVAL;

	ret = security_kernel_read_file(file, id);
	if (ret)
		return ret;

	ret = deny_write_access(file);
	if (ret)
		return ret;

	i_size = i_size_read(file_inode(file));
	if (max_size > 0 && i_size > max_size) {
		ret = -EFBIG;
		goto out;
	}
	if (i_size <= 0) {
		ret = -EINVAL;
		goto out;
	}

	*buf = vmalloc(i_size);
	if (!*buf) {
		ret = -ENOMEM;
		goto out;
	}

	pos = 0;
	while (pos < i_size) {
		bytes = kernel_read(file, pos, (char *)(*buf) + pos,
				    i_size - pos);
		if (bytes < 0) {
			ret = bytes;
			goto out;
		}

		if (bytes == 0)
			break;
		pos += bytes;
	}

	if (pos != i_size) {
		ret = -EIO;
		goto out_free;
	}

	ret = security_kernel_post_read_file(file, *buf, i_size, id);
	if (!ret)
		*size = pos;

out_free:
	if (ret < 0) {
		vfree(*buf);
		*buf = NULL;
	}

out:
	allow_write_access(file);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file);

int kernel_read_file_from_path(char *path, void **buf, loff_t *size,
			       loff_t max_size, enum kernel_read_file_id id)
{
	struct file *file;
	int ret;

	if (!path || !*path)
		return -EINVAL;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = kernel_read_file(file, buf, size, max_size, id);
	fput(file);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file_from_path);

int kernel_read_file_from_fd(int fd, void **buf, loff_t *size, loff_t max_size,
			     enum kernel_read_file_id id)
{
	struct fd f = fdget(fd);
	int ret = -EBADF;

	if (!f.file)
		goto out;

	ret = kernel_read_file(f.file, buf, size, max_size, id);
out:
	fdput(f);
	return ret;
}
EXPORT_SYMBOL_GPL(kernel_read_file_from_fd);

ssize_t read_code(struct file *file, unsigned long addr, loff_t pos, size_t len)
{
	ssize_t res = vfs_read(file, (void __user *)addr, len, &pos);
	if (res > 0)
		flush_icache_range(addr, addr + len);
	return res;
}
EXPORT_SYMBOL(read_code);

static int exec_mmap(struct mm_struct *mm)
{
	struct task_struct *tsk;
	struct mm_struct *old_mm, *active_mm;

	/* Notify parent that we're no longer interested in the old VM */
	tsk = current;
	old_mm = current->mm;
	mm_release(tsk, old_mm);

	if (old_mm) {
		sync_mm_rss(old_mm);
		/*
		 * Make sure that if there is a core dump in progress
		 * for the old mm, we get out and die instead of going
		 * through with the exec.  We must hold mmap_sem around
		 * checking core_state and changing tsk->mm.
		 */
		down_read(&old_mm->mmap_sem);
		if (unlikely(old_mm->core_state)) {
			up_read(&old_mm->mmap_sem);
			return -EINTR;
		}
	}
	task_lock(tsk);
	active_mm = tsk->active_mm;
	tsk->mm = mm;
	tsk->active_mm = mm;
	activate_mm(active_mm, mm);
	tsk->mm->vmacache_seqnum = 0;
	vmacache_flush(tsk);
	task_unlock(tsk);
	if (old_mm) {
		up_read(&old_mm->mmap_sem);
		BUG_ON(active_mm != old_mm);
		setmax_mm_hiwater_rss(&tsk->signal->maxrss, old_mm);
		mm_update_next_owner(old_mm);
		mmput(old_mm);
		return 0;
	}
	mmdrop(active_mm);
	return 0;
}

/*
 * This function makes sure the current process has its own signal table,
 * so that flush_signal_handlers can later reset the handlers without
 * disturbing other processes.  (Other processes might share the signal
 * table via the CLONE_SIGHAND option to clone().)
 */
static int de_thread(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct sighand_struct *oldsighand = tsk->sighand;
	spinlock_t *lock = &oldsighand->siglock;

	if (thread_group_empty(tsk))
		goto no_thread_group;

	/*
	 * Kill all other threads in the thread group.
	 */
	spin_lock_irq(lock);
	if (signal_group_exit(sig)) {
		/*
		 * Another group action in progress, just
		 * return so that the signal is processed.
		 */
		spin_unlock_irq(lock);
		return -EAGAIN;
	}

	sig->group_exit_task = tsk;
	sig->notify_count = zap_other_threads(tsk);
	if (!thread_group_leader(tsk))
		sig->notify_count--;

	while (sig->notify_count) {
		__set_current_state(TASK_KILLABLE);
		spin_unlock_irq(lock);
		schedule();
		if (unlikely(__fatal_signal_pending(tsk)))
			goto killed;
		spin_lock_irq(lock);
	}
	spin_unlock_irq(lock);

	/*
	 * At this point all other threads have exited, all we have to
	 * do is to wait for the thread group leader to become inactive,
	 * and to assume its PID:
	 */
	if (!thread_group_leader(tsk)) {
		struct task_struct *leader = tsk->group_leader;

		for (;;) {
			threadgroup_change_begin(tsk);
			write_lock_irq(&tasklist_lock);
			/*
			 * Do this under tasklist_lock to ensure that
			 * exit_notify() can't miss ->group_exit_task
			 */
			sig->notify_count = -1;
			if (likely(leader->exit_state))
				break;
			__set_current_state(TASK_KILLABLE);
			write_unlock_irq(&tasklist_lock);
			threadgroup_change_end(tsk);
			schedule();
			if (unlikely(__fatal_signal_pending(tsk)))
				goto killed;
		}

		/*
		 * The only record we have of the real-time age of a
		 * process, regardless of execs it's done, is start_time.
		 * All the past CPU time is accumulated in signal_struct
		 * from sister threads now dead.  But in this non-leader
		 * exec, nothing survives from the original leader thread,
		 * whose birth marks the true age of this process now.
		 * When we take on its identity by switching to its PID, we
		 * also take its birthdate (always earlier than our own).
		 */
		tsk->start_time = leader->start_time;
		tsk->real_start_time = leader->real_start_time;

		BUG_ON(!same_thread_group(leader, tsk));
		BUG_ON(has_group_leader_pid(tsk));
		/*
		 * An exec() starts a new thread group with the
		 * TGID of the previous thread group. Rehash the
		 * two threads with a switched PID, and release
		 * the former thread group leader:
		 */

		/* Become a process group leader with the old leader's pid.
		 * The old leader becomes a thread of the this thread group.
		 * Note: The old leader also uses this pid until release_task
		 *       is called.  Odd but simple and correct.
		 */
		tsk->pid = leader->pid;
		change_pid(tsk, PIDTYPE_PID, task_pid(leader));
		transfer_pid(leader, tsk, PIDTYPE_PGID);
		transfer_pid(leader, tsk, PIDTYPE_SID);

		list_replace_rcu(&leader->tasks, &tsk->tasks);
		list_replace_init(&leader->sibling, &tsk->sibling);

		tsk->group_leader = tsk;
		leader->group_leader = tsk;

		tsk->exit_signal = SIGCHLD;
		leader->exit_signal = -1;

		BUG_ON(leader->exit_state != EXIT_ZOMBIE);
		leader->exit_state = EXIT_DEAD;

		/*
		 * We are going to release_task()->ptrace_unlink() silently,
		 * the tracer can sleep in do_wait(). EXIT_DEAD guarantees
		 * the tracer wont't block again waiting for this thread.
		 */
		if (unlikely(leader->ptrace))
			__wake_up_parent(leader, leader->parent);
		write_unlock_irq(&tasklist_lock);
		threadgroup_change_end(tsk);

		release_task(leader);
	}

	sig->group_exit_task = NULL;
	sig->notify_count = 0;

no_thread_group:
	/* we have changed execution domain */
	tsk->exit_signal = SIGCHLD;

	exit_itimers(sig);
	flush_itimer_signals();

	if (atomic_read(&oldsighand->count) != 1) {
		struct sighand_struct *newsighand;
		/*
		 * This ->sighand is shared with the CLONE_SIGHAND
		 * but not CLONE_THREAD task, switch to the new one.
		 */
		newsighand = kmem_cache_alloc(sighand_cachep, GFP_KERNEL);
		if (!newsighand)
			return -ENOMEM;

		atomic_set(&newsighand->count, 1);
		memcpy(newsighand->action, oldsighand->action,
		       sizeof(newsighand->action));

		write_lock_irq(&tasklist_lock);
		spin_lock(&oldsighand->siglock);
		rcu_assign_pointer(tsk->sighand, newsighand);
		spin_unlock(&oldsighand->siglock);
		write_unlock_irq(&tasklist_lock);

		__cleanup_sighand(oldsighand);
	}

	BUG_ON(!thread_group_leader(tsk));
	return 0;

killed:
	/* protects against exit_notify() and __exit_signal() */
	read_lock(&tasklist_lock);
	sig->group_exit_task = NULL;
	sig->notify_count = 0;
	read_unlock(&tasklist_lock);
	return -EAGAIN;
}

char *get_task_comm(char *buf, struct task_struct *tsk)
{
	/* buf must be at least sizeof(tsk->comm) in size */
	task_lock(tsk);
	strncpy(buf, tsk->comm, sizeof(tsk->comm));
	task_unlock(tsk);
	return buf;
}
EXPORT_SYMBOL_GPL(get_task_comm);

/*
 * These functions flushes out all traces of the currently running executable
 * so that a new one can be started
 */

void __set_task_comm(struct task_struct *tsk, const char *buf, bool exec)
{
	task_lock(tsk);
	trace_task_rename(tsk, buf);
	strlcpy(tsk->comm, buf, sizeof(tsk->comm));
	task_unlock(tsk);
	perf_event_comm(tsk, exec);
}

int flush_old_exec(struct linux_binprm * bprm)
{
	int retval;

	/*
	 * Make sure we have a private signal table and that
	 * we are unassociated from the previous thread group.
	 */
	retval = de_thread(current);
	if (retval)
		goto out;

	/*
	 * Must be called _before_ exec_mmap() as bprm->mm is
	 * not visibile until then. This also enables the update
	 * to be lockless.
	 */
	set_mm_exe_file(bprm->mm, bprm->file);

	/*
	 * Release all of the old mmap stuff
	 */
	acct_arg_size(bprm, 0);
	retval = exec_mmap(bprm->mm);
	if (retval)
		goto out;

	bprm->mm = NULL;		/* We're using it now */

	set_fs(USER_DS);
	current->flags &= ~(PF_RANDOMIZE | PF_FORKNOEXEC | PF_KTHREAD |
					PF_NOFREEZE | PF_NO_SETAFFINITY);
	flush_thread();
	current->personality &= ~bprm->per_clear;

	return 0;

out:
	return retval;
}
EXPORT_SYMBOL(flush_old_exec);

void would_dump(struct linux_binprm *bprm, struct file *file)
{
	if (inode_permission(file_inode(file), MAY_READ) < 0)
		bprm->interp_flags |= BINPRM_FLAGS_ENFORCE_NONDUMP;
}
EXPORT_SYMBOL(would_dump);

void setup_new_exec(struct linux_binprm * bprm)
{
	arch_pick_mmap_layout(current->mm);

	/* This is the point of no return */
	current->sas_ss_sp = current->sas_ss_size = 0;

	if (uid_eq(current_euid(), current_uid()) && gid_eq(current_egid(), current_gid()))
		set_dumpable(current->mm, SUID_DUMP_USER);
	else
		set_dumpable(current->mm, suid_dumpable);

	perf_event_exec();
	__set_task_comm(current, kbasename(bprm->filename), true);

	/* Set the new mm task size. We have to do that late because it may
	 * depend on TIF_32BIT which is only updated in flush_thread() on
	 * some architectures like powerpc
	 */
	current->mm->task_size = TASK_SIZE;

	/* install the new credentials */
	if (!uid_eq(bprm->cred->uid, current_euid()) ||
	    !gid_eq(bprm->cred->gid, current_egid())) {
		current->pdeath_signal = 0;
	} else {
		would_dump(bprm, bprm->file);
		if (bprm->interp_flags & BINPRM_FLAGS_ENFORCE_NONDUMP)
			set_dumpable(current->mm, suid_dumpable);
	}

	/* An exec changes our domain. We are no longer part of the thread
	   group */
	current->self_exec_id++;
	flush_signal_handlers(current, 0);
	do_close_on_exec(current->files);
}
EXPORT_SYMBOL(setup_new_exec);

/*
 * Prepare credentials and lock ->cred_guard_mutex.
 * install_exec_creds() commits the new creds and drops the lock.
 * Or, if exec fails before, free_bprm() should release ->cred and
 * and unlock.
 */
int prepare_bprm_creds(struct linux_binprm *bprm)
{
	if (mutex_lock_interruptible(&current->signal->cred_guard_mutex))
		return -ERESTARTNOINTR;

	bprm->cred = prepare_exec_creds();
	if (likely(bprm->cred))
		return 0;

	mutex_unlock(&current->signal->cred_guard_mutex);
	return -ENOMEM;
}

static void free_bprm(struct linux_binprm *bprm)
{
	free_arg_pages(bprm);
	if (bprm->cred) {
		mutex_unlock(&current->signal->cred_guard_mutex);
		abort_creds(bprm->cred);
	}
	if (bprm->file) {
		allow_write_access(bprm->file);
		fput(bprm->file);
	}
	/* If a binfmt changed the interp, free it. */
	if (bprm->interp != bprm->filename)
		kfree(bprm->interp);
	kfree(bprm);
}

int bprm_change_interp(char *interp, struct linux_binprm *bprm)
{
	/* If a binfmt changed the interp, free it first. */
	if (bprm->interp != bprm->filename)
		kfree(bprm->interp);
	bprm->interp = kstrdup(interp, GFP_KERNEL);
	if (!bprm->interp)
		return -ENOMEM;
	return 0;
}
EXPORT_SYMBOL(bprm_change_interp);

/*
 * install the new credentials for this executable
 */
void install_exec_creds(struct linux_binprm *bprm)
{
	security_bprm_committing_creds(bprm);

	commit_creds(bprm->cred);
	bprm->cred = NULL;

	/*
	 * Disable monitoring for regular users
	 * when executing setuid binaries. Must
	 * wait until new credentials are committed
	 * by commit_creds() above
	 */
	if (get_dumpable(current->mm) != SUID_DUMP_USER)
		perf_event_exit_task(current);
	/*
	 * cred_guard_mutex must be held at least to this point to prevent
	 * ptrace_attach() from altering our determination of the task's
	 * credentials; any time after this it may be unlocked.
	 */
	security_bprm_committed_creds(bprm);
	mutex_unlock(&current->signal->cred_guard_mutex);
}
EXPORT_SYMBOL(install_exec_creds);

/*
 * determine how safe it is to execute the proposed program
 * - the caller must hold ->cred_guard_mutex to protect against
 *   PTRACE_ATTACH or seccomp thread-sync
 */
static void check_unsafe_exec(struct linux_binprm *bprm)
{
	struct task_struct *p = current, *t;
	unsigned n_fs;

	if (p->ptrace) {
		if (p->ptrace & PT_PTRACE_CAP)
			bprm->unsafe |= LSM_UNSAFE_PTRACE_CAP;
		else
			bprm->unsafe |= LSM_UNSAFE_PTRACE;
	}

	/*
	 * This isn't strictly necessary, but it makes it harder for LSMs to
	 * mess up.
	 */
	if (task_no_new_privs(current))
		bprm->unsafe |= LSM_UNSAFE_NO_NEW_PRIVS;

	t = p;
	n_fs = 1;
	spin_lock(&p->fs->lock);
	rcu_read_lock();
	while_each_thread(p, t) {
		if (t->fs == p->fs)
			n_fs++;
	}
	rcu_read_unlock();

	if (atomic_read(&p->fs->users) > n_fs)
		bprm->unsafe |= LSM_UNSAFE_SHARE;
	else
		p->fs->in_exec = 1;
	spin_unlock(&p->fs->lock);
}

static void bprm_fill_uid(struct linux_binprm *bprm)
{
	struct inode *inode;
	unsigned int mode;
	kuid_t uid;
	kgid_t gid;

	/*
	 * Since this can be called multiple times (via prepare_binprm),
	 * we must clear any previous work done when setting set[ug]id
	 * bits from any earlier bprm->file uses (for example when run
	 * first for a setuid script then again for its interpreter).
	 */
	bprm->cred->euid = current_euid();
	bprm->cred->egid = current_egid();

	if (bprm->file->f_path.mnt->mnt_flags & MNT_NOSUID)
		return;

	if (task_no_new_privs(current))
		return;

	inode = file_inode(bprm->file);
	mode = READ_ONCE(inode->i_mode);
	if (!(mode & (S_ISUID|S_ISGID)))
		return;

	/* Be careful if suid/sgid is set */
	inode_lock(inode);

	/* reload atomically mode/uid/gid now that lock held */
	mode = inode->i_mode;
	uid = inode->i_uid;
	gid = inode->i_gid;
	inode_unlock(inode);

	/* We ignore suid/sgid if there are no mappings for them in the ns */
	if (!kuid_has_mapping(bprm->cred->user_ns, uid) ||
		 !kgid_has_mapping(bprm->cred->user_ns, gid))
		return;

	if (mode & S_ISUID) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->euid = uid;
	}

	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
		bprm->per_clear |= PER_CLEAR_ON_SETID;
		bprm->cred->egid = gid;
	}
}

/*
 * Fill the binprm structure from the inode.
 * Check permissions, then read the first 128 (BINPRM_BUF_SIZE) bytes
 *
 * This may be called multiple times for binary chains (scripts for example).
 */
int prepare_binprm(struct linux_binprm *bprm)
{
	int retval;

	bprm_fill_uid(bprm);

	/* fill in binprm security blob */
	retval = security_bprm_set_creds(bprm);
	if (retval)
		return retval;
	bprm->cred_prepared = 1;

	memset(bprm->buf, 0, BINPRM_BUF_SIZE);
	return kernel_read(bprm->file, 0, bprm->buf, BINPRM_BUF_SIZE);
}

EXPORT_SYMBOL(prepare_binprm);

/*
 * Arguments are '\0' separated strings found at the location bprm->p
 * points to; chop off the first by relocating brpm->p to right after
 * the first '\0' encountered.
 */
int remove_arg_zero(struct linux_binprm *bprm)
{
	int ret = 0;
	unsigned long offset;
	char *kaddr;
	struct page *page;

	if (!bprm->argc)
		return 0;

	do {
		offset = bprm->p & ~PAGE_MASK;
		page = get_arg_page(bprm, bprm->p, 0);
		if (!page) {
			ret = -EFAULT;
			goto out;
		}
		kaddr = kmap_atomic(page);

		for (; offset < PAGE_SIZE && kaddr[offset];
				offset++, bprm->p++)
			;

		kunmap_atomic(kaddr);
		put_arg_page(page);
	} while (offset == PAGE_SIZE);

	bprm->p++;
	bprm->argc--;
	ret = 0;

out:
	return ret;
}
EXPORT_SYMBOL(remove_arg_zero);

#define printable(c) (((c)=='\t') || ((c)=='\n') || (0x20<=(c) && (c)<=0x7e))
/*
 * cycle the list of binary formats handler, until one recognizes the image
 */
int search_binary_handler(struct linux_binprm *bprm)
{
	bool need_retry = IS_ENABLED(CONFIG_MODULES);
	struct linux_binfmt *fmt;
	int retval;

	/* This allows 4 levels of binfmt rewrites before failing hard. */
	if (bprm->recursion_depth > 5)
		return -ELOOP;

	retval = security_bprm_check(bprm);
	if (retval)
		return retval;

	retval = -ENOENT;
 retry:
	read_lock(&binfmt_lock);
	list_for_each_entry(fmt, &formats, lh) {
		if (!try_module_get(fmt->module))
			continue;
		read_unlock(&binfmt_lock);
		bprm->recursion_depth++;
		retval = fmt->load_binary(bprm);
		read_lock(&binfmt_lock);
		put_binfmt(fmt);
		bprm->recursion_depth--;
		if (retval < 0 && !bprm->mm) {
			/* we got to flush_old_exec() and failed after it */
			read_unlock(&binfmt_lock);
			force_sigsegv(SIGSEGV, current);
			return retval;
		}
		if (retval != -ENOEXEC || !bprm->file) {
			read_unlock(&binfmt_lock);
			return retval;
		}
	}
	read_unlock(&binfmt_lock);

	if (need_retry) {
		if (printable(bprm->buf[0]) && printable(bprm->buf[1]) &&
		    printable(bprm->buf[2]) && printable(bprm->buf[3]))
			return retval;
		if (request_module("binfmt-%04x", *(ushort *)(bprm->buf + 2)) < 0)
			return retval;
		need_retry = false;
		goto retry;
	}

	return retval;
}
EXPORT_SYMBOL(search_binary_handler);

static int exec_binprm(struct linux_binprm *bprm)
{
	pid_t old_pid, old_vpid;
	int ret;

	/* Need to fetch pid before load_binary changes it */
	old_pid = current->pid;
	rcu_read_lock();
	old_vpid = task_pid_nr_ns(current, task_active_pid_ns(current->parent));
	rcu_read_unlock();

	ret = search_binary_handler(bprm);
	if (ret >= 0) {
		audit_bprm(bprm);
		trace_sched_process_exec(current, old_pid, bprm);
		ptrace_event(PTRACE_EVENT_EXEC, old_vpid);
		proc_exec_connector(current);
	}

	return ret;
}

#ifdef CONFIG_GRKERNSEC_PROC_MEMMAP
static DEFINE_PER_CPU(u64, exec_counter);
static int __init init_exec_counters(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		per_cpu(exec_counter, cpu) = (u64)cpu;
	}

	return 0;
}
early_initcall(init_exec_counters);
static inline void increment_exec_counter(void)
{
	BUILD_BUG_ON(NR_CPUS > (1 << 16));
	current->exec_id = this_cpu_add_return(exec_counter, 1 << 16);
}
#else
static inline void increment_exec_counter(void) {}
#endif

extern void gr_handle_exec_args(struct linux_binprm *bprm,
				struct user_arg_ptr argv);

/*
 * sys_execve() executes a new program.
 */
static int do_execveat_common(int fd, struct filename *filename,
			      struct user_arg_ptr argv,
			      struct user_arg_ptr envp,
			      int flags)
{
#ifdef CONFIG_GRKERNSEC
	struct file *old_exec_file;
	struct acl_subject_label *old_acl;
	struct rlimit old_rlim[RLIM_NLIMITS];
#endif
	char *pathbuf = NULL;
	struct linux_binprm *bprm;
	struct file *file;
	struct files_struct *displaced;
	int retval;

	if (IS_ERR(filename))
		return PTR_ERR(filename);

	gr_learn_resource(current, RLIMIT_NPROC, atomic_read(&current_user()->processes), 1);

	/*
	 * We move the actual failure in case of RLIMIT_NPROC excess from
	 * set*uid() to execve() because too many poorly written programs
	 * don't check setuid() return code.  Here we additionally recheck
	 * whether NPROC limit is still exceeded.
	 */
	if ((current->flags & PF_NPROC_EXCEEDED) &&
	    atomic_read(&current_user()->processes) > rlimit(RLIMIT_NPROC)) {
		retval = -EAGAIN;
		goto out_ret;
	}

	/* We're below the limit (still or again), so we don't want to make
	 * further execve() calls fail. */
	current->flags &= ~PF_NPROC_EXCEEDED;

	retval = unshare_files(&displaced);
	if (retval)
		goto out_ret;

	retval = -ENOMEM;
	bprm = kzalloc(sizeof(*bprm), GFP_KERNEL);
	if (!bprm)
		goto out_files;

	retval = prepare_bprm_creds(bprm);
	if (retval)
		goto out_free;

	check_unsafe_exec(bprm);
	current->in_execve = 1;

	file = do_open_execat(fd, filename, flags);
	retval = PTR_ERR(file);
	if (IS_ERR(file))
		goto out_unmark;

	sched_exec();

	bprm->file = file;
	if (fd == AT_FDCWD || filename->name[0] == '/') {
		bprm->filename = filename->name;
	} else {
		if (filename->name[0] == '\0')
			pathbuf = kasprintf(GFP_TEMPORARY, "/dev/fd/%d", fd);
		else
			pathbuf = kasprintf(GFP_TEMPORARY, "/dev/fd/%d/%s",
					    fd, filename->name);
		if (!pathbuf) {
			retval = -ENOMEM;
			goto out_unmark;
		}
		/*
		 * Record that a name derived from an O_CLOEXEC fd will be
		 * inaccessible after exec. Relies on having exclusive access to
		 * current->files (due to unshare_files above).
		 */
		if (close_on_exec(fd, rcu_dereference_raw(current->files->fdt)))
			bprm->interp_flags |= BINPRM_FLAGS_PATH_INACCESSIBLE;
		bprm->filename = pathbuf;
	}
	bprm->interp = bprm->filename;

	if (!gr_acl_handle_execve(file->f_path.dentry, file->f_path.mnt)) {
		retval = -EACCES;
		goto out_unmark;
	}

	retval = bprm_mm_init(bprm);
	if (retval)
		goto out_unmark;

	bprm->argc = count(argv, MAX_ARG_STRINGS);
	if ((retval = bprm->argc) < 0)
		goto out;

	bprm->envc = count(envp, MAX_ARG_STRINGS);
	if ((retval = bprm->envc) < 0)
		goto out;

	retval = prepare_binprm(bprm);
	if (retval < 0)
		goto out;

#ifdef CONFIG_GRKERNSEC
	old_acl = current->acl;
	memcpy(old_rlim, current->signal->rlim, sizeof(old_rlim));
	old_exec_file = current->exec_file;
	get_file(file);
	current->exec_file = file;
#endif
#ifdef CONFIG_GRKERNSEC_PROC_MEMMAP
	/* limit suid stack to 8MB
	 * we saved the old limits above and will restore them if this exec fails
	 */
	if (((!uid_eq(bprm->cred->euid, current_euid())) || (!gid_eq(bprm->cred->egid, current_egid()))) &&
	    (old_rlim[RLIMIT_STACK].rlim_cur > (8 * 1024 * 1024)))
		current->signal->rlim[RLIMIT_STACK].rlim_cur = 8 * 1024 * 1024;
#endif

	if (gr_process_kernel_exec_ban() || gr_process_sugid_exec_ban(bprm)) {
		retval = -EPERM;
		goto out_fail;
	}

	if (!gr_tpe_allow(file)) {
		retval = -EACCES;
		goto out_fail;
	}

	if (gr_check_crash_exec(file)) {
		retval = -EACCES;
		goto out_fail;
	}

	retval = gr_set_proc_label(file->f_path.dentry, file->f_path.mnt,
					bprm->unsafe);
	if (retval < 0)
		goto out_fail;

	retval = copy_strings_kernel(1, &bprm->filename, bprm);
	if (retval < 0)
		goto out_fail;

	bprm->exec = bprm->p;
	retval = copy_strings(bprm->envc, envp, bprm);
	if (retval < 0)
		goto out_fail;

	retval = copy_strings(bprm->argc, argv, bprm);
	if (retval < 0)
		goto out_fail;

	gr_log_chroot_exec(file->f_path.dentry, file->f_path.mnt);

	gr_handle_exec_args(bprm, argv);

	retval = exec_binprm(bprm);
	if (retval < 0)
		goto out_fail;
#ifdef CONFIG_GRKERNSEC
	if (old_exec_file)
		fput(old_exec_file);
#endif

	/* execve succeeded */

	increment_exec_counter();
	current->fs->in_exec = 0;
	current->in_execve = 0;
	acct_update_integrals(current);
	task_numa_free(current);
	free_bprm(bprm);
	kfree(pathbuf);
	putname(filename);
	if (displaced)
		put_files_struct(displaced);
	return retval;

out_fail:
#ifdef CONFIG_GRKERNSEC
	current->acl = old_acl;
	memcpy(current->signal->rlim, old_rlim, sizeof(old_rlim));
	fput(current->exec_file);
	current->exec_file = old_exec_file;
#endif

out:
	if (bprm->mm) {
		acct_arg_size(bprm, 0);
		mmput(bprm->mm);
	}

out_unmark:
	current->fs->in_exec = 0;
	current->in_execve = 0;

out_free:
	free_bprm(bprm);
	kfree(pathbuf);

out_files:
	if (displaced)
		reset_files_struct(displaced);
out_ret:
	putname(filename);
	return retval;
}

int do_execve(struct filename *filename,
	const char __user *const __user *__argv,
	const char __user *const __user *__envp)
{
	struct user_arg_ptr argv = { .ptr.native = __argv };
	struct user_arg_ptr envp = { .ptr.native = __envp };
	return do_execveat_common(AT_FDCWD, filename, argv, envp, 0);
}

int do_execveat(int fd, struct filename *filename,
		const char __user *const __user *__argv,
		const char __user *const __user *__envp,
		int flags)
{
	struct user_arg_ptr argv = { .ptr.native = __argv };
	struct user_arg_ptr envp = { .ptr.native = __envp };

	return do_execveat_common(fd, filename, argv, envp, flags);
}

#ifdef CONFIG_COMPAT
static int compat_do_execve(struct filename *filename,
	const compat_uptr_t __user *__argv,
	const compat_uptr_t __user *__envp)
{
	struct user_arg_ptr argv = {
		.is_compat = true,
		.ptr.compat = __argv,
	};
	struct user_arg_ptr envp = {
		.is_compat = true,
		.ptr.compat = __envp,
	};
	return do_execveat_common(AT_FDCWD, filename, argv, envp, 0);
}

static int compat_do_execveat(int fd, struct filename *filename,
			      const compat_uptr_t __user *__argv,
			      const compat_uptr_t __user *__envp,
			      int flags)
{
	struct user_arg_ptr argv = {
		.is_compat = true,
		.ptr.compat = __argv,
	};
	struct user_arg_ptr envp = {
		.is_compat = true,
		.ptr.compat = __envp,
	};
	return do_execveat_common(fd, filename, argv, envp, flags);
}
#endif

void set_binfmt(struct linux_binfmt *new)
{
	struct mm_struct *mm = current->mm;

	if (mm->binfmt)
		module_put(mm->binfmt->module);

	mm->binfmt = new;
	if (new)
		__module_get(new->module);
}
EXPORT_SYMBOL(set_binfmt);

/*
 * set_dumpable stores three-value SUID_DUMP_* into mm->flags.
 */
void set_dumpable(struct mm_struct *mm, int value)
{
	unsigned long old, new;

	if (WARN_ON((unsigned)value > SUID_DUMP_ROOT))
		return;

	do {
		old = ACCESS_ONCE(mm->flags);
		new = (old & ~MMF_DUMPABLE_MASK) | value;
	} while (cmpxchg(&mm->flags, old, new) != old);
}

SYSCALL_DEFINE3(execve,
		const char __user *, filename,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp)
{
	return do_execve(getname(filename), argv, envp);
}

SYSCALL_DEFINE5(execveat,
		int, fd, const char __user *, filename,
		const char __user *const __user *, argv,
		const char __user *const __user *, envp,
		int, flags)
{
	int lookup_flags = (flags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;

	return do_execveat(fd,
			   getname_flags(filename, lookup_flags, NULL),
			   argv, envp, flags);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE3(execve, const char __user *, filename,
	const compat_uptr_t __user *, argv,
	const compat_uptr_t __user *, envp)
{
	return compat_do_execve(getname(filename), argv, envp);
}

COMPAT_SYSCALL_DEFINE5(execveat, int, fd,
		       const char __user *, filename,
		       const compat_uptr_t __user *, argv,
		       const compat_uptr_t __user *, envp,
		       int,  flags)
{
	int lookup_flags = (flags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;

	return compat_do_execveat(fd,
				  getname_flags(filename, lookup_flags, NULL),
				  argv, envp, flags);
}
#endif

int pax_check_flags(unsigned long *flags)
{
	int retval = 0;

#if !defined(CONFIG_X86_32) || !defined(CONFIG_PAX_SEGMEXEC)
	if (*flags & MF_PAX_SEGMEXEC)
	{
		*flags &= ~MF_PAX_SEGMEXEC;
		retval = -EINVAL;
	}
#endif

	if ((*flags & MF_PAX_PAGEEXEC)

#ifdef CONFIG_PAX_PAGEEXEC
	    &&  (*flags & MF_PAX_SEGMEXEC)
#endif

	   )
	{
		*flags &= ~MF_PAX_PAGEEXEC;
		retval = -EINVAL;
	}

	if ((*flags & MF_PAX_MPROTECT)

#ifdef CONFIG_PAX_MPROTECT
	    && !(*flags & (MF_PAX_PAGEEXEC | MF_PAX_SEGMEXEC))
#endif

	   )
	{
		*flags &= ~MF_PAX_MPROTECT;
		retval = -EINVAL;
	}

	if ((*flags & MF_PAX_EMUTRAMP)

#ifdef CONFIG_PAX_EMUTRAMP
	    && !(*flags & (MF_PAX_PAGEEXEC | MF_PAX_SEGMEXEC))
#endif

	   )
	{
		*flags &= ~MF_PAX_EMUTRAMP;
		retval = -EINVAL;
	}

	return retval;
}

EXPORT_SYMBOL(pax_check_flags);

#if defined(CONFIG_PAX_PAGEEXEC) || defined(CONFIG_PAX_SEGMEXEC)
char *pax_get_path(const struct path *path, char *buf, int buflen)
{
	char *pathname = d_path(path, buf, buflen);

	if (IS_ERR(pathname))
		goto toolong;

	pathname = mangle_path(buf, pathname, "\t\n\\");
	if (!pathname)
		goto toolong;

	*pathname = 0;
	return buf;

toolong:
	return "<path too long>";
}
EXPORT_SYMBOL(pax_get_path);

void pax_report_fault(struct pt_regs *regs, void *pc, void *sp)
{
	struct task_struct *tsk = current;
	struct mm_struct *mm = current->mm;
	char *buffer_exec = (char *)__get_free_page(GFP_KERNEL);
	char *buffer_fault = (char *)__get_free_page(GFP_KERNEL);
	char *path_exec = NULL;
	char *path_fault = NULL;
	unsigned long start = 0UL, end = 0UL, offset = 0UL;
	siginfo_t info = { };

	if (buffer_exec && buffer_fault) {
		struct vm_area_struct *vma, *vma_exec = NULL, *vma_fault = NULL;

		down_read(&mm->mmap_sem);
		vma = mm->mmap;
		while (vma && (!vma_exec || !vma_fault)) {
			if (vma->vm_file && mm->exe_file == vma->vm_file && (vma->vm_flags & VM_EXEC))
				vma_exec = vma;
			if (vma->vm_start <= (unsigned long)pc && (unsigned long)pc < vma->vm_end)
				vma_fault = vma;
			vma = vma->vm_next;
		}
		if (vma_exec)
			path_exec = pax_get_path(&vma_exec->vm_file->f_path, buffer_exec, PAGE_SIZE);
		if (vma_fault) {
			start = vma_fault->vm_start;
			end = vma_fault->vm_end;
			offset = vma_fault->vm_pgoff << PAGE_SHIFT;
			if (vma_fault->vm_file)
				path_fault = pax_get_path(&vma_fault->vm_file->f_path, buffer_fault, PAGE_SIZE);
			else if ((unsigned long)pc >= mm->start_brk && (unsigned long)pc < mm->brk)
				path_fault = "<heap>";
			else if (vma_fault->vm_flags & (VM_GROWSDOWN | VM_GROWSUP))
				path_fault = "<stack>";
			else
				path_fault = "<anonymous mapping>";
		}
		up_read(&mm->mmap_sem);
	}
	if (tsk->signal->curr_ip)
		printk(KERN_ERR "PAX: From %pI4: execution attempt in: %s, %08lx-%08lx %08lx\n", &tsk->signal->curr_ip, path_fault, start, end, offset);
	else
		printk(KERN_ERR "PAX: execution attempt in: %s, %08lx-%08lx %08lx\n", path_fault, start, end, offset);
	printk(KERN_ERR "PAX: terminating task: %s(%s):%d, uid/euid: %u/%u, PC: %p, SP: %p\n", path_exec, tsk->comm, task_pid_nr(tsk),
			from_kuid_munged(&init_user_ns, task_uid(tsk)), from_kuid_munged(&init_user_ns, task_euid(tsk)), pc, sp);
	free_page((unsigned long)buffer_exec);
	free_page((unsigned long)buffer_fault);
	pax_report_insns(regs, pc, sp);
	info.si_signo = SIGKILL;
	info.si_errno = 0;
	info.si_code = SI_KERNEL;
	info.si_pid = 0;
	info.si_uid = 0;
	do_coredump(&info);
}
#endif

#ifdef CONFIG_PAX_REFCOUNT
void pax_report_refcount_overflow(struct pt_regs *regs)
{
	if (current->signal->curr_ip)
		printk(KERN_EMERG "PAX: From %pI4: refcount overflow detected in: %s:%d, uid/euid: %u/%u\n",
				&current->signal->curr_ip, current->comm, task_pid_nr(current),
				from_kuid_munged(&init_user_ns, current_uid()), from_kuid_munged(&init_user_ns, current_euid()));
	else
		printk(KERN_EMERG "PAX: refcount overflow detected in: %s:%d, uid/euid: %u/%u\n", current->comm, task_pid_nr(current),
				from_kuid_munged(&init_user_ns, current_uid()), from_kuid_munged(&init_user_ns, current_euid()));
	print_symbol(KERN_EMERG "PAX: refcount overflow occured at: %s\n", instruction_pointer(regs));
	BUG();
}
#endif

#ifdef CONFIG_PAX_USERCOPY
/* 0: not at all, 1: fully, 2: fully inside frame, -1: partially (implies an error) */
static noinline int check_stack_object(unsigned long obj, unsigned long len)
{
	unsigned long stack = (unsigned long)task_stack_page(current);
	unsigned long stackend = (unsigned long)stack + THREAD_SIZE;

#if defined(CONFIG_FRAME_POINTER) && defined(CONFIG_X86)
	unsigned long frame = 0;
	unsigned long oldframe;
#endif

	if (obj + len < obj)
		return -1;

	if (obj + len <= (unsigned long)stack || (unsigned long)stackend <= obj)
		return 0;

	if (obj < (unsigned long)stack || (unsigned long)stackend < obj + len)
		return -1;

#if defined(CONFIG_FRAME_POINTER) && defined(CONFIG_X86)
	oldframe = (unsigned long)__builtin_frame_address(1);
	if (oldframe)
		frame = (unsigned long)__builtin_frame_address(2);
	/*
	  low ----------------------------------------------> high
	  [saved bp][saved ip][args][local vars][saved bp][saved ip]
			      ^----------------^
			  allow copies only within here
	*/
	while (stack <= frame && frame < stackend) {
		/* if obj + len extends past the last frame, this
		   check won't pass and the next frame will be 0,
		   causing us to bail out and correctly report
		   the copy as invalid
		*/
		if (obj + len <= frame)
			return obj >= oldframe + 2 * sizeof(unsigned long) ? 2 : -1;
		oldframe = frame;
		frame = *(unsigned long *)frame;
	}
	return -1;
#else
	return 1;
#endif
}

static __noreturn void pax_report_usercopy(const void *ptr, unsigned long len, bool to_user, const char *type)
{
	if (current->signal->curr_ip)
		printk(KERN_EMERG "PAX: From %pI4: kernel memory %s attempt detected %s %p (%s) (%lu bytes)\n",
			&current->signal->curr_ip, to_user ? "leak" : "overwrite", to_user ? "from" : "to", ptr, type ? : "unknown", len);
	else
		printk(KERN_EMERG "PAX: kernel memory %s attempt detected %s %p (%s) (%lu bytes)\n",
			to_user ? "leak" : "overwrite", to_user ? "from" : "to", ptr, type ? : "unknown", len);
	dump_stack();
	gr_handle_kernel_exploit();
	do_group_exit(SIGKILL);
}
#endif

#ifdef CONFIG_PAX_USERCOPY

static inline bool check_kernel_text_object(unsigned long low, unsigned long high)
{
#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_KERNEXEC)
	unsigned long textlow = ktla_ktva((unsigned long)_stext);
#ifdef CONFIG_MODULES
	unsigned long texthigh = (unsigned long)MODULES_EXEC_VADDR;
#else
	unsigned long texthigh = ktla_ktva((unsigned long)_etext);
#endif

#else
	unsigned long textlow = (unsigned long)_stext;
	unsigned long texthigh = (unsigned long)_etext;

#ifdef CONFIG_X86_64
	/* check against linear mapping as well */
	if (high > (unsigned long)__va(__pa(textlow)) &&
	    low < (unsigned long)__va(__pa(texthigh)))
		return true;
#endif

#endif

	if (high <= textlow || low >= texthigh)
		return false;
	else
		return true;
}
#endif

void __check_object_size(const void *ptr, unsigned long n, bool to_user, bool const_size)
{
#ifdef CONFIG_PAX_USERCOPY
	const char *type;
#endif

#if !defined(CONFIG_STACK_GROWSUP) && !defined(CONFIG_X86_64)
	unsigned long stackstart = (unsigned long)task_stack_page(current);
	unsigned long currentsp = (unsigned long)&stackstart;
	if (unlikely((currentsp < stackstart + 512 ||
		     currentsp >= stackstart + THREAD_SIZE) && !in_interrupt()))
		BUG();
#endif

#ifndef CONFIG_PAX_USERCOPY_DEBUG
	if (const_size)
		return;
#endif

#ifdef CONFIG_PAX_USERCOPY
	if (!n)
		return;

	type = check_heap_object(ptr, n);
	if (!type) {
		int ret = check_stack_object((unsigned long)ptr, n);
		if (ret == 1 || ret == 2)
			return;
		if (ret == 0) {
			if (check_kernel_text_object((unsigned long)ptr, (unsigned long)ptr + n))
				type = "<kernel text>";
			else
				return;
		} else
			type = "<process stack>";
	}

	pax_report_usercopy(ptr, n, to_user, type);
#endif

}
EXPORT_SYMBOL(__check_object_size);

#ifdef CONFIG_PAX_MEMORY_STACKLEAK
void __used pax_track_stack(void)
{
	unsigned long sp = (unsigned long)&sp;
	if (sp < current_thread_info()->lowest_stack &&
	    sp >= (unsigned long)task_stack_page(current) + 2 * sizeof(unsigned long))
		current_thread_info()->lowest_stack = sp;
	if (unlikely((sp & ~(THREAD_SIZE - 1)) < (THREAD_SIZE/16)))
		BUG();
}
EXPORT_SYMBOL(pax_track_stack);
#endif

#ifdef CONFIG_PAX_SIZE_OVERFLOW

static DEFINE_RATELIMIT_STATE(size_overflow_ratelimit, 15 * HZ, 3);
extern bool pax_size_overflow_report_only;

void __nocapture(1, 3, 4) __used report_size_overflow(const char *file, unsigned int line, const char *func, const char *ssa_name)
{
	if (!pax_size_overflow_report_only || __ratelimit(&size_overflow_ratelimit)) {
		printk(KERN_EMERG "PAX: size overflow detected in function %s %s:%u %s", func, file, line, ssa_name);
		dump_stack();
	}
	if (!pax_size_overflow_report_only)
		do_group_exit(SIGKILL);
}
EXPORT_SYMBOL(report_size_overflow);
#endif
