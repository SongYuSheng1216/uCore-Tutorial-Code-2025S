// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "fs.h"
#include "bio.h"
#include "defs.h"
#include "file.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
static void readsb(int dev, struct superblock *sb)
{
	struct buf *bp;
	bp = bread(dev, 1);
	memmove(sb, bp->data, sizeof(*sb));
	brelse(bp);
}

// Init fs（将硬盘格式化的操作，在哪里做的？
void fsinit()
{
	int dev = ROOTDEV;
	readsb(dev, &sb);
	if (sb.magic != FSMAGIC) {
		panic("invalid file system");
	}
}

// Zero a block.
// 将dev设备的bno块在磁盘上清零
static void bzero(int dev, int bno)
{
	struct buf *bp;
	bp = bread(dev, bno);
	memset(bp->data, 0, BSIZE);
	bwrite(bp);
	brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// 在dev设备上分配一个新的块，并且把它清零
// 即获得dev设备上的一个block的控制权
static uint balloc(uint dev)
{
	int b, bi, m;
	struct buf *bp;

	bp = 0;
	for (b = 0; b < sb.size; b += BPB) {
		bp = bread(dev, BBLOCK(b, sb));
		for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
			m = 1 << (bi % 8);
			if ((bp->data[bi / 8] & m) == 0) { // Is block free?
				bp->data[bi / 8] |= m; // Mark block in use.
				bwrite(bp);
				brelse(bp);
				bzero(dev, b + bi);
				return b + bi;
			}
		}
		brelse(bp);
	}
	panic("balloc: out of blocks");
	return 0;
}

// Free a disk block.
// 在dev设备上释放块b，b是块号
// 大致原理，是将bit map上的bit置为0，就可以了
static void bfree(int dev, uint b)
{
	struct buf *bp;
	int bi, m;

	// 获得块号为b的bit map信息 所在数据块的buffer
	bp = bread(dev, BBLOCK(b, sb));	
	bi = b % BPB;	// 获得块b在位图块中的偏移量
	m = 1 << (bi % 8);	
	if ((bp->data[bi / 8] & m) == 0)
		panic("freeing free block");
	bp->data[bi / 8] &= ~m;
	bwrite(bp);
	brelse(bp);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// 返回inode ip的第bn块在磁盘上的地址，如果没有就分配一个新的块
// 返回的是ip数据块的第bn块的block_cache
// 该函数会被readi和writei调用，来获取数据块的地址
static uint bmap(struct inode *ip, uint bn)
{
	uint addr, *a;
	struct buf *bp;

	if (bn < NDIRECT) {
		if ((addr = ip->addrs[bn]) == 0)
			ip->addrs[bn] = addr = balloc(ip->dev);
		return addr;
	}
	bn -= NDIRECT;

	if (bn < NINDIRECT) {
		// Load indirect block, allocating if necessary.
		if ((addr = ip->addrs[NDIRECT]) == 0)
			ip->addrs[NDIRECT] = addr = balloc(ip->dev);
		bp = bread(ip->dev, addr);
		a = (uint *)bp->data;
		if ((addr = a[bn]) == 0) {
			a[bn] = addr = balloc(ip->dev);
			bwrite(bp);
		}
		brelse(bp);
		return addr;
	}

	panic("bmap: out of range");
	return 0;
}

//The inode table in memory
struct {
	struct inode inode[NINODE];
} itable;

static struct inode *iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type `type`.
// Returns an allocated and referenced inode.
// 在dev设备上分配一个新的inode，并且把它的type设置为type
// 返回一个已经分配并且被引用的inode
struct inode *ialloc(uint dev, short type)
{
	int inum;
	struct buf *bp;
	struct dinode *dip;

	for (inum = 1; inum < sb.ninodes; inum++) {
		// 获得inode所在的block_buffer，inode号为inum
		bp = bread(dev, IBLOCK(inum, sb));
		dip = (struct dinode *)bp->data + inum % IPB;
		if (dip->type == 0) { // a free inode
			// 这里修改的是block_buffer中的数据，真正的inode在内存中的cache还没有被创建
			memset(dip, 0, sizeof(*dip));
			dip->type = type;

			// 修改完block，就写回磁盘
			bwrite(bp);
			brelse(bp);
			//在这里面分配inode去复制磁盘中的inode信息
			// 但是只给了inum和dev,data的block数据还没复制
			// valid没有置为1，待会会去ivalid拷贝
			return iget(dev, inum);
		}
		brelse(bp);
	}
	panic("ialloc: no inodes");
	return 0;
}

// Copy a modified in-memory inode to disk.
// 将内存中被修改过的inode写回磁盘
// Must be called after every change to an ip->xxx field
// that lives on disk.
// 如果ip->xxx字段被修改了，就必须调用该函数来把修改后的inode写回磁盘
void iupdate(struct inode *ip)
{
	struct buf *bp;
	struct dinode *dip;

	bp = bread(ip->dev, IBLOCK(ip->inum, sb));
	dip = (struct dinode *)bp->data + ip->inum % IPB;
	dip->type = ip->type;
	dip->size = ip->size;
	dip->link = ip->link; 
	// LAB4: you may need to update link count here
	memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
	bwrite(bp);
	brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// it from disk.
// 在dev设备上找到inode号为inum的inode，并且返回它在内存中的cache
static struct inode *iget(uint dev, uint inum)
{
	struct inode *ip, *empty;
	// Is the inode already in the table?
	empty = 0;
	for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
		if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
			ip->ref++;
			return ip;
		}
		// 如果上面的if一直没中，那么就说明这个inode在内存中的cache还没有被创建
		// 然后就这里分配一个新的inode来复制磁盘中的inode信息
		// empty指的是itable的
		if (empty == 0 && ip->ref == 0) // Remember empty slot.
			empty = ip;
	}

	// Recycle an inode entry.
	if (empty == 0)
		panic("iget: no inodes");

	ip = empty;
	ip->dev = dev;
	ip->inum = inum;
	ip->ref = 1;
	ip->valid = 0;
	return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
// 增加ip的引用计数，并且返回ip
// 这样就可以在调用函数的时候直接写成ip = idup(ip1)的形式了
struct inode *idup(struct inode *ip)
{
	ip->ref++;
	return ip;
}

// Reads the inode from disk if necessary.
// inode不是最新的，从磁盘中读出来
void ivalid(struct inode *ip)
{
	struct buf *bp;
	struct dinode *dip;
	if (ip->valid == 0) {
		// 获得inode所在的block_buffer
		bp = bread(ip->dev, IBLOCK(ip->inum, sb));
		// 找到disk_inode在block_buffer中的位置
		dip = (struct dinode *)bp->data + ip->inum % IPB;
		ip->type = dip->type;
		ip->size = dip->size;
		// LAB4: You may need to get lint count here
		ip->link = dip->link;
		// printf("ip->link = %d,dip-link = %d\n", ip->link, dip->link);
		memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
		brelse(bp);	// 新的block已经拷贝到新的buffer上了，旧的可以释放了
		ip->valid = 1;
		if (ip->type == 0)
			panic("ivalid: no type");
	}
}

// Drop a reference to an in-memory inode.
// 内存中inode的引用计数减一

// If that was the last reference, the inode table entry can
// be recycled.
// 如果这是最后一个引用了，那么这个inode表项就可以被回收了

// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// 如果这是最后一个引用了，并且这个inode没有链接了
// 那么就把这个inode（和它的内容）在磁盘上释放了

// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// 所有对iput()的调用都必须在一个事务中，以防它需要释放inode

void iput(struct inode *ip)
{
	// if (ip->link == 0) {
    //     printf("DEBUG iput1: inum=%d ref=%d link=%d\n", ip->inum, ip->ref, ip->link);
    // }
	// if(ip->ref == 1){
	// 	printf("DEBUG iput2: inum=%d ref=%d link=%d\n", ip->inum, ip->ref, ip->link);
	// }
	
	// LAB4: Unmark the condition and change link count variable name (nlink) if needed
	// 当此时的进程是最后一个对这个inode的引用（现在用完了
	// 同时这个inode也是和磁盘一致的
	// 并且这个inode没有链接了（也就是说磁盘上没有任何目录项指向它了）
	// 那么就可以把这个inode（包括内存和磁盘）和它占用的磁盘块（内存和磁盘）都释放了
	if (ip->ref == 1 && ip->valid && ip->link == 0) {
		// printf("aaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
		// inode has no links and no other references: truncate and free.
		itrunc(ip);		// 这里是清理内存和磁盘中的数据块
		ip->type = 0;	// 这里是清理内存中的inode
		iupdate(ip);	// 这里是清理磁盘上的inode
		ip->valid = 0;	// 这里是清理内存中的inode
	}
	ip->ref--;
}


// Truncate inode (discard contents).
// “Truncate” 这个词在英文原意中是“切断、截短”的意思
//将一个文件的大小重置为 0，并释放该文件占用的所有磁盘块。
// 两种情况调用
// 1. 删除文件时
// 2. 写文件时，打开刷新文件，释放掉文件内容，重新往文件里写新内容
// 释放掉ip这个inode的每一个数据块
void itrunc(struct inode *ip)
{
	int i, j;
	struct buf *bp;
	uint *a;

	for (i = 0; i < NDIRECT; i++) {
		if (ip->addrs[i]) {
			// 释放掉ip这个inode的每一个数据块
			bfree(ip->dev, ip->addrs[i]);
			ip->addrs[i] = 0;
		}
	}

	if (ip->addrs[NDIRECT]) {
		// 非直接地址
		// 第NDIRECT块是一个间接块，里面存储了NINDIRECT个数据块的块号
		// 这些块也都是inode的数据块
		// 释放掉间接块中指示的其他数据块
		bp = bread(ip->dev, ip->addrs[NDIRECT]);
		a = (uint *)bp->data;
		for (j = 0; j < NINDIRECT; j++) {
			if (a[j])
				bfree(ip->dev, a[j]);
		}
		brelse(bp);
		bfree(ip->dev, ip->addrs[NDIRECT]);
		ip->addrs[NDIRECT] = 0;
	}

	ip->size = 0;
	iupdate(ip);
}

// Read data from inode.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
// 读取inode中的数据到dst地址，dst地址可以是用户虚拟地址，也可以是内核地址
// 返回成功读取的字节数，如果返回值小于请求的n，说明发生
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
	uint tot, m;
	struct buf *bp;

	if (off > ip->size || off + n < off)
		return 0;
	if (off + n > ip->size)
		n = ip->size - off;

	for (tot = 0; tot < n; tot += m, off += m, dst += m) {
		bp = bread(ip->dev, bmap(ip, off / BSIZE));
		m = MIN(n - tot, BSIZE - off % BSIZE);
		if (either_copyout(user_dst, dst,
				   (char *)bp->data + (off % BSIZE), m) == -1) {
			brelse(bp);
			tot = -1;
			break;
		}
		brelse(bp);
	}
	return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
// 将src地址的数据写入inode中，src地址可以是用户虚拟地址，也可以是内核地址
// 返回成功写入的字节数，如果返回值小于请求的n，说明发生了错误
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
	uint tot, m;
	struct buf *bp;

	if (off > ip->size || off + n < off)
		return -1;
	if (off + n > MAXFILE * BSIZE)
		return -1;

	for (tot = 0; tot < n; tot += m, off += m, src += m) {
		bp = bread(ip->dev, bmap(ip, off / BSIZE));
		m = MIN(n - tot, BSIZE - off % BSIZE);
		if (either_copyin(user_src, src,
				  (char *)bp->data + (off % BSIZE), m) == -1) {
			brelse(bp);
			break;
		}
		bwrite(bp);
		brelse(bp);
	}

	if (off > ip->size)
		ip->size = off;

	// write the i-node back to disk even if the size didn't change
	// because the loop above might have called bmap() and added a new
	// block to ip->addrs[].
	iupdate(ip);

	return tot;
}

//Show the filenames of all files in the directory
// 返回目录dp中所有文件的名字
int dirls(struct inode *dp)
{
	uint64 off, count;
	struct dirent de;

	if (dp->type != T_DIR)
		panic("dirlookup not DIR");

	count = 0;
	for (off = 0; off < dp->size; off += sizeof(de)) {
		if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
			panic("dirlookup read");
		if (de.inum == 0)
			continue;
		printf("%s\n", de.name);
		count++;
	}
	return count;
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// 返回目录dp中名字为name的inode，如果没有找到，返回0
// 如果找到了，*poff被设置为目录项的字节偏移量
// 返回的inode是内存中的cache
// 重要！！！
struct inode *dirlookup(struct inode *dp, char *name, uint *poff)
{
	uint off, inum;
	struct dirent de;

	if (dp->type != T_DIR)
		panic("dirlookup not DIR");

	for (off = 0; off < dp->size; off += sizeof(de)) {
		if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
			panic("dirlookup read");
		if (de.inum == 0)
			continue;
		if (strncmp(name, de.name, DIRSIZ) == 0) {
			// entry matches path element
			if (poff)
				*poff = off;
			inum = de.inum;
			return iget(dp->dev, inum);
		}
	}

	return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// 写一个新的目录项（name，inum）到目录dp中
// 返回0表示成功，-1表示失败
// 重要！！！
int dirlink(struct inode *dp, char *name, uint inum)
{
	int off;
	struct dirent de;
	struct inode *ip;
	// Check that name is not present.
	if ((ip = dirlookup(dp, name, 0)) != 0) {
		iput(ip);
		return -1;
	}

	// Look for an empty dirent.
	for (off = 0; off < dp->size; off += sizeof(de)) {
		if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
			panic("dirlink read");
		if (de.inum == 0)
			break;
	}
	strncpy(de.name, name, DIRSIZ);
	de.inum = inum;
	if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
		panic("dirlink");
	return 0;
}

// LAB4: You may want to add dirunlink here
// 将目录dp中的目录项<name,inum>删除
// 返回0表示成功，-1表示失败
int dirunlink(struct inode *dp, char *name, uint inum)
{
	uint off;
	struct dirent de;

	for (off = 0; off < dp->size; off += sizeof(de)) {
		if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
			panic("dirunlink read");
		if (de.inum == inum && strncmp(name, de.name, DIRSIZ) == 0) {
			// Found the entry to delete
			de.inum = 0;
			if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
				panic("dirunlink write");
			return 0;
		}
	}
	return -1;
}

//Return the inode of the root directory
// 返回根目录的inode
struct inode *root_dir()
{
	struct inode *r = iget(ROOTDEV, ROOTINO);
	ivalid(r);
	return r;
}

//Find the corresponding inode according to the path
// 如果path存在，返回对应的inode；如果path不存在，返回0
struct inode *namei(char *path)
{
	int skip = 0;
	// if(path[0] == '.' && path[1] == '/')
	//     skip = 2;
	// if (path[0] == '/') {
	//     skip = 1;
	// }
	struct inode *dp = root_dir();
	if (dp == 0)
		panic("fs dumped.\n");
	return dirlookup(dp, path + skip, 0);
}
