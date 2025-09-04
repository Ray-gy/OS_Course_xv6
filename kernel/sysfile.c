//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// Check if a page is dirty (has PTE_D flag)
static int
is_page_dirty(pagetable_t pagetable, uint64 va)
{
  return uvmgetdirty(pagetable, va);
}

// Write back dirty pages to file for MAP_SHARED mappings
static int
writeback_dirty_pages(struct proc *p, struct vm_area *vma, uint64 addr, int len)
{
  if(vma->f == 0 || (vma->flags & MAP_SHARED) == 0)
    return 0; // No file or not shared mapping
  
  uint64 start = PGROUNDUP(addr);
  uint64 end = PGROUNDDOWN(addr + len);
  
  for(uint64 va = start; va < end; va += PGSIZE){
    if(is_page_dirty(p->pagetable, va)){
      // Get the physical address of the page
      uint64 pa = walkaddr(p->pagetable, va);
      if(pa == 0)
        continue;
      
      // Calculate file offset
      uint64 offset = va - vma->addr;
      int file_offset = vma->offset + offset;
      
      // Write back to file
      begin_op();
      ilock(vma->f->ip);
      int n = PGSIZE;
      if(file_offset + n > vma->f->ip->size)
        n = vma->f->ip->size - file_offset;
      if(n > 0){
        if(writei(vma->f->ip, 0, pa, file_offset, n) != n){
          iunlock(vma->f->ip);
          end_op();
          return -1;
        }
      }
      iunlock(vma->f->ip);
      end_op();
    }
  }
  
  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  int len, prot, flags, fd, offset;
  
  // Extract parameters
  if(argaddr(0, &addr) < 0)
    return -1;
  if(argint(1, &len) < 0)
    return -1;
  if(argint(2, &prot) < 0)
    return -1;
  if(argint(3, &flags) < 0)
    return -1;
  if(argint(4, &fd) < 0)
    return -1;
  if(argint(5, &offset) < 0)
    return -1;
  
  // Parameter validation
  if(len <= 0)
    return -1;
  if(offset < 0)
    return -1;
  if(flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;
  if((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
    return -1;
  
  struct proc *p = myproc();
  if(p == 0)
    return -1;
  
  // Find an empty VMA slot
  int vma_idx = -1;
  for(int i = 0; i < NVMA; i++){
    if(p->vma[i].addr == 0){
      vma_idx = i;
      break;
    }
  }
  
  if(vma_idx == -1)
    return -1; // No free VMA slots
  
  // Determine mapping address
  if(addr == 0){
    // Find the highest mapped address
    uint64 max_addr = MMAPMINADDR;
    for(int i = 0; i < NVMA; i++){
      if(p->vma[i].addr != 0){
        uint64 end_addr = p->vma[i].addr + p->vma[i].len;
        if(end_addr > max_addr)
          max_addr = end_addr;
      }
    }
    // Align to page boundary
    addr = PGROUNDUP(max_addr);
  }
  
  // Check if mapping would overlap with TRAPFRAME
  if(addr + len > TRAPFRAME)
    return -1;
  
  // Get the file if fd is valid
  struct file *f = 0;
  if(fd >= 0 && fd < NOFILE){
    f = p->ofile[fd];
    if(f){
      // Check if file permissions allow the requested mapping
      if((prot & PROT_WRITE) && (f->type == FD_INODE)){
        if(f->readable && !f->writable && (flags & MAP_SHARED)){
          // File is read-only, cannot map with write permission for MAP_SHARED
          return -1;
        }
      }
      filedup(f); // Increment reference count
    }
  }
  
  // Set up the VMA
  p->vma[vma_idx].addr = addr;
  p->vma[vma_idx].len = len;
  p->vma[vma_idx].prot = prot;
  p->vma[vma_idx].flags = flags;
  p->vma[vma_idx].offset = offset;
  p->vma[vma_idx].f = f;
  
  return addr;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int len;
  
  // Extract parameters
  if(argaddr(0, &addr) < 0)
    return -1;
  if(argint(1, &len) < 0)
    return -1;
  
  // Parameter validation
  if(len < 0)
    return -1;
  if(len == 0)
    return 0; // Success for zero length
  
  // Check if addr is page-aligned
  if((addr % PGSIZE) != 0)
    return -1;
  
  struct proc *p = myproc();
  if(p == 0)
    return -1;
  
  // Find the VMA containing this address
  int vma_idx = -1;
  for(int i = 0; i < NVMA; i++){
    if(p->vma[i].addr != 0 && addr >= p->vma[i].addr && 
       addr < p->vma[i].addr + p->vma[i].len){
      vma_idx = i;
      break;
    }
  }
  
  if(vma_idx == -1)
    return -1; // Address not found in any VMA
  
  struct vm_area *vma = &p->vma[vma_idx];
  
  // Check if the unmapping range is valid
  if(addr + len > vma->addr + vma->len)
    return -1;
  
  // Write back dirty pages for MAP_SHARED mappings
  if(writeback_dirty_pages(p, vma, addr, len) != 0)
    return -1;
  
  // Unmap the pages
  uint64 start = PGROUNDUP(addr);
  uint64 end = PGROUNDDOWN(addr + len);
  if(start < end){
    uvmunmap_mmap(p->pagetable, start, (end - start) / PGSIZE, 1);
  }
  
  // Update VMA structure
  if(addr == vma->addr){
    // Unmapping from the beginning
    vma->addr += len;
    vma->len -= len;
    vma->offset += len;
    
    // If VMA is now empty, clear it
    if(vma->len <= 0){
      if(vma->f){
        fileclose(vma->f);
      }
      memset(vma, 0, sizeof(*vma));
    }
  } else if(addr + len == vma->addr + vma->len){
    // Unmapping from the end
    vma->len -= len;
  } else {
    // Unmapping from the middle - this is not supported in this simple implementation
    return -1;
  }
  
  return 0;
}
