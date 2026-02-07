#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->trapframe = (struct trapframe *)trapframe[p - pool];
	}
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = IDLE_PID;
	current_proc = &idle;
	init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

// 需要做的事情
// 1. 弹出队列前，需要得到pool中哪个进程的stride最小
// 可以在队列里面去比较stride
struct proc *fetch_task()
{
	int index = pop_queue(&task_queue);
	if (index < 0) {
		debugf("No task to fetch\n");
		return NULL;
	}
	debugf("fetch task %d(pid=%d) to task queue\n", index, pool[index].pid);
	return pool + index;
}

// 需要做的事情是
// 1. 放进队列前，增加当前进程的stride
void add_task(struct proc *p)
{
	uint64 pass = 65536 / p->prio.priority;
	p->prio.stride += pass;
	push_queue(&task_queue, p - pool, p->prio.stride);
	debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;
found:
	// init proc
	p->pid = allocpid();
	p->state = USED;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);
	p->program_brk = 0;
	p->heap_bottom = 0;
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;

	// gemini需要的（解开
	p->vma_head = NULL;
	
	// gemini需要的（冻上
	// p->vma_head = (struct vma*)kalloc();
	// memset((char*)p->vma_head, 0, PGSIZE);
	// p->vma_head->start_addr = 0;
	// p->vma_head->end_addr   = 0;
	// p->vma_head->length     = 0;
	// p->vma_head->page_num   = 0;
	// p->vma_head->front_vma  = NULL;
	// p->vma_head->next_vma   = NULL;

	p->prio.stride   = 0;
	p->prio.priority = 16;
	return p;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler()
{
	struct proc *p;
	for (;;) {
		/*int has_proc = 0;
		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state == RUNNABLE) {
				has_proc = 1;
				tracef("swtich to proc %d", p - pool);
				p->state = RUNNING;
				current_proc = p;
				swtch(&idle.context, &p->context);
			}
		}
		if(has_proc == 0) {
			panic("all app are over!\n");
		}*/
		p = fetch_task();
		if (p == NULL) {
			panic("all app are over!\n");
		}
		tracef("swtich to proc %d", p - pool);
		p->state = RUNNING;
		current_proc = p;
		swtch(&idle.context, &p->context);
	}
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield()
{
	current_proc->state = RUNNABLE;
	add_task(current_proc);
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, struct vma* curr_vma)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, curr_vma);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->vma_head);
	p->pagetable = 0;
	p->state = UNUSED;
}

int fork()
{
    struct proc *np;
    struct proc *p = curr_proc();

    // 1. 分配进程块
    if ((np = allocproc()) == 0) {
        return -1; // 不要 panic，返回错误码
    }

    // 2. 复制 VMA 链表 (使用尾插法保持顺序)
    struct vma *curr = p->vma_head;
    struct vma *prev_new = NULL;

    // 假设 allocproc 把 np->vma_head 初始化为 NULL
    np->vma_head = NULL; 

    while(curr != NULL){
        // A. 物理内存/页表复制
        // 注意：这里必须用 start_addr 和 length！
        // 且假设 uvmcopy 能处理 VMA 范围
        if (uvmcopy(p->pagetable, np->pagetable, curr->start_addr, curr->length) < 0) {
            // 错误处理：应该释放 np 及其已分配的资源，这里简化处理
            panic("uvmcopy fail"); 
        }

        // B. VMA 结构体复制
        struct vma *new_vma = (struct vma*)kalloc();
        if(new_vma == NULL) panic("kalloc fail");
        
        // 复制属性
        *new_vma = *curr; // 结构体直接赋值，复制 start, end, length, perm 等
        new_vma->next_vma = NULL;
        new_vma->front_vma = prev_new;

        // C. 链接到子进程链表
        if (prev_new == NULL) {
            np->vma_head = new_vma; //这是第一个节点
        } else {
            prev_new->next_vma = new_vma;
        }

        prev_new = new_vma; // 步进
        curr = curr->next_vma;
    }

    // 3. 复制其他属性
    np->max_page = p->max_page;
    *(np->trapframe) = *(p->trapframe);
    np->trapframe->a0 = 0; // 子进程返回 0
    np->parent = p;
    np->state = RUNNABLE;
    
    add_task(np);
    return np->pid;
}

int exec(char *name)
{
    int id = get_id_by_name(name);
    if (id < 0) return -1;

    struct proc *p = curr_proc();
    struct vma *curr = p->vma_head;
    struct vma *next;

    // 1. 彻底清理旧内存
    while(curr != NULL){
        // 保存下一个，防止断链
        next = curr->next_vma;

        // 解映射物理页 (va, npages, do_free=1)
        uvmunmap(p->pagetable, curr->start_addr, curr->page_num, 1);
        
        // 释放 VMA 结构体本身 (kalloc 出来的必须 kfree)
        kfree((void*)curr);

        curr = next;
    }

    // 2. 重置指针，状态清零
    p->vma_head = NULL; // 彻底清空，不要保留 dummy node
    p->max_page = 0;
    // p->program_brk = 0; // 如果有的话也要清零

    // 3. 加载新程序
    // loader 内部会根据 ELF 头重新 kalloc 新的 VMA 并挂载到 p->vma_head
    loader(id, p); 

    return 0; // exec 不返回（除非出错），这里实际上是返回到新程序的入口
}

// int fork()
// {
// 	struct proc *np;
// 	struct proc *p = curr_proc();
// 	// Allocate process.
// 	if ((np = allocproc()) == 0) {	// ==0的分支，就是进程池满
// 		panic("allocproc\n");
// 	}

// 	struct vma* curr_vma = p->vma_head;
// 	while(curr_vma != NULL){
// 		if (uvmcopy(p->pagetable, np->pagetable, curr_vma->start_addr, curr_vma->page_num) < 0) {	// uvmcopy返回-1，表示内存不足
// 			panic("uvmcopy\n");
// 		}
// 		if((np->vma_head->start_addr == 0) && (np->vma_head->end_addr == 0)){
// 			np->vma_head->start_addr = p->vma_head->start_addr;
// 			np->vma_head->end_addr   = p->vma_head->end_addr;
// 			np->vma_head->length     = p->vma_head->length;
// 			np->vma_head->page_num   = p->vma_head->page_num;
// 			// allocproc时，front和next都默认设为NULL，默认就好了
// 		}else {
// 			struct vma* vma_temp = (struct vma*)kalloc();
// 			if(vma_temp == NULL)
// 				panic("kalloc error\n");

// 			memset((char*)vma_temp, 0, PGSIZE);
// 			vma_temp->start_addr = curr_vma->start_addr;
// 			vma_temp->end_addr   = curr_vma->end_addr;
// 			vma_temp->length     = curr_vma->length;
// 			vma_temp->page_num   = curr_vma->page_num;
// 			vma_temp->front_vma  = NULL;
// 			vma_temp->next_vma   = np->vma_head;
// 			np->vma_head->front_vma = vma_temp;
// 			np->vma_head         = vma_temp;
// 		}
// 		curr_vma = curr_vma->next_vma;
// 	}

// 	np->max_page = p->max_page;
// 	// copy saved user registers.
// 	*(np->trapframe) = *(p->trapframe);
// 	// Cause fork to return 0 in the child.
// 	np->trapframe->a0 = 0;
// 	np->parent = p;				
// 	np->state = RUNNABLE;		
// 	add_task(np);				
// 	return np->pid;
// }

// int exec(char *name)
// {
// 	int id = get_id_by_name(name);
// 	if (id < 0)
// 		return -1;
// 	struct proc *p = curr_proc();
// 	// exec是全部清除，即如果自身拥有多端内存，也都是全部清理掉
// 	struct vma* next = p->vma_head->next_vma;
// 	while(p->vma_head){
// 		uvmunmap(p->pagetable, p->vma_head->start_addr, p->vma_head->page_num, 1);
// 		if(next == NULL){	// 保留一个vma，供重新加载load新程序
// 			break;
// 		}
// 		// 这里有问题啊，kfree跟的好像是pa
// 		// 但是我vma_head就是物理地址，vma都是kalloc得到的
// 		// 且没有去做一个映射，页表中没它，因此目前先这样粗糙处理
// 		kfree((void*)p->vma_head);
// 		p->vma_head = next;
// 		next = p->vma_head->next_vma;
// 	}
// 	//清理 vma_head指向的vma中的数据
// 	p->max_page = 0;
// 	p->vma_head->start_addr = 0;
// 	p->vma_head->end_addr   = 0;
// 	p->vma_head->front_vma  = NULL;
// 	p->vma_head->next_vma   = NULL;
// 	p->vma_head->page_num   = 0;
// 	p->vma_head->length     = 0;

// 	loader(id, p);
// 	return 0;
// }

int wait(int pid, int *code)
// pid如果小于等于0，则表示回收哪一个僵尸子进程都行
// code地址如果为NULL，则表示不需要写入退出原因，不关注退出信息
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		// Scan through table looking for exited children.
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				if (np->state == ZOMBIE) {
					// Found one.
					np->state = UNUSED;
					pid = np->pid;
					*code = np->exit_code;
					return pid;
				}
			}
		}
		if (!havekids) {
			return -1;
		}
		p->state = RUNNABLE;
		add_task(p);
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	p->exit_code = code;
	debugf("proc %d exit with %d\n", p->pid, code);
	freeproc(p);
	if (p->parent != NULL) {
		// Parent should `wait`
		p->state = ZOMBIE;
	}
	// Set the `parent` of all children to NULL
	struct proc *np;
	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p) {
			np->parent = NULL;
		}
	}
	sched();
}

// Grow or shrink user memory by n bytes.
// Return 0 on succness, -1 on failure.
int growproc(int n)
{
	uint64 program_brk;
	struct proc *p = curr_proc();
	program_brk = p->program_brk;
	int new_brk = program_brk + n - p->heap_bottom;
	if (new_brk < 0) {
		return -1;
	}
	if (n > 0) {
		if ((program_brk = uvmalloc(p->pagetable, program_brk,
					    program_brk + n, PTE_W)) == 0) {
			return -1;
		}
	} else if (n < 0) {
		program_brk =
			uvmdealloc(p->pagetable, program_brk, program_brk + n);
	}
	p->program_brk = program_brk;
	return 0;
}
