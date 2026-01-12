#include "loader.h"
#include "defs.h"
#include "trap.h"

static int app_num;
static uint64 *app_info_ptr;
extern char _app_num[];

// Count finished programs. If all apps exited, shutdown.
int finished()
{
	static int fin = 0;
	if (++fin >= app_num)
		panic("all apps over");
	return 0;
}

// Get user progs' infomation through pre-defined symbol in `link_app.S`
void loader_init()
{
	app_info_ptr = (uint64 *)_app_num;
	app_num = *app_info_ptr;
	app_info_ptr++;
}

pagetable_t bin_loader(uint64 start, uint64 end, struct proc *p)
{
	// int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
	pagetable_t pg = uvmcreate();
	if (mappages(pg, TRAPFRAME, PGSIZE, (uint64)p->trapframe,
		     PTE_R | PTE_W) < 0) {	// 将trapframe虚拟地址 映射到p进程的trapframe结构
		panic("mappages fail");		// 因此此时P进程的页表就能通过trapframe虚拟地址获得trapframe结构
	}
	if (!PGALIGNED(start)) {
		panic("user program not aligned, start = %p", start);
	}
	if (!PGALIGNED(end)) {
		// Fix in ch5
		warnf("Some kernel data maybe mapped to user, start = %p, end = %p",
		      start, end);
	}
	end = PGROUNDUP(end);
	uint64 length = end - start;
	if (mappages(pg, BASE_ADDRESS, length, start,
		     PTE_U | PTE_R | PTE_W | PTE_X) != 0) {	// 映射app的代码
		panic("mappages fail");
	}
	p->pagetable = pg;	// 设置p进程的页表为pg
	uint64 ustack_bottom_vaddr = BASE_ADDRESS + length + PAGE_SIZE;// 栈空间的最低地址（栈顶方向） 
	// 加一个page_size 可能是为了
	// 1. 保护页，防止栈开辟到代码/数据段了
	// 2. 对齐要求
	// 3. 为栈的元数据预留空间（环境变量指针，命令行参数，辅助向量）
	if (USTACK_SIZE != PAGE_SIZE) {
		// Fix in ch5
		panic("Unsupported");
	}
	mappages(pg, ustack_bottom_vaddr, USTACK_SIZE, (uint64)kalloc(),
		 PTE_U | PTE_R | PTE_W | PTE_X);	// 映射用户栈
	// 设置进程
	p->ustack = ustack_bottom_vaddr;
	p->trapframe->epc = BASE_ADDRESS;
	p->trapframe->sp = p->ustack + USTACK_SIZE;
	p->max_page = PGROUNDUP(p->ustack + USTACK_SIZE - 1) / PAGE_SIZE;
	p->program_brk = p->ustack + USTACK_SIZE;	// 不了解
    p->heap_bottom = p->ustack + USTACK_SIZE;	// 很奇怪的名字
	return pg;
}

// load all apps and init the corresponding `proc` structure.
int run_all_app()
{
	for (int i = 0; i < app_num; ++i) {
		struct proc *p = allocproc();
		tracef("load app %d", i);
		bin_loader(app_info_ptr[i], app_info_ptr[i + 1], p);
		p->state = RUNNABLE;
		/*
		* LAB1: you may need to initialize your new fields of proc here
		*/
	}
	return 0;
}
