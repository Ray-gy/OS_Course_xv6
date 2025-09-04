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


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

extern uint ticks;
extern struct spinlock tickslock;

#define NBUCKET 13

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  
  // Hash table with separate locks for each bucket
  struct spinlock bucket_locks[NBUCKET];
  struct buf buckets[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;
  int i;

  initlock(&bcache.lock, "bcache");

  // Initialize bucket locks and empty bucket lists
  for(i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket_locks[i], "bcache.bucket");
    bcache.buckets[i].next = &bcache.buckets[i];
    bcache.buckets[i].prev = &bcache.buckets[i];
  }

  // Initialize all buffers and add them to bucket 0
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.buckets[0].next;
    b->prev = &bcache.buckets[0];
    b->timestamp = 0;
    bcache.buckets[0].next->prev = b;
    bcache.buckets[0].next = b;
  }
}

// Hash function for block number
static uint
hash(uint dev, uint blockno)
{
  return (dev << 16) ^ blockno;
}

// Get current timestamp
static uint
get_timestamp(void)
{
  uint t;
  acquire(&tickslock);
  t = ticks;
  release(&tickslock);
  return t;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket = hash(dev, blockno) % NBUCKET;
  uint timestamp = get_timestamp();

  // First, check if the block is already cached in the target bucket
  acquire(&bcache.bucket_locks[bucket]);
  for(b = bcache.buckets[bucket].next; b != &bcache.buckets[bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->timestamp = timestamp;
      release(&bcache.bucket_locks[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket_locks[bucket]);

  // Not cached. Need to find a free buffer.
  // First try to find a free buffer in the target bucket
  acquire(&bcache.bucket_locks[bucket]);
  for(b = bcache.buckets[bucket].next; b != &bcache.buckets[bucket]; b = b->next){
    if(b->refcnt == 0) {
      // Remove from current bucket
      b->next->prev = b->prev;
      b->prev->next = b->next;
      release(&bcache.bucket_locks[bucket]);
      
      // Set up the buffer
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      b->timestamp = timestamp;
      
      // Add to target bucket
      acquire(&bcache.bucket_locks[bucket]);
      b->next = bcache.buckets[bucket].next;
      b->prev = &bcache.buckets[bucket];
      bcache.buckets[bucket].next->prev = b;
      bcache.buckets[bucket].next = b;
      release(&bcache.bucket_locks[bucket]);
      
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket_locks[bucket]);

  // No free buffer in target bucket, need to steal from other buckets
  // Use global lock to serialize eviction
  acquire(&bcache.lock);
  
  // Double-check if block was cached while we were waiting
  acquire(&bcache.bucket_locks[bucket]);
  for(b = bcache.buckets[bucket].next; b != &bcache.buckets[bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->timestamp = timestamp;
      release(&bcache.bucket_locks[bucket]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.bucket_locks[bucket]);

  // Find the least recently used buffer across all buckets
  struct buf *lru_buf = 0;
  uint lru_timestamp = 0xffffffff;
  
  for(int i = 0; i < NBUCKET; i++){
    acquire(&bcache.bucket_locks[i]);
    for(b = bcache.buckets[i].next; b != &bcache.buckets[i]; b = b->next){
      if(b->refcnt == 0 && b->timestamp < lru_timestamp){
        lru_timestamp = b->timestamp;
        lru_buf = b;
      }
    }
    release(&bcache.bucket_locks[i]);
  }
  
  if(lru_buf == 0)
    panic("bget: no buffers");
  
  // Remove lru_buf from its current bucket
  uint old_bucket = hash(lru_buf->dev, lru_buf->blockno) % NBUCKET;
  acquire(&bcache.bucket_locks[old_bucket]);
  lru_buf->next->prev = lru_buf->prev;
  lru_buf->prev->next = lru_buf->next;
  release(&bcache.bucket_locks[old_bucket]);
  
  // Set up the buffer
  lru_buf->dev = dev;
  lru_buf->blockno = blockno;
  lru_buf->valid = 0;
  lru_buf->refcnt = 1;
  lru_buf->timestamp = timestamp;
  
  // Add to target bucket
  acquire(&bcache.bucket_locks[bucket]);
  lru_buf->next = bcache.buckets[bucket].next;
  lru_buf->prev = &bcache.buckets[bucket];
  bcache.buckets[bucket].next->prev = lru_buf;
  bcache.buckets[bucket].next = lru_buf;
  release(&bcache.bucket_locks[bucket]);
  
  release(&bcache.lock);
  acquiresleep(&lru_buf->lock);
  return lru_buf;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Update timestamp for LRU.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket = hash(b->dev, b->blockno) % NBUCKET;
  acquire(&bcache.bucket_locks[bucket]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // Update timestamp for LRU
    b->timestamp = get_timestamp();
  }
  release(&bcache.bucket_locks[bucket]);
}

void
bpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno) % NBUCKET;
  acquire(&bcache.bucket_locks[bucket]);
  b->refcnt++;
  release(&bcache.bucket_locks[bucket]);
}

void
bunpin(struct buf *b) {
  uint bucket = hash(b->dev, b->blockno) % NBUCKET;
  acquire(&bcache.bucket_locks[bucket]);
  b->refcnt--;
  release(&bcache.bucket_locks[bucket]);
}


