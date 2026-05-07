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

#ifdef NONE_PIP
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

#endif

#ifdef PIP
void mutex_lock(struct mutex *m)
{
    struct thread *t = curr_thread();

    // 1. 锁未被占用，直接拿到锁
    if (!m->locked) {	
        m->locked = 1;
        m->owner = t;	                // 记录锁的持有者
        t->waiting_for = NULL;          // 拿到锁了，不再等待
        return;
    }

    // 2. 锁被占用，准备等待
    t->waiting_for = m;	                // 记录当前线程正在等待的锁
    struct thread *owner = m->owner;	// 获取当前锁的持有者

    // 3. 【优先级继承逻辑】：拔高持有者的优先级
    if (owner != NULL && owner->prio.priority < t->prio.priority) {
        
        // 【关键】只有第一次被提拔时，才保存它原本落后的 stride，防止被连续覆盖
        if (owner->prio.priority == owner->prio.base_priority) {
            owner->prio.saved_stride = owner->prio.stride; 
        }

        owner->prio.priority = t->prio.priority;    
        
        // 瞬间拉低它的 stride，让它立刻能被调度执行
        if (owner->prio.stride > t->prio.stride) {
            owner->prio.stride = t->prio.stride; 
        }
    }

    // 4. 等待锁释放
    if (!m->blocking) {
        // 自旋锁：不断 yield 让出 CPU，直到 m->locked 变为 0
        while (m->locked) {	
            yield();
        }
        // 【修复你的漏洞】：跳出 while 循环说明锁空闲了，必须把锁占为己有！
        m->locked = 1;
        m->owner = t;
        t->waiting_for = NULL;
        return;
    }
    
    // 互斥阻塞锁：加入等待队列，睡眠
    push_queue(&m->wait_queue, task_to_id(t));
    t->state = SLEEPING;
    sched(); // 切走，直到被 mutex_unlock 唤醒

    // 注意：因为你在 unlock 里是“直接把锁传递给它”，
    // 所以 sched() 醒来后，不需要再做 m->locked=1，锁已经是它的了。
}

void mutex_unlock(struct mutex *m)
{
    struct thread *curr = curr_thread();

    // 1. 【归还优先级逻辑】：如果有借用过优先级，现在还回去
    if (curr->prio.priority > curr->prio.base_priority) {
        curr->prio.priority = curr->prio.base_priority; // 恢复原本优先级
        curr->prio.stride = curr->prio.saved_stride;    // 恢复原本那很大的 stride
    }

    // 2. 释放锁或传递锁
    if (m->blocking) {
        struct thread *t = id_to_task(pop_queue(&m->wait_queue));
        if (t == NULL) {
            // 等待队列为空，直接释放锁
            m->locked = 0;
            m->owner = NULL;
        } else {
            // 【修复你的漏洞】：存在等待线程，直接把锁传递给它！
            m->owner = t;           // 锁的主人直接变成唤醒的线程
            t->waiting_for = NULL;  // 它不再等待这把锁了
            // 注意：m->locked 保持为 1 不变，因为锁没被解开，只是换了主人

            t->state = RUNNABLE;
            add_task(t);            // 放入就绪队列
        }
    } else {
        // 自旋锁：直接解开，让 yield() 的人自己抢
        m->locked = 0;
        m->owner = NULL;
    }
}

#endif

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
