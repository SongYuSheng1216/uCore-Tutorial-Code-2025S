#include "defs.h"
#include "proc.h"
#include "riscv.h"

int pipealloc(struct file *f0, struct file *f1)
{
	struct pipe *pi;
	pi = 0;
	if ((pi = (struct pipe *)kalloc()) == 0)	// 分配一页给到pipe
		goto bad;
	// pipe的读写端口打开
	pi->readopen = 1;
	pi->writeopen = 1;
	pi->nwrite = 0;
	pi->nread = 0;
	// 这是返回给用户的文件描述符，分别指向pipe的读端和写端
	// f0为读端，f1为写端
	// f0文件不可写，f1文件不可读
	// f0和f1都指向同一个pipe
	f0->type = FD_PIPE;
	f0->readable = 1;
	f0->writable = 0;
	f0->pipe = pi;
	f1->type = FD_PIPE;
	f1->readable = 0;
	f1->writable = 1;
	f1->pipe = pi;
	return 0;
bad:
	if (pi)
		kfree((char *)pi);
	return -1;
}

void pipeclose(struct pipe *pi, int writable)
{
	if (writable) {	// 文件不可写
		pi->writeopen = 0;	// 关闭写端口
	} else {
		pi->readopen = 0;
	}
	if (pi->readopen == 0 && pi->writeopen == 0) {
		kfree((char *)pi);	// 自动回收
	}
}

int pipewrite(struct pipe *pi, uint64 addr, int n)
{
	int w = 0;
	uint64 size;
	struct proc *p = curr_proc();
	if (n <= 0) {	// 无效写入
		panic();
	}
	while (w < n) {
		if (pi->readopen == 0) {	// 读端口已经关闭了，写进去也没法读，行为错误
			return -1;
		}
		if (pi->nwrite == pi->nread + PIPESIZE) { // 环形缓冲区被写满了
			yield();
		} else {
			// n-w：用户还需要写入的剩余字节数
			// nread + PIPESIZE - nwrite：管道当前总空闲空间
			// PIPESIZE - (nwrite % PIPESIZE)：当前写指针到缓冲区末尾的连续空间长度
			// 判断第三个是因为写道缓冲区末尾，要再写的话，就要从环形缓冲区开始写了
			// 选择他们三个最小的进行拷贝，然后循环
			size = MIN(MIN(n - w,
				       pi->nread + PIPESIZE - pi->nwrite),
				   PIPESIZE - (pi->nwrite % PIPESIZE));
			// 从用户空间复制数据到内核空间的pipe缓冲区中
			if (copyin(p->pagetable,
				   &pi->data[pi->nwrite % PIPESIZE], addr + w,
				   size) < 0) {
				panic("copyin");
			}
			pi->nwrite += size;
			w += size;
		}
	}
	return w;
}

int piperead(struct pipe *pi, uint64 addr, int n)
{
	int r = 0;
	uint64 size = -1;
	struct proc *p = curr_proc();
	if (n <= 0) {	// 无效读取
		panic("invalid read num");
	}
	while (pi->nread == pi->nwrite) {	// 没有东西可读
		if (pi->writeopen)
			yield();	// 切走
		else
			return -1;	// 写端关闭了，不可能再读到了，直接异常
	}
	while (r < n && size != 0) {
		if (pi->nread == pi->nwrite)	// 读完了
			break;
		// n-r：用户还需要读取的剩余字节数
		// nwrite - nread：管道当前总可读字节数（和第一个不相等，因为可能不需要都读出来
		// PIPESIZE - (nread % PIPESIZE)：当前读指针到缓冲区末尾的连续可读字节数
		// 判断第三个是因为读到缓冲区末尾，要再读的话，就要从环形缓冲区开始读了
		size = MIN(MIN(n - r, pi->nwrite - pi->nread),
			   PIPESIZE - (pi->nread % PIPESIZE));
		// 从pipe缓冲区中复制数据到用户空间的地址中
		if (copyout(p->pagetable, addr + r,
			    &pi->data[pi->nread % PIPESIZE], size) < 0) {
			panic("copyout");
		}
		pi->nread += size;
		r += size;
	}
	return r;
}
