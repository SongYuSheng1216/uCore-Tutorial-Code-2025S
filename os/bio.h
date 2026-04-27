#ifndef BUF_H
#define BUF_H

#include "fs.h"
#include "types.h"

struct buf {
	int valid; // has data been read from disk?
	int disk; // does disk "own" buf?
	// 如果这个buf此时和磁盘交互，正在被读写
	// 那么disk置为1，
	// 如果有其他进程想要使用这个buf，就必须等待disk置为0
	uint dev;
	uint blockno;
	uint refcnt;
	struct buf *prev; // LRU cache list
	struct buf *next;
	uchar data[BSIZE];
};

void binit(void);
struct buf *bread(uint, uint);
void brelse(struct buf *);
void bwrite(struct buf *);
void bpin(struct buf *);
void bunpin(struct buf *);

#endif // BUF_H
