// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "bio.h"
#include "defs.h"
#include "fs.h"
#include "riscv.h"
#include "types.h"
#include "virtio.h"

struct {
	struct buf buf[NBUF];
	struct buf head;
	// 将buf数组组织成一个双向链表，head是链表的头结点
	// head 用来把上面数组里的格子串成一条链表（双向链表）。
	// 通过这个链表，我们就能管理书的“新鲜度”（LRU算法 - 最近最少使用算法） 
} bcache;

void binit()
{
	struct buf *b;
	// Create linked list of buffers
	bcache.head.prev = &bcache.head;
	bcache.head.next = &bcache.head;
	for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
		b->next = bcache.head.next;
		b->prev = &bcache.head;
		bcache.head.next->prev = b;
		bcache.head.next = b;
	}
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// 在bcache里面找到dev设备的blockno块，如果没有找到，就分配一个新的buffer
// 返回一个buf结构体指针，里面包含了blockno块的内容
// 得到指定block号的buf结构体指针
static struct buf *bget(uint dev, uint blockno)
{
	struct buf *b;
	// Is the block already cached?
	for (b = bcache.head.next; b != &bcache.head; b = b->next) {
		if (b->dev == dev && b->blockno == blockno) {
			b->refcnt++;
			return b;
		}
	}
	// Not cached.
	// Recycle the least recently used (LRU) unused buffer.
	// 这里是真正的释放，直接在这个buf里面写入新的数据
	for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
		if (b->refcnt == 0) {
			b->dev = dev;
			b->blockno = blockno;
			b->valid = 0;
			b->refcnt = 1;
			return b;
		}
	}
	panic("bget: no buffers");
	return 0;
}

const int R = 0;
const int W = 1;

// Return a buf with the contents of the indicated block.
// 返回一个buf，里面是指定block的内容
// 得到指定block号的内容
struct buf *bread(uint dev, uint blockno)
{
	struct buf *b;
	b = bget(dev, blockno);
	if (!b->valid) {	// 如果不是最新获得磁盘中的数据
		virtio_disk_rw(b, R);	// 那么去磁盘读写
		b->valid = 1;
	}
	return b;
}

// Write b's contents to disk.
void bwrite(struct buf *b)
{
	virtio_disk_rw(b, W);
}

// Release a buffer.
// Move to the head of the most-recently-used list.
// 把它放置在bcache链表的首部
void brelse(struct buf *b)
{
	b->refcnt--;
	if (b->refcnt == 0) {
		// no one is waiting for it.
		b->next->prev = b->prev;
		b->prev->next = b->next;
		b->next = bcache.head.next;
		b->prev = &bcache.head;
		bcache.head.next->prev = b;
		bcache.head.next = b;
	}
}

void bpin(struct buf *b)
{
	b->refcnt++;
}

void bunpin(struct buf *b)
{
	b->refcnt--;
}
