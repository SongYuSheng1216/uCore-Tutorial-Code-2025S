#include "trap.h"
#include "defs.h"
#include "loader.h"
#include "plic.h"
#include "syscall.h"
#include "timer.h"
#include "virtio.h"
#include "proc.h"

extern char trampoline[], uservec[];
extern char userret[], kernelvec[];
extern struct thread *sleep_queue_head;
extern struct queue_prio task_queue;
void kerneltrap();

// set up to take exceptions and traps while in the kernel.
void set_usertrap()
{
	w_stvec(((uint64)TRAMPOLINE + (uservec - trampoline)) & ~0x3); // DIRECT
}

void set_kerneltrap()
{
	w_stvec((uint64)kernelvec & ~0x3); // DIRECT
}

// set up to take exceptions and traps while in the kernel.
void trap_init()
{
	// intr_on();
	set_kerneltrap();
	w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
}

void unknown_trap()
{
	errorf("unknown trap: %p, stval = %p", r_scause(), r_stval());
	exit(-1);
}

void devintr(uint64 cause)
{
	int irq;
	switch (cause) {
	case SupervisorTimer:
        set_next_timer();
        // if from user, allow yield
        if ((r_sstatus() & SSTATUS_SPP) == 0) {
            
            // struct thread *t = sleep_queue_head;
            // uint64 current_cycle = get_cycle();

            // while (t != NULL) {
            //     // 提前保存下一个节点，因为 t 如果被删除，t->next_sleep 可能会发生改变
            //     struct thread *next_t = t->next_sleep;

            //     if (t->state == SLEEPING && current_cycle >= t->time_sleep) {
                    
            //         // 1. Stride 补偿逻辑
            //         uint64 min_stride = get_queue_min_stride(&task_queue);
            //         if (t->prio.stride < min_stride) {
            //             t->prio.stride = min_stride;
            //         }
                    
            //         // 2. 改变状态并加入就绪队列
            //         t->state = RUNNABLE;
            //         add_task(t);

            //         // 3. === 双向链表安全删除逻辑 ===
            //         if (t->prev_sleep != NULL) {
            //             // t 不是头节点，让前一个节点跨过 t 指向下一个
            //             t->prev_sleep->next_sleep = t->next_sleep;
            //         } else {
            //             // t 是头节点，删除后，新的头节点变成下一个
            //             sleep_queue_head = t->next_sleep;
            //         }

            //         if (t->next_sleep != NULL) {
            //             // t 不是尾节点，让后一个节点跨过 t 指向前一个
            //             t->next_sleep->prev_sleep = t->prev_sleep;
            //         }
                    
            //         // 养成好习惯，清理摘下节点的指针（防止成为野指针）
            //         t->next_sleep = NULL;
            //         t->prev_sleep = NULL;
            //         // ===============================================
            //     }
                
            //     // 移动到刚才保存的下一个节点继续遍历
            //     t = next_t;
            // }
            
            // // 遍历结束，触发当前任务的时间片检查与调度
            yield();
        }
        break;
	case SupervisorExternal:
		irq = plic_claim();
		if (irq == UART0_IRQ) {
			// do nothing
		} else if (irq == VIRTIO0_IRQ) {
			virtio_disk_intr();
		} else if (irq) {
			infof("unexpected interrupt irq=%d\n", irq);
		}
		if (irq)
			plic_complete(irq);
		break;
	default:
		unknown_trap();
		break;
	}
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void usertrap()
{
	set_kerneltrap();
	struct trapframe *trapframe = curr_thread()->trapframe;
	tracef("trap from user epc = %p", trapframe->epc);
	if ((r_sstatus() & SSTATUS_SPP) != 0)
		panic("usertrap: not from user mode");

	uint64 cause = r_scause();
	if (cause & (1ULL << 63)) {
		devintr(cause & 0xff);
	} else {
		switch (cause) {
		case UserEnvCall:
			trapframe->epc += 4;
			syscall();
			break;
		case StoreMisaligned:
		case StorePageFault:
		case InstructionMisaligned:
		case InstructionPageFault:
		case LoadMisaligned:
		case LoadPageFault:
			errorf("%d in application, bad addr = %p, bad instruction = %p, "
			       "core dumped.",
			       cause, r_stval(), trapframe->epc);
			exit(-2);
			break;
		case IllegalInstruction:
			errorf("IllegalInstruction in application, core dumped.");
			exit(-3);
			break;
		default:
			unknown_trap();
			break;
		}
	}
	usertrapret();
}

//
// return to user space
//
void usertrapret()
{
	set_usertrap();
	struct trapframe *trapframe = curr_thread()->trapframe;
	trapframe->kernel_satp = r_satp(); // kernel page table
	trapframe->kernel_sp =
		curr_thread()->kstack + KSTACK_SIZE; // process's kernel stack
	trapframe->kernel_trap = (uint64)usertrap;
	trapframe->kernel_hartid = r_tp(); // unuesd

	w_sepc(trapframe->epc);
	// set up the registers that trampoline.S's sret will use
	// to get to user space.

	// set S Previous Privilege mode to User.
	uint64 x = r_sstatus();
	x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
	x |= SSTATUS_SPIE; // enable interrupts in user mode
	w_sstatus(x);

	// tell trampoline.S the user page table to switch to.
	uint64 satp = MAKE_SATP(curr_proc()->pagetable);
	uint64 fn = TRAMPOLINE + (userret - trampoline);
	uint64 trapframe_va = get_thread_trapframe_va(curr_thread()->tid);
	debugf("return to user @ %p, sp @ %p", trapframe->epc, trapframe->sp);
	((void (*)(uint64, uint64))fn)(trapframe_va, satp);
}

void kerneltrap()
{
	uint64 sepc = r_sepc();
	uint64 sstatus = r_sstatus();
	uint64 scause = r_scause();

	debugf("kernel trap: epc = %p, cause = %d", sepc, scause);

	if ((sstatus & SSTATUS_SPP) == 0)
		panic("kerneltrap: not from supervisor mode");

	if (scause & (1ULL << 63)) {
		devintr(scause & 0xff);
	} else {
		errorf("invalid trap from kernel: %p, stval = %p sepc = %p\n",
		       scause, r_stval(), sepc);
		exit(-1);
	}
	// the yield() may have caused some traps to occur,
	// so restore trap registers for use by kernelvec.S's sepc instruction.
	w_sepc(sepc);
	w_sstatus(sstatus);
}
