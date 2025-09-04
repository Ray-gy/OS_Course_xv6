// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  for(int i = 0; i < NCPU; i++) {
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);
  
  // If current CPU's freelist is empty, try to steal from other CPUs
  if(!r) {
    for(int i = 0; i < NCPU; i++) {
      if(i == id) continue;
      
      acquire(&kmem[i].lock);
      r = kmem[i].freelist;
      if(r) {
        // Steal half of the freelist
        struct run *steal = r;
        int count = 0;
        while(steal && count < 10) { // Steal up to 10 pages
          steal = steal->next;
          count++;
        }
        
        if(steal) {
          // Update the source CPU's freelist
          kmem[i].freelist = steal->next;
          steal->next = 0;
          
          // Add stolen pages to current CPU's freelist
          acquire(&kmem[id].lock);
          steal->next = kmem[id].freelist;
          kmem[id].freelist = r;
          release(&kmem[id].lock);
        } else {
          // Take all remaining pages
          kmem[i].freelist = 0;
          
          acquire(&kmem[id].lock);
          kmem[id].freelist = r;
          release(&kmem[id].lock);
        }
        
        // Get one page from our freelist
        acquire(&kmem[id].lock);
        r = kmem[id].freelist;
        if(r)
          kmem[id].freelist = r->next;
        release(&kmem[id].lock);
      }
      release(&kmem[i].lock);
      
      if(r) break;
    }
  }
  
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
