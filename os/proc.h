#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"

#define NPROC (24)

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };
enum trace_choose {
	read_choose,
	write_choose,
	call_choose
};

struct counter_do {
	int sys_write_counter;
	int sys_exit_counter;
	int sys_sched_yield_counter;
	int sys_gettimeofday;
	int sys_sbrk;
	int sys_trace;
	int sys_mmap;
	int sys_munmap;
};

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	pagetable_t pagetable; // User page table
	uint64 ustack;
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	uint64 max_page;
	uint64 program_brk;
	uint64 heap_bottom;
	/*
	* LAB1: you may need to add some new fields here
	*/
	struct counter_do coun;

};

struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
struct proc *allocproc();
// swtch.S
void swtch(struct context *, struct context *);

int growproc(int n);

#endif // PROC_H
