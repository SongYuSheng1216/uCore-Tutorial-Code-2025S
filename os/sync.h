#ifndef SYNC_H
#define SYNC_H
#include "queue.h"
#include "types.h"

#define WAIT_QUEUE_MAX_LENGTH 16

struct mutex {
	uint blocking;	// 表示是阻塞锁，还是自旋锁
	uint locked;	// 是否上锁
	struct queue wait_queue;	// 阻塞的线程等待队列
	// "alloc" data for wait queue
// 注意：这里的_wait_queue_data只是为了给wait_queue提供一个静态的数组作为数据存储空间
// 它用来存放那些因为拿不到锁而被阻塞的线程 ID（或者线程控制块的索引）。
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];	
};

struct semaphore {
	int count;
	struct queue wait_queue;
	// "alloc" data for wait queue
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

struct condvar {
	struct queue wait_queue;
	// "alloc" data for wait queue
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

struct mutex *mutex_create(int blocking);
void mutex_lock(struct mutex *);
void mutex_unlock(struct mutex *);
struct semaphore *semaphore_create(int count);
void semaphore_up(struct semaphore *);
void semaphore_down(struct semaphore *);
struct condvar *condvar_create();
void cond_signal(struct condvar *);
void cond_wait(struct condvar *, struct mutex *);
#endif
