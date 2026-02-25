#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

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

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	return -1;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
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

int sys_fstat(int fd, uint64 stat)
{
	//TODO: your job is to complete the syscall
	if(fd < 0 || fd >= FD_BUFFER_SIZE){
		return -1;
	}
	struct file *files = curr_proc()->files[fd];
	if (files == NULL) {
		return -1;	
	}

	Stat stat_temp;
 	memset(&stat_temp, 0, sizeof(Stat));
	stat_temp.dev = files->ip->dev;
	stat_temp.ino = files->ip->inum;
	stat_temp.nlink = files->ip->link; // link count
	printf("files->ip->link = %d\n", files->ip->link);
	printf("dev = %d, ino = %d, nlink = %d\n", stat_temp.dev, stat_temp.ino, stat_temp.nlink);

    // 处理文件类型
    if (files->ip->type == T_DIR) {
        stat_temp.mode = 0x040000;
    } else if (files->ip->type == T_FILE) {
        stat_temp.mode = 0x100000;
    } else {
        return -1;
    }
    if(copyout(curr_proc()->pagetable, stat, (char *)&stat_temp, sizeof(Stat)) < 0){
        return -1;
    }
	return 0;
}


int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath,
	       uint64 flags)
{
	//TODO: your job is to complete the syscall
	// 目前没有锁机制，所以不考虑竞争问题，测试程序也不涉及这些
	// 0. 获得用户传入的路径字符串
	char path_old[DIRSIZ + 1];
	char path_new[DIRSIZ + 1];
	if(copyinstr(curr_proc()->pagetable, path_old, oldpath, DIRSIZ) < 0){
		return -1;
	}
    if(copyinstr(curr_proc()->pagetable, path_new, newpath, DIRSIZ) < 0){
        return -1;
    }
	if(strncmp(path_old, path_new, DIRSIZ) == 0){
		return -1; // 不允许链接同名文件
	}
	// 1. 找到oldpath对应的inode，然后获取它在内存中的复制体，将nlink增加
	struct inode* old_ip = namei((char *)path_old);
	if (old_ip == NULL) {
		return -1;
	}
	// 新增的
	if(old_ip->valid == 0){
		ivalid(old_ip);
	}
	if(old_ip->type != T_FILE){
		iput(old_ip);	// 因为namei到dirlookup，再到iget函数里面ip->ref++了，所以这里要iput把它减回去
		return -1;
	}
	old_ip->link++; // 增加链接数

	// 2. 然后将inode的修改写回磁盘
	iupdate(old_ip);

	// 3. 找到newpath文件所在的目录项 (既然只有一级，目标永远是根目录)
    struct inode *dp = root_dir();
    if(dp == 0){
        goto bad; // 理论上不应该发生
    }

	// 4. 在newpath所在目录项中添加一个新的目录项，文件名为newpath，inode为oldpath对应的inode
    // dirlink(目录Inode, 新文件名, 指向的Inode编号)
    // 如果重名或磁盘满，dirlink 会返回 -1
    if(dirlink(dp, path_new, old_ip->inum) < 0){
        goto bad;
    }
    iput(old_ip); // 因为namei到dirlookup，再到iget函数里面ip->ref++了，所以这里要iput把它减回去
    return 0;

bad:
    // =================================================
    // 错误回滚 (Rollback)
    // =================================================
    // 如果走到这里，说明 link 建立失败了，但是 ip->nlink 已经被加了 1
    // 必须把它减回去，否则这个文件永远删不掉 (Space Leak)
    old_ip->link--; // 把刚才加的减回去
    iupdate(old_ip);
    iput(old_ip);
    return -1;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
	//TODO: your job is to complete the syscall
	// 0. 找到name文件所在的目录（即根目录
	char path[DIRSIZ + 1];
	if(copyinstr(curr_proc()->pagetable, path, name, DIRSIZ) < 0){
		return -1;
	}
	struct inode *dp = root_dir();
	if(dp == 0){
		return -1; // 理论上不应该发生
	}
	// 1. 找到name对应的inode
	struct inode *ip = namei(path);
	if(ip == 0){
		return -1; // 文件不存在
	}
	if(ip->valid == 0){
		ivalid(ip);
	}

	// 2. 将目录文件中的目录项删除,并将目录文件更新回磁盘(writei里面更新了)
	if(dirunlink(dp, path, ip->inum) < 0){
		iput(ip);	// 这里是否需要iput？而且iput和namei中iget的对应，我有点怀疑
		return -1; // 删除失败，可能是文件不存在
	}
	// 3. 将name对应的inode的link数减1，并更新到磁盘上
	ip->link--;
	iupdate(ip);

	// 一方面是为了namei的ref加了1，这里减少
	// 另一方面是为了如果link为0，ref剩下1，那么进入iput减少以后就没有链接了，可以被删除了
    iput(ip); 
	return 0;

}

uint64 sys_sbrk(int n)
{
	uint64 addr;
	struct proc *p = curr_proc();
	addr = p->program_brk;
	if (growproc(n) < 0)
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
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
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
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
		ret = sys_fstat(args[0], args[1]);
		break;
	case SYS_linkat:
		ret = sys_linkat(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_unlinkat:
		ret = sys_unlinkat(args[0], args[1], args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_sbrk:
		ret = sys_sbrk(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
