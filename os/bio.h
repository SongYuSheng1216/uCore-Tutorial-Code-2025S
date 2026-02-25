#ifndef BUF_H
#define BUF_H

#include "fs.h"
#include "types.h"

struct buf {
	int valid; // has data been read from disk?
	int disk; // does disk "own" buf?
	// disk 是缓冲区的 IO 状态标记位
	// 核心作用是标记 “该缓冲区是否正在和磁盘进行读写交互”；
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
