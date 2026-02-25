#include "plic.h"
#include "log.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//

void plicinit()
{
	// set desired IRQ priorities non-zero (otherwise disabled).
	int hart = cpuid();
	*(uint32 *)(PLIC + VIRTIO0_IRQ * 4) = 1;	// virtual io优先级被设置为1
	// set uart's enable bit for this hart's S-mode.
	*(uint32 *)PLIC_SENABLE(hart) = (1 << VIRTIO0_IRQ);	// 打开这个hart的s-mode的virtual io的enable
	// set this hart's S-mode priority threshold to 0.
	*(uint32 *)PLIC_SPRIORITY(hart) = 0;	// 这个hart的s-mode的优先级阈值设置为0，表示所有优先级的中断都可以被处理
}

// ask the PLIC what interrupt we should serve.
int plic_claim()
{
	int hart = cpuid();
	int irq = *(uint32 *)PLIC_SCLAIM(hart);
	return irq;
}

// tell the PLIC we've served this IRQ.
void plic_complete(int irq)
{
	int hart = cpuid();
	*(uint32 *)PLIC_SCLAIM(hart) = irq;
}
