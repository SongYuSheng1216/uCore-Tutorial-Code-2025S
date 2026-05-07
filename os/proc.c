#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
struct thread *sleep_queue_head = NULL;
__attribute__((aligned(16))) char kstack[NPROC][NTHREAD][KSTACK_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][NTHREAD][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct thread *current_thread;
struct thread idle;
struct queue_prio task_queue;

int procid()
{
	return curr_proc()->pid;
}

int threadid()
{
	return curr_thread()->tid;
}

int cpuid()
{
	return 0;
}

struct proc *curr_proc()
{
	return current_thread->process;
}

struct thread *curr_thread()
{
	return current_thread;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = P_UNUSED;
		for (int tid = 0; tid < NTHREAD; ++tid) {
			struct thread *t = &p->threads[tid];
			t->state = T_UNUSED;
		}
	}
	idle.kstack = (uint64)boot_stack_top;
	current_thread = &idle;
	// for procid() and threadid()
	idle.process = pool;
	idle.tid = -1;
	init_queue_prio(&task_queue, QUEUE_SIZE, process_queue_data);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

int alloctid(const struct proc *process)
{
	for (int i = 0; i < NTHREAD; ++i) {
		if (process->threads[i].state == T_UNUSED)
			return i;
	}
	return -1;
}

// 解开task_id，参考task_to_id的计算方法，反过来算出pool_id和tid
struct thread *id_to_task(int index)
{
	if (index < 0) {
		return NULL;
	}
	int pool_id = index / NTHREAD;
	int tid = index % NTHREAD;
	struct thread *t = &pool[pool_id].threads[tid];
	return t;
}

// 计算出当前进程是第几个进程的
// 然后乘以一个进程拥有的线程数，再加上自身线程在本进程的tid
// 获得task_id
int task_to_id(struct thread *t)
{
	int pool_id = t->process - pool;
	int task_id = pool_id * NTHREAD + t->tid;
	return task_id;
}

struct thread *fetch_task()
{
	int index = pop_queue_prio(&task_queue);
	struct thread *t = id_to_task(index);
	if (t == NULL) {
		panic();
		return t;
	}
	return t;
}

// 需要做的事情是
// 1. 放进队列前，增加当前进程的stride
void add_task(struct thread *t)
{
	int task_id = task_to_id(t);
	// int pid = t->process->pid;
	//push_queue(&task_queue, task_id);

	uint64 pass = 65536 / t->prio.priority;
	t->prio.stride += pass;
	// if(pass != 4096)
	// 	printf("pass : %d\n", pass);
	push_queue_prio(&task_queue, task_id, t->prio.stride);	// 所有和task_queue相关的都要检查
}

struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == P_UNUSED) {
			goto found;
		}
	}
	return 0;

found:
	// init proc
	p->pid = allocpid();
	p->state = P_USED;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate();
	memset((void *)p->files, 0, sizeof(struct file *) * FD_BUFFER_SIZE);
	p->next_mutex_id = 0;
	p->next_sema_id = 0;
	p->next_cond_id = 0;
	// OPT: (1) you may initialize your new proc variables here
	for(int i = 0; i < LOCK_POOL_SIZE; i++){
		p->available_mutex[i] = 0;
		p->available_sema[i] = 0;
	}
	for(int i = 0; i < NTHREAD; i++){
		for(int j = 0; j < LOCK_POOL_SIZE; j++){
			p->mutex_allocation[i][j] = 0;
			p->mutex_request[i][j] = 0;
			p->sema_allocation[i][j] = 0;
			p->sema_request[i][j] = 0;
		}
	}
	p->deadlock_detect_enabled = 0;
	return p;
}

inline uint64 get_thread_trapframe_va(int tid)
{
	return TRAPFRAME - tid * TRAP_PAGE_SIZE;
}

inline uint64 get_thread_ustack_base_va(struct thread *t)
{
	return t->process->ustack_base + t->tid * USTACK_SIZE;
}

int allocthread(struct proc *p, uint64 entry, int alloc_user_res)
{
	int tid;
	struct thread *t;
	for (tid = 0; tid < NTHREAD; ++tid) {
		t = &p->threads[tid];
		if (t->state == T_UNUSED) {
			goto found;
		}
	}
	return -1;

found:
	t->tid = tid;
	t->state = T_USED;
	t->process = p;
	t->exit_code = 0;
	// kernel stack
	t->kstack = (uint64)kstack[p - pool][tid];
	// don't clear kstack now for exec()
	// memset((void *)t->kstack, 0, KSTACK_SIZE);
	// user stack
	t->ustack = get_thread_ustack_base_va(t);
	if (alloc_user_res != 0) {
		if (uvmmap(p->pagetable, t->ustack, USTACK_SIZE / PAGE_SIZE,
			   PTE_U | PTE_R | PTE_W) < 0) {
			panic("map ustack fail");
		}
		p->max_page =
			MAX(p->max_page,
			    PGROUNDUP(t->ustack + USTACK_SIZE - 1) / PAGE_SIZE);
	}
	// trap frame
	t->trapframe = (struct trapframe *)trapframe[p - pool][tid];
	memset((void *)t->trapframe, 0, TRAP_PAGE_SIZE);
	if (mappages(p->pagetable, get_thread_trapframe_va(tid), TRAP_PAGE_SIZE,
		     (uint64)t->trapframe, PTE_R | PTE_W) < 0) {
		panic("map trapframe fail");
	}
	t->trapframe->sp = t->ustack + USTACK_SIZE;
	t->trapframe->epc = entry;
	//task context
	memset(&t->context, 0, sizeof(t->context));
	t->context.ra = (uint64)usertrapret;
	t->context.sp = t->kstack + KSTACK_SIZE;

	t->prio.priority = 16;	// 默认优先级为1
	t->prio.base_priority = 16;	// base_priority初始值和priority一样
	t->prio.stride = get_queue_min_stride(&task_queue);	// 初始pass值为最小stride值
	t->prio.saved_stride = 0;	// 初始saved_stride值为0

	t->time_sleep = 0;	// 初始不睡眠
	t->next_sleep = NULL;
	t->prev_sleep = NULL;
	// we do not add thread to scheduler immediately
	debugf("allocthread p: %d, o: %d, t: %d, e: %p, sp: %p, spp: %p",
	       p->pid, (p - pool), t->tid, entry, t->ustack,
	       useraddr(p->pagetable, t->ustack));
	return tid;
}

int init_stdio(struct proc *p)
{
	for (int i = 0; i < 3; i++) {
		if (p->files[i] != NULL) {
			return -1;
		}
		p->files[i] = stdio_init(i);
	}
	return 0;
}

// 从不返回的调度函数。它循环执行操作：
//  1.选择一个要运行的进程。
//  2.切换到该进程开始运行。
//  3.最终该进程通过切换回调度器来转移控制权。
void scheduler()
{
	struct thread *t;
	for (;;) {
		t = fetch_task();
		if (t == NULL) {
			panic("");
		}
		// throw out freed threads
		if (t->state != RUNNABLE) {
			warnf("not RUNNABLE", t->process->pid, t->tid);
			continue;
		}
		tracef("swtich to proc %d, thread %d", t->process->pid, t->tid);
		t->state = RUNNING;
		current_thread = t;
		swtch(&idle.context, &t->context);
	}
}

void sched()
{
	struct thread *t = curr_thread();
	if (t->state == RUNNING)
		panic("sched running");
	swtch(&t->context, &idle.context);
}

void yield()
{
	current_thread->state = RUNNABLE;
	add_task(current_thread);
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmfree(pagetable, max_page);
}

void freethread(struct thread *t)
{
	pagetable_t pt = t->process->pagetable;
	// fill with junk
	memset((void *)t->trapframe, 6, TRAP_PAGE_SIZE);
	memset(&t->context, 6, sizeof(t->context));
	uvmunmap(pt, get_thread_trapframe_va(t->tid), 1, 0);
	uvmunmap(pt, get_thread_ustack_base_va(t), USTACK_SIZE / PAGE_SIZE, 1);
}

void freeproc(struct proc *p)
{
	for (int tid = 0; tid < NTHREAD; ++tid) {
		struct thread *t = &p->threads[tid];
		if (t->state != T_UNUSED && t->state != EXITED) {
			freethread(t);
		}
		t->state = T_UNUSED;
	}
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);
	p->pagetable = 0;
	p->max_page = 0;
	p->ustack_base = 0;
	for (int i = 0; i > FD_BUFFER_SIZE; i++) {
		if (p->files[i] != NULL) {
			fileclose(p->files[i]);
		}
	}
	p->state = P_UNUSED;
}

int fork()
{
	struct proc *np;
	struct proc *p = curr_proc();
	int i;
	// 分配一个新的进程
	if ((np = allocproc()) == 0) {
		panic("allocproc\n");
	}
	// 拷贝父进程的用户内存到新进程
	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		panic("uvmcopy\n");
	}
	np->max_page = p->max_page;
	np->ustack_base = p->ustack_base;
	// 拷贝父进程的文件描述符表
	for (i = 0; i < FD_BUFFER_SIZE; i++) {
		if (p->files[i] != NULL) {
			// TODO: f->type == STDIO ?
			p->files[i]->ref++;
			np->files[i] = p->files[i];
		}
	}

	np->parent = p;
	// 为新进程的主线程分配资源，由于拷贝了陷阱帧所有信息
	// 因此epc也是和原来的线程一样，都会回到fork下一行代码
	struct thread *nt = &np->threads[allocthread(np, 0, 0)],
		      *t = &p->threads[0];
	// copy saved user registers.
	*(nt->trapframe) = *(t->trapframe);
	// 子线程返回0的约定
	nt->trapframe->a0 = 0;
	// 加入调度队列
	nt->state = RUNNABLE;
	add_task(nt);
	return np->pid;
}
// 为新程序在主线程的用户栈上布置好 argc 和 argv，并设置好 sp 和 a1 寄存器
// 使新程序启动时能通过 main(argc, argv) 获得参数。
int push_argv(struct proc *p, char **argv)
{
	uint64 argc, ustack[MAX_ARG_NUM + 1];
	// only push to main thread
	struct thread *t = &p->threads[0];
	uint64 sp = t->ustack + USTACK_SIZE, spb = t->ustack;
	debugf("[push] sp: %p, spb: %p", sp, spb);
	// Push argument strings, prepare rest of stack in ustack.
	for (argc = 0; argv[argc]; argc++) {
		if (argc >= MAX_ARG_NUM)
			panic("too many args!");
		sp -= strlen(argv[argc]) + 1;
		sp -= sp % 16; // riscv sp must be 16-byte aligned
		if (sp < spb) {
			panic("uset stack overflow!");
		}
		if (copyout(p->pagetable, sp, argv[argc],
			    strlen(argv[argc]) + 1) < 0) {
			panic("copy argv failed!");
		}
		ustack[argc] = sp;
	}
	ustack[argc] = 0;
	// push the array of argv[] pointers.
	sp -= (argc + 1) * sizeof(uint64);
	sp -= sp % 16;
	if (sp < spb) {
		panic("uset stack overflow!");
	}
	if (copyout(p->pagetable, sp, (char *)ustack,
		    (argc + 1) * sizeof(uint64)) < 0) {
		panic("copy argc failed!");
	}
	t->trapframe->a1 = sp;
	t->trapframe->sp = sp;
	// clear files ?
	return argc; // this ends up in a0, the first argument to main(argc, argv)
}

int exec(char *path, char **argv)
{
	infof("exec : %s\n", path);
	struct inode *ip;
	struct proc *p = curr_proc();
	// 通过路径找到文件的 inode 
	if ((ip = namei(path)) == 0) {
		errorf("invalid file name %s\n", path);
		return -1;
	}
	// free current main thread's ustack and trapframe
	// 释放当前主线程的用户栈和 trapframe
	struct thread *t = curr_thread();
	freethread(t);
	t->state = T_UNUSED;
	// 解除整个用户页表的所有映射（释放所有用户内存）
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	// 加载可执行文件
	bin_loader(ip, p);
	// 释放 inode 引用
	iput(ip);
	t->state = RUNNING;
	// 布置用户栈上的 argc/argv，返回 argc（最终会写入 a0）
	return push_argv(p, argv);
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();

	for (;;) {
		havekids = 0;
		// 在当前进程的线程池中寻找子进程（parent == p）并且 pid 匹配的进程
		// pid小于等于0，表示随便找一个子进程回收
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != P_UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				if (np->state == ZOMBIE) {	// 如果找到了一个僵尸子进程，就回收它
					// Found one.
					np->state = P_UNUSED;
					pid = np->pid;
					*code = np->exit_code;	// 返回子线程退出码
					memset((void *)np->threads[0].kstack, 9,	// 回收进程的主线程的内核栈
					       KSTACK_SIZE);
					return pid;
				}
			}
		}
		if (!havekids) {	// 压根没有子进程，报错
			return -1;
		}
		// 如果有子进程但是没有僵尸子进程，说明子进程还在运行，当前线程需要等待
		t->state = RUNNABLE;
		add_task(t);	// 切走
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();
	t->exit_code = code;
	t->state = EXITED;
	int tid = t->tid;
	debugf("thread exit with %d", code);
	freethread(t);
	// tid == 0 意味着这个线程是主线程，退出时需要释放整个进程资源
	if (tid == 0) {
		p->exit_code = code;
		freeproc(p);
		debugf("proc exit");
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
	}
	sched();
}

int fdalloc(struct file *f)
{
	debugf("debugf f = %p, type = %d", f, f->type);
	struct proc *p = curr_proc();
	for (int i = 0; i < FD_BUFFER_SIZE; ++i) {
		if (p->files[i] == NULL) {
			p->files[i] = f;
			debugf("debugf fd = %d, f = %p", i, p->files[i]);
			return i;
		}
	}
	return -1;
}