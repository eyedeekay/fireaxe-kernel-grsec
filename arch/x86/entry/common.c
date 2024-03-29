/*
 * common.c - C code for kernel entry and exit
 * Copyright (c) 2015 Andrew Lutomirski
 * GPL v2
 *
 * Based on asm and ptrace code by many authors.  The code here originated
 * in ptrace.c and signal.c.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/tracehook.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <linux/signal.h>
#include <linux/export.h>
#include <linux/context_tracking.h>
#include <linux/user-return-notifier.h>
#include <linux/uprobes.h>

#include <asm/desc.h>
#include <asm/traps.h>
#include <asm/vdso.h>
#include <asm/uaccess.h>
#include <asm/cpufeature.h>

#define CREATE_TRACE_POINTS
#include <trace/events/syscalls.h>

static struct thread_info *pt_regs_to_thread_info(struct pt_regs *regs)
{
	return current_thread_info();
}

#ifdef CONFIG_CONTEXT_TRACKING
/* Called on entry from user mode with IRQs off. */
__visible void enter_from_user_mode(void)
{
	CT_WARN_ON(ct_state() != CONTEXT_USER);
	user_exit();
}
#else
static inline void enter_from_user_mode(void) {}
#endif

#ifdef CONFIG_PAX_MEMORY_STACKLEAK
asmlinkage void pax_erase_kstack(void);
#else
static void pax_erase_kstack(void) {}
#endif

static void do_audit_syscall_entry(struct pt_regs *regs, u32 arch)
{
#ifdef CONFIG_X86_64
	if (arch == AUDIT_ARCH_X86_64) {
		audit_syscall_entry(regs->orig_ax, regs->di,
				    regs->si, regs->dx, regs->r10);
	} else
#endif
	{
		audit_syscall_entry(regs->orig_ax, regs->bx,
				    regs->cx, regs->dx, regs->si);
	}
}

/*
 * We can return 0 to resume the syscall or anything else to go to phase
 * 2.  If we resume the syscall, we need to put something appropriate in
 * regs->orig_ax.
 *
 * NB: We don't have full pt_regs here, but regs->orig_ax and regs->ax
 * are fully functional.
 *
 * For phase 2's benefit, our return value is:
 * 0:			resume the syscall
 * 1:			go to phase 2; no seccomp phase 2 needed
 * anything else:	go to phase 2; pass return value to seccomp
 */
unsigned long syscall_trace_enter_phase1(struct pt_regs *regs, u32 arch)
{
	struct thread_info *ti = pt_regs_to_thread_info(regs);
	unsigned long ret = 0;
	u32 work;

	if (IS_ENABLED(CONFIG_DEBUG_ENTRY))
		BUG_ON(regs != task_pt_regs(current));

	work = ACCESS_ONCE(ti->flags) & _TIF_WORK_SYSCALL_ENTRY;

#ifdef CONFIG_SECCOMP
	/*
	 * Do seccomp first -- it should minimize exposure of other
	 * code, and keeping seccomp fast is probably more valuable
	 * than the rest of this.
	 */
	if (work & _TIF_SECCOMP) {
		struct seccomp_data sd;

		sd.arch = arch;
		sd.nr = regs->orig_ax;
		sd.instruction_pointer = regs->ip;
#ifdef CONFIG_X86_64
		if (arch == AUDIT_ARCH_X86_64) {
			sd.args[0] = regs->di;
			sd.args[1] = regs->si;
			sd.args[2] = regs->dx;
			sd.args[3] = regs->r10;
			sd.args[4] = regs->r8;
			sd.args[5] = regs->r9;
		} else
#endif
		{
			sd.args[0] = regs->bx;
			sd.args[1] = regs->cx;
			sd.args[2] = regs->dx;
			sd.args[3] = regs->si;
			sd.args[4] = regs->di;
			sd.args[5] = regs->bp;
		}

		BUILD_BUG_ON(SECCOMP_PHASE1_OK != 0);
		BUILD_BUG_ON(SECCOMP_PHASE1_SKIP != 1);

		ret = seccomp_phase1(&sd);
		if (ret == SECCOMP_PHASE1_SKIP) {
			regs->orig_ax = -1;
			ret = 0;
		} else if (ret != SECCOMP_PHASE1_OK) {
			return ret;  /* Go directly to phase 2 */
		}

		work &= ~_TIF_SECCOMP;
	}
#endif

	/* Do our best to finish without phase 2. */
	if (work == 0)
		return ret;  /* seccomp and/or nohz only (ret == 0 here) */

#ifdef CONFIG_AUDITSYSCALL
	if (work == _TIF_SYSCALL_AUDIT) {
		/*
		 * If there is no more work to be done except auditing,
		 * then audit in phase 1.  Phase 2 always audits, so, if
		 * we audit here, then we can't go on to phase 2.
		 */
		do_audit_syscall_entry(regs, arch);
		return 0;
	}
#endif

	return 1;  /* Something is enabled that we can't handle in phase 1 */
}

#ifdef CONFIG_GRKERNSEC_SETXID
extern void gr_delayed_cred_worker(void);
#endif

/* Returns the syscall nr to run (which should match regs->orig_ax). */
long syscall_trace_enter_phase2(struct pt_regs *regs, u32 arch,
				unsigned long phase1_result)
{
	struct thread_info *ti = pt_regs_to_thread_info(regs);
	long ret = 0;
	u32 work = ACCESS_ONCE(ti->flags) & _TIF_WORK_SYSCALL_ENTRY;

	if (IS_ENABLED(CONFIG_DEBUG_ENTRY))
		BUG_ON(regs != task_pt_regs(current));

#ifdef CONFIG_GRKERNSEC_SETXID
	if (unlikely(test_and_clear_thread_flag(TIF_GRSEC_SETXID)))
		gr_delayed_cred_worker();
#endif

#ifdef CONFIG_SECCOMP
	/*
	 * Call seccomp_phase2 before running the other hooks so that
	 * they can see any changes made by a seccomp tracer.
	 */
	if (phase1_result > 1 && seccomp_phase2(phase1_result)) {
		/* seccomp failures shouldn't expose any additional code. */
		return -1;
	}
#endif

	if (unlikely(work & _TIF_SYSCALL_EMU))
		ret = -1L;

	if ((ret || test_thread_flag(TIF_SYSCALL_TRACE)) &&
	    tracehook_report_syscall_entry(regs))
		ret = -1L;

	if (unlikely(test_thread_flag(TIF_SYSCALL_TRACEPOINT)))
		trace_sys_enter(regs, regs->orig_ax);

	do_audit_syscall_entry(regs, arch);

	return ret ?: regs->orig_ax;
}

static long syscall_trace_enter(struct pt_regs *regs)
{
	u32 arch = in_ia32_syscall() ? AUDIT_ARCH_I386 : AUDIT_ARCH_X86_64;
	unsigned long phase1_result = syscall_trace_enter_phase1(regs, arch);

	phase1_result = phase1_result ? syscall_trace_enter_phase2(regs, arch, phase1_result) : regs->orig_ax;
	pax_erase_kstack();
	return phase1_result;
}

#define EXIT_TO_USERMODE_LOOP_FLAGS				\
	(_TIF_SIGPENDING | _TIF_NOTIFY_RESUME | _TIF_UPROBE |	\
	 _TIF_NEED_RESCHED | _TIF_USER_RETURN_NOTIFY)

static void exit_to_usermode_loop(struct pt_regs *regs, u32 cached_flags)
{
	/*
	 * In order to return to user mode, we need to have IRQs off with
	 * none of _TIF_SIGPENDING, _TIF_NOTIFY_RESUME, _TIF_USER_RETURN_NOTIFY,
	 * _TIF_UPROBE, or _TIF_NEED_RESCHED set.  Several of these flags
	 * can be set at any time on preemptable kernels if we have IRQs on,
	 * so we need to loop.  Disabling preemption wouldn't help: doing the
	 * work to clear some of the flags can sleep.
	 */
	while (true) {
		/* We have work to do. */
		local_irq_enable();

		if (cached_flags & _TIF_NEED_RESCHED)
			schedule();

		if (cached_flags & _TIF_UPROBE)
			uprobe_notify_resume(regs);

		/* deal with pending signal delivery */
		if (cached_flags & _TIF_SIGPENDING)
			do_signal(regs);

		if (cached_flags & _TIF_NOTIFY_RESUME) {
			clear_thread_flag(TIF_NOTIFY_RESUME);
			tracehook_notify_resume(regs);
		}

		if (cached_flags & _TIF_USER_RETURN_NOTIFY)
			fire_user_return_notifiers();

		/* Disable IRQs and retry */
		local_irq_disable();

		cached_flags = READ_ONCE(pt_regs_to_thread_info(regs)->flags);

		if (!(cached_flags & EXIT_TO_USERMODE_LOOP_FLAGS))
			break;

	}
}

/* Called with IRQs disabled. */
__visible inline void prepare_exit_to_usermode(struct pt_regs *regs)
{
	struct thread_info *ti = pt_regs_to_thread_info(regs);
	u32 cached_flags;

	if (IS_ENABLED(CONFIG_PROVE_LOCKING) && WARN_ON(!irqs_disabled()))
		local_irq_disable();

	lockdep_sys_exit();

	cached_flags = READ_ONCE(ti->flags);

	if (unlikely(cached_flags & EXIT_TO_USERMODE_LOOP_FLAGS))
		exit_to_usermode_loop(regs, cached_flags);

#ifdef CONFIG_COMPAT
	/*
	 * Compat syscalls set TS_COMPAT.  Make sure we clear it before
	 * returning to user mode.  We need to clear it *after* signal
	 * handling, because syscall restart has a fixup for compat
	 * syscalls.  The fixup is exercised by the ptrace_syscall_32
	 * selftest.
	 */
	ti->status &= ~TS_COMPAT;
#endif

	user_enter();
}

#define SYSCALL_EXIT_WORK_FLAGS				\
	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT |	\
	 _TIF_SINGLESTEP | _TIF_SYSCALL_TRACEPOINT)

static void syscall_slow_exit_work(struct pt_regs *regs, u32 cached_flags)
{
	bool step;

	audit_syscall_exit(regs);

	if (cached_flags & _TIF_SYSCALL_TRACEPOINT)
		trace_sys_exit(regs, regs->ax);

	/*
	 * If TIF_SYSCALL_EMU is set, we only get here because of
	 * TIF_SINGLESTEP (i.e. this is PTRACE_SYSEMU_SINGLESTEP).
	 * We already reported this syscall instruction in
	 * syscall_trace_enter().
	 */
	step = unlikely(
		(cached_flags & (_TIF_SINGLESTEP | _TIF_SYSCALL_EMU))
		== _TIF_SINGLESTEP);
	if (step || (cached_flags & _TIF_SYSCALL_TRACE))
		tracehook_report_syscall_exit(regs, step);
}

/*
 * Called with IRQs on and fully valid regs.  Returns with IRQs off in a
 * state such that we can immediately switch to user mode.
 */
__visible inline void syscall_return_slowpath(struct pt_regs *regs)
{
	struct thread_info *ti = pt_regs_to_thread_info(regs);
	u32 cached_flags = READ_ONCE(ti->flags);

	CT_WARN_ON(ct_state() != CONTEXT_KERNEL);

	if (IS_ENABLED(CONFIG_PROVE_LOCKING) &&
	    WARN(irqs_disabled(), "syscall %ld left IRQs disabled", regs->orig_ax))
		local_irq_enable();

#ifdef CONFIG_GRKERNSEC_SETXID
	if (unlikely(test_and_clear_thread_flag(TIF_GRSEC_SETXID)))
		gr_delayed_cred_worker();
#endif

	/*
	 * First do one-time work.  If these work items are enabled, we
	 * want to run them exactly once per syscall exit with IRQs on.
	 */
	if (unlikely(cached_flags & SYSCALL_EXIT_WORK_FLAGS))
		syscall_slow_exit_work(regs, cached_flags);

	local_irq_disable();
	prepare_exit_to_usermode(regs);
}

#ifdef CONFIG_X86_64
__visible void do_syscall_64(struct pt_regs *regs)
{
	struct thread_info *ti = pt_regs_to_thread_info(regs);
	unsigned long nr = regs->orig_ax;

	enter_from_user_mode();
	local_irq_enable();

	if (READ_ONCE(ti->flags) & _TIF_WORK_SYSCALL_ENTRY)
		nr = syscall_trace_enter(regs);

	/*
	 * NB: Native and x32 syscalls are dispatched from the same
	 * table.  The only functional difference is the x32 bit in
	 * regs->orig_ax, which changes the behavior of some syscalls.
	 */
	if (likely((nr & __SYSCALL_MASK) < NR_syscalls)) {
#ifdef CONFIG_PAX_RAP
		asm volatile("movq %[param1],%%rdi\n\t"
			     "movq %[param2],%%rsi\n\t"
			     "movq %[param3],%%rdx\n\t"
			     "movq %[param4],%%rcx\n\t"
			     "movq %[param5],%%r8\n\t"
			     "movq %[param6],%%r9\n\t"
			     "call *%P[syscall]\n\t"
			     "mov %%rax,%[result]\n\t"
			: [result] "=m" (regs->ax)
			: [syscall] "m" (sys_call_table[nr & __SYSCALL_MASK]),
			  [param1] "m" (regs->di),
			  [param2] "m" (regs->si),
			  [param3] "m" (regs->dx),
			  [param4] "m" (regs->r10),
			  [param5] "m" (regs->r8),
			  [param6] "m" (regs->r9)
			: "ax", "di", "si", "dx", "cx", "r8", "r9", "r10", "r11", "memory");
#else
		regs->ax = sys_call_table[nr & __SYSCALL_MASK](
			regs->di, regs->si, regs->dx,
			regs->r10, regs->r8, regs->r9);
#endif
	}

	syscall_return_slowpath(regs);
}
#endif

#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
/*
 * Does a 32-bit syscall.  Called with IRQs on in CONTEXT_KERNEL.  Does
 * all entry and exit work and returns with IRQs off.  This function is
 * extremely hot in workloads that use it, and it's usually called from
 * do_fast_syscall_32, so forcibly inline it to improve performance.
 */
static __always_inline void do_syscall_32_irqs_on(struct pt_regs *regs)
{
	struct thread_info *ti = pt_regs_to_thread_info(regs);
	unsigned int nr = (unsigned int)regs->orig_ax;

#ifdef CONFIG_IA32_EMULATION
	ti->status |= TS_COMPAT;
#endif

	if (READ_ONCE(ti->flags) & _TIF_WORK_SYSCALL_ENTRY) {
		/*
		 * Subtlety here: if ptrace pokes something larger than
		 * 2^32-1 into orig_ax, this truncates it.  This may or
		 * may not be necessary, but it matches the old asm
		 * behavior.
		 */
		nr = syscall_trace_enter(regs);
	}

	if (likely(nr < IA32_NR_syscalls)) {
		/*
		 * It's possible that a 32-bit syscall implementation
		 * takes a 64-bit parameter but nonetheless assumes that
		 * the high bits are zero.  Make sure we zero-extend all
		 * of the args.
		 */
#ifdef CONFIG_PAX_RAP
#ifdef CONFIG_X86_64
		asm volatile("movl %[param1],%%edi\n\t"
			     "movl %[param2],%%esi\n\t"
			     "movl %[param3],%%edx\n\t"
			     "movl %[param4],%%ecx\n\t"
			     "movl %[param5],%%r8d\n\t"
			     "movl %[param6],%%r9d\n\t"
			     "call *%P[syscall]\n\t"
			     "mov %%rax,%[result]\n\t"
			: [result] "=m" (regs->ax)
			: [syscall] "m" (ia32_sys_call_table[nr]),
			  [param1] "m" (regs->bx),
			  [param2] "m" (regs->cx),
			  [param3] "m" (regs->dx),
			  [param4] "m" (regs->si),
			  [param5] "m" (regs->di),
			  [param6] "m" (regs->bp)
			: "ax", "di", "si", "dx", "cx", "r8", "r9", "r10", "r11", "memory");
#else
		asm volatile("pushl %[param6]\n\t"
			     "pushl %[param5]\n\t"
			     "pushl %[param4]\n\t"
			     "pushl %[param3]\n\t"
			     "pushl %[param2]\n\t"
			     "pushl %[param1]\n\t"
			     "call *%P[syscall]\n\t"
			     "addl $6*8,%%esp\n\t"
			     "mov %%eax,%[result]\n\t"
			: [result] "=m" (regs->ax)
			: [syscall] "m" (ia32_sys_call_table[nr]),
			  [param1] "m" (regs->bx),
			  [param2] "m" (regs->cx),
			  [param3] "m" (regs->dx),
			  [param4] "m" (regs->si),
			  [param5] "m" (regs->di),
			  [param6] "m" (regs->bp)
			: "ax", "dx", "cx", "memory");
#endif
#else
		regs->ax = ia32_sys_call_table[nr](
			(unsigned int)regs->bx, (unsigned int)regs->cx,
			(unsigned int)regs->dx, (unsigned int)regs->si,
			(unsigned int)regs->di, (unsigned int)regs->bp);
#endif
	}

	syscall_return_slowpath(regs);
}

/* Handles int $0x80 */
__visible void do_int80_syscall_32(struct pt_regs *regs)
{
	enter_from_user_mode();
	local_irq_enable();
	do_syscall_32_irqs_on(regs);
}

/* Returns 0 to return using IRET or 1 to return using SYSEXIT/SYSRETL. */
__visible long do_fast_syscall_32(struct pt_regs *regs)
{
	/*
	 * Called using the internal vDSO SYSENTER/SYSCALL32 calling
	 * convention.  Adjust regs so it looks like we entered using int80.
	 */

	unsigned long landing_pad = (unsigned long)current->mm->context.vdso +
		vdso_image_32.sym_int80_landing_pad;
	u32 __user *saved_bp = (u32 __force_user *)(unsigned long)(u32)regs->sp;

	/*
	 * SYSENTER loses EIP, and even SYSCALL32 needs us to skip forward
	 * so that 'regs->ip -= 2' lands back on an int $0x80 instruction.
	 * Fix it up.
	 */
	regs->ip = landing_pad;

	enter_from_user_mode();

	local_irq_enable();

	/* Fetch EBP from where the vDSO stashed it. */
	if (
#ifdef CONFIG_X86_64
		/*
		 * Micro-optimization: the pointer we're following is explicitly
		 * 32 bits, so it can't be out of range.
		 */
		__get_user_nocheck(*(u32 *)&regs->bp, saved_bp, sizeof(u32))
#else
		get_user(regs->bp, saved_bp)
#endif
		) {

		/* User code screwed up. */
		local_irq_disable();
		regs->ax = -EFAULT;
		prepare_exit_to_usermode(regs);
		return 0;	/* Keep it simple: use IRET. */
	}

	/* Now this is just like a normal syscall. */
	do_syscall_32_irqs_on(regs);

#ifdef CONFIG_X86_64
	/*
	 * Opportunistic SYSRETL: if possible, try to return using SYSRETL.
	 * SYSRETL is available on all 64-bit CPUs, so we don't need to
	 * bother with SYSEXIT.
	 *
	 * Unlike 64-bit opportunistic SYSRET, we can't check that CX == IP,
	 * because the ECX fixup above will ensure that this is essentially
	 * never the case.
	 */
	return regs->cs == __USER32_CS && regs->ss == __USER_DS &&
		regs->ip == landing_pad &&
		(regs->flags & (X86_EFLAGS_RF | X86_EFLAGS_TF)) == 0;
#else
	/*
	 * Opportunistic SYSEXIT: if possible, try to return using SYSEXIT.
	 *
	 * Unlike 64-bit opportunistic SYSRET, we can't check that CX == IP,
	 * because the ECX fixup above will ensure that this is essentially
	 * never the case.
	 *
	 * We don't allow syscalls at all from VM86 mode, but we still
	 * need to check VM, because we might be returning from sys_vm86.
	 */
	return static_cpu_has(X86_FEATURE_SEP) &&
		regs->cs == __USER_CS && regs->ss == __USER_DS &&
		regs->ip == landing_pad &&
		(regs->flags & (X86_EFLAGS_RF | X86_EFLAGS_TF | X86_EFLAGS_VM)) == 0;
#endif
}
#endif
