#include "defs.h"
#include "proc.h"
#include "sync.h"

struct mutex *mutex_create(int blocking)
{
	struct proc *p = curr_proc();
	if (p->next_mutex_id >= LOCK_POOL_SIZE) {
		return NULL;
	}
	struct mutex *m = &p->mutex_pool[p->next_mutex_id];
	p->next_mutex_id++;
	m->blocking = blocking;
	m->locked = 0;                                                                                                                                                                                                                                                                                                                                                                                     
	if (blocking) {	// 阻塞情况
		// 被阻塞的线程入队
		init_queue(&m->wait_queue, WAIT_QUEUE_MAX_LENGTH,
			   m->_wait_queue_data);
	}
	return m;
}

void mutex_lock(struct mutex *m)
{
	if (!m->locked) {	// 锁未被占用，直接拿到锁
		m->locked = 1;
		return;
	}
	if (!m->blocking) {
		while (m->locked) {	// 自旋锁，这里粗糙了，直接切换线程
			yield();
		};
		return;
	}
	// 阻塞
	struct thread *t = curr_thread();
	push_queue(&m->wait_queue, task_to_id(t));	// 将当前线程加入锁m等待队列
	t->state = SLEEPING;
	sched();
}

void mutex_unlock(struct mutex *m)
{
	if (m->blocking) {
		struct thread *t = id_to_task(pop_queue(&m->wait_queue));
		if (t == NULL) {
			// 没有等待调度锁的线程
			m->locked = 0;	// 18
		} else {
			// 存在有等待调度锁的线程，直接把锁传递给它
			t->state = RUNNABLE;	// 21
			add_task(t);	// 22
		}
	} else {
		m->locked = 0;
	}
}

struct sema *sema_create(int count)
{
	struct proc *p = curr_proc();
	// 超过信号量池大小限制，无法创建更多信号量
	if (p->next_sema_id >= LOCK_POOL_SIZE) {
		return NULL;
	}
	struct sema *s = &p->sema_pool[p->next_sema_id];
	p->next_sema_id++;
	// 信号量数值
	s->count = count;
	// 初始化信号量的队列
	init_queue(&s->wait_queue, WAIT_QUEUE_MAX_LENGTH, s->_wait_queue_data);
	return s;
}

// count > 0：有这么多个可用资源，无等待线程
// count = 0：无可用资源，也无等待线程
// count < 0：无可用资源，且有 |count| 个线程在等待

void sema_V(struct sema *s)	// V 操作
{
	// 释放了一个资源，唤醒一个等待的线程（如果有的话）
	s->count++;
	if (s->count <= 0) {
		// 此时有线程在等待信号量，需要唤醒一个线程
		struct thread *t = id_to_task(pop_queue(&s->wait_queue));
		if (t == NULL) {
			panic();
		}
		t->state = RUNNABLE;
		add_task(t);
	}
}

void sema_P(struct sema *s)	// P 操作
{
	s->count--;
	if (s->count < 0) {
		// 此时没有资源了，需要阻塞当前线程
		struct thread *t = curr_thread();
		push_queue(&s->wait_queue, task_to_id(t));
		t->state = SLEEPING;
		sched();
	}
}

struct cond *cond_create()
{
	struct proc *p = curr_proc();
	// 超过池子限制
	if (p->next_cond_id >= LOCK_POOL_SIZE) {
		return NULL;
	}
	struct cond *c = &p->cond_pool[p->next_cond_id];
	p->next_cond_id++;
	init_queue(&c->wait_queue, WAIT_QUEUE_MAX_LENGTH, c->_wait_queue_data);
	return c;
}

// ch8b_test_condvar.c可以测试

void cond_notify(struct cond *cond)
{
	// 通知等待的线程可以运行了
	struct thread *t = id_to_task(pop_queue(&cond->wait_queue));
	if (t) {
		t->state = RUNNABLE;
		add_task(t);
	} else {

	}
}

void cond_wait(struct cond *cond, struct mutex *m)
{
	// 释放锁，供notify通知的时候获取锁
	mutex_unlock(m);
	struct thread *t = curr_thread();
	// 等待
	push_queue(&cond->wait_queue, task_to_id(t));
	t->state = SLEEPING;
	sched();
	// 被唤醒重新开始运行时候，自动获取锁
	mutex_lock(m);
}
