#include "queue.h"
#include "defs.h"

int process_queue_data[QUEUE_SIZE];

void init_queue(struct queue *q, int size, int *data)
{
	q->size = size;
	q->data = data;
	q->front = q->tail = 0;
	q->empty = 1;
}

void push_queue(struct queue *q, int value)
{
	if (!q->empty && q->front == q->tail) {
		panic("queue shouldn't be overflow");
	}
	q->empty = 0;
	q->data[q->tail] = value;
	q->tail = (q->tail + 1) % q->size;
}

int pop_queue(struct queue *q)
{
	if (q->empty)
		return -1;
	int value = q->data[q->front];
	q->front = (q->front + 1) % q->size;
	if (q->front == q->tail)
		q->empty = 1;
	return value;
}

void init_queue_prio(struct queue_prio *q, int size, int *data)
{
	q->size = size;
	q->data = data;
	q->front = q->tail = 0;
	q->empty = 1;
	for(int i = 0; i < size; i++){
		q->stride[i] = 0;
	}
}

void push_queue_prio(struct queue_prio *q, int value, int stride)
{
	if (!q->empty && q->front == q->tail) {
		panic("queue shouldn't be overflow");
	}
	q->empty = 0;
	q->data[q->tail] = value;
	q->stride[q->tail] = stride;
	q->tail = (q->tail + 1) % NPROC;
}

int pop_queue_prio(struct queue_prio *q)
{
    if (q->empty)
        return -1;

    // 1. 寻找最小 stride 的索引
    // 我们需要遍历当前队列中所有有效元素
    int min_stride_idx = -1;
    uint64 min_stride_val = 0xFFFFFFFFFFFFFFFF; // 使用最大值初始化
    
    // 这是一个标准的环形队列遍历惯用写法
    // 无论是否满，我们需要遍历 (tail - front + N) % N 个元素
    
    // 计算当前元素个数
    int num_items;
    if (q->front == q->tail && !q->empty) {
        num_items = NPROC;
    } else {
        num_items = (q->tail - q->front + NPROC) % NPROC;
    }

    for (int i = 0; i < num_items; i++) {
        int idx = (q->front + i) % NPROC;
        if (q->stride[idx] < min_stride_val) {
            min_stride_val = q->stride[idx];
            min_stride_idx = idx;
        }
    }

    // 2. 取出数据
    int ret_value = q->data[min_stride_idx];

    // 3. 移除该元素 (使用与队尾前一个元素交换的方法)
    // 这种方法破坏了插入顺序（FIFO），但对 Stride 调度无影响且效率最高
    
    // 找到逻辑上的“最后一个元素”的物理索引
    int last_item_idx = (q->tail - 1 + NPROC) % NPROC;

    // 如果要删除的不是最后一个，就把最后一个搬过来覆盖它
    if (min_stride_idx != last_item_idx) {
        q->data[min_stride_idx] = q->data[last_item_idx];
        q->stride[min_stride_idx] = q->stride[last_item_idx];
    }

    // 4. 更新 tail 指针 (回退一格)
    q->tail = last_item_idx;

    // 5. 更新 empty 状态
    if (q->front == q->tail) {
        q->empty = 1;
    }

    // 注意：不要移动 q->front，因为我们是把空洞填到了 tail 处并收缩了 tail
    return ret_value;
}

// 获取当前队列中的最小 stride（不改变队列内容）
uint64 get_queue_min_stride(struct queue_prio *q) {
    if (q->empty) return 0; // 如果队列为空，返回 0

    uint64 min_stride_val = 0xFFFFFFFFFFFFFFFF;
    int num_items = (q->front == q->tail && !q->empty) ? NPROC : (q->tail - q->front + NPROC) % NPROC;

    for (int i = 0; i < num_items; i++) {
        int idx = (q->front + i) % NPROC;
        if (q->stride[idx] < min_stride_val) {
            min_stride_val = q->stride[idx];
        }
    }
    return min_stride_val;
}
