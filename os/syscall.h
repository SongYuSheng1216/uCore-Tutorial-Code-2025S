#ifndef SYSCALL_H
#define SYSCALL_H

// QEMU RISC-V virt 机器的串口物理基地址
#define UART_BASE 0x10000000L  

// LSR (Line Status Register) 的偏移是 5
// 它的第 0 位 (Data Ready) 标志着是否有字符到来
#define UART_LSR ((volatile unsigned char *)(UART_BASE + 0x05))

// RBR (Receiver Buffer Register) 的偏移是 0
// 读取它就能拿到字符
#define UART_RBR ((volatile unsigned char *)(UART_BASE + 0x00))


void syscall();

#endif // SYSCALL_H
