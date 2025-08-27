#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
sieve(int input_fd)
{
  int prime, n;
  int p[2];
  
  // 读取第一个数字，这就是素数
  if(read(input_fd, &prime, sizeof(prime)) != sizeof(prime)) {
    close(input_fd);
    return;
  }
  
  printf("prime %d\n", prime);
  
  // 创建管道用于传递给下一级筛选器
  if(pipe(p) < 0) {
    fprintf(2, "pipe failed\n");
    exit(1);
  }
  
  // 创建子进程作为下一级筛选器
  if(fork() == 0) {
    close(p[1]);      // 子进程关闭写端
    sieve(p[0]);      // 递归调用筛选
    close(p[0]);
    exit(0);
  } else {
    close(p[0]);      // 父进程关闭读端
    
    // 读取剩余数字，过滤当前素数的倍数
    while(read(input_fd, &n, sizeof(n)) == sizeof(n)) {
      if(n % prime != 0) {
        write(p[1], &n, sizeof(n));  // 传递非倍数给下一级
      }
    }
    
    close(p[1]);      // 关闭写端，告诉子进程没有更多数据
    close(input_fd);  // 关闭输入端
    wait(0);          // 等待子进程完成
  }
}

int
main(int argc, char *argv[])
{
  int p[2];
  int i;
  
  // 创建初始管道
  if(pipe(p) < 0) {
    fprintf(2, "pipe failed\n");
    exit(1);
  }
  
  // 创建第一个筛选进程
  if(fork() == 0) {
    close(p[1]);      // 子进程关闭写端
    sieve(p[0]);      // 开始筛选
    close(p[0]);
    exit(0);
  } else {
    close(p[0]);      // 父进程关闭读端
    
    // 生成数字2到35
    for(i = 2; i <= 35; i++) {
      write(p[1], &i, sizeof(i));
    }
    
    close(p[1]);      // 关闭写端
    wait(0);          // 等待所有筛选进程完成
  }
  
  exit(0);
}