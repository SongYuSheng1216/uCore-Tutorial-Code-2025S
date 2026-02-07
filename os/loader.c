#include "loader.h"
#include "defs.h"
#include "trap.h"

static int app_num;
static uint64 *app_info_ptr;
extern char _app_num[], _app_names[], INIT_PROC[];
char names[MAX_APP_NUM][MAX_STR_LEN];

// Get user progs' infomation through pre-defined symbol in `link_app.S`
void loader_init()
{
	char *s;
	app_info_ptr = (uint64 *)_app_num;
	app_num = *app_info_ptr;
	app_info_ptr++;
	s = _app_names;
	printf("app list:\n");
	for (int i = 0; i < app_num; ++i) {
		int len = strlen(s);
		strncpy(names[i], (const char *)s, len);
		s += len + 1;
		printf("%s\n", names[i]);
	}
}

int get_id_by_name(char *name)
{
	for (int i = 0; i < app_num; ++i) {
		if (strncmp(name, names[i], 100) == 0)
			return i;
	}
	warnf("Cannot find such app %s", name);
	return -1;
}

int bin_loader(uint64 start, uint64 end, struct proc *p)
{
	if (p == NULL || p->state == UNUSED)
		panic("...");
	void *page;
	uint64 pa_start = PGROUNDDOWN(start);
	uint64 pa_end = PGROUNDUP(end);
	uint64 length = pa_end - pa_start;
	uint64 va_start = BASE_ADDRESS;
	uint64 va_end = BASE_ADDRESS + length;
	for (uint64 va = va_start, pa = pa_start; pa < pa_end;
	     va += PGSIZE, pa += PGSIZE) {
		page = kalloc();
		if (page == 0) {
			panic("...");
		}
		memmove(page, (const void *)pa, PGSIZE);
		if (pa < start) {
			memset(page, 0, start - va);
		} else if (pa + PAGE_SIZE > end) {
			memset(page + (end - pa), 0, PAGE_SIZE - (end - pa));
		}
		if (mappages(p->pagetable, va, PGSIZE, (uint64)page,
			     PTE_U | PTE_R | PTE_W | PTE_X) != 0)
			panic("...");
	}
	// map ustack
	p->ustack = va_end + PAGE_SIZE;
	for (uint64 va = p->ustack; va < p->ustack + USTACK_SIZE;
	     va += PGSIZE) {
		page = kalloc();
		if (page == 0) {
			panic("...");
		}
		memset(page, 0, PGSIZE);
		if (mappages(p->pagetable, va, PGSIZE, (uint64)page,
			     PTE_U | PTE_R | PTE_W) != 0)
			panic("...");
	}
	p->trapframe->sp = p->ustack + USTACK_SIZE;
	p->trapframe->epc = va_start;
	p->max_page = PGROUNDUP(p->ustack + USTACK_SIZE - 1) / PAGE_SIZE;
	p->program_brk = p->ustack + USTACK_SIZE;
        p->heap_bottom = p->ustack + USTACK_SIZE;
	p->state = RUNNABLE;

	// gemini需要的（解开
	p->vma_head = (struct vma*)kalloc();
	memset((char*)p->vma_head, 0, PGSIZE);
	p->vma_head->front_vma  = NULL;
	p->vma_head->next_vma   = NULL; 

	// p->vma_head->start_addr = 0;
	// p->vma_head->end_addr   = 0;
	// p->vma_head->length     = 0;
	// p->vma_head->page_num   = 0;

	p->vma_head->start_addr = va_start;
	p->vma_head->end_addr   = va_end;
	p->vma_head->length     = length;
	p->vma_head->page_num   = p->max_page;
	// printf("p->vma_head->start_addr = %x\n", va_start);
	// printf("p->vma_head->end_addr = %x\n", va_end);
	// printf("p->vma_head->length = %d\n", length);
	// printf("p->vma_head->page_num = %d\n", p->max_page);
	return 0;
}

int loader(int app_id, struct proc *p)
{
	return bin_loader(app_info_ptr[app_id], app_info_ptr[app_id + 1], p);
}

// load all apps and init the corresponding `proc` structure.
int load_init_app()
{
	int id = get_id_by_name(INIT_PROC);
	if (id < 0)
		panic("Cannpt find INIT_PROC %s", INIT_PROC);
	struct proc *p = allocproc();
	if (p == NULL) {
		panic("allocproc\n");
	}
	debugf("load init proc %s", INIT_PROC);
	loader(id, p);
	add_task(p);
	return 0;
}
