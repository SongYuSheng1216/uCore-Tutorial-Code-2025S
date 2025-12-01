#ifndef SYSCALL_H
#define SYSCALL_H

void syscall();

enum trace_function {
	TRACE_READ,
	TRACE_WRITE,
	TRACE_SYSCALL,
};

#endif // SYSCALL_H
