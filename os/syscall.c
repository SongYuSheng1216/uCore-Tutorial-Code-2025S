#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
// pid，父进程指定回收僵尸子进程的序列号
// va是父进程传入的，要求把pid子进程的退出信息，写在这个虚拟地址上
// sys_wait的返回值就是回收的僵尸子进程的pid号，如果错误返回-1
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}


uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	struct proc *np;
	struct proc *p = curr_proc();
	char name[200];
	if(copyinstr(p->pagetable, name, va, 200) < 0){
		return -1;
	}
	// 判断文件名是否有效
	int id = get_id_by_name(name);
	if (id < 0)
		return -1;
	// 判断进程池是否满
	// Allocate process.
	if ((np = allocproc()) == 0) {	// ==0的分支，就是进程池满
		return -1;
	}
	// 判断内存不足,直接载入新程序的elf
	if (loader(id, np) < 0) {	// 返回-1，表示内存不足
		return -1;
	}
	// 新进程的pcb更新
	np->parent = p;
	np->state = RUNNABLE;
	add_task(np);
	return np->pid;
}

uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
	if(prio <= 1){
		return -1;
	}
	struct proc* current_proc = curr_proc();
	current_proc->prio.priority = prio;
	return prio;
}

int mmap(void* start, unsigned long long len, int prot, int flags){
	uint64 va = (uint64)start;
	struct proc* current_proc = curr_proc();
	if(len == 0){
		return 0;
	}
	if(len % PGSIZE != 0){
		len = PGROUNDUP(len);
	}
	if(va % PGSIZE != 0){
		return -1;
	}
	if(prot & (~0x7)){
		// printf("44444444444444444444444444\n");
		return -1;
	}
	if((prot & 0x7) == 0){
		// printf("55555555555555555555555555555\n");
		return -1;
	}
	int read_flag = prot & 1;
	int write_flag = prot & 2;
	int exe_flag = prot & 4;
	int u_mode_flag = 8;
	int perm = (u_mode_flag | exe_flag | write_flag | read_flag) << 1;
	uint64 end_addr = va + len;
	for(; va < end_addr; va += PGSIZE){
		if(walkaddr(current_proc->pagetable, va) != 0){	// 此时的va已经被映射
			// printf("66666666666666666666666666666\n");
			return -1;
		}
	}
	for(va = (uint64)start; va < end_addr; va += PGSIZE){
		char *mem = kalloc();	
		if(mem == NULL){	// 物理内存不足
			// printf("77777777777777777777777777777\n");
			return -1;
		}
		memset(mem, 0, PGSIZE);
		if(mappages(current_proc->pagetable, va, PGSIZE, (uint64)mem, perm) < 0){
			// printf("8888888888888888888888888\n");
			return -1;
		}
	}
	struct vma* next = (struct vma*)kalloc();
	memset((char*)next, 0, PGSIZE);
	next->start_addr = (uint64)start;
	next->end_addr   = end_addr;
	next->length     = len;
	next->page_num   = len / PGSIZE;
	next->front_vma  = NULL;
	next->next_vma   = current_proc->vma_head;
	current_proc->vma_head->front_vma = next;
	current_proc->vma_head = next;
	return 0;
}

int munmap(void* start, unsigned long long len){
	uint64 va = (uint64)start;
	uint64 end = va + len;
	// pte_t *pte;
	// uint64 pa;
	if(!(PGALIGNED(va))){
		return -1;
	}
	// printf("va = %x\n", va);
	// printf("end = %x\n", end);
	// printf("len = %x\n", len);
	for(; va < end; va += PGSIZE){
		if(walkaddr(curr_proc()->pagetable, va) == 0){	// 存在未被映射的虚存
			// printf("1111111111111111111111\n");
			return -1;
		}
	}
	// 关键在于start和len找到对应的vma，传入对应start_addr, end_addr
	// 这里的end_addr由start_addr + len得到
	// 底层要考虑vma只释放部分内存的情况
	if(uvmdealloc(curr_proc()->pagetable, end, (uint64)start) != (uint64)start){
		// printf("2222222222222222222222222\n");
		return -1;
	}
	// printf("33333333333333333333333\n");
	free_vma_range(curr_proc(), (uint64)start, end);
	return 0;
}


uint64 sys_sbrk(int n)
{
        uint64 addr;
        struct proc *p = curr_proc();
        addr = p->program_brk;
        if(growproc(n) < 0)
                return -1;
        return addr;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_sbrk:
        ret = sys_sbrk(args[0]);
        break;
	case SYS_mmap:
		ret = mmap((void*)args[0],args[1],args[2],args[3]);
		break;
	case SYS_munmap:
		ret = munmap((void*)args[0],args[1]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
