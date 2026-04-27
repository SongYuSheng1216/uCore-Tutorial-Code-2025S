#include "console.h"
#include "defs.h"
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

extern struct thread *sleep_queue_head;

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_PIPE:
		return piperead(f->pipe, va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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
	// CPU_FREQ 是每秒的时钟周期数，除以它就得到了秒数，余数部分乘以 1000000 再除以 CPU_FREQ 就得到了微秒数
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
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);	// va复制到内核空间的path
	return fileopen(path, omode);			// 打开文件，返回fd
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

// 纯裸机的非阻塞读取！
int hardware_uart_try_getc(void) {
    // 检查 LSR 的第 0 位
    if ((*UART_LSR) & 0x01) {
        // 如果是 1，说明缓冲区有字符，直接读走
        return *UART_RBR;
    } else {
        // 如果是 0，说明没按键，瞬间返回 -1，绝不阻塞！
        return -1;
    }
}

int consgetc_noblock()
{
	return hardware_uart_try_getc();
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

/*
*	OPT: (3) In the TA's reference implementation, here defines funtion
*					int deadlock_detect(const int available[LOCK_POOL_SIZE],
*						const int allocation[NTHREAD][LOCK_POOL_SIZE],
*						const int request[NTHREAD][LOCK_POOL_SIZE])
*				for both mutex and semaphore detect, you can also
*				use this idea or just ignore it.
*/

// 当多个请求到达时，它们会被放入一个等待队列，或者通过自旋锁/互斥锁保证一次只有一个线程进入银行家算法模块。
// 一次只有一个线程进入该模块，那为什么要模拟是否能满足所有线程的需求
// 因为这是模拟一个顺序执行的过程，虽然一次只有一个线程进入，模拟给这个线程资源，满足它完成任务
// 然后释放资源，模拟分配到下一个线程，看看能不能满足它的需求，直到所有线程都完成或者没有线程能满足需求了
// 就说明我现在分配给这个线程的资源不会导致死锁
// 但是我是按数组的顺序满足资源，不是第一个去满足现在送进来的线程，就有点奇怪
int deadlock_detect(const int available[LOCK_POOL_SIZE],const int allocation[NTHREAD][LOCK_POOL_SIZE],const int request[NTHREAD][LOCK_POOL_SIZE]){
	// 步骤1，设置work和finish
	int work[LOCK_POOL_SIZE];
	int finish[NTHREAD] = {0};

    for (int i = 0; i < LOCK_POOL_SIZE; i++) {
        work[i] = available[i];
    }
	// 步骤2，找到一个满足条件的线程
	while(1){
		int found = 0;
		for(int i = 0; i < NTHREAD; i++){
			if(finish[i] == 0){
				int j = 0;
				for(; j < LOCK_POOL_SIZE; j++){
					// 我要查request[i][j] <= work[j]是否对所有资源都满足
					if(request[i][j] > work[j]){	// i线程下需要的j锁，超过了可用值，查询下一个线程
						break;
					}
				}
				if(j == LOCK_POOL_SIZE){
					// 步骤3，说明线程i的请求都满足
					finish[i] = 1;	// 标记线程i完成
					found = 1;
					for(int k = 0; k < LOCK_POOL_SIZE; k++){
						work[k] += allocation[i][k];	// 释放线程i占有的资源
					}
				}
			}
		}
		if(found == 0){
			break;	// 没有找到满足条件的线程了，退出循环
		}
	}
	// 步骤4，检查是否所有线程都完成了
	for(int i = 0; i < NTHREAD; i++){
		if(finish[i] == 0){
			return 1;	// 存在线程没有完成，说明有死锁
		}
	}
	return 0;	// 所有线程都完成了，没有死锁
}

// 其实送进来的线程已经分配了资源，在mutex_lock 或 semaphore_down，调用这个deadlock_detect的上层函数就已经修改了available和allocation

int sys_mutex_create(int blocking)
{
	struct mutex *m = mutex_create(blocking);
	if (m == NULL) {
		errorf("fail to create mutex: out of resource");
		return -1;
	}
	// OPT: (4-1) You may want to maintain some variables for detect here
	int mutex_id = m - curr_proc()->mutex_pool;
	curr_proc()->available_mutex[mutex_id] = 1;	// 可用的互斥锁数量加1
	debugf("create mutex %d", mutex_id);
	return mutex_id;
}

int sys_mutex_lock(int mutex_id)
{
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	// OPT: (4-1) You may want to maintain some variables for detect
	//       or call your detect algorithm here

	if(curr_proc()->deadlock_detect_enabled == 1){
		// 调用死锁检测算法
		curr_proc()->mutex_request[curr_thread()->tid][mutex_id] = 1;

		if(deadlock_detect(curr_proc()->available_mutex, curr_proc()->mutex_allocation, curr_proc()->mutex_request) == 1){
			errorf("Deadlock detected when locking mutex %d", mutex_id);
			curr_proc()->mutex_request[curr_thread()->tid][mutex_id] = 0; // 回滚请求
            return -0xDEAD;
		}
	}
	mutex_lock(&curr_proc()->mutex_pool[mutex_id]);
	if(curr_proc()->deadlock_detect_enabled) {
        curr_proc()->mutex_request[curr_thread()->tid][mutex_id] = 0;
        curr_proc()->mutex_allocation[curr_thread()->tid][mutex_id] = 1;
        curr_proc()->available_mutex[mutex_id] = 0;
    }
	return 0;
}

int sys_mutex_unlock(int mutex_id)
{
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	// OPT: (4-1) You may want to maintain some variables for detect here
    if (curr_proc()->deadlock_detect_enabled == 1) {
        int tid = curr_thread()->tid;

        // 1. 确保该线程确实持有这个锁，防止误解锁
        if (curr_proc()->mutex_allocation[tid][mutex_id] == 0) {
            errorf("Thread %d tries to unlock mutex %d which it doesn't hold", tid, mutex_id);
            return -1;
        }

        // 2. 将分配矩阵设为 0（不再持有）
        curr_proc()->mutex_allocation[tid][mutex_id] = 0;

        // 3. 将可用矩阵设为 1（锁变回空闲状态，其他线程可以竞争了）
        curr_proc()->available_mutex[mutex_id] = 1;
        
        // 注意：Request 数组通常在 lock 成功后就已经清零了，这里不需要动
    }
	mutex_unlock(&curr_proc()->mutex_pool[mutex_id]);
	return 0;
}

int sys_semaphore_create(int res_count)
{
	struct semaphore *s = semaphore_create(res_count);
	if (s == NULL) {
		errorf("fail to create semaphore: out of resource");
		return -1;
	}
	// OPT: (4-2) You may want to maintain some variables for detect here
	int sem_id = s - curr_proc()->semaphore_pool;
	curr_proc()->available_semaphore[sem_id] = res_count;	// 可用的信号量数量加1
	debugf("create semaphore %d", sem_id);
	return sem_id;
}

int sys_semaphore_up(int semaphore_id)// V操作，相当于锁的释放
{
	if (semaphore_id < 0 ||
	    semaphore_id >= curr_proc()->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}
	// OPT: (4-2) You may want to maintain some variables for detect here
	if(curr_proc()->deadlock_detect_enabled == 1) {
		int tid = curr_thread()->tid;

		// 1. 确保该线程确实持有这个信号量，防止误操作
		if (curr_proc()->semaphore_allocation[tid][semaphore_id] == 0) {
			errorf("Thread %d tries to up semaphore %d which it doesn't hold", tid, semaphore_id);
			return -1;
		}
		// 2. 将分配矩阵设为 0（不再持有）
		curr_proc()->semaphore_allocation[tid][semaphore_id]--;
		// 3. 将可用矩阵设为 1（信号量变回可用状态，其他线程可以竞争了）
		curr_proc()->available_semaphore[semaphore_id]++;
	}
	semaphore_up(&curr_proc()->semaphore_pool[semaphore_id]);
	return 0;
}

int sys_semaphore_down(int semaphore_id)
{
	if (semaphore_id < 0 ||
	    semaphore_id >= curr_proc()->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}
	// OPT: (4-2) You may want to maintain some variables for detect
	//       or call your detect algorithm here
	if(curr_proc()->deadlock_detect_enabled == 1){
		// 调用死锁检测算法
		curr_proc()->semaphore_request[curr_thread()->tid][semaphore_id] = 1;

		if(deadlock_detect(curr_proc()->available_semaphore, curr_proc()->semaphore_allocation, curr_proc()->semaphore_request) == 1){
			errorf("Deadlock detected when downing semaphore %d", semaphore_id);
			curr_proc()->semaphore_request[curr_thread()->tid][semaphore_id] = 0; // 回滚请求
			return -0xDEAD;
		}
	}
	semaphore_down(&curr_proc()->semaphore_pool[semaphore_id]);
	if(curr_proc()->deadlock_detect_enabled) {
		int tid = curr_thread()->tid;

		// 1. 将请求矩阵设为 0（请求已经满足了）
		curr_proc()->semaphore_request[tid][semaphore_id] = 0;

		// 2. 将分配矩阵设为 1（现在持有这个资源了）
		curr_proc()->semaphore_allocation[tid][semaphore_id]++;

		// 3. 将可用矩阵设为 0（资源被占用了，其他线程不能竞争了）
		curr_proc()->available_semaphore[semaphore_id]--;
	}
	return 0;
}

int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}

int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}

int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	cond_wait(&curr_proc()->condvar_pool[cond_id],
		  &curr_proc()->mutex_pool[mutex_id]);
	return 0;
}

// OPT: (2) you may need to define function enable_deadlock_detect here
int sys_enable_deadlock_detect(int is_enable)
{
	if(is_enable == 0){
		return 0;
	}
	if((is_enable != 0) && (is_enable != 1)){
		errorf("Unexpected argument for enable_deadlock_detect: %d", is_enable);
		return -1;
	}
	// 还有两种情况
	// 1. 死锁检测开启失败
	// 2. 死锁检测开启成功，实现功能
	// 功能，我目前的想法是：每次sys_mutex_lock和sys_semaphore_down的时候都调用死锁检测算法
	// 如果检测到死锁了就打印死锁信息并且杀死相关线程
	curr_proc()->deadlock_detect_enabled = 1;
	return 0;
}

int sys_sleep(int ms) {
    struct thread *t = curr_thread();
    
    // 1秒=12.5M次，1秒等于1000ms，1毫秒走过的时钟周期就是 CPU_FREQ/1000
    uint64 cycles_per_ms = CPU_FREQ / 1000; 
    t->time_sleep = get_cycle() + (ms * cycles_per_ms); 
    t->state = SLEEPING;
    
    // === 双向链表头插法 (必须处理好 prev 和 next) ===
    t->next_sleep = sleep_queue_head;
    t->prev_sleep = NULL; // 新节点作为头部，prev 必须是 NULL
    
    if (sleep_queue_head != NULL) {
        // 如果链表本来不为空，要把原来的头节点的 prev 指向新节点
        sleep_queue_head->prev_sleep = t;
    }
    
    // 更新全局链表头
    sleep_queue_head = t;

    // 主动让出 CPU
    sched();
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	// case SYS_nanosleep:
	// 	ret = sys_nanosleep(args[0]);
	// 	break;
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
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
		break;
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	// OPT: (2) you may need to add case SYS_enable_deadlock_detect here
	case SYS_enable_deadlock_detect:
		ret = sys_enable_deadlock_detect(args[0]);
		break;
	case SYS_getchar_noblock:
		ret = consgetc_noblock();
		break;
	case SYS_sleep:
		ret = sys_sleep(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}
