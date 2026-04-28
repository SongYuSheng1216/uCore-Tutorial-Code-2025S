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

void set_usertrap()
{
	w_stvec(((uint64)TRAMPOLINE + (uservec - trampoline)) & ~0x3); // DIRECT
}

void set_kerneltrap()
{
	w_stvec((uint64)kernelvec & ~0x3); // DIRECT
}

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
		case LoadPageFault:
			errorf("%d in application, bad addr = %p, bad instruction = %p, "
			       "core dumped.",
			       cause, r_stval(), trapframe->epc);
			exit(-2);
			break;
		// case IllegalInstruction:
		// 	errorf("IllegalInstruction in application, core dumped.");
		// 	exit(-3);
		// 	break;
		default:
			unknown_trap();
			break;
		}
	}
	usertrapret();
}


// 返回 user space
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
	uint64 x = r_sstatus();
	x &= ~SSTATUS_SPP; // 清除spp，表示返回用户模式
	x |= SSTATUS_SPIE; // 允许用户模式中断
	w_sstatus(x);

	uint64 satp = MAKE_SATP(curr_proc()->pagetable);
	uint64 fn = TRAMPOLINE + (userret - trampoline);
	uint64 trapframe_va = get_thread_trapframe_va(curr_thread()->tid);
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
		// 不应该进入这个分支，目前内核只开启了外部中断和时钟中断
		exit(-1);
	}

	w_sepc(sepc);
	w_sstatus(sstatus);
}
