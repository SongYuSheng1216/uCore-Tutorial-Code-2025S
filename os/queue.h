#ifndef QUEUE_H
#define QUEUE_H
#define QUEUE_SIZE (1024)

#include "types.h"
// queue data for processing scheduling only
// for queue for wait queue of mutex/semaphore/condvar, provide other data
extern int process_queue_data[QUEUE_SIZE];

struct queue {
	int *data;
	int size;
	int front;
	int tail;
	int empty;
};


struct queue_prio {
	int *data;
	int size;
	int front;
	int tail;
	int empty;
	uint64 stride[QUEUE_SIZE];
};

void init_queue(struct queue *, int, int *);
void push_queue(struct queue *, int);
int pop_queue(struct queue *);
void init_queue_prio(struct queue_prio *q, int size, int *data);
void push_queue_prio(struct queue_prio *q, int value, int stride);
int pop_queue_prio(struct queue_prio *q);
uint64 get_queue_min_stride(struct queue_prio *q);

#endif // QUEUE_H
