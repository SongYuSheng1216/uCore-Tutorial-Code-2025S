#ifndef FILE_H
#define FILE_H

#include "fs.h"
#include "proc.h"
#include "types.h"

#define PIPESIZE (512)
#define FILEPOOLSIZE (NPROC * FD_BUFFER_SIZE)

// in-memory copy of an inode,it can be used to quickly locate file entities on disk
struct inode {
	uint dev; // Device number（这个inode是哪个分区/硬盘上的
	uint inum; // Inode number（inode的值
	int ref; // Reference count
	int valid; // inode has been read from disk?
	short type; // copy of disk inode
				// (inode指向的数据区，是哪种文件类型
				// 通常是 T_FILE (普通文件), T_DIR (目录), T_DEV (设备文件))
	uint size;
	uint addrs[NDIRECT + 1];
	// NDIRECT：直接索引。例如前 12 个直接存块号（Block 100, Block 101...）。
	// 1：通常指间接索引（Indirect Block）
	// 如果文件太大，这最后一个指针指向一个“存满指针的块”，用来扩展文件大小。
	// LAB4: You may need to add link count here
};

//a struct for pipe
struct pipe {
	char data[PIPESIZE];
	uint nread; // number of bytes read
	uint nwrite; // number of bytes written
	int readopen; // read fd is still open
	int writeopen; // write fd is still open
};

// file.h
// Defines a file in memory that provides information about the current use of the file and the corresponding inode location
struct file {
	enum { FD_NONE = 0, FD_PIPE, FD_INODE, FD_STDIO } type;
	int ref; // reference count
	char readable;
	char writable;
	struct pipe *pipe; // FD_PIPE
	struct inode *ip; // FD_INODE
	uint off;
};

//A few specific fd
enum {
	STDIN = 0,
	STDOUT = 1,
	STDERR = 2,
};

extern struct file filepool[FILEPOOLSIZE];

int pipealloc(struct file *, struct file *);
void pipeclose(struct pipe *, int);
int piperead(struct pipe *, uint64, int);
int pipewrite(struct pipe *, uint64, int);
void fileclose(struct file *);
struct file *filealloc();
int fileopen(char *, uint64);
uint64 inodewrite(struct file *, uint64, uint64);
uint64 inoderead(struct file *, uint64, uint64);
struct file *stdio_init(int);
int show_all_files();

#endif // FILE_H

