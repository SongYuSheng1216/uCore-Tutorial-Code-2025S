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

struct sema {
	int count;
	struct queue wait_queue;
	// 等待队列
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

struct cond {
	struct queue wait_queue;
	// 等大队列
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

struct mutex *mutex_create(int blocking);
void mutex_lock(struct mutex *);
void mutex_unlock(struct mutex *);
struct sema *sema_create(int count);
void sema_V(struct sema *);
void sema_P(struct sema *);
struct cond *cond_create();
void cond_notify(struct cond *);
void cond_wait(struct cond *, struct mutex *);
#endif
