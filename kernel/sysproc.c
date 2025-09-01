#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  uint64 va;        // 用户提供的起始虚拟地址
  int pagenum;      // 要检查的页数
  uint64 abitsaddr; // 用户缓冲区地址（写回位掩码）

  if (argaddr(0, &va) < 0)
    return -1;
  if (argint(1, &pagenum) < 0)
    return -1;
  if (argaddr(2, &abitsaddr) < 0)
    return -1;

  if (pagenum < 0)
    return -1;

  struct proc *p = myproc();

  // 每个 word (uint64) 保存 64 页的信息
  int nwords = (pagenum + 63) / 64;

  for (int w = 0; w < nwords; w++) {
    uint64 wordbits = 0;

    for (int b = 0; b < 64; b++) {
      int idx = w * 64 + b;
      if (idx >= pagenum)
        break;

      // 计算当前页的虚拟地址（向下对齐到页）
      uint64 cur_va = PGROUNDDOWN(va + (uint64)idx * PGSIZE);

      // 查找该虚拟地址对应的 PTE（不分配）
      pte_t *pte = walk(p->pagetable, cur_va, 0);
      if (pte && (*pte & PTE_A)) {
        wordbits |= (1ULL << b);
        *pte &= ~PTE_A;
      }
    }

    // 把这个 64-bit word 写回用户空间 (abitsaddr + w*8)
    if (copyout(p->pagetable, abitsaddr + (uint64)w * sizeof(uint64),
                (char *)&wordbits, sizeof(uint64)) < 0)
      return -1;
  }

  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

