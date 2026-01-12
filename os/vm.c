#include "vm.h"
#include "defs.h"
#include "riscv.h"

pagetable_t kernel_pagetable;

extern char e_text[]; // kernel.ld sets this to end of kernel code.
extern char trampoline[];

// Make a direct-map page table for the kernel.
// 内核页表的创建，能够映射到所有可以使用的物理内存，以获得所有地址的使用权
pagetable_t kvmmake(void)
{
	pagetable_t kpgtbl;
	kpgtbl = (pagetable_t)kalloc();	// 内核进程的L2级页表512个页表项已经被开辟了
	memset(kpgtbl, 0, PGSIZE);
	// map kernel text executable and read-only. 内核的物理地址与虚拟地址是一致的，权限为可读可执行
	// 内核代码的映射
	kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)e_text - KERNBASE,
	       PTE_R | PTE_X);
	// map kernel data and the physical RAM we'll make use of.数据（用户程序）物理地址与虚拟地址也是一致的
	// 剩下的所有存储空间
	kvmmap(kpgtbl, (uint64)e_text, (uint64)e_text, PHYSTOP - (uint64)e_text,
	       PTE_R | PTE_W);
	// 用户程序进入内核的保存代码
	kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
	return kpgtbl;
}

// Initialize the one kernel_pagetable
// Switch h/w page table register to the kernel's page table,
// and enable paging.

void kvm_init(void)
{
	kernel_pagetable = kvmmake();
	w_satp(MAKE_SATP(kernel_pagetable));
	sfence_vma();
	infof("enable pageing at %p", r_satp());
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc)//walk函数模拟了CPU进行MMU的过程
// 它的参数分别是页表，待转换的虚拟地址va，以及如果没有对应的物理地址时是否分配物理地址
{
	if (va >= MAXVA)
		panic("walk");

	for (int level = 2; level > 0; level--) {
		pte_t *pte = &pagetable[PX(level, va)];
		if (*pte & PTE_V) {	// 页表项有效
			pagetable = (pagetable_t)PTE2PA(*pte);	// 取出下一级页表的物理首地址
		} else {
			if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)	// pagetable这里只是复用变量，分配物理地址，即创建页表
				return 0;
			memset(pagetable, 0, PGSIZE);
			*pte = PA2PTE(pagetable) | PTE_V;
			// 这里创建的是页表，补全L2、L1级页表
		}
	}
	return &pagetable[PX(0, va)];	// 返回L0页表项的地址
									// 这个L0页表项指示的是一个4KB页的物理地址，没有offset
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
	pte_t *pte;
	uint64 pa;

	if (va >= MAXVA)
		return 0;

	pte = walk(pagetable, va, 0);
	if (pte == 0)
		return 0;
	if ((*pte & PTE_V) == 0)
		return 0;
	if ((*pte & PTE_U) == 0)	// 疑惑？等于0，不就意味着S-mode可以使用
		return 0;
	pa = PTE2PA(*pte);	// 获得数据所在的物理页首地址，没有考虑offset
	return pa;
}

// Look up a virtual address, return the physical address,
uint64 useraddr(pagetable_t pagetable, uint64 va)
{
	uint64 page = walkaddr(pagetable, va);
	if (page == 0)
		return 0;
	return page | (va & 0xFFFULL);	// 获得数据所在的物理页首地址，考虑到了offset
}

// Add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
	if (mappages(kpgtbl, va, sz, pa, perm) != 0)
		panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
// mappages 在 pagetable 中建立 [va, va + size) 到 [pa, pa + size) 的映射
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
	uint64 a, last;
	pte_t *pte;

	a = PGROUNDDOWN(va);
	last = PGROUNDDOWN(va + size - 1);
	for (;;) {	// 逐页进行映射
		if ((pte = walk(pagetable, a, 1)) == 0)
			return -1;
		if (*pte & PTE_V) {
			errorf("remap");
			return -1;
		}
		*pte = PA2PTE(pa) | perm | PTE_V;	
		// 将walk获取的L0 PTE中的值变成指定PA和permission的即算完成了映射
		if (a == last)
			break;
		a += PGSIZE;
		pa += PGSIZE;
	}
	return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.（User Virtual Memory Unmap
// 回收页表指向的地址空间及设置叶子页表项为0
// 把所有叶子节点的页表项都设置为0，取消了映射关系，逻辑上物理空间未使用
// 同时调用了kfree，实际上也将释放的4KB物理内存给到了free状态
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
	uint64 a;
	pte_t *pte;

	if ((va % PGSIZE) != 0)
		panic("uvmunmap: not aligned");

	for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
		if ((pte = walk(pagetable, a, 0)) == 0)	// 没有叶子节点的页表项（即这个va没有对应的L0页表项
			continue;
		if ((*pte & PTE_V) != 0) {	// 此时为有效页表
			if (PTE_FLAGS(*pte) == PTE_V)	// 不是叶子节点的页表
				panic("uvmunmap: not a leaf");
			if (do_free) {	// 是否回收页表
				uint64 pa = PTE2PA(*pte);
				kfree((void *)pa);
			}
		}
		*pte = 0;	// 叶子节点的页表项为0，取消掉映射关系
	}
}

// create an empty user page table.
// returns 0 if out of memory.
// 创建一个进程使用的页表
pagetable_t uvmcreate()
{
	pagetable_t pagetable;
	pagetable = (pagetable_t)kalloc();	// 进程页表的L2级页表512个页表项已经被开辟出来
	if (pagetable == 0) {
		errorf("uvmcreate: kalloc error");
		return 0;
	}
	memset(pagetable, 0, PGSIZE);
	// int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
	if (mappages(pagetable, TRAMPOLINE, PAGE_SIZE, (uint64)trampoline,
		     PTE_R | PTE_X) < 0) {	// 设置trampoline也能够被进程页表找到
		kfree(pagetable);
		errorf("uvmcreate: mappages error");
		return 0;
	}
	return pagetable;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// 回收掉所有的页表（L2、L1级页表回收）
void freewalk(pagetable_t pagetable)
{
	// there are 2^9 = 512 PTEs in a page table.
	for (int i = 0; i < 512; i++) {
		pte_t pte = pagetable[i];
		if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
			// this PTE points to a lower-level page table.
			uint64 child = PTE2PA(pte);
			freewalk((pagetable_t)child);
			pagetable[i] = 0;
		} else if (pte & PTE_V) {
			panic("freewalk: leaf");
		}
	}
	kfree((void *)pagetable);
}

/**
 * @brief Free user memory pages, then free page-table pages.
 *
 * @param max_page The max vaddr of user-space.
 */
void uvmfree(pagetable_t pagetable, uint64 max_page)
{
	if (max_page > 0)
		uvmunmap(pagetable, 0, max_page, 1);	// 回收页表指向的地址空间及L0页表
	freewalk(pagetable);						// 回收L2、L1页表
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
// 将src的len字节移到dstva地址处
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
	uint64 n, va0, pa0;

	while (len > 0) {
		va0 = PGROUNDDOWN(dstva);
		pa0 = walkaddr(pagetable, va0);
		if (pa0 == 0)
			return -1;
		n = PGSIZE - (dstva - va0);	// 跨页处理，把n限定在一页大小内
		if (n > len)
			n = len;
		memmove((void *)(pa0 + (dstva - va0)), src, n);

		len -= n;
		src += n;
		dstva = va0 + PGSIZE;
	}
	return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// 将srcva的len字节拷贝到dst地址处
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
	uint64 n, va0, pa0;

	while (len > 0) {
		va0 = PGROUNDDOWN(srcva);
		pa0 = walkaddr(pagetable, va0);
		if (pa0 == 0)
			return -1;
		n = PGSIZE - (srcva - va0);
		if (n > len)
			n = len;
		memmove(dst, (void *)(pa0 + (srcva - va0)), n);

		len -= n;
		dst += n;
		srcva = va0 + PGSIZE;
	}
	return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
// 将srcva的内容拷贝到dst地址处
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
	uint64 n, va0, pa0;
	int got_null = 0, len = 0;

	while (got_null == 0 && max > 0) {
		va0 = PGROUNDDOWN(srcva);
		pa0 = walkaddr(pagetable, va0);
		if (pa0 == 0)
			return -1;
		n = PGSIZE - (srcva - va0);
		if (n > max)
			n = max;

		char *p = (char *)(pa0 + (srcva - va0));
		while (n > 0) {
			if (*p == '\0') {
				*dst = '\0';
				got_null = 1;
				break;
			} else {
				*dst = *p;
			}
			--n;
			--max;
			p++;
			dst++;
			len++;
		}

		srcva = va0 + PGSIZE;
	}
	return len;
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// 某个进程的页表需要新的内存，开辟新页
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
        char *mem;
        uint64 a;

        if(newsz < oldsz)
                return oldsz;

        oldsz = PGROUNDUP(oldsz);
        for(a = oldsz; a < newsz; a += PGSIZE){
                mem = kalloc();
                if(mem == 0){
                        uvmdealloc(pagetable, a, oldsz);
                        return 0;
                }
                memset(mem, 0, PGSIZE);
                if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
                        kfree(mem);
                        uvmdealloc(pagetable, a, oldsz);
                        return 0;
                }
        }
        return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
// 某个进程的页表回收部分，释放一部分存储空间
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
        if(newsz >= oldsz)
                return oldsz;

        if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
                int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
                uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
        }

        return newsz;
}

