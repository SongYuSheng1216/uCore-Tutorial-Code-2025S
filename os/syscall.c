#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
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

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	//panic("gettimeofday error!\n");
	// YOUR CODE
	uint64 time_data = get_cycle();
	uint64 sec = time_data / CPU_FREQ;
	uint64 usec = (time_data % CPU_FREQ) * 1000000 / CPU_FREQ;
	uint64 base_address = (uint64)val;
	char *src = (char*)&sec;
	copyout(curr_proc()->pagetable, base_address, src, 8);
	
	src = (char*)&usec;
	copyout(curr_proc()->pagetable, base_address + sizeof(uint64), src, 8);
	/* The code in `ch3` will leads to memory bugs*/

	// uint64 cycle = get_cycle();
	// val->sec = cycle / CPU_FREQ;
	// val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
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



// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
int mmap(void* start, unsigned long long len, int prot, int flags){
	len = PGROUNDUP(len);

	if(len == 0){
		return 0;
	}

	if(prot & 0xFFFFFFF8){
		return -1;
	}

	if((prot & 0x7) == 0){
		return -1;
	}

	// 判断【start，start+len】是否存在已经被映射的页
	// 可以使用start去搜索它对应的pa，然后不断+PAGE_SIZE
	// 如果返回物理地址则说明，存在
	uint64 start_addr = (uint64)start;
	uint64 end_addr = start_addr + len;
	struct proc* current_proc = curr_proc();

    if (start_addr % PAGE_SIZE != 0) {
        //panic("error!\n");
		return -1; // 地址必须对齐
    }

	while(start_addr < end_addr){
		if(useraddr(current_proc->pagetable, start_addr) != 0){
			return -1;
		}
		start_addr += PAGE_SIZE;
	}

	start_addr = (uint64)start;
	int flag_all = ((prot & 0x7) << 1) | PTE_U;
	while(start_addr < end_addr){
		void* mem_alloc = kalloc();
		if(mem_alloc == 0){
			//uvmdealloc(current_proc->pagetable, start_addr, (uint64)start);
			return -1;
		}
		memset(mem_alloc, 0, PAGE_SIZE);
		if(mappages(current_proc->pagetable, start_addr, PAGE_SIZE, (uint64)mem_alloc, flag_all) != 0){
			kfree(mem_alloc);
			//uvmdealloc(current_proc->pagetable, start_addr, (uint64)start);
			return -1;
		}
		start_addr += PAGE_SIZE;
	}

	return 0;
}

int munmap(void* start, unsigned long long len){
	uint64 curr = (uint64)start;
	if(!(PGALIGNED(curr))){
		return -1;
	}
	while(curr < (uint64)start + len){
		if(useraddr(curr_proc()->pagetable, curr) == 0){
			return -1;
		}
		curr += PAGE_SIZE;
	}
	if(uvmdealloc(curr_proc()->pagetable, (uint64)start + len, (uint64)start) != (uint64)start){
		return -1;
	}

	return 0;
}


/*
* LAB1: you may need to define sys_trace here
*/
int sys_trace(int trace_request, unsigned long id, uint8 data){
	switch(trace_request){
		case read_choose:
		{
			// address 对应的页表项中的权限，比对
			// walk函数获得PTE
			//printf("1111111111111\n");
			pte_t* pte = walk(curr_proc()->pagetable, id, 0);
			if(pte == 0){
				return -1;
			}
			if((*pte & PTE_V) == 0){
				return -1;
			}
			if((*pte & PTE_U) == 0){
				return -1;
			}
			if((*pte & PTE_R) == 0){
				return -1;
			}
			// 读用户地址的东西
			uint8 data1 = 0;
			char* dst = (char*)&data1;
			if(copyin(curr_proc()->pagetable, dst, id, 1) != 0){
				return -1;
			}
			return data1;
		}
		case write_choose:
		{
			printf("222222222222222\n");
			pte_t* pte = walk(curr_proc()->pagetable, id, 0);
			if(pte == 0){
				return -1;
			}
			if((*pte & PTE_V) == 0){
				return -1;
			}
			if((*pte & PTE_U) == 0){
				return -1;	
			}
			if((*pte & PTE_W) == 0){
				return -1;
			}
			// 往用户地址里写东西
			int data1 = data;
			char* src = (char*)&data1;
			if(copyout(curr_proc()->pagetable, id, src, 1) != 0){
				return -1;
			}
			return 0;
		}
		case call_choose:
		{
			printf("33333333333333333333\n");
			if(id == SYS_write){
				return curr_proc()->coun.sys_write_counter;
			}else if(id == SYS_exit){
				return curr_proc()->coun.sys_exit_counter;
			}else if(id == SYS_sched_yield){
				return curr_proc()->coun.sys_sched_yield_counter;
			}else if(id == SYS_gettimeofday){
				return curr_proc()->coun.sys_gettimeofday;
			}else if(id == SYS_sbrk){
				return curr_proc()->coun.sys_sbrk;
			}else if(id == SYS_trace){
				return curr_proc()->coun.sys_trace;
			}else if(id == SYS_mmap){
				return curr_proc()->coun.sys_mmap;
			}else if(id == SYS_munmap){
				return curr_proc()->coun.sys_munmap;
			}
		}
		default:
		{
			panic("error\n");
			return 0;
		}
	}
	return 0;
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
	/*
	* LAB1: you may need to update syscall counter here
	*/
	switch (id) {
	case SYS_write:
		curr_proc()->coun.sys_write_counter++;
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		curr_proc()->coun.sys_exit_counter++;
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		curr_proc()->coun.sys_sched_yield_counter++;
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		curr_proc()->coun.sys_gettimeofday++;
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	case SYS_sbrk:
		curr_proc()->coun.sys_sbrk++;
		ret = sys_sbrk(args[0]);
		break;
	/*
	* LAB1: you may need to add SYS_trace case here
	*/
	case SYS_trace:
		curr_proc()->coun.sys_trace++;
		//panic("sys_trace\n");
		ret = sys_trace(args[0],args[1],args[2]);
		break;
	case SYS_mmap:
		curr_proc()->coun.sys_mmap++;
		//panic("bbbbbbbb\n");
		ret = mmap((void*)args[0],args[1],args[2],args[3]);
		break;
	case SYS_munmap:
		curr_proc()->coun.sys_munmap++;
		ret = munmap((void*)args[0],args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
